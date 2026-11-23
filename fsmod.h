#ifndef INCLUDE_FM_FSMOD_H
#define INCLUDE_FM_FSMOD_H

#ifndef FSMOD_VERSION
#error FSMOD_VERSION is not defined
#endif

#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include "backend/devfsev.h"
#include "util.h"

#define eprintf(x,y...) fprintf(stderr,x,##y)

struct filemonitor_backend_t;
struct filemonitor_event_t;
struct filemonitor_t;

struct filemonitor_event_t {
	int pid;
	int ppid;
	const char *proc;
	const char *file;
	const char *newfile; // destination for rename, exchange, or clone
	const char *event; // named event
	uint32_t uid;
	bool uid_available; /* Distinguish a real UID 0 from absent backend metadata. */
	uint32_t gid;
	bool gid_available; /* Independent of UID availability, including real GID 0. */
	int type;
	int mode;
	uint64_t inode;
	uint64_t tstamp;
	int dev_major;
	int dev_minor;
};

typedef bool (*FileMonitorCallback)(struct filemonitor_t *fm, struct filemonitor_event_t *ev);

struct filemonitor_backend_t {
	const char *name;
	bool (*begin)(struct filemonitor_t *fm);
	bool (*loop)(struct filemonitor_t *fm, FileMonitorCallback cb);
	bool (*end)(struct filemonitor_t *fm);
};

struct filemonitor_t {
	const char *root;
	const char **roots;
	size_t root_count;
	const char *proc;
	const char *link;
	int pid;
	int child;
	int alarm;
	int fd;
	bool json;
	bool jsonStream;
	volatile sig_atomic_t running;
	bool fileonly;
	bool show_timestamps;
	bool changes_only; /* Daemon: do not report reads performed by HTTP delivery. */
	uint64_t count;
	void (*control_c)();
	struct filemonitor_backend_t backend;
};

typedef struct filemonitor_backend_t FileMonitorBackend;
typedef struct filemonitor_event_t FileMonitorEvent;
typedef struct filemonitor_t FileMonitor;

#if __APPLE__
extern FileMonitorBackend fmb_devfsev;
extern FileMonitorBackend fmb_fsevapi;
extern FileMonitorBackend fmb_kqueue;
extern FileMonitorBackend fmb_kdebug;
#else
extern FileMonitorBackend fmb_inotify;
extern FileMonitorBackend fmb_fanotify;
#endif

#endif
