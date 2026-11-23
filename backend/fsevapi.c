/* fsmod -- MIT - Copyright NowSecure 2016 - pancake@nowsecure.com  */

#if __APPLE__ && !TARGET_IOS
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
#include "fsmod.h"
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <sys/time.h>
#include <unistd.h>

#define FSEVAPI_DEBUG 0

#if TARGET_WATCHOS

#warning No FSEventMonitor API for WatchOS yet

#else

#include <CoreFoundation/CoreFoundation.h>

#if TARGET_IOS

#include <MobileCoreServices/MobileCoreServices.h>

const int kFSEventStreamCreateFlagFileEvents = 0x10;
const uint64_t kFSEventStreamEventIdSinceNow = UINT64_MAX;

#define kFSEventStreamEventFlagMustScanSubDirs 0x00000001
#define kFSEventStreamEventFlagUserDropped 0x00000002
#define kFSEventStreamEventFlagKernelDropped 0x00000004
#define kFSEventStreamEventFlagRootChanged 0x00000020
#define kFSEventStreamEventFlagItemCreated 0x00000100
#define kFSEventStreamEventFlagItemRemoved 0x00000200
#define kFSEventStreamEventFlagItemInodeMetaMod 0x00000400
#define kFSEventStreamEventFlagItemRenamed 0x00000800
#define kFSEventStreamEventFlagItemModified 0x00001000
#define kFSEventStreamEventFlagItemFinderInfoMod 0x00002000
#define kFSEventStreamEventFlagItemChangeOwner 0x00004000
#define kFSEventStreamEventFlagItemXattrMod 0x00008000
#define kFSEventStreamEventFlagItemIsDir 0x00020000

typedef struct {
        CFIndex version;
        void *info;
        CFAllocatorRetainCallBack retain;
        CFAllocatorReleaseCallBack release;
        CFAllocatorCopyDescriptionCallBack copyDescription;
} FSEventStreamContext;

typedef void * ConstFSEventStreamRef;
typedef void * FSEventStreamRef;
typedef uint32_t FSEventStreamEventFlags;
typedef uint64_t FSEventStreamEventId;
typedef uint32_t FSEventStreamCreateFlags;

typedef void ( *FSEventStreamCallback )( ConstFSEventStreamRef streamRef, void *clientCallBackInfo, size_t numEvents, void *eventPaths, const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId eventIds[]);
extern FSEventStreamRef FSEventStreamCreate( CFAllocatorRef allocator, FSEventStreamCallback callback, FSEventStreamContext *context, CFArrayRef pathsToWatch, FSEventStreamEventId sinceWhen, CFTimeInterval latency, FSEventStreamCreateFlags flags);
extern Boolean FSEventStreamStart( FSEventStreamRef streamRef);
extern void FSEventStreamScheduleWithRunLoop( FSEventStreamRef streamRef, CFRunLoopRef runLoop, CFStringRef runLoopMode);
extern void FSEventStreamStop( FSEventStreamRef streamRef);
extern void FSEventStreamInvalidate( FSEventStreamRef streamRef);
extern void FSEventStreamRelease( FSEventStreamRef streamRef);

#else

#include <CoreServices/CoreServices.h>

#endif

/* TODO: move into event's ctx */
static FileMonitorCallback global_cb;
static FSEventStreamRef stream;
static CFArrayRef watched_paths;
static bool stream_started;

static int event_type(FSEventStreamEventFlags flags) {
	if (flags & kFSEventStreamEventFlagItemRenamed) return FSE_RENAME;
	if (flags & kFSEventStreamEventFlagItemRemoved) return FSE_DELETE;
	if (flags & kFSEventStreamEventFlagItemCreated) {
		return (flags & kFSEventStreamEventFlagItemIsDir)? FSE_CREATE_DIR: FSE_CREATE_FILE;
	}
	if (flags & kFSEventStreamEventFlagItemModified) return FSE_CONTENT_MODIFIED;
	if (flags & kFSEventStreamEventFlagItemChangeOwner) return FSE_CHOWN;
	if (flags & kFSEventStreamEventFlagItemXattrMod) return FSE_XATTR_MODIFIED;
	if (flags & kFSEventStreamEventFlagItemFinderInfoMod) return FSE_FINDER_INFO_CHANGED;
	if (flags & (kFSEventStreamEventFlagItemInodeMetaMod | kFSEventStreamEventFlagRootChanged)) {
		return FSE_STAT_CHANGED;
	}
	return FSE_UNKNOWN;
}

static void event_cb(ConstFSEventStreamRef streamRef, void *ctx, size_t count, void *paths,
                const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
	FileMonitor *fm = (FileMonitor*)ctx;
        size_t i;

	if (!fm->running) {
		return;
	}
        for (i = 0; i < count && fm->running; i++) {
		FileMonitorEvent ev = {0};
                char *path = ((char **)paths)[i];
#if FSEVAPI_DEBUG
                /* flags are unsigned long, IDs are uint64_t */
                printf ("%zu Change 0x%" PRIx64 " in %s, flags %lu",
                        i, (uint64_t) ids[i], path, (unsigned long)flags[i]);
#endif
		if (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs |
				kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) {
			eprintf ("Warning: FSEvents reported dropped/coalesced events under %s\n", path);
		}
		ev.type = event_type (flags[i]);
		ev.file = path;
		if (global_cb) global_cb (fm, &ev);
        }
}

static bool fm_end(FileMonitor *fm) {
	bool active = stream != NULL;
	if (stream) {
		if (stream_started) FSEventStreamStop (stream);
		FSEventStreamInvalidate (stream);
		FSEventStreamRelease (stream);
		stream = NULL;
	}
	if (watched_paths) {
		CFRelease (watched_paths);
		watched_paths = NULL;
	}
	stream_started = false;
	global_cb = NULL;
	return active;
}

static bool fm_begin (FileMonitor *fm) {
        FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagFileEvents;
        FSEventStreamContext ctx = {
                0,
                fm,
                NULL,
                NULL,
                NULL
        };

	size_t i, count = fm->root_count? fm->root_count: 1;
	CFMutableArrayRef paths = CFArrayCreateMutable (NULL, count, &kCFTypeArrayCallBacks);
	if (!paths) return false;
	watched_paths = paths;
	for (i = 0; i < count; i++) {
		const char *root = fm->root_count? fm->roots[i]: (fm->root? fm->root: "/");
		char *absolute = realpath (root, NULL);
		struct stat st;
		if (!absolute || stat (absolute, &st)) {
			eprintf ("Cannot resolve FSEvents root %s: %s\n", root, strerror (errno));
			free (absolute);
			fm_end (fm);
			return false;
		}
		/* Watch a file's parent so atomic replacement keeps being reported. */
		if (!S_ISDIR (st.st_mode)) {
			char *slash = strrchr (absolute, '/');
			if (slash == absolute) slash[1] = 0;
			else *slash = 0;
		}
		CFStringRef path = CFStringCreateWithCString (NULL, absolute, kCFStringEncodingUTF8);
		free (absolute);
		if (!path) {
			eprintf ("Cannot encode FSEvents path %s as UTF-8\n", root);
			fm_end (fm);
			return false;
		}
		CFArrayAppendValue (paths, path);
		CFRelease (path);
	}

        stream = FSEventStreamCreate (NULL, &event_cb,
		&ctx, paths, kFSEventStreamEventIdSinceNow, 0, flags);
	if (!stream) {
		eprintf ("Cannot create FSEvents stream\n");
		fm_end (fm);
		return false;
	}
        FSEventStreamScheduleWithRunLoop (stream, CFRunLoopGetCurrent(),
		kCFRunLoopDefaultMode);
        stream_started = FSEventStreamStart (stream);
	if (!stream_started) {
		eprintf ("Cannot start FSEvents stream\n");
		fm_end (fm);
		return false;
	}
	return true;
}

static bool fm_loop (FileMonitor *fm, FileMonitorCallback cb) {
	global_cb = cb;
	while (fm->running) {
		SInt32 status = CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.25, true);
		if (status == kCFRunLoopRunFinished && fm->running) {
			eprintf ("FSEvents run loop lost its event source\n");
			return false;
		}
	}
	return true;
}

FileMonitorBackend fmb_fsevapi = {
	.name = "fsevapi",
	.begin = fm_begin,
	.loop = fm_loop,
	.end = fm_end,
};

#endif
#endif
