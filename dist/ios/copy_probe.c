/* Explicit clonefile versus ordinary read/write diagnostics; no fallback. */
#define _DARWIN_C_SOURCE 1
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(int fd, const void *bytes, size_t length) {
	const char *cursor = bytes;
	while (length) {
		ssize_t count = write(fd, cursor, length);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return -1;
		cursor += count;
		length -= (size_t)count;
	}
	return 0;
}

static int byte_copy(const char *source, const char *destination) {
	char buffer[4096];
	int input = open(source, O_RDONLY);
	if (input < 0) return -1;
	int output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (output < 0) {
		int saved = errno;
		close(input);
		errno = saved;
		return -1;
	}
	int result = 0;
	for (;;) {
		ssize_t count = read(input, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR) continue;
		if (count < 0) { result = -1; break; }
		if (!count) break;
		if (write_all(output, buffer, (size_t)count)) { result = -1; break; }
	}
	if (!result && fsync(output)) result = -1;
	int saved = errno;
	if (close(output) && !result) { result = -1; saved = errno; }
	close(input);
	errno = saved;
	return result;
}

int main(int argc, char **argv) {
	char directory[PATH_MAX], source[PATH_MAX], cloned[PATH_MAX], copied[PATH_MAX];
	const char payload[] = "fsmod explicit copy diagnostic\n";
	if (argc != 2 || argv[1][0] != '/') {
		fprintf(stderr, "Usage: %s /absolute/watched/directory\n", argv[0]);
		return 2;
	}
	char *root = realpath(argv[1], NULL);
	if (!root) { perror("realpath"); return 1; }
	int length = snprintf(directory, sizeof(directory), "%s/fsmod-probe-XXXXXX", root);
	free(root);
	if (length < 0 || (size_t)length >= sizeof(directory) - 32) {
		fprintf(stderr, "Watched directory path is too long\n");
		return 2;
	}
	if (!mkdtemp(directory)) { perror("mkdtemp"); return 1; }
	snprintf(source, sizeof(source), "%s/source.txt", directory);
	snprintf(cloned, sizeof(cloned), "%s/cloned.txt", directory);
	snprintf(copied, sizeof(copied), "%s/byte-copy.txt", directory);
	printf("pid=%ld directory=%s\n", (long)getpid(), directory);
	int fd = open(source, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) { perror("create source"); return 1; }
	if (write_all(fd, payload, sizeof(payload) - 1) || fsync(fd)) {
		perror("write source"); close(fd); return 1;
	}
	if (close(fd)) { perror("close source"); return 1; }
	fflush(stdout);
	sleep(1);
	int clone_ok = clonefile(source, cloned, 0) == 0;
	int clone_error = clone_ok ? 0 : errno;
	printf("clonefile: source=%s destination=%s result=%s errno=%d (%s)\n",
		source, cloned, clone_ok ? "OK" : "FAILED", clone_error,
		clone_ok ? "success" : strerror(clone_error));
	fflush(stdout);
	sleep(1);
	int copy_ok = byte_copy(source, copied) == 0;
	int copy_error = copy_ok ? 0 : errno;
	printf("read/write: source=%s destination=%s result=%s errno=%d (%s)\n",
		source, copied, copy_ok ? "OK" : "FAILED", copy_error,
		copy_ok ? "success" : strerror(copy_error));
	puts("Files are retained in the printed directory for inspection and cleanup.");
	/* Keep process metadata available while the daemon consumes its events. */
	fflush(stdout);
	sleep(2);
	return clone_ok && copy_ok ? 0 : 1;
}
