/* fsmod -- MIT - Copyright NowSecure 2016-2026 */

#if __linux__

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include "fsmod.h"

typedef struct {
	int wd;
	char *path;
} Watch;

typedef struct {
	char *path;
	bool directory;
} Root;

typedef struct PendingMove {
	uint32_t cookie;
	char *path;
	bool directory;
	uint64_t expires;
	struct PendingMove *next;
} PendingMove;

static int fd = -1;
static uint32_t watch_mask = IN_ALL_EVENTS;
static Watch *watches;
static size_t watch_count;
static Root *roots;
static size_t root_count;
static PendingMove *moves;

static bool path_under(const char *path, const char *root) {
	size_t n = strlen (root);
	return !strncmp (path, root, n) &&
		(!path[n] || root[n - 1] == '/' || path[n] == '/');
}

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *watch_path(int wd) {
	size_t i;
	for (i = 0; i < watch_count; i++) {
		if (watches[i].wd == wd) {
			return watches[i].path;
		}
	}
	return NULL;
}

static void remove_watch(int wd) {
	size_t i;
	for (i = 0; i < watch_count; i++) {
		if (watches[i].wd == wd) {
			free (watches[i].path);
			watches[i] = watches[--watch_count];
			return;
		}
	}
}

static bool add_watch(const char *path) {
	int wd = inotify_add_watch (fd, path, watch_mask | IN_ONLYDIR | IN_DONT_FOLLOW);
	if (wd < 0) {
		eprintf ("Cannot watch %s: %s\n", path, strerror (errno));
		return false;
	}
	if (watch_path (wd)) {
		return true;
	}
	char *copy = strdup (path);
	Watch *tmp = copy? realloc (watches, (watch_count + 1) * sizeof (*watches)): NULL;
	if (!tmp) {
		free (copy);
		inotify_rm_watch (fd, wd);
		eprintf ("Cannot allocate inotify watch\n");
		return false;
	}
	watches = tmp;
	watches[watch_count].wd = wd;
	watches[watch_count++].path = copy;
	return true;
}

static bool add_tree(const char *path) {
	if (!add_watch (path)) {
		return false;
	}
	DIR *dir = opendir (path);
	if (!dir) {
		eprintf ("Cannot scan monitor directory %s: %s\n", path, strerror (errno));
		return false;
	}
	bool complete = true;
	struct dirent *entry;
	for (;;) {
		errno = 0;
		entry = readdir (dir);
		if (!entry) {
			if (errno) {
				eprintf ("Cannot read monitor directory %s: %s\n", path, strerror (errno));
				complete = false;
			}
			break;
		}
		if (!strcmp (entry->d_name, ".") || !strcmp (entry->d_name, "..")) {
			continue;
		}
		char child[PATH_MAX];
		int n = snprintf (child, sizeof (child), "%s%s%s", path,
			strcmp (path, "/")? "/": "", entry->d_name);
		struct stat st;
		if (n < 0 || (size_t)n >= sizeof (child)) {
			eprintf ("Monitor directory path exceeds PATH_MAX under %s\n", path);
			complete = false;
		} else if (lstat (child, &st)) {
			if (errno != ENOENT) {
				eprintf ("Cannot inspect monitor path %s: %s\n", child, strerror (errno));
				complete = false;
			}
		} else if (S_ISDIR (st.st_mode) && !add_tree (child)) {
			complete = false;
		}
	}
	closedir (dir);
	return complete;
}

static bool in_directory_root(const char *path) {
	size_t i;
	for (i = 0; i < root_count; i++) {
		if (roots[i].directory && path_under (path, roots[i].path)) {
			return true;
		}
	}
	return false;
}

static void move_watches(const char *oldpath, const char *newpath) {
	size_t i, n = strlen (oldpath);
	for (i = 0; i < watch_count; i++) {
		if (path_under (watches[i].path, oldpath)) {
			size_t size = strlen (newpath) + strlen (watches[i].path + n) + 1;
			char *path = malloc (size);
			if (path) {
				snprintf (path, size, "%s%s", newpath, watches[i].path + n);
				free (watches[i].path);
				watches[i].path = path;
			}
		}
	}
}

static void remove_tree(const char *path) {
	size_t i = 0;
	while (i < watch_count) {
		if (path_under (watches[i].path, path)) {
			int wd = watches[i].wd;
			inotify_rm_watch (fd, wd);
			remove_watch (wd);
		} else {
			i++;
		}
	}
}

static void emit(FileMonitor *fm, FileMonitorCallback cb, int type,
		const char *path, const char *newpath) {
	FileMonitorEvent ev = {0};
	ev.type = type;
	ev.file = path;
	ev.newfile = newpath;
	/* inotify has no originating process information. */
	if (cb) {
		cb (fm, &ev);
	}
}

static void flush_moves(FileMonitor *fm, FileMonitorCallback cb, bool all) {
	PendingMove **p = &moves;
	uint64_t now = now_ms ();
	while (*p) {
		PendingMove *move = *p;
		if (all || now >= move->expires) {
			if (cb) {
				emit (fm, cb, FSE_RENAME, move->path, NULL);
			}
			if (move->directory) {
				remove_tree (move->path);
			}
			*p = move->next;
			free (move->path);
			free (move);
		} else {
			p = &move->next;
		}
	}
}

static void handle_event(FileMonitor *fm, FileMonitorCallback cb, struct inotify_event *ie) {
	if (ie->mask & IN_Q_OVERFLOW) {
		eprintf ("Warning: inotify queue overflow; events were lost, refreshing watches\n");
		size_t i;
		for (i = 0; i < root_count; i++) {
			if (roots[i].directory) {
				add_tree (roots[i].path);
			}
		}
		return;
	}
	if (ie->mask & IN_IGNORED) {
		remove_watch (ie->wd);
		return;
	}
	const char *parent = watch_path (ie->wd);
	if (!parent) {
		return;
	}
	char path[PATH_MAX];
	int n = ie->len && *ie->name
		? snprintf (path, sizeof (path), "%s%s%s", parent, strcmp (parent, "/")? "/": "", ie->name)
		: snprintf (path, sizeof (path), "%s", parent);
	if (n < 0 || (size_t)n >= sizeof (path)) {
		eprintf ("Warning: inotify path exceeds PATH_MAX\n");
		return;
	}
	if (ie->mask & IN_MOVED_FROM) {
		PendingMove *move = calloc (1, sizeof (*move));
		if (move && (move->path = strdup (path))) {
			move->cookie = ie->cookie;
			move->directory = (ie->mask & IN_ISDIR) != 0;
			move->expires = now_ms () + 100;
			move->next = moves;
			moves = move;
		} else {
			free (move);
			emit (fm, cb, FSE_RENAME, path, NULL);
		}
		return;
	}
	if (ie->mask & IN_MOVED_TO) {
		PendingMove **p = &moves;
		while (*p && (!ie->cookie || (*p)->cookie != ie->cookie)) {
			p = &(*p)->next;
		}
		if (*p) {
			PendingMove *move = *p;
			*p = move->next;
			if (move->directory) {
				move_watches (move->path, path);
				if (!in_directory_root (path)) {
					remove_tree (path);
				}
			}
			emit (fm, cb, FSE_RENAME, move->path, path);
			free (move->path);
			free (move);
		} else {
			emit (fm, cb, FSE_RENAME, path, NULL);
		}
		if ((ie->mask & IN_ISDIR) && in_directory_root (path)) {
			add_tree (path);
		}
		return;
	}
	int type = FSE_INVALID;
	if (ie->mask & IN_CREATE) {
		type = (ie->mask & IN_ISDIR)? FSE_CREATE_DIR: FSE_CREATE_FILE;
		if ((ie->mask & IN_ISDIR) && in_directory_root (path)) {
			add_tree (path);
		}
	} else if (ie->mask & (IN_DELETE | IN_DELETE_SELF)) {
		type = FSE_DELETE;
	} else if (ie->mask & IN_MOVE_SELF) {
		type = FSE_RENAME;
	} else if (ie->mask & IN_MODIFY) {
		type = FSE_CONTENT_MODIFIED;
	} else if (ie->mask & IN_ATTRIB) {
		type = FSE_STAT_CHANGED;
	} else if (ie->mask & IN_CLOSE_WRITE) {
		type = FSE_CLOSE_WRITABLE;
	} else if (ie->mask & IN_CLOSE_NOWRITE) {
		type = FSE_CLOSE;
	} else if (!(ie->mask & IN_ISDIR) && (ie->mask & IN_OPEN)) {
		type = FSE_OPEN;
	} else if (!(ie->mask & IN_ISDIR) && (ie->mask & IN_ACCESS)) {
		type = FSE_STAT_CHANGED;
	}
	if (type != FSE_INVALID) {
		emit (fm, cb, type, path, NULL);
	}
}

static bool fm_end(FileMonitor *fm) {
	bool open = fd >= 0;
	flush_moves (fm, NULL, true);
	if (open) {
		close (fd);
		fd = -1;
	}
	size_t i;
	for (i = 0; i < watch_count; i++) {
		free (watches[i].path);
	}
	free (watches);
	watches = NULL;
	watch_count = 0;
	for (i = 0; i < root_count; i++) {
		free (roots[i].path);
	}
	free (roots);
	roots = NULL;
	root_count = 0;
	return open;
}

static bool fm_begin(FileMonitor *fm) {
	/* Service reporting reads programs and TLS certificates. Watching reads
	 * would turn each delivery into another event and a new delivery. */
	watch_mask = fm->changes_only
		? IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVED_TO |
		  IN_MOVE_SELF | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE
		: IN_ALL_EVENTS;
	fd = inotify_init ();
	if (fd < 0) {
		perror ("inotify_init");
		return false;
	}
	if (fcntl (fd, F_SETFD, FD_CLOEXEC) == -1) {
		perror ("inotify FD_CLOEXEC");
		fm_end (fm);
		return false;
	}
	size_t count = fm->root_count? fm->root_count: 1;
	roots = calloc (count, sizeof (*roots));
	if (!roots) {
		fm_end (fm);
		return false;
	}
	size_t i;
	for (i = 0; i < count; i++) {
		const char *input = fm->root_count? fm->roots[i]: (fm->root? fm->root: ".");
		char *path = realpath (input, NULL);
		struct stat st;
		if (!path || stat (path, &st)) {
			eprintf ("Cannot resolve monitor path %s: %s\n", input, strerror (errno));
			free (path);
			fm_end (fm);
			return false;
		}
		roots[root_count].path = path;
		roots[root_count++].directory = S_ISDIR (st.st_mode);
		bool installed;
		if (S_ISDIR (st.st_mode)) {
			installed = add_tree (path);
		} else {
			char *parent = strdup (path);
			if (!parent) {
				fm_end (fm);
				return false;
			}
			char *slash = strrchr (parent, '/');
			if (slash == parent) {
				slash[1] = 0;
			} else {
				*slash = 0;
			}
			installed = add_watch (parent);
			free (parent);
		}
		if (!installed) {
			eprintf ("Cannot completely monitor configured root %s\n", input);
			fm_end (fm);
			return false;
		}
	}
	if (!watch_count) {
		eprintf ("No inotify watches could be installed\n");
		fm_end (fm);
		return false;
	}
	return true;
}

static bool fm_loop(FileMonitor *fm, FileMonitorCallback cb) {
	char buf[65536] __attribute__ ((aligned(8)));
	while (fm->running) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int ready = poll (&pfd, 1, moves? 100: 250);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror ("inotify poll");
			return false;
		}
		if (!fm->running) {
			break;
		}
		if (ready && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			eprintf ("inotify descriptor closed unexpectedly\n");
			return false;
		}
		if (ready && (pfd.revents & POLLIN)) {
			ssize_t len = read (fd, buf, sizeof (buf));
			if (len < 0 && errno == EINTR) {
				continue;
			}
			if (len <= 0) {
				perror ("inotify read");
				return false;
			}
			size_t offset = 0;
			while (offset + sizeof (struct inotify_event) <= (size_t)len) {
				struct inotify_event *ie = (void *)(buf + offset);
				size_t size = sizeof (*ie) + ie->len;
				if (size > (size_t)len - offset) {
					eprintf ("Malformed inotify event\n");
					return false;
				}
				handle_event (fm, cb, ie);
				offset += size;
			}
		}
		flush_moves (fm, cb, false);
	}
	flush_moves (fm, cb, true);
	return true;
}

FileMonitorBackend fmb_inotify = {
	.name = "inotify",
	.begin = fm_begin,
	.loop = fm_loop,
	.end = fm_end,
};

#endif
