/* fsmod -- MIT - Copyright NowSecure 2015-2016 - pancake@nowsecure.com  */

#if __APPLE__
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <poll.h>
#include "fsmod.h"

#define FM_DEV "/dev/fsevents"
#define FM_BUFSIZE 65536

/* XNU copy_out_kfse()/fill_buff(): int32 type, int32 pid, then
 * uint16 type + uint16 payload length + payload. DONE is only uint16.
 * Return bytes consumed, zero for an incomplete record, or -1 for bad data.
 * Pointers belong to the input buffer and are used only after DONE arrives. */
static int parse_record(const uint8_t *buffer, size_t available, FileMonitorEvent *ev) {
	int32_t type, pid;
	size_t offset = 8;
	memset(ev, 0, sizeof(*ev));
	ev->type = FSE_INVALID;
	if (available < offset) return 0;
	memcpy(&type, buffer, sizeof(type));
	memcpy(&pid, buffer + 4, sizeof(pid));
	ev->type = type & FSE_TYPE_MASK;
	ev->pid = pid;
	for (;;) {
		uint16_t arg_type, length;
		if (available - offset < sizeof(arg_type)) return 0;
		memcpy(&arg_type, buffer + offset, sizeof(arg_type));
		if (arg_type == FSE_ARG_DONE) return (int)(offset + sizeof(arg_type));
		if (available - offset < 4) return 0;
		memcpy(&length, buffer + offset + 2, sizeof(length));
		if (length > available - offset - 4) return 0;
		const uint8_t *value = buffer + offset + 4;
		bool source = ev->newfile == NULL;
		uint32_t scalar;
		switch (arg_type) {
		case FSE_ARG_STRING:
		case FSE_ARG_PATH:
			if (!length || value[length - 1] != 0 ||
				memchr(value, 0, length - 1)) return -1;
			if (!ev->file) ev->file = (const char *)value;
			else if (!ev->newfile && (ev->type == FSE_RENAME || ev->type == FSE_EXCHANGE ||
				ev->type == FSE_CLONE))
				ev->newfile = (const char *)value;
			break;
		case FSE_ARG_INT64:
			if (length != sizeof(ev->tstamp)) return -1;
			/* XNU appends mach_absolute_time() as its last INT64 argument. */
			memcpy(&ev->tstamp, value, sizeof(ev->tstamp));
			break;
		case FSE_ARG_INO:
			if (length == sizeof(uint64_t)) {
				if (source) memcpy(&ev->inode, value, sizeof(ev->inode));
			} else if (length == sizeof(uint32_t)) {
				memcpy(&scalar, value, sizeof(scalar));
				if (source) ev->inode = scalar;
			} else return -1;
			break;
		case FSE_ARG_DEV:
		case FSE_ARG_MODE:
		case FSE_ARG_UID:
		case FSE_ARG_GID:
			if (length != sizeof(scalar)) return -1;
			memcpy(&scalar, value, sizeof(scalar));
			if (!source) break;
			if (arg_type == FSE_ARG_DEV) {
				ev->dev_major = major(scalar);
				ev->dev_minor = minor(scalar);
			} else if (arg_type == FSE_ARG_MODE) ev->mode = scalar;
			else if (arg_type == FSE_ARG_UID) {
				ev->uid = scalar;
				ev->uid_available = true;
			}
			else {
				ev->gid = scalar;
				ev->gid_available = true;
			}
			break;
		case FSE_ARG_FINFO:
			/* XNU compact layout: ino64, dev32, mode32, uid32, gid32. */
			if (length != 24) return -1;
			if (!source) break;
			memcpy(&ev->inode, value, sizeof(ev->inode));
			memcpy(&scalar, value + 8, sizeof(scalar));
			ev->dev_major = major(scalar);
			ev->dev_minor = minor(scalar);
			memcpy(&ev->mode, value + 12, sizeof(ev->mode));
			memcpy(&ev->uid, value + 16, sizeof(ev->uid));
			ev->uid_available = true;
			memcpy(&ev->gid, value + 20, sizeof(ev->gid));
			ev->gid_available = true;
			break;
		default:
			/* Unknown extensions still have a length; preserve framing. */
			break;
		}
		offset += 4 + length;
	}
}

static void event_subscription(int8_t events[FSE_MAX_EVENTS]) {
	memset(events, FSE_IGNORE, FSE_MAX_EVENTS);
	memset(events, FSE_REPORT, FSE_LEGACY_MAX_EVENTS);
	events[FSE_CLONE] = FSE_REPORT;
	/* DOCID records have a separate layout. UNMOUNT_PENDING requires an ACK;
	 * leave both ignored when extending the subscription to reach CLONE. */
}

static int fdsetup(int fd) {
	fsevent_clone_args clone_args = {0};
	int8_t events[FSE_MAX_EVENTS];
	int rc, cloned_fd = -1;

	event_subscription(events);

	clone_args.fd = &cloned_fd; // This is the descriptor we get back
	/* This is the kernel event queue, separate from the HTTP report queue.
	 * Leave enough headroom for bursts while userspace processes a read. */
	clone_args.event_queue_depth = 4096;
	clone_args.event_list = events;
	clone_args.num_events = FSE_MAX_EVENTS;

	rc = ioctl (fd, FSEVENTS_CLONE, &clone_args);
	close (fd);
	if (rc < 0) {
		perror ("ioctl");
		return -1;
	}
	/* get extended info from events */
	if ((rc = ioctl (cloned_fd, FSEVENTS_WANT_EXTENDED_INFO, NULL)) < 0) {
		perror ("ioctl");
		close (cloned_fd);
		return -1;
	}
	return cloned_fd;
}

static bool fm_begin (FileMonitor *fm) {
	int fd;
	fm->fd = -1;
	fd = open (FM_DEV, O_RDONLY);
	if (fd == -1) {
		perror ("open "FM_DEV);
		return false;
	}
	if (fcntl (fd, F_SETFD, FD_CLOEXEC) == -1) {
		perror ("fsevents FD_CLOEXEC");
		close (fd);
		return false;
	}
	fd = fdsetup (fd);
	if (fd == -1) {
		perror ("fdclone");
		return false;
	}
	if (fcntl (fd, F_SETFD, FD_CLOEXEC) == -1) {
		perror ("fsevents clone FD_CLOEXEC");
		close (fd);
		return false;
	}
	fm->fd = fd;
	return true;
}

static bool fm_loop (FileMonitor *fm, FileMonitorCallback cb) {
	uint8_t buf[FM_BUFSIZE];
	size_t used = 0;
	for (; fm->running; ) {
		struct pollfd pfd = { .fd = fm->fd, .events = POLLIN };
		int ready = poll (&pfd, 1, 250);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror ("fsevents poll");
			return false;
		}
		if (!fm->running) break;
		if (!ready) continue;
		if (!(pfd.revents & POLLIN)) {
			eprintf ("fsevents descriptor closed unexpectedly\n");
			return false;
		}
		/* XNU requires at least 2048 bytes of space on every device read. */
		if (sizeof(buf) - used < 2048) {
			eprintf("Oversized or incomplete fsevents record\n");
			return false;
		}
		ssize_t rc = read (fm->fd, buf + used, sizeof(buf) - used);
		if (rc < 1) {
			if (rc < 0 && errno == EINTR) {
				if (!fm->running) {
					return true;
				}
				continue;
			}
			perror ("read");
			return false;
		}
		used += (size_t)rc;
		size_t offset = 0;
		while (fm->running && offset < used) {
			FileMonitorEvent ev;
			int consumed = parse_record(buf + offset, used - offset, &ev);
			if (!consumed) break;
			if (consumed < 0) {
				eprintf("Malformed fsevents record\n");
				return false;
			}
			if (ev.type == FSE_EVENTS_DROPPED) {
				eprintf("fsmod: kernel fsevents queue dropped events\n");
			} else if (ev.type < FSE_MAX_EVENTS && ev.pid && ev.file && ev.file[0] == '/' && cb) {
				ev.proc = get_proc_name(ev.pid, &ev.ppid);
				cb(fm, &ev);
			}
			offset += (size_t)consumed;
		}
		used -= offset;
		if (used) memmove(buf, buf + offset, used);
		if (used == sizeof(buf)) {
			eprintf("fsevents record exceeds receive buffer\n");
			return false;
		}
	}
	return true;
}

static bool fm_end(FileMonitor *fm) {
	if (fm && fm->fd != -1) {
		close (fm->fd);
		fm->fd = -1;
		return true;
	}
	return false;
}

FileMonitorBackend fmb_devfsev = {
	.name = "devfsev",
	.begin = fm_begin,
	.loop = fm_loop,
	.end = fm_end,
};

#endif
