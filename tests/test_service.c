#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "service.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void on_term(int signal_number) {
	(void)signal_number;
	stopping = 1;
}

static void wait_child(pid_t child, int expected) {
	int status;
	pid_t result;
	do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
	assert(result == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == expected);
}

static void write_file(const char *path, const char *contents) {
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	assert(fd >= 0);
	assert(write(fd, contents, strlen(contents)) == (ssize_t)strlen(contents));
	assert(close(fd) == 0);
}

static void wait_for_file(const char *path, bool present) {
	const struct timespec delay = { 0, 10000000L };
	for (int i = 0; i < 500; i++) {
		if ((access(path, F_OK) == 0) == present) return;
		(void)nanosleep(&delay, NULL);
	}
	assert(0 && "timed out waiting for temporary service");
}

static pid_t start_locked_child(const char *pid_file) {
	int pipefd[2];
	assert(pipe(pipefd) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		char error[256];
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_handler = on_term;
		sigemptyset(&action.sa_mask);
		close(pipefd[0]);
		assert(sigaction(SIGTERM, &action, NULL) == 0);
		assert(fm_service_lock(pid_file, error, sizeof(error)));
		assert(write(pipefd[1], "1", 1) == 1);
		close(pipefd[1]);
		while (!stopping) {
			const struct timespec delay = { 0, 10000000L };
			(void)nanosleep(&delay, NULL);
		}
		fm_service_release();
		_exit(0);
	}
	char ready = 0;
	close(pipefd[1]);
	assert(read(pipefd[0], &ready, 1) == 1 && ready == '1');
	close(pipefd[0]);
	return child;
}

static void test_detach(const char *pid_file, const char *log_file, const char *done_file, bool fail_startup, bool closed_stdio) {
	pid_t launcher = fork();
	assert(launcher >= 0);
	if (launcher == 0) {
		char error[256];
		int inherited = open("/dev/null", O_RDONLY);
		int high = fcntl(inherited, F_DUPFD, 100);
		assert(inherited >= 0 && high >= 100);
		close(inherited);
		if (closed_stdio) { close(0); close(1); close(2); }
		if (!fm_service_detach(log_file, error, sizeof(error))) {
			fm_service_ready(false);
			_exit(1);
		}
		assert(fcntl(high, F_GETFD) == -1 && errno == EBADF);
		assert(fm_service_lock(pid_file, error, sizeof(error)));
		fm_service_ready(!fail_startup);
		fm_service_release();
		write_file(done_file, "finished\n");
		_exit(fail_startup ? 1 : 0);
	}
	wait_child(launcher, fail_startup ? EXIT_FAILURE : EXIT_SUCCESS);
	wait_for_file(done_file, true);
	assert(access(pid_file, F_OK) != 0);
	assert(unlink(done_file) == 0);
	assert(unlink(log_file) == 0);
}

int main(void) {
	char template[] = "/tmp/fsmod-service-test-XXXXXX";
	char *directory = mkdtemp(template);
	char pid_file[1024], log_file[1024], other[1024], moved[1024], done_file[1024];
	char error[256], contents[64];
	struct stat st;
	assert(directory);
	assert(snprintf(pid_file, sizeof(pid_file), "%s/service.pid", directory) > 0);
	assert(snprintf(log_file, sizeof(log_file), "%s/service.log", directory) > 0);
	assert(snprintf(other, sizeof(other), "%s/other.pid", directory) > 0);
	assert(snprintf(moved, sizeof(moved), "%s/moved.pid", directory) > 0);
	assert(snprintf(done_file, sizeof(done_file), "%s/done", directory) > 0);
	assert(!fm_service_stop(pid_file, error, sizeof(error)));
	write_file(pid_file, "123\n");
	assert(!fm_service_stop(pid_file, error, sizeof(error)));
	assert(unlink(pid_file) == 0);
	assert(fm_service_lock(pid_file, error, sizeof(error)));
	FILE *stream = fopen(pid_file, "r");
	long recorded = 0;
	assert(stream && fscanf(stream, "%ld", &recorded) == 1);
	assert(recorded == (long)getpid());
	assert(fclose(stream) == 0);
	assert(!fm_service_lock(other, error, sizeof(error)));
	/* A failed competing lock/release must not unlink the holder's PID file. */
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		fm_service_release();
		assert(access(pid_file, F_OK) == 0);
		assert(!fm_service_lock(pid_file, error, sizeof(error)));
		fm_service_release();
		assert(access(pid_file, F_OK) == 0);
		_exit(0);
	}
	wait_child(child, 0);
	assert(!fm_service_stop(pid_file, error, sizeof(error)));
	fm_service_release();
	assert(access(pid_file, F_OK) != 0);
	/* A stale pathname replacement must survive release of the old inode. */
	assert(fm_service_lock(pid_file, error, sizeof(error)));
	assert(rename(pid_file, moved) == 0);
	write_file(pid_file, "replacement\n");
	fm_service_release();
	assert(stat(pid_file, &st) == 0);
	assert(unlink(pid_file) == 0);
	assert(unlink(moved) == 0);
	write_file(other, "target\n");
	assert(symlink(other, pid_file) == 0);
	assert(!fm_service_lock(pid_file, error, sizeof(error)));
	assert(!fm_service_stop(pid_file, error, sizeof(error)));
	fm_service_release();
	assert(unlink(pid_file) == 0);
	assert(link(other, pid_file) == 0);
	assert(!fm_service_lock(pid_file, error, sizeof(error)));
	fm_service_release();
	assert(unlink(pid_file) == 0);
	assert(unlink(other) == 0);
	write_file(pid_file, "123\n");
	assert(chmod(pid_file, 0666) == 0);
	assert(!fm_service_lock(pid_file, error, sizeof(error)));
	assert(!fm_service_stop(pid_file, error, sizeof(error)));
	fm_service_release();
	assert(unlink(pid_file) == 0);
	assert(mkfifo(pid_file, 0600) == 0);
	assert(!fm_service_lock(pid_file, error, sizeof(error)));
	assert(!fm_service_stop(pid_file, error, sizeof(error)));
	fm_service_release();
	assert(unlink(pid_file) == 0);
	/* Invalid locked data must never be interpreted as a PID to signal. */
	int invalid_fd = open(pid_file, O_RDWR | O_CREAT, 0600);
	assert(invalid_fd >= 0 && flock(invalid_fd, LOCK_EX | LOCK_NB) == 0);
	assert(snprintf(contents, sizeof(contents), "%ld\nnot-a-pid", (long)getpid()) > 0);
	assert(write(invalid_fd, contents, strlen(contents)) == (ssize_t)strlen(contents));
	assert(!fm_service_stop(pid_file, error, sizeof(error)));
	assert(close(invalid_fd) == 0);
	assert(unlink(pid_file) == 0);
	/* The only process signaled by this suite is this temporary child. */
	child = start_locked_child(pid_file);
	assert(!fm_service_lock(pid_file, error, sizeof(error)));
	fm_service_release();
	assert(access(pid_file, F_OK) == 0);
	assert(fm_service_stop(pid_file, error, sizeof(error)));
	wait_child(child, 0);
	assert(access(pid_file, F_OK) != 0);
	test_detach(pid_file, log_file, done_file, false, false);
	test_detach(pid_file, log_file, done_file, false, true);
	test_detach(pid_file, log_file, done_file, true, false);
	assert(rmdir(directory) == 0);
	puts("service tests passed");
	return 0;
}
