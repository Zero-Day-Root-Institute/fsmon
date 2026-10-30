/* fsmod -- MIT -- daemon process and singleton PID-file lifecycle. */
/* flock is a BSD extension when Apple's headers are in strict POSIX mode. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if TARGET_WATCHOS
#include <stdio.h>
#include "service.h"

static bool desktop_only(char *error, size_t size) {
	if (error && size) snprintf(error, size, "daemon reporting requires desktop Linux or macOS");
	return false;
}
bool fm_service_detach(const char *path, char *error, size_t size) {
	(void)path; return desktop_only(error, size);
}
bool fm_service_lock(const char *path, char *error, size_t size) {
	(void)path; return desktop_only(error, size);
}
bool fm_service_stop(const char *path, char *error, size_t size) {
	(void)path; return desktop_only(error, size);
}
void fm_service_ready(bool success) { (void)success; }
void fm_service_release(void) {}
#else
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "service.h"

static int ready_fd = -1;
static int pid_fd = -1;
static char *locked_path;
static pid_t lock_owner;

static bool fail(char *error, size_t size, const char *message) {
	int saved = errno;
	if (error && size) snprintf(error, size, "%s: %s", message, strerror(saved));
	errno = saved;
	return false;
}

static bool set_cloexec(int fd) {
	int flags = fcntl(fd, F_GETFD);
	return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) >= 0;
}

#if !TARGET_IOS
static bool move_pipe_fd(int *fd) {
	if (*fd <= STDERR_FILENO) {
		int moved = fcntl(*fd, F_DUPFD, STDERR_FILENO + 1);
		if (moved < 0) return false;
		close(*fd);
		*fd = moved;
	}
	return set_cloexec(*fd);
}

static void close_inherited_fds(void) {
	struct rlimit limit;
	long maximum = sysconf(_SC_OPEN_MAX);
	if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
		maximum = limit.rlim_cur > INT_MAX ? INT_MAX : (long)limit.rlim_cur;
	}
	if (maximum < 0) maximum = 1024;
	for (int fd = STDERR_FILENO + 1; fd < maximum; fd++) {
		if (fd != ready_fd) close(fd);
	}
}
#endif

static bool write_all(int fd, const char *data, size_t size) {
	while (size) {
		ssize_t count = write(fd, data, size);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) {
			if (count == 0) errno = EIO;
			return false;
		}
		data += count;
		size -= (size_t)count;
	}
	return true;
}

void fm_service_ready(bool success) {
	if (ready_fd >= 0) {
		char status = success ? '1' : '0';
		ssize_t n;
		do { n = write(ready_fd, &status, 1); } while (n < 0 && errno == EINTR);
		close(ready_fd);
		ready_fd = -1;
	}
}

bool fm_service_detach(const char *log_file, char *error, size_t size) {
#if TARGET_IOS
	(void)log_file;
	if (error && size) snprintf(error, size, "on iOS use -F with launchd; background fork is unsupported");
	return false;
#else
	int pipefd[2];
	if (!log_file || !*log_file || ready_fd >= 0 || pid_fd >= 0) {
		errno = EINVAL;
		return fail(error, size, "invalid daemon startup state");
	}
	if (pipe(pipefd) < 0) return fail(error, size, "startup pipe");
	/* A caller may launch us with stdin/stdout/stderr closed. Keep the
	 * handshake pipe clear of descriptors which dup2() will overwrite. */
	if (!move_pipe_fd(&pipefd[0]) || !move_pipe_fd(&pipefd[1])) {
		int saved = errno;
		close(pipefd[0]); close(pipefd[1]); errno = saved;
		return fail(error, size, "prepare startup pipe");
	}
	(void)fflush(NULL);
	pid_t child = fork();
	if (child < 0) {
		int saved = errno;
		close(pipefd[0]); close(pipefd[1]);
		errno = saved;
		return fail(error, size, "fork");
	}
	if (child > 0) {
		char status = '0';
		ssize_t n;
		close(pipefd[1]);
		do { n = read(pipefd[0], &status, 1); } while (n < 0 && errno == EINTR);
		close(pipefd[0]);
		while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
		if (n != 1 || status != '1') {
			fprintf(stderr, "Daemon startup failed; inspect %s\n", log_file);
			exit(EXIT_FAILURE);
		}
		exit(EXIT_SUCCESS);
	}
	close(pipefd[0]);
	ready_fd = pipefd[1];
	if (setsid() < 0) return fail(error, size, "setsid");
	child = fork();
	if (child < 0) return fail(error, size, "second fork");
	if (child > 0) _exit(EXIT_SUCCESS);
	close_inherited_fds();
	umask(0027);
	if (chdir("/") < 0) return fail(error, size, "chdir");
	int input = open("/dev/null", O_RDONLY);
	int log = open(log_file, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK, 0640);
	struct stat st;
	if (input < 0 || log < 0 || fstat(log, &st) < 0 || !S_ISREG(st.st_mode) ||
		st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & (S_IWGRP | S_IWOTH))) {
		int saved = errno;
		if (input >= 0 && log >= 0) saved = EACCES;
		if (input >= 0) close(input);
		if (log >= 0) close(log);
		errno = saved;
		return fail(error, size, "open daemon log");
	}
	if (dup2(input, STDIN_FILENO) < 0 || dup2(log, STDOUT_FILENO) < 0 ||
		dup2(log, STDERR_FILENO) < 0) {
		int saved = errno;
		close(input); close(log);
		errno = saved;
		return fail(error, size, "redirect daemon output");
	}
	if (input > STDERR_FILENO) close(input);
	if (log > STDERR_FILENO) close(log);
	setvbuf(stdout, NULL, _IOLBF, 0);
	return true;
#endif
}

bool fm_service_lock(const char *pid_file, char *error, size_t size) {
	int fd, saved;
	char *path;
	struct stat st, current;
	if (!pid_file || !*pid_file || pid_fd >= 0) {
		errno = EINVAL;
		return fail(error, size, "invalid PID lock state");
	}
	fd = open(pid_file, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0640);
	if (fd < 0) return fail(error, size, "open PID file");
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		st.st_nlink != 1 || (st.st_mode & (S_IWGRP | S_IWOTH))) {
		close(fd); errno = EACCES;
		return fail(error, size, "PID file must be a private regular, singly linked file owned by this user");
	}
	if (!set_cloexec(fd)) {
		saved = errno; close(fd); errno = saved;
		return fail(error, size, "protect PID descriptor");
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		saved = errno; close(fd); errno = saved;
		return fail(error, size, "another instance holds the PID file");
	}
	if (lstat(pid_file, &current) < 0 || current.st_dev != st.st_dev || current.st_ino != st.st_ino) {
		close(fd); errno = EAGAIN;
		return fail(error, size, "PID file changed while acquiring lock");
	}
	path = strdup(pid_file);
	if (!path) {
		saved = errno; close(fd); errno = saved;
		return fail(error, size, "allocate PID path");
	}
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
	if (ftruncate(fd, 0) < 0 || !write_all(fd, buf, (size_t)len)) {
		saved = errno; free(path); close(fd); errno = saved;
		return fail(error, size, "write PID file");
	}
	pid_fd = fd;
	locked_path = path;
	lock_owner = getpid();
	return true;
}

bool fm_service_stop(const char *pid_file, char *error, size_t size) {
	if (!pid_file || !*pid_file) { errno = EINVAL; return fail(error, size, "invalid PID path"); }
	int fd = open(pid_file, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) return fail(error, size, "open PID file");
	struct stat st, current;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
		(st.st_mode & (S_IWGRP | S_IWOTH)) || (st.st_uid != geteuid() && geteuid() != 0)) {
		close(fd); errno = EACCES;
		return fail(error, size, "unsafe PID file");
	}
	if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
		close(fd);
		if (error && size) snprintf(error, size, "no running instance holds %s", pid_file);
		return false;
	}
	if (errno != EWOULDBLOCK && errno != EAGAIN) {
		int saved = errno; close(fd); errno = saved; return fail(error, size, "inspect PID lock");
	}
	char buf[32] = {0}, *end;
	ssize_t n;
	do { n = read(fd, buf, sizeof(buf) - 1); } while (n < 0 && errno == EINTR);
	if (n < 1 || st.st_size != n || st.st_size >= (off_t)sizeof(buf)) {
		close(fd); errno = EINVAL; return fail(error, size, "read PID");
	}
	for (ssize_t i = 0; i < n; i++) {
		if ((buf[i] < '0' || buf[i] > '9') && !(buf[i] == '\n' && i == n - 1)) {
			close(fd); errno = EINVAL; return fail(error, size, "invalid PID file");
		}
	}
	errno = 0;
	long pid = strtol(buf, &end, 10);
	if (*end == '\n') end++;
	if (errno || *end || pid <= 1 || pid > INT_MAX || pid == (long)getpid()) {
		close(fd); errno = EINVAL; return fail(error, size, "invalid PID file");
	}
	/* Reject an already-released lock before signaling a potentially stale PID. */
	if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
		close(fd); errno = ESRCH; return fail(error, size, "instance already stopped");
	}
	if (errno != EWOULDBLOCK && errno != EAGAIN) {
		int saved = errno; close(fd); errno = saved; return fail(error, size, "recheck PID lock");
	}
	if (lstat(pid_file, &current) < 0 || current.st_dev != st.st_dev || current.st_ino != st.st_ino) {
		close(fd); errno = EAGAIN; return fail(error, size, "PID file changed during stop");
	}
	int result = kill((pid_t)pid, SIGTERM);
	int saved = errno;
	close(fd); errno = saved;
	if (result < 0) return fail(error, size, "send SIGTERM");
	return true;
}

void fm_service_release(void) {
	if (pid_fd >= 0) {
		/* Do not unlink a replacement created by another process. */
		struct stat held, path;
		if (lock_owner == getpid() && locked_path && fstat(pid_fd, &held) == 0 && lstat(locked_path, &path) == 0 &&
			held.st_dev == path.st_dev && held.st_ino == path.st_ino) unlink(locked_path);
		close(pid_fd); pid_fd = -1;
	}
	free(locked_path); locked_path = NULL;
	lock_owner = 0;
}
#endif
