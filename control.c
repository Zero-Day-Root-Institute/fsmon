#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include "control.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <inttypes.h>
#include <spawn.h>
#include <sys/wait.h>
#if __APPLE__
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#endif

#define CONTROL_MAX_HOLDS 64
#define CONTROL_ID_SIZE 128
#define CONTROL_RESPONSE_SIZE 32768
#define CONTROL_TIMEOUT 5

typedef struct {
	char id[CONTROL_ID_SIZE];
	bool all;
	double expires;
} Hold;

struct FmControl {
	FmReporter *reporter;
	const FileMonitor *monitor;
	pthread_t worker;
	pthread_mutex_t mutex;
	int fd;
	bool stopping, monitor_paused, report_paused;
	char socket_path[PATH_MAX], state_path[PATH_MAX], boot[128];
	Hold holds[CONTROL_MAX_HOLDS];
	size_t hold_count;
	uint64_t suppressed_monitor, suppressed_report;
	uint64_t monitor_cutoff, report_cutoff;
	dev_t socket_device; ino_t socket_inode;
};

static void errorf(char *error, size_t size, const char *format, ...) {
	if (!error || !size) return;
	va_list args; va_start(args, format); vsnprintf(error, size, format, args); va_end(args);
}

static double monotonic_now(void) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static uint64_t event_clock(void) {
#if __APPLE__
	return mach_absolute_time();
#else
	return 0;
#endif
}

static bool boot_identity(char *buffer, size_t size) {
#if __APPLE__
	/* Prefer the boot session UUID: kern.boottime may move when wall time is
	 * corrected. Older systems fall back to their kernel boot time. */
	size_t identity_size = size;
	if (!sysctlbyname("kern.bootsessionuuid", buffer, &identity_size, NULL, 0) &&
		identity_size > 1 && identity_size <= size && memchr(buffer, 0, identity_size)) return true;
	struct timeval boot; size_t length = sizeof(boot);
	if (sysctlbyname("kern.boottime", &boot, &length, NULL, 0)) return false;
	int count = snprintf(buffer, size, "%lld-%lld", (long long)boot.tv_sec, (long long)boot.tv_usec);
	return count > 0 && (size_t)count < size;
#else
	FILE *stream = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (!stream) return false;
	bool ok = fgets(buffer, (int)size, stream) != NULL; fclose(stream);
	if (ok) buffer[strcspn(buffer, "\r\n")] = 0;
	return ok && *buffer;
#endif
}

bool fm_control_path(const char *pid_file, char *path, size_t capacity) {
	if (!pid_file || !path || !capacity) return false;
	int count = snprintf(path, capacity, "%s.ctl", pid_file);
	if (count > 0 && (size_t)count < capacity && (size_t)count < sizeof(((struct sockaddr_un *)0)->sun_path)) return true;
	/* Darwin's sockaddr_un path is much shorter than PATH_MAX. Long valid PID
	 * paths retain their state beside the PID file and use a stable short
	 * endpoint, with the same owner/peer protections as the normal endpoint. */
	uint64_t hash = UINT64_C(14695981039346656037);
	for (const unsigned char *p = (const unsigned char *)pid_file; *p; p++) { hash ^= *p; hash *= UINT64_C(1099511628211); }
	count = snprintf(path, capacity, "/tmp/fsmod-%lu-%016" PRIx64 ".ctl", (unsigned long)geteuid(), hash);
	return count > 0 && (size_t)count < capacity && (size_t)count < sizeof(((struct sockaddr_un *)0)->sun_path);
}

static void unlink_own_socket(FmControl *control) {
	struct stat st;
	if (control->socket_inode && !lstat(control->socket_path, &st) &&
		st.st_dev == control->socket_device && st.st_ino == control->socket_inode && S_ISSOCK(st.st_mode))
		(void)unlink(control->socket_path);
}

static bool valid_id(const char *id) {
	if (!id || !*id || strlen(id) >= CONTROL_ID_SIZE) return false;
	for (const unsigned char *p = (const unsigned char *)id; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return false;
	return true;
}

static bool owned_private(const struct stat *st) {
	return st->st_uid == geteuid() && !(st->st_mode & 0077);
}

static bool write_all(int fd, const char *buffer, size_t length) {
	double deadline = monotonic_now() + CONTROL_TIMEOUT + 3;
	while (length) {
		if (monotonic_now() >= deadline) { errno = ETIMEDOUT; return false; }
		ssize_t sent = write(fd, buffer, length);
		if (sent < 0 && errno == EINTR) continue;
		if (sent <= 0) return false;
		buffer += sent; length -= (size_t)sent;
	}
	return true;
}

static bool connect_local(int fd, const struct sockaddr_un *address, unsigned timeout_seconds) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) return false;
	int result = connect(fd, (const struct sockaddr *)address, sizeof(*address));
	int saved = errno;
	if (result && saved == EINPROGRESS) {
		struct pollfd ready = { .fd = fd, .events = POLLOUT };
		int polled; do { polled = poll(&ready, 1, (int)timeout_seconds * 1000); } while (polled < 0 && errno == EINTR);
		if (polled > 0) {
			socklen_t size = sizeof(saved);
			if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &saved, &size)) saved = errno;
			result = saved ? -1 : 0;
		} else { result = -1; saved = polled == 0 ? ETIMEDOUT : errno; }
	}
	(void)fcntl(fd, F_SETFL, flags); errno = saved; return result == 0;
}

/* State is separate from the event queue. Only leases survive daemon restart.
 * A boot identity and monotonic deadlines prevent old leases crossing reboot. */
static bool save_state(FmControl *control, char *error, size_t error_size) {
	char buffer[16384], temporary[PATH_MAX];
	struct stat st;
	if (!lstat(control->state_path, &st)) {
		if (!S_ISREG(st.st_mode) || !owned_private(&st) || st.st_nlink != 1) {
			errorf(error, error_size, "unsafe control state file"); return false;
		}
	} else if (errno != ENOENT) { errorf(error, error_size, "inspect control state: %s", strerror(errno)); return false; }
	size_t used = (size_t)snprintf(buffer, sizeof(buffer), "FSMOD-CONTROL-1 %s\n", control->boot);
	for (size_t i = 0; i < control->hold_count; i++) {
		int count = snprintf(buffer + used, sizeof(buffer) - used, "%s %s %.9f\n",
			control->holds[i].id, control->holds[i].all ? "all" : "report", control->holds[i].expires);
		if (count < 0 || (size_t)count >= sizeof(buffer) - used) { errorf(error, error_size, "control state too large"); return false; }
		used += (size_t)count;
	}
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", control->state_path) >= (int)sizeof(temporary)) return false;
	int fd = mkstemp(temporary);
	if (fd < 0) { errorf(error, error_size, "create control state: %s", strerror(errno)); return false; }
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	bool ok = !fchmod(fd, 0600) && write_all(fd, buffer, used) && !fsync(fd);
	int saved = errno; if (close(fd) && ok) { ok = false; saved = errno; }
	if (ok && rename(temporary, control->state_path)) { ok = false; saved = errno; }
	if (!ok) { unlink(temporary); errorf(error, error_size, "save control state: %s", strerror(saved)); }
	return ok;
}

static bool load_state(FmControl *control, char *error, size_t error_size) {
	int fd = open(control->state_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT) return true;
		errorf(error, error_size, "open control state: %s", strerror(errno)); return false;
	}
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || !owned_private(&st) || st.st_nlink != 1 || st.st_size > 16383) {
		close(fd); errorf(error, error_size, "unsafe control state file"); return false;
	}
	FILE *stream = fdopen(fd, "r");
	if (!stream) { close(fd); return false; }
	char line[512], boot[128], extra;
	if (!fgets(line, sizeof(line), stream) || sscanf(line, "FSMOD-CONTROL-1 %127s %c", boot, &extra) != 1) {
		fclose(stream); errorf(error, error_size, "invalid control state header"); return false;
	}
	if (strcmp(boot, control->boot)) { fclose(stream); return true; }
	double now = monotonic_now();
	while (fgets(line, sizeof(line), stream)) {
		Hold hold; char scope[16];
		if (sscanf(line, "%127s %15s %lf %c", hold.id, scope, &hold.expires, &extra) != 3 ||
			!valid_id(hold.id) || (strcmp(scope, "all") && strcmp(scope, "report")) ||
			!(hold.expires > 0) || hold.expires > now + 86401.0) {
			fclose(stream); errorf(error, error_size, "invalid control state lease"); return false;
		}
		if (hold.expires <= now) continue;
		if (control->hold_count == CONTROL_MAX_HOLDS) { fclose(stream); return false; }
		hold.all = !strcmp(scope, "all");
		control->holds[control->hold_count++] = hold;
	}
	bool ok = !ferror(stream); fclose(stream); return ok;
}

/* mutex is the callback barrier and also serializes leases, expiration and
 * transport quiescing, so status/TTL can never race a pending pause ACK. */
static bool apply_state(FmControl *control, bool discard) {
	bool all = false, report = control->hold_count != 0;
	for (size_t i = 0; i < control->hold_count; i++) all |= control->holds[i].all;
	/* Once the signal handler has stopped producers, expiration may not wake a
	 * paused worker to send retained payloads during shutdown. */
	if (control->monitor && !control->monitor->running && control->report_paused) return true;
	bool kernel_clock = control->monitor && control->monitor->backend.name &&
		!strcmp(control->monitor->backend.name, "devfsev");
	if (kernel_clock && control->monitor_paused && !all) control->monitor_cutoff = event_clock();
	if (kernel_clock && control->report_paused && !report) control->report_cutoff = event_clock();
	control->monitor_paused = all;
	control->report_paused = report;
	return fm_reporter_pause(control->reporter, report, discard, CONTROL_TIMEOUT);
}

static void expire_holds(FmControl *control) {
	if (control->stopping || (control->monitor && !control->monitor->running)) return;
	double now = monotonic_now(); bool changed = false;
	for (size_t i = 0; i < control->hold_count;) {
		if (control->holds[i].expires > now) { i++; continue; }
		fprintf(stderr, "fsmod: control lease expired: id=%s\n", control->holds[i].id);
		control->holds[i] = control->holds[--control->hold_count]; changed = true;
	}
	if (changed) {
		char error[256];
		if (!save_state(control, error, sizeof(error))) fprintf(stderr, "fsmod: %s\n", error);
		(void)apply_state(control, false);
	}
}

static void status_json(FmControl *control, char *buffer, size_t size) {
	FmReporterStatus report; fm_reporter_status(control->reporter, &report);
	size_t used = (size_t)snprintf(buffer, size,
		"{\"ok\":true,\"pid\":%ld,\"monitor_paused\":%s,\"report_paused\":%s,\"queued\":%zu,\"in_flight\":%s,\"delivered\":%" PRIu64 ",\"failed\":%" PRIu64 ",\"dropped\":%" PRIu64 ",\"suppressed_monitor\":%" PRIu64 ",\"suppressed_report\":%" PRIu64 ",\"holds\":[",
		(long)getpid(), control->monitor_paused ? "true" : "false", control->report_paused ? "true" : "false",
		report.queued, report.in_flight ? "true" : "false", report.delivered, report.failed, report.dropped,
		control->suppressed_monitor, control->suppressed_report);
	double now = monotonic_now();
	for (size_t i = 0; i < control->hold_count && used < size; i++) {
		double remaining = control->holds[i].expires - now;
		int count = snprintf(buffer + used, size - used, "%s{\"id\":\"%s\",\"scope\":\"%s\",\"remaining_seconds\":%.0f}",
			i ? "," : "", control->holds[i].id, control->holds[i].all ? "all" : "report", remaining > 0 ? remaining : 0);
		if (count < 0 || (size_t)count >= size - used) break;
		used += (size_t)count;
	}
	if (used < size) snprintf(buffer + used, size - used, "]}\n");
}

static bool peer_allowed(int fd) {
#if __APPLE__ || defined(__FreeBSD__)
	uid_t uid; gid_t gid; return !getpeereid(fd, &uid, &gid) && uid == geteuid();
#elif defined(__linux__)
	struct ucred credentials; socklen_t size = sizeof(credentials);
	return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) && credentials.uid == geteuid();
#else
	(void)fd; return false;
#endif
}

static bool read_line(int fd, char *buffer, size_t size, unsigned timeout_seconds) {
	size_t used = 0; double deadline = monotonic_now() + timeout_seconds;
	while (used + 1 < size) {
		double left = deadline - monotonic_now(); if (left <= 0) return false;
		struct pollfd ready = { .fd = fd, .events = POLLIN };
		int result = poll(&ready, 1, (int)(left * 1000));
		if (result < 0 && errno == EINTR) continue;
		if (result <= 0) return false;
		char c; ssize_t count = read(fd, &c, 1);
		if (count < 0 && errno == EINTR) continue;
		if (count != 1 || c == 0 || c == '\r') return false;
		if (c == '\n') { buffer[used] = 0; return true; }
		buffer[used++] = c;
	}
	return false;
}

static void handle_client(FmControl *control, int fd) {
	char request[512], response[CONTROL_RESPONSE_SIZE];
	if (!peer_allowed(fd)) {
		const char *denied = "{\"ok\":false,\"error\":\"permission denied\"}\n";
		(void)write_all(fd, denied, strlen(denied)); return;
	}
	if (!read_line(fd, request, sizeof(request), CONTROL_TIMEOUT)) return;
	char *words[7], *save = NULL; size_t count = 0;
	for (char *word = strtok_r(request, " ", &save); word && count < 7; word = strtok_r(NULL, " ", &save)) words[count++] = word;
	const char *error = NULL; char state_error[256];
	pthread_mutex_lock(&control->mutex); expire_holds(control);
	if (control->stopping || (control->monitor && !control->monitor->running)) error = "daemon is stopping";
	else if (count == 1 && !strcmp(words[0], "STATUS")) { /* no mutation */ }
	else if (count == 5 && !strcmp(words[0], "PAUSE") && valid_id(words[1]) &&
		(!strcmp(words[2], "all") || !strcmp(words[2], "report")) &&
		(!strcmp(words[4], "0") || !strcmp(words[4], "1"))) {
		char *end; errno = 0; unsigned long ttl = strtoul(words[3], &end, 10);
		if (errno || *end || ttl < 1 || ttl > 86400) error = "ttl must be 1..86400";
		else {
			size_t at; for (at = 0; at < control->hold_count && strcmp(words[1], control->holds[at].id); at++);
			if (at == CONTROL_MAX_HOLDS) error = "too many pause owners";
			else {
				Hold previous[CONTROL_MAX_HOLDS]; memcpy(previous, control->holds, sizeof(previous)); size_t previous_count = control->hold_count;
				if (at == control->hold_count) control->hold_count++;
				snprintf(control->holds[at].id, sizeof(control->holds[at].id), "%s", words[1]);
				control->holds[at].all = !strcmp(words[2], "all"); control->holds[at].expires = monotonic_now() + ttl;
				if (!save_state(control, state_error, sizeof(state_error))) {
					memcpy(control->holds, previous, sizeof(previous)); control->hold_count = previous_count; error = "cannot persist pause state";
				} else if (!apply_state(control, !strcmp(words[4], "1"))) error = "pause active but HTTP quiesce timed out";
				else {
					/* A slow cancellation must not consume the caller's maintenance TTL. */
					control->holds[at].expires = monotonic_now() + ttl;
					if (!save_state(control, state_error, sizeof(state_error))) error = "pause active but cannot persist renewed deadline";
					else fprintf(stderr, "fsmod: control pause: id=%s scope=%s ttl=%lu\n", words[1], words[2], ttl);
				}
			}
		}
	} else if (count == 2 && !strcmp(words[0], "RESUME") && valid_id(words[1])) {
		size_t at; for (at = 0; at < control->hold_count && strcmp(words[1], control->holds[at].id); at++);
		if (at < control->hold_count) {
			Hold previous[CONTROL_MAX_HOLDS]; memcpy(previous, control->holds, sizeof(previous)); size_t previous_count = control->hold_count;
			control->holds[at] = control->holds[--control->hold_count];
			if (!save_state(control, state_error, sizeof(state_error))) {
				memcpy(control->holds, previous, sizeof(previous)); control->hold_count = previous_count; error = "cannot persist resume state";
			} else if (!apply_state(control, false)) error = "control transition timed out";
			else fprintf(stderr, "fsmod: control resume: id=%s\n", words[1]);
		}
	} else error = "invalid control command";
	if (error) snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}\n", error);
	else status_json(control, response, sizeof(response));
	pthread_mutex_unlock(&control->mutex);
	(void)write_all(fd, response, strlen(response));
}

static void *control_worker(void *opaque) {
	FmControl *control = opaque; sigset_t signals; sigfillset(&signals); (void)pthread_sigmask(SIG_BLOCK, &signals, NULL);
	for (;;) {
		pthread_mutex_lock(&control->mutex); bool stopping = control->stopping; if (!stopping) expire_holds(control); pthread_mutex_unlock(&control->mutex);
		if (stopping) break;
		struct pollfd ready = { .fd = control->fd, .events = POLLIN };
		int result = poll(&ready, 1, 100);
		if (result <= 0 || !(ready.revents & POLLIN)) continue;
		int client = accept(control->fd, NULL, NULL);
		if (client < 0) continue;
		(void)fcntl(client, F_SETFD, FD_CLOEXEC);
		int flags = fcntl(client, F_GETFL, 0);
		if (flags >= 0) (void)fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
		struct timeval timeout = { .tv_sec = CONTROL_TIMEOUT + 2 };
		(void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
		int enabled = 1; (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
		handle_client(control, client); close(client);
	}
	return NULL;
}

FmControl *fm_control_start(const char *pid_file, FmReporter *reporter, const FileMonitor *monitor, char *error, size_t error_size) {
	FmControl *control = calloc(1, sizeof(*control));
	if (!control) { errorf(error, error_size, "cannot allocate runtime control"); return NULL; }
	control->fd = -1; control->reporter = reporter; control->monitor = monitor;
	if (!fm_control_path(pid_file, control->socket_path, sizeof(control->socket_path)) ||
		snprintf(control->state_path, sizeof(control->state_path), "%s.ctl.state", pid_file) >= (int)sizeof(control->state_path)) {
		errorf(error, error_size, "PID path too long for control socket"); free(control); return NULL;
	}
	int mutex_error = pthread_mutex_init(&control->mutex, NULL);
	if (mutex_error) { errorf(error, error_size, "control mutex: %s", strerror(mutex_error)); free(control); return NULL; }
	if (!boot_identity(control->boot, sizeof(control->boot))) { errorf(error, error_size, "cannot read boot identity"); goto fail; }
	if (!load_state(control, error, error_size)) goto fail;
	if (!apply_state(control, false)) { errorf(error, error_size, "cannot restore control state"); goto fail; }
	struct stat st;
	if (!lstat(control->socket_path, &st)) {
		if (!S_ISSOCK(st.st_mode) || !owned_private(&st)) { errorf(error, error_size, "unsafe existing control socket"); goto fail; }
		int probe = socket(AF_UNIX, SOCK_STREAM, 0);
		if (probe < 0) goto system_error;
		struct sockaddr_un address; memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
		memcpy(address.sun_path, control->socket_path, strlen(control->socket_path) + 1);
		bool active = connect_local(probe, &address, 1); int probe_error = errno; close(probe);
		if (active || (probe_error != ECONNREFUSED && probe_error != ENOENT)) {
			errorf(error, error_size, "control socket is already in use"); goto fail;
		}
		struct stat current;
		if (lstat(control->socket_path, &current) || st.st_dev != current.st_dev || st.st_ino != current.st_ino) {
			errorf(error, error_size, "control socket changed during startup"); goto fail;
		}
		if (unlink(control->socket_path)) goto system_error;
	} else if (errno != ENOENT) goto system_error;
	control->fd = socket(AF_UNIX, SOCK_STREAM, 0); if (control->fd < 0) goto system_error;
	(void)fcntl(control->fd, F_SETFD, FD_CLOEXEC);
	struct sockaddr_un address; memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
	memcpy(address.sun_path, control->socket_path, strlen(control->socket_path) + 1);
	mode_t previous = umask(0077); int bound = bind(control->fd, (struct sockaddr *)&address, sizeof(address)); int saved = errno; umask(previous); errno = saved;
	if (bound) goto system_error;
	if (lstat(control->socket_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) goto system_error;
	control->socket_device = st.st_dev; control->socket_inode = st.st_ino;
	if (chmod(control->socket_path, 0600) || listen(control->fd, 8)) goto system_error;
	sigset_t blocked, previous_signals; sigfillset(&blocked);
	int thread_error = pthread_sigmask(SIG_BLOCK, &blocked, &previous_signals);
	if (!thread_error) {
		thread_error = pthread_create(&control->worker, NULL, control_worker, control);
		(void)pthread_sigmask(SIG_SETMASK, &previous_signals, NULL);
	}
	if (thread_error) { errorf(error, error_size, "control thread: %s", strerror(thread_error)); goto fail_socket; }
	return control;
system_error:
	errorf(error, error_size, "control socket: %s", strerror(errno));
fail_socket:
	if (control->fd >= 0) { close(control->fd); unlink_own_socket(control); }
fail:
	pthread_mutex_destroy(&control->mutex); free(control); return NULL;
}

void fm_control_stop(FmControl *control) {
	if (!control) return;
	pthread_mutex_lock(&control->mutex); control->stopping = true; pthread_mutex_unlock(&control->mutex);
	pthread_join(control->worker, NULL); close(control->fd); unlink_own_socket(control);
	pthread_mutex_destroy(&control->mutex); free(control);
}

bool fm_control_event_begin(FmControl *control, const FileMonitorEvent *event) {
	if (!control) return true;
	pthread_mutex_lock(&control->mutex);
	if (control->monitor_paused || (event && event->tstamp && control->monitor_cutoff && event->tstamp <= control->monitor_cutoff)) {
		control->suppressed_monitor++; pthread_mutex_unlock(&control->mutex); return false;
	}
	return true;
}

bool fm_control_should_report(FmControl *control, const FileMonitorEvent *event) {
	if (!control) return true;
	if (control->report_paused || (event && event->tstamp && control->report_cutoff && event->tstamp <= control->report_cutoff)) {
		control->suppressed_report++; return false;
	}
	return true;
}

void fm_control_event_end(FmControl *control) { if (control) pthread_mutex_unlock(&control->mutex); }

bool fm_control_is_internal(FmControl *control, const char *path) {
	if (!control || !path) return false;
	return !strcmp(path, control->socket_path) || !strcmp(path, control->state_path) ||
		(!strncmp(path, control->state_path, strlen(control->state_path)) && !strncmp(path + strlen(control->state_path), ".tmp.", 5));
}

static bool control_request(const char *socket_path, const char *request, char *response, size_t capacity) {
	if (!socket_path || !*socket_path || strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) { fprintf(stderr, "Control: invalid socket path\n"); return false; }
	struct stat st; if (lstat(socket_path, &st) || !S_ISSOCK(st.st_mode) || !owned_private(&st)) { fprintf(stderr, "Control: socket unavailable or not private: %s\n", socket_path); return false; }
	int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) { perror("Control"); return false; }
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	struct sockaddr_un address; memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX; memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
	struct timeval timeout = { .tv_sec = CONTROL_TIMEOUT + 3 }; (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
	int enabled = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
	if (!connect_local(fd, &address, CONTROL_TIMEOUT) || !peer_allowed(fd) || !write_all(fd, request, strlen(request))) {
		fprintf(stderr, "Control: connect/send failed: %s\n", strerror(errno)); close(fd); return false;
	}
	bool read_ok = read_line(fd, response, capacity, CONTROL_TIMEOUT + 3); close(fd);
	if (!read_ok) { fprintf(stderr, "Control: response timeout or invalid response\n"); return false; }
	return !strncmp(response, "{\"ok\":true,", 11);
}

#if !TARGET_WATCHOS
static volatile sig_atomic_t run_signal;
static volatile sig_atomic_t run_process_group;

static void run_signal_handler(int signal_number) {
	int saved = errno;
	if (!run_signal) run_signal = signal_number;
	if (run_process_group > 0) (void)kill(-(pid_t)run_process_group, signal_number);
	errno = saved;
}

static bool unique_run_id(char *id, size_t capacity) {
	unsigned char random[16]; size_t offset = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	while (offset < sizeof(random)) {
		ssize_t count = read(fd, random + offset, sizeof(random) - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) { close(fd); return false; }
		offset += (size_t)count;
	}
	close(fd);
	int count = snprintf(id, capacity, "run-%ld-", (long)getpid());
	if (count < 0 || (size_t)count + sizeof(random) * 2 + 1 > capacity) return false;
	for (size_t i = 0; i < sizeof(random); i++) snprintf(id + count + i * 2, capacity - (size_t)count - i * 2, "%02x", random[i]);
	return true;
}

/* Change terminal ownership with SIGTTOU blocked, so a background wrapper
 * can restore its parent's foreground group after its child has exited. */
static bool terminal_group(int fd, pid_t group) {
	sigset_t blocked, previous; sigemptyset(&blocked); sigaddset(&blocked, SIGTTOU);
	if (sigprocmask(SIG_BLOCK, &blocked, &previous)) return false;
	int result; do { result = tcsetpgrp(fd, group); } while (result < 0 && errno == EINTR);
	int saved = errno; (void)sigprocmask(SIG_SETMASK, &previous, NULL); errno = saved;
	return result == 0;
}

extern char **environ;

static int control_run(const char *socket_path, const char *scope, unsigned long ttl, bool discard, char **command) {
	char id[CONTROL_ID_SIZE], request[512], response[CONTROL_RESPONSE_SIZE] = {0};
	if (!unique_run_id(id, sizeof(id))) { fprintf(stderr, "Control run: cannot generate lease ID\n"); return 1; }
	const int signals[] = { SIGINT, SIGTERM, SIGHUP };
	struct sigaction previous_actions[3], handler = { .sa_handler = run_signal_handler };
	sigemptyset(&handler.sa_mask); for (size_t i = 0; i < 3; i++) sigaddset(&handler.sa_mask, signals[i]);
	sigset_t blocked, original_mask; sigemptyset(&blocked); for (size_t i = 0; i < 3; i++) sigaddset(&blocked, signals[i]);
	if (sigprocmask(SIG_BLOCK, &blocked, &original_mask)) { perror("Control run: block signals"); return 1; }
	run_signal = run_process_group = 0;
	size_t installed = 0;
	for (; installed < 3; installed++) if (sigaction(signals[installed], &handler, &previous_actions[installed])) break;
	if (installed < 3) {
		int saved = errno; while (installed) { installed--; (void)sigaction(signals[installed], &previous_actions[installed], NULL); }
		(void)sigprocmask(SIG_SETMASK, &original_mask, NULL); errno = saved; perror("Control run: signal handler"); return 1;
	}
	struct sigaction previous_child, default_child = { .sa_handler = SIG_DFL };
	sigemptyset(&default_child.sa_mask);
	if (sigaction(SIGCHLD, &default_child, &previous_child)) {
		int saved = errno; for (size_t i = 0; i < 3; i++) (void)sigaction(signals[i], &previous_actions[i], NULL);
		(void)sigprocmask(SIG_SETMASK, &original_mask, NULL); errno = saved; perror("Control run: child signal handler"); return 1;
	}
	(void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
	snprintf(request, sizeof(request), "PAUSE %s %s %lu %d\n", id, scope, ttl, discard ? 1 : 0);
	int result = 1; bool paused = control_request(socket_path, request, response, sizeof(response));
	if (!paused) { if (*response) fprintf(stderr, "Control run: pause failed: %s\n", response); if (run_signal) result = 128 + run_signal; }
	else if (run_signal) result = 128 + run_signal;
	else {
		/* Spawn with the forwarded signals blocked in this parent until the
		 * child process group is published. The child's original mask and
		 * default handlers are installed atomically by posix_spawn. */
		(void)sigprocmask(SIG_BLOCK, &blocked, NULL);
		posix_spawnattr_t attributes; int spawn_error = posix_spawnattr_init(&attributes); bool attributes_ready = !spawn_error;
		sigset_t defaults; sigemptyset(&defaults); for (size_t i = 0; i < 3; i++) sigaddset(&defaults, signals[i]); sigaddset(&defaults, SIGPIPE);
		if (!spawn_error) spawn_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
		if (!spawn_error) spawn_error = posix_spawnattr_setpgroup(&attributes, 0);
		if (!spawn_error) spawn_error = posix_spawnattr_setsigmask(&attributes, &original_mask);
		if (!spawn_error) spawn_error = posix_spawnattr_setsigdefault(&attributes, &defaults);
		pid_t child = -1, foreground = -1;
		int terminal = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (terminal >= 0 && tcgetpgrp(terminal) == getpgrp()) foreground = getpgrp();
		else if (terminal >= 0) { close(terminal); terminal = -1; }
		bool terminal_failed = false;
		bool child_reaped = false; int early_status = 0;
		if (!spawn_error && !run_signal) spawn_error = posix_spawnp(&child, command[0], NULL, &attributes, command, environ);
		if (attributes_ready) (void)posix_spawnattr_destroy(&attributes);
		if (child > 0) {
			run_process_group = (sig_atomic_t)child;
			if (foreground > 0) {
				if (terminal_group(terminal, child)) (void)kill(-child, SIGCONT);
				else {
					int handoff_error = errno;
					/* A command such as true may already have exited, leaving no
					 * process group to receive the terminal. Preserve that result. */
					pid_t early; do { early = waitpid(child, &early_status, WNOHANG); } while (early < 0 && errno == EINTR);
					if (early == child) child_reaped = true;
					else {
						fprintf(stderr, "Control run: cannot hand terminal to command: %s\n", strerror(handoff_error));
						terminal_failed = true; (void)kill(-child, SIGKILL);
					}
				}
			}
		}
		(void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
		if (child > 0) {
			int status = early_status; pid_t waited = child;
			if (!child_reaped) do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
			(void)sigprocmask(SIG_BLOCK, &blocked, NULL); run_process_group = 0;
			if (foreground > 0 && !terminal_group(terminal, foreground)) { fprintf(stderr, "Control run: cannot restore terminal: %s\n", strerror(errno)); terminal_failed = true; }
			(void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
			if (waited < 0) { perror("Control run: waitpid"); result = 1; }
			else if (run_signal) result = 128 + run_signal;
			else if (WIFEXITED(status)) result = WEXITSTATUS(status);
			else if (WIFSIGNALED(status)) result = 128 + WTERMSIG(status);
			else result = 1;
			if (terminal_failed && !result) result = 1;
		} else if (run_signal) result = 128 + run_signal;
		else { fprintf(stderr, "Control run: execute %s: %s\n", command[0], strerror(spawn_error)); result = spawn_error == ENOENT ? 127 : 126; }
		if (terminal >= 0) close(terminal);
	}
	/* A lost PAUSE acknowledgement may still have created the lease. It is
	 * safe to release our unique ID even when that transaction reported error. */
	snprintf(request, sizeof(request), "RESUME %s\n", id); response[0] = 0;
	bool resumed = control_request(socket_path, request, response, sizeof(response));
	if (!resumed) { fprintf(stderr, "Control run: resume failed%s%s; lease expires after TTL\n", *response ? ": " : "", response); if (!result) result = 1; }
	if (run_signal && !result) result = 128 + run_signal;
	(void)sigprocmask(SIG_BLOCK, &blocked, NULL); run_process_group = 0;
	for (size_t i = 0; i < 3; i++) (void)sigaction(signals[i], &previous_actions[i], NULL);
	(void)sigaction(SIGCHLD, &previous_child, NULL);
	(void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
	return result;
}
#else
static int control_run(const char *socket_path, const char *scope, unsigned long ttl, bool discard, char **command) {
	(void)socket_path; (void)scope; (void)ttl; (void)discard; (void)command;
	fprintf(stderr, "Control run is unavailable on watchOS\n"); return 1;
}
#endif

int fm_control_cli(int argc, char **argv, const char *default_socket) {
	struct sigaction pipe_handler = { .sa_handler = SIG_IGN };
	sigemptyset(&pipe_handler.sa_mask); (void)sigaction(SIGPIPE, &pipe_handler, NULL);
	const char *command = argc ? argv[0] : "", *id = NULL, *scope = "all", *socket_path = default_socket;
	unsigned long ttl = 600; bool discard = false, json = false, scope_set = false, ttl_set = false;
	int child_index = -1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--") && !strcmp(command, "run")) { child_index = i + 1; break; }
		if (!strcmp(argv[i], "--json")) json = true;
		else if (!strcmp(argv[i], "--discard-pending")) discard = true;
		else if ((!strcmp(argv[i], "--id") || !strcmp(argv[i], "--scope") || !strcmp(argv[i], "--ttl") || !strcmp(argv[i], "--socket")) && i + 1 < argc) {
			const char *option = argv[i++];
			if (!strcmp(option, "--id")) id = argv[i];
			else if (!strcmp(option, "--scope")) { scope = argv[i]; scope_set = true; }
			else if (!strcmp(option, "--socket")) socket_path = argv[i];
			else { char *end; errno = 0; ttl = strtoul(argv[i], &end, 10); if (errno || *end || ttl < 1 || ttl > 86400) goto usage; ttl_set = true; }
		} else goto usage;
	}
	char request[512];
	if (!strcmp(command, "run") && !id && !json && child_index > 0 && child_index < argc && *argv[child_index] &&
		(!strcmp(scope, "all") || !strcmp(scope, "report"))) return control_run(socket_path, scope, ttl, discard, argv + child_index);
	if (!strcmp(command, "status") && !id && !discard && !scope_set && !ttl_set) snprintf(request, sizeof(request), "STATUS\n");
	else if (!strcmp(command, "pause") && !json && valid_id(id) && (!strcmp(scope, "all") || !strcmp(scope, "report")))
		snprintf(request, sizeof(request), "PAUSE %s %s %lu %d\n", id, scope, ttl, discard ? 1 : 0);
	else if (!strcmp(command, "resume") && valid_id(id) && !discard && !json && !scope_set && !ttl_set) snprintf(request, sizeof(request), "RESUME %s\n", id);
	else goto usage;
	char response[CONTROL_RESPONSE_SIZE] = {0}; bool ok = control_request(socket_path, request, response, sizeof(response));
	if (*response) puts(response);
	return ok ? 0 : 1;
usage:
	fprintf(stderr, "Usage: fsmod [-C config] ctl status [--json] [--socket PATH]\n"
		"       fsmod [-C config] ctl pause --id OWNER [--scope all|report] [--ttl 1..86400] [--discard-pending] [--socket PATH]\n"
		"       fsmod [-C config] ctl resume --id OWNER [--socket PATH]\n"
		"       fsmod [-C config] ctl run [--scope all|report] [--ttl 1..86400] [--discard-pending] [--socket PATH] -- COMMAND ARGS...\n");
	return 2;
}
