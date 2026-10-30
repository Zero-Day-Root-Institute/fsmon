/* fsmod -- MIT - Copyright NowSecure 2016 - pancake@nowsecure.com  */

#if __linux__

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include "fsmod.h"

/* available on 2.6.37 and android-21 */
/* kernel syscall */
#ifndef HAVE_FANOTIFY
#define HAVE_FANOTIFY 1
#endif

/* available on glibc, not in bionic */
/* libc api */
#ifndef HAVE_SYS_FANOTIFY
#define HAVE_SYS_FANOTIFY 1
#endif

#include <sys/inotify.h>
#if HAVE_FANOTIFY
#if HAVE_SYS_FANOTIFY && __GLIBC__
#include <sys/fanotify.h>
#else
#include <asm/unistd.h>

static int fanotify_init(unsigned int __flags, unsigned int __event_f_flags) {
	return syscall (__NR_fanotify_init, __flags, __event_f_flags);
}

static int fanotify_mark(int __fanotify_fd, unsigned int __flags,
	uint64_t __mask, int __dfd, const char *__pathname) {
	return syscall (__NR_fanotify_mark, __fanotify_fd, __flags, __mask, __dfd, __pathname);
}

#endif

#include <linux/fanotify.h>
 
#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

#if HAVE_FANOTIFY
static volatile int fan_fd = -1;
#endif

static void fm_control_c(void) {
	if (fan_fd != -1) {
		close (fan_fd);
		fan_fd = -1;
	}
}

/* fanotify fallback */
static void usr1_handler(int sig __attribute__((unused)),
		siginfo_t *si __attribute__((unused)), void *unused __attribute__((unused))) {
	fanotify_mark (fan_fd, FAN_MARK_FLUSH, 0, 0, NULL);
}

static int handle_perm(int fan_fd, struct fanotify_event_metadata *metadata) {
	struct fanotify_response response_struct = {
		.fd = metadata->fd,
		.response = FAN_ALLOW
	};
	const int ret = write (fan_fd, &response_struct, sizeof (response_struct));
	return (ret < 0) ? ret : 0;
}

static bool parseFaEvent(FileMonitor *fm, struct fanotify_event_metadata *metadata, FileMonitorEvent *ev, char *opath, size_t opath_size) {
	if (metadata->fd >= 0) {
		char path[PATH_MAX];
		sprintf (path, "/proc/self/fd/%d", metadata->fd);
		ssize_t path_len;
		do {
			path_len = readlink (path, opath, opath_size - 1);
		} while (path_len < 0 && errno == EINTR && fm->running);
		if (path_len < 0 || (size_t)path_len >= opath_size - 1) {
			if (fm->running) eprintf ("Cannot resolve fanotify event path for fd %d\n", metadata->fd);
			return false;
		}
		opath[path_len] = '\0';
	} else {
		strcpy (opath, ".");
	}
	ev->file = opath;
	ev->pid = metadata->pid;
	ev->proc = get_proc_name (ev->pid, &ev->ppid);
	if (metadata->mask & FAN_ACCESS) {
		ev->type = FSE_STAT_CHANGED;
	}
	if (metadata->mask & FAN_OPEN) {
		ev->type = FSE_OPEN;
	}
	if (metadata->mask & FAN_MODIFY) {
		ev->type = FSE_CONTENT_MODIFIED;
	}
	if (metadata->mask & FAN_CLOSE) {
		if (metadata->mask & FAN_CLOSE_WRITE) {
			ev->type = FSE_CLOSE_WRITABLE;
		}
		if (metadata->mask & FAN_CLOSE_NOWRITE) {
			ev->type = FSE_CLOSE;
		}
	}
	if (metadata->mask & FAN_OPEN_PERM) {
		ev->type = FSE_OPEN;
	}
	if (metadata->mask & FAN_ACCESS_PERM) {
		ev->type = FSE_STAT_CHANGED;
	}
	if (metadata->mask & FAN_ALL_PERM_EVENTS) {
		if (handle_perm (fan_fd, metadata)) {
			return false;
		}
	}
	return true;
}

static bool fm_loop (FileMonitor *fm, FileMonitorCallback cb) {
	char buf[4096] __attribute__ ((aligned(8)));
	while (fm->running && fan_fd >= 0) {
		struct pollfd pfd = { .fd = fan_fd, .events = POLLIN };
		int ready = poll (&pfd, 1, 250);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror ("fanotify poll");
			return false;
		}
		if (!fm->running) {
			break;
		}
		if (!ready) {
			continue;
		}
		if (!(pfd.revents & POLLIN)) {
			eprintf ("fanotify descriptor closed unexpectedly\n");
			return false;
		}
		ssize_t len = read (fan_fd, buf, sizeof (buf));
		if (len < 0 && errno == EINTR) {
			continue;
		}
		if (len <= 0) {
			perror ("fanotify read");
			return false;
		}
		struct fanotify_event_metadata *metadata = (void *)buf;
		bool valid = true;
		while (FAN_EVENT_OK (metadata, len)) {
			FileMonitorEvent ev = { .type = FSE_INVALID };
			char path[PATH_MAX];
			if (metadata->vers != FANOTIFY_METADATA_VERSION) {
				eprintf ("Unsupported fanotify metadata version\n");
				valid = false;
			} else if (metadata->mask & FAN_Q_OVERFLOW) {
				eprintf ("Warning: fanotify queue overflow; events were lost\n");
			} else if (valid && parseFaEvent (fm, metadata, &ev, path, sizeof (path))) {
				if (ev.type != FSE_INVALID && cb) {
					cb (fm, &ev);
				}
			}
			if (metadata->fd >= 0) {
				close (metadata->fd);
			}
			metadata = FAN_EVENT_NEXT (metadata, len);
		}
		if (!valid) {
			return false;
		}
	}
	return true;
}

static bool fm_begin(FileMonitor *fm) {
	/* A reporter opens executables/certificates for every delivery. Services
	 * subscribe only to mutations to avoid reporting their own read traffic.
	 * Keep the mount API compatible: do not add FAN_CREATE/FAN_DELETE. */
	uint64_t fan_mask = fm->changes_only
		? FAN_MODIFY | FAN_CLOSE_WRITE
		: FAN_OPEN | FAN_CLOSE | FAN_ACCESS | FAN_MODIFY;
	unsigned int mark_flags = FAN_MARK_ADD, init_flags = FAN_CLOEXEC;
	struct sigaction sa;

	fm->control_c = fm_control_c;
	//mark_flags |= FAN_MARK_REMOVE;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset (&sa.sa_mask);
	sa.sa_sigaction = usr1_handler;
	if (sigaction (SIGUSR1, &sa, NULL) == -1) {
		eprintf ("Cannot set SIGUSR1 signal handler\n");
		return false;
	}
	fan_mask |= FAN_ONDIR;
	fan_mask |= FAN_EVENT_ON_CHILD;
	mark_flags |= FAN_MARK_MOUNT; // walk into subdirectories
	init_flags |= (fan_mask & FAN_ALL_PERM_EVENTS)
		? FAN_CLASS_CONTENT
		: FAN_CLASS_NOTIF;
	fan_fd = fanotify_init (init_flags, O_RDONLY | O_CLOEXEC);
	if (fan_fd < 0) {
		perror ("fanotify_init");
		return false;
	}
	size_t i, count = fm->root_count? fm->root_count: 1;
	for (i = 0; i < count; i++) {
		const char *root = fm->root_count? fm->roots[i]: (fm->root? fm->root: "/");
		if (fanotify_mark (fan_fd, mark_flags, fan_mask, AT_FDCWD, root) != 0) {
			eprintf ("Cannot mark fanotify root %s: %s\n", root, strerror (errno));
			close (fan_fd);
			fan_fd = -1;
			return false;
		}
	}
	return true;
}

static bool fm_end(FileMonitor *fm) {
#define FMCLOSE(x) \
	if (x != -1) { \
		close (x); \
		x = -1; \
		done = true; \
	}
	bool done = false;
	FMCLOSE (fan_fd);
	return done;
}

FileMonitorBackend fmb_fanotify = {
	.name = "fanotify",
	.begin = fm_begin,
	.loop = fm_loop,
	.end = fm_end,
};

#endif
#endif
