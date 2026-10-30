/* fsmod -- MIT - Copyright NowSecure 2015-2025 - pancake@nowsecure.com */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#if __APPLE__
#include <sys/sysctl.h>
#endif
#if __linux__
#include <linux/limits.h>
#endif
#include <errno.h>
#include "fsmod.h"

void hexdump(const uint8_t *buf, unsigned int len, int w) {
	size_t i, j;
	if (w < 1) {
		w = 16;
	}
	for (i = 0; i < len; i += w) {
		printf ("0x%08zx: ", i);
		for (j = i; j < i + w; j++) {
			if (j < len) {
				printf (j%2 ? "%02x ":"%02x", buf[j]);
			} else {
				printf (j%2 ? "   " : "  ");
			}
		}
		printf (" ");
		for (j = i; j < i + w; j++) {
			printf ("%c", isprint (buf[j])? buf[j]: '.');
		}
		printf ("\n");
	}
}

const char *fm_typestr(int type) {
#define __(x) [x]=#x
	const char *types[FSE_MAX_EVENTS] = {
		__ (FSE_CREATE_FILE),
		__ (FSE_DELETE),
		__ (FSE_STAT_CHANGED),
		__ (FSE_RENAME),
		__ (FSE_CONTENT_MODIFIED),
		__ (FSE_CREATE_DIR),
		__ (FSE_CHOWN),
		__ (FSE_EXCHANGE),
		__ (FSE_FINDER_INFO_CHANGED),
		__ (FSE_XATTR_MODIFIED),
		__ (FSE_XATTR_REMOVED),
		__ (FSE_CLONE),
	};
	switch (type) {
	case FSE_ARG_DONE: return "FSE_ARG_DONE";
	case FSE_OPEN: return "FSE_OPEN";
	case FSE_CLOSE: return "FSE_CLOSE";
	case FSE_CLOSE_WRITABLE: return "FSE_CLOSE_WRITABLE";
	case FSE_UNKNOWN: return "FSE_UNKNOWN";
	}
	return (type >= 0 && type < FSE_MAX_EVENTS && types[type])? types[type]: "";
}

const char *fm_argstr(int type) {
#define __(x) [x]=#x
	const char *args[13] = {
		__ (FSE_ARG_NONE),
		__ (FSE_ARG_VNODE),
		__ (FSE_ARG_STRING),
		__ (FSE_ARG_PATH),
		__ (FSE_ARG_INT32),
		__ (FSE_ARG_INT64),
		__ (FSE_ARG_RAW),
		__ (FSE_ARG_INO),
		__ (FSE_ARG_UID),
		__ (FSE_ARG_DEV),
		__ (FSE_ARG_MODE),
		__ (FSE_ARG_GID),
		__ (FSE_ARG_FINFO),
	};
	switch (type) {
	case FSE_ARG_DONE: return "FSE_ARG_DONE";
	case 0: return "FSE_UNKNOWN";
	}
	return (type >= 0 && (size_t)type < sizeof(args) / sizeof(args[0]) && args[type])? args[type]: "";
}

const char *fm_colorstr(int type) {
	const char *colors[FSE_MAX_EVENTS] = {
		[FSE_CREATE_FILE] = Color_MAGENTA,
		[FSE_DELETE] = Color_RED,
		[FSE_STAT_CHANGED] = Color_YELLOW,
		[FSE_RENAME] = Color_GREEN,
		[FSE_CONTENT_MODIFIED] = Color_YELLOW,
		[FSE_CREATE_DIR] = Color_BLUE,
		[FSE_CHOWN] = Color_YELLOW,
		[FSE_EXCHANGE] = Color_GREEN,
		[FSE_FINDER_INFO_CHANGED] = Color_YELLOW,
		[FSE_XATTR_MODIFIED] = Color_YELLOW,
		[FSE_XATTR_REMOVED] = Color_RED,
		[FSE_CLONE] = Color_GREEN,
	};
	switch (type) {
	case FSE_ARG_DONE: return Color_GREEN;
	case FSE_OPEN: return Color_GREEN;
	case FSE_UNKNOWN: return Color_RED;
	}
	return (type >= 0 && type < FSE_MAX_EVENTS && colors[type])? colors[type]: "";
}

const char *get_proc_name(int pid, int *ppid) {
	static char path[PATH_MAX] = {0};
#if __APPLE__
	struct kinfo_proc * kinfo = (struct kinfo_proc*)&path;
	size_t len = 1000;
	int rc, mib[4];

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = pid;

	memset (path, 0, sizeof (path));
	if ((rc = sysctl (mib, 4, path, &len, NULL, 0)) != 0) {
		/* The process may have exited before its metadata was read. */
		if (ppid) *ppid = 0;
		return NULL;
	}

	if (ppid) *ppid = kinfo->kp_eproc.e_ppid;
	return kinfo->kp_proc.p_comm;
#elif __linux__
	char *p, *q;
	int fd;
	snprintf (path, sizeof (path), "/proc/%d/stat", pid);
	fd = open (path, O_RDONLY);
	if (fd == -1) {
		// eprintf ("Cannot open '%s'\n", path);
		return NULL;
	}
	if (fcntl (fd, F_SETFD, FD_CLOEXEC) == -1) {
		close (fd);
		return NULL;
	}
	path[0] = 0;
	(void) read (fd, path, sizeof (path));
	path[sizeof (path) - 1] = 0;
	close (fd);
	p = strchr (path, '(');
	q = strchr (path, ')');

	if (p && q && p < q && q[1] && q[2]) {
		*q = 0;
		if (ppid) {
			char *r = strchr (q + 2, ' ');
			if (r) *ppid = atoi (r + 1);
		}
		return p + 1;
	}
	return NULL;
#else
#warning getProcName not supported for this platform
	return NULL;
#endif
}

bool is_directory(const char *str) {
        struct stat buf = {0};
        if (!str || !*str) {
		return false;
	}
        if (stat (str, &buf) == -1) {
		return false;
	}
        if ((S_IFBLK & buf.st_mode) == S_IFBLK) {
		return false;
	}
        return S_IFDIR == (S_IFDIR & buf.st_mode);
}

bool copy_file(const char *src, const char *dst) {
	char buf[4096];
	struct stat stat_src;
	int count, mode = 0640;
	int fd_src, fd_dst;
	fd_src = open (src, O_RDONLY);
	if (fd_src == -1) {
		perror ("open");
		return false;
	}
	if (fcntl (fd_src, F_SETFD, FD_CLOEXEC) == -1) {
		close (fd_src);
		return false;
	}
	if (!fstat (fd_src, &stat_src)) {
		mode = stat_src.st_mode;
	}
	fd_dst = open (dst, O_RDWR | O_CREAT | O_TRUNC, mode);
	if (fd_dst == -1) {
		(void) close (fd_src);
		return false;
	}
	if (fcntl (fd_dst, F_SETFD, FD_CLOEXEC) == -1) {
		close (fd_src);
		close (fd_dst);
		return false;
	}
	for (;;) {
		count = read (fd_src, buf, sizeof (buf));
		if (count < 1) {
			break;
		}
		(void) write (fd_dst, buf, count);
	}
	(void) close (fd_src);
	(void) close (fd_dst);
	return true;
}

char *fmu_jsonfilter(const char *s) {
	static const char hex[] = "0123456789abcdef";
	if (!s) s = "";
	size_t len = strlen (s);
	if (len > (SIZE_MAX - 1) / 6) return NULL;
	char *result = malloc (len * 6 + 1);
	if (!result) return NULL;
	char *out = result;
	const unsigned char *p = (const unsigned char *)s;
	for (; *p; p++) {
		unsigned char c = *p;
		switch (c) {
		case '"': *out++ = '\\'; *out++ = '"'; break;
		case '\\': *out++ = '\\'; *out++ = '\\'; break;
		case '\b': *out++ = '\\'; *out++ = 'b'; break;
		case '\f': *out++ = '\\'; *out++ = 'f'; break;
		case '\n': *out++ = '\\'; *out++ = 'n'; break;
		case '\r': *out++ = '\\'; *out++ = 'r'; break;
		case '\t': *out++ = '\\'; *out++ = 't'; break;
		default:
			if (c < 0x20) {
				*out++ = '\\'; *out++ = 'u';
				*out++ = '0'; *out++ = '0';
				*out++ = hex[c >> 4]; *out++ = hex[c & 15];
			} else {
				*out++ = c;
			}
		}
	}
	*out = 0;
	return result;
}
