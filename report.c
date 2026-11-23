/* Asynchronous, bounded, best-effort HTTP event delivery. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#if __APPLE__ && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if (defined(TARGET_IOS) && TARGET_IOS) || defined(FSMOD_CFNETWORK_TEST)
#define FM_USE_CFNETWORK 1
#else
#define FM_USE_CFNETWORK 0
#endif
#if TARGET_WATCHOS || FSMOD_NO_REPORT
#include <stdio.h>
#include "report.h"

char *fm_event_json(const FileMonitor *fm, const FileMonitorEvent *ev) {
	(void)fm; (void)ev; return NULL;
}
FmReporter *fm_reporter_start(const FmConfig *config, char *error, size_t size) {
	(void)config;
	if (error && size) snprintf(error, size, "daemon reporting is not enabled in this build");
	return NULL;
}
bool fm_reporter_is_internal(FmReporter *reporter, const FileMonitorEvent *ev) {
	(void)reporter; (void)ev; return false;
}
bool fm_reporter_submit(FmReporter *reporter, const FileMonitor *fm, const FileMonitorEvent *ev) {
	(void)reporter; (void)fm; (void)ev; return false;
}
bool fm_reporter_pause(FmReporter *reporter, bool paused, bool discard_pending, unsigned timeout_seconds) {
	(void)reporter; (void)paused; (void)discard_pending; (void)timeout_seconds; return false;
}
void fm_reporter_status(FmReporter *reporter, FmReporterStatus *status) {
	(void)reporter; if (status) *status = (FmReporterStatus){0};
}
void fm_reporter_stop(FmReporter *reporter) { (void)reporter; }
#else
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#if TARGET_IOS || defined(FSMOD_IOS_METADATA_TEST)
#include <dlfcn.h>
#endif
#if FM_USE_CFNETWORK
#include <CFNetwork/CFNetwork.h>
#else
#include <curl/curl.h>
#if LIBCURL_VERSION_NUM < 0x072000
#error libcurl 7.32.0 or newer is required
#endif
#endif
#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#include <uuid/uuid.h>
#endif
#include "report.h"

typedef struct {
	/* iOS UniqueDeviceID is not necessarily an RFC 4122 UUID. */
	char uuid[128];
	char os_name[256];
	char os_version[128];
	char kernel_version[256];
} HostInfo;

static HostInfo host_info;
static pthread_once_t host_info_once = PTHREAD_ONCE_INIT;

/* Read metadata before watching starts; never execute a shell per event. */
static FILE *metadata_file(const char *path) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return NULL;
	FILE *file = fdopen(fd, "r");
	if (!file) close(fd);
	return file;
}

#if __APPLE__
static void product_version(void) {
	size_t length = sizeof(host_info.os_version);
	if (!sysctlbyname("kern.osproductversion", host_info.os_version, &length, NULL, 0) &&
		length > 1 && length <= sizeof(host_info.os_version)) {
		host_info.os_version[sizeof(host_info.os_version) - 1] = 0;
		return;
	}
	host_info.os_version[0] = 0;
	/* Older macOS versions do not expose kern.osproductversion. */
	FILE *file = metadata_file("/System/Library/CoreServices/SystemVersion.plist");
	if (!file) return;
	unsigned char bytes[16384];
	size_t count = fread(bytes, 1, sizeof(bytes), file);
	bool ok = !ferror(file) && count < sizeof(bytes);
	fclose(file);
	if (!ok || !count) return;
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)count);
	if (!data) return;
	CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
		kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (!plist) return;
	if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
		CFTypeRef version = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("ProductVersion"));
		if (version && CFGetTypeID(version) == CFStringGetTypeID() &&
			!CFStringGetCString((CFStringRef)version, host_info.os_version,
				sizeof(host_info.os_version), kCFStringEncodingUTF8)) host_info.os_version[0] = 0;
	}
	CFRelease(plist);
}

#if TARGET_IOS || defined(FSMOD_IOS_METADATA_TEST)
static bool copy_ios_identifier(CFTypeRef answer) {
	if (!answer || CFGetTypeID(answer) != CFStringGetTypeID()) return false;
	char value[sizeof(host_info.uuid)];
	CFIndex length = CFStringGetLength((CFStringRef)answer);
	if (length <= 0 || length >= (CFIndex)sizeof(value) ||
		!CFStringGetCString((CFStringRef)answer, value, sizeof(value), kCFStringEncodingASCII) ||
		strlen(value) != (size_t)length) return false;
	for (size_t i = 0; i < (size_t)length; i++) {
		if ((unsigned char)value[i] < 0x21 || (unsigned char)value[i] > 0x7e) return false;
	}
	memcpy(host_info.uuid, value, (size_t)length + 1);
	return true;
}

static void ios_device_identifier(void) {
	typedef CFTypeRef (*MgCopyAnswer)(CFStringRef, CFDictionaryRef);
	void *handle = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "fsmod: cannot read iOS UniqueDeviceID: libMobileGestalt unavailable; uuid=null\n");
		return;
	}
	MgCopyAnswer copy_answer = (MgCopyAnswer)dlsym(handle, "MGCopyAnswer");
	if (!copy_answer) {
		fprintf(stderr, "fsmod: cannot read iOS UniqueDeviceID: MGCopyAnswer unavailable; uuid=null\n");
		dlclose(handle);
		return;
	}
	CFTypeRef answer = copy_answer(CFSTR("UniqueDeviceID"), NULL);
	if (!copy_ios_identifier(answer))
		fprintf(stderr, "fsmod: cannot read iOS UniqueDeviceID: MobileGestalt returned no usable identifier; uuid=null\n");
	if (answer) CFRelease(answer);
	dlclose(handle);
}
#endif
#else
/* os-release values are data, not shell input. Decode quotes without expansion. */
static bool release_value(const char *text, char *out, size_t capacity) {
	size_t length = strlen(text);
	while (length && (text[length - 1] == '\n' || text[length - 1] == '\r')) length--;
	char quote = length && (text[0] == '"' || text[0] == 39) ? text[0] : 0;
	size_t first = quote ? 1 : 0;
	if (quote && (length < 2 || text[length - 1] != quote)) return false;
	size_t end = quote ? length - 1 : length, used = 0;
	for (size_t i = first; i < end; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c == 92 && quote != 39) {
			if (++i == end) return false;
			char next = text[i];
			if (quote == '"' && next != '"' && next != 92 && next != '$' && next != '`') {
				if (used + 1 >= capacity) return false;
				out[used++] = 92;
			}
			c = (unsigned char)next;
		} else if (quote && c == (unsigned char)quote) return false;
		if (c < 0x20 || c == 0x7f || used + 1 >= capacity) return false;
		out[used++] = (char)c;
	}
	out[used] = 0;
	return true;
}

static bool machine_id(const char *path) {
	FILE *file = metadata_file(path);
	if (!file) return false;
	char line[128];
	bool ok = fgets(line, sizeof(line), file) != NULL && !ferror(file);
	if (ok && fgetc(file) != EOF) ok = false;
	fclose(file);
	if (!ok) return false;
	size_t length = strlen(line);
	while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
	if (length != 32) return false;
	bool nonzero = false;
	for (size_t i = 0; i < length; i++) {
		if (!((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f'))) return false;
		if (line[i] != '0') nonzero = true;
	}
	if (!nonzero) return false;
	memcpy(host_info.uuid, line, length + 1);
	return true;
}

static void linux_release(void) {
	FILE *file = metadata_file("/etc/os-release");
	if (!file) file = metadata_file("/usr/lib/os-release");
	if (!file) return;
	char line[4096], decoded[256];
	while (fgets(line, sizeof(line), file)) {
		size_t length = strlen(line);
		if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
			int c; while ((c = fgetc(file)) != '\n' && c != EOF) {}
			continue;
		}
		char *value = strchr(line, '=');
		if (!value) continue;
		*value++ = 0;
		if (strcmp(line, "NAME") && strcmp(line, "VERSION_ID")) continue;
		if (!release_value(value, decoded, sizeof(decoded))) continue;
		if (!strcmp(line, "NAME")) snprintf(host_info.os_name, sizeof(host_info.os_name), "%s", decoded);
		else if (strlen(decoded) < sizeof(host_info.os_version))
			memcpy(host_info.os_version, decoded, strlen(decoded) + 1);
	}
	fclose(file);
}
#endif

static void collect_host_info(void) {
	struct utsname system;
	if (!uname(&system)) {
		snprintf(host_info.kernel_version, sizeof(host_info.kernel_version), "%s", system.release);
		snprintf(host_info.os_name, sizeof(host_info.os_name), "%s", system.sysname);
	}
#if __APPLE__
#if !TARGET_IOS && !defined(FSMOD_IOS_METADATA_TEST)
	uuid_t id; struct timespec timeout = {1, 0};
	if (!gethostuuid(id, &timeout) && !uuid_is_null(id)) uuid_unparse_upper(id, host_info.uuid);
#endif
#if TARGET_IOS || defined(FSMOD_IOS_METADATA_TEST)
	ios_device_identifier();
	snprintf(host_info.os_name, sizeof(host_info.os_name), "iOS");
#else
	snprintf(host_info.os_name, sizeof(host_info.os_name), "macOS");
#endif
	product_version();
#else
	if (!machine_id("/etc/machine-id")) machine_id("/var/lib/dbus/machine-id");
	linux_release();
#endif
}

typedef struct {
	char *data;
	size_t length, capacity;
	bool failed;
} JsonBuffer;

struct FmReporter {
	const FmConfig *config;
	pthread_t worker;
	pthread_mutex_t mutex;
	pthread_cond_t changed;
	char **queue;
	size_t capacity, head, count;
	bool stopping, paused, in_flight, discard_active;
	uint64_t cancel_generation;
	double stop_deadline;
#if FM_USE_CFNETWORK
	CFURLRef url;
	CFStringRef authorization;
#else
	CURL *easy;
	CURLM *multi;
	struct curl_slist *headers;
#endif
	uint64_t delivered, failed, dropped;
};

static void set_error(char *error, size_t size, const char *fmt, ...) {
	if (!error || !size) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(error, size, fmt, ap);
	va_end(ap);
}

static double monotonic_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void append_bytes(JsonBuffer *b, const char *s, size_t length) {
	if (b->failed) return;
	if (length > SIZE_MAX - b->length - 1) { b->failed = true; return; }
	size_t needed = b->length + length + 1;
	if (needed > b->capacity) {
		size_t capacity = b->capacity ? b->capacity : 512;
		while (capacity < needed) {
			if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
			capacity *= 2;
		}
		char *data = realloc(b->data, capacity);
		if (!data) { b->failed = true; return; }
		b->data = data; b->capacity = capacity;
	}
	memcpy(b->data + b->length, s, length);
	b->length += length; b->data[b->length] = 0;
}

static void append_text(JsonBuffer *b, const char *s) { append_bytes(b, s, strlen(s)); }

/* Preserve UTF-8; encode arbitrary invalid path bytes as U+00xx. */
static size_t utf8_length(const unsigned char *s) {
	unsigned char a = s[0];
	size_t n = a >= 0xc2 && a <= 0xdf ? 2 : a >= 0xe0 && a <= 0xef ? 3 : a >= 0xf0 && a <= 0xf4 ? 4 : 0;
	if (!n) return 0;
	for (size_t i = 1; i < n; i++) if (!s[i] || (s[i] & 0xc0) != 0x80) return 0;
	if ((a == 0xe0 && s[1] < 0xa0) || (a == 0xed && s[1] >= 0xa0) ||
		(a == 0xf0 && s[1] < 0x90) || (a == 0xf4 && s[1] >= 0x90)) return 0;
	return n;
}

static void append_string(JsonBuffer *b, const char *value) {
	if (!value) { append_text(b, "null"); return; }
	append_text(b, "\"");
	const unsigned char *s = (const unsigned char *)value;
	while (*s) {
		if (*s == '"' || *s == 92) {
			char escaped[2] = {92, (char)*s++}; append_bytes(b, escaped, 2);
		} else if (*s < 0x20 || *s >= 0x80) {
			size_t n = *s >= 0x80 ? utf8_length(s) : 0;
			if (n) { append_bytes(b, (const char *)s, n); s += n; }
			else { char escaped[7]; snprintf(escaped, sizeof(escaped), "%cu%04x", 92, (unsigned int)*s++); append_text(b, escaped); }
		} else { append_bytes(b, (const char *)s++, 1); }
	}
	append_text(b, "\"");
}

static void string_field(JsonBuffer *b, const char *key, const char *value, bool first) {
	if (!first) append_text(b, ",");
	append_string(b, key); append_text(b, ":"); append_string(b, value);
}

static void number_field(JsonBuffer *b, const char *key, int64_t value) {
	char number[64]; snprintf(number, sizeof(number), ",\"%s\":%" PRId64, key, value); append_text(b, number);
}

char *fm_event_json(const FileMonitor *fm, const FileMonitorEvent *ev) {
	if (!fm || !ev) return NULL;
	if (pthread_once(&host_info_once, collect_host_info)) return NULL;
	char hostname[256] = {0}, observed[40];
	if (gethostname(hostname, sizeof(hostname) - 1)) strcpy(hostname, "unknown");
	struct timespec ts; struct tm utc;
	if (clock_gettime(CLOCK_REALTIME, &ts) || !gmtime_r(&ts.tv_sec, &utc)) return NULL;
	char seconds[24];
	if (!strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc)) return NULL;
	snprintf(observed, sizeof(observed), "%s.%03ldZ", seconds, ts.tv_nsec / 1000000);
	JsonBuffer b = {0}; append_text(&b, "{");
	string_field(&b, "source", "fsmod", true);
	append_text(&b, ",\"data\":{");
	string_field(&b, "hostname", hostname, true);
	string_field(&b, "uuid", host_info.uuid[0] ? host_info.uuid : NULL, false);
	string_field(&b, "os_name", host_info.os_name[0] ? host_info.os_name : NULL, false);
	string_field(&b, "os_version", host_info.os_version[0] ? host_info.os_version : NULL, false);
	string_field(&b, "kernel_version", host_info.kernel_version[0] ? host_info.kernel_version : NULL, false);
	string_field(&b, "backend", fm->backend.name, false);
	string_field(&b, "observed_at", observed, false);
	string_field(&b, "type", fm_typestr(ev->type), false);
	string_field(&b, "filename", ev->file, false);
	string_field(&b, "newfile", ev->newfile, false);
	string_field(&b, "proc", ev->proc, false);
	string_field(&b, "event", ev->event, false);
	number_field(&b, "pid", ev->pid); number_field(&b, "ppid", ev->ppid);
	number_field(&b, "uid", ev->uid); number_field(&b, "gid", ev->gid);
	char inode[64];
	snprintf(inode, sizeof(inode), ",\"inode\":%" PRIu64, ev->inode);
	append_text(&b, inode); number_field(&b, "mode", ev->mode);
	number_field(&b, "dev_major", ev->dev_major); number_field(&b, "dev_minor", ev->dev_minor);
	char timestamp[80];
	snprintf(timestamp, sizeof(timestamp), ",\"kernel_timestamp\":%" PRIu64 "}}", ev->tstamp);
	append_text(&b, timestamp);
	if (b.failed) { free(b.data); return NULL; }
	return b.data;
}

static bool valid_url(const char *url) {
	if (!url) return false;
	size_t scheme = !strncasecmp(url, "https://", 8) ? 8 : !strncasecmp(url, "http://", 7) ? 7 : 0;
	if (!scheme || !url[scheme] || url[scheme] == '/' || url[scheme] == '?' || url[scheme] == '#') return false;
	for (const unsigned char *p = (const unsigned char *)url; *p; p++) if (*p <= 0x20 || *p == 0x7f) return false;
	return true;
}

#if !FM_USE_CFNETWORK
/* Initialize before creating any reporter threads; keep globals alive for the
 * process lifetime so multiple reporters cannot race curl_global_cleanup(). */
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_init_result = CURLE_FAILED_INIT;
static void initialize_curl(void) { curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT); }
#endif

static bool valid_token(const char *token) {
	if (!token) return true;
	for (const unsigned char *p = (const unsigned char *)token; *p; p++)
		if (*p < 0x20 || *p == 0x7f) return false;
	return true;
}

static bool transfer_cancelled(FmReporter *r, uint64_t generation) {
	pthread_mutex_lock(&r->mutex);
	bool expired = r->paused || generation != r->cancel_generation ||
		(r->stopping && monotonic_now() >= r->stop_deadline);
	pthread_mutex_unlock(&r->mutex); return expired;
}

bool fm_reporter_is_internal(FmReporter *r, const FileMonitorEvent *ev) {
	if (!r || !ev) return false;
	pid_t own_pid = getpid();
	return ev->pid == own_pid || ev->ppid == own_pid;
}

#if FM_USE_CFNETWORK
/* CFNetwork is part of iOS, so both jailbreak layouts use the system HTTP/TLS
 * implementation without an external curl binary or bundled TLS libraries. */
static void transport_cleanup(FmReporter *r) {
	if (r->url) CFRelease(r->url);
	if (r->authorization) CFRelease(r->authorization);
	r->url = NULL; r->authorization = NULL;
}

static bool transport_setup(FmReporter *r, char *error, size_t error_size) {
	CFStringRef text = CFStringCreateWithCString(kCFAllocatorDefault,
		r->config->report_url, kCFStringEncodingUTF8);
	if (text) {
		r->url = CFURLCreateWithString(kCFAllocatorDefault, text, NULL);
		CFRelease(text);
	}
	CFStringRef host = r->url ? CFURLCopyHostName(r->url) : NULL;
	bool valid = host && CFStringGetLength(host) > 0;
	if (host) CFRelease(host);
	if (!valid) { set_error(error, error_size, "invalid HTTP report URL"); return false; }
	if (r->config->report_token && *r->config->report_token) {
		CFStringRef token = CFStringCreateWithCString(kCFAllocatorDefault,
			r->config->report_token, kCFStringEncodingUTF8);
		if (token) {
			r->authorization = CFStringCreateWithFormat(kCFAllocatorDefault, NULL,
				CFSTR("Bearer %@"), token);
			CFRelease(token);
		}
		if (!r->authorization) {
			set_error(error, error_size, "cannot allocate bearer header"); return false;
		}
	}
	return true;
}

typedef struct { bool complete, failed; } HttpReadState;

static void receive_http(CFReadStreamRef stream, CFStreamEventType event, void *context) {
	HttpReadState *state = context;
	if (event == kCFStreamEventErrorOccurred) { state->failed = true; return; }
	if (event == kCFStreamEventEndEncountered) { state->complete = true; return; }
	if (event != kCFStreamEventHasBytesAvailable) return;
	/* A bounded callback lets the outer run loop check the stop/transfer deadline
	 * even when a peer streams a response continuously. */
	unsigned char bytes[4096];
	for (unsigned i = 0; i < 16 && CFReadStreamHasBytesAvailable(stream); i++) {
		CFIndex count = CFReadStreamRead(stream, bytes, sizeof(bytes));
		if (count < 0) { state->failed = true; break; }
		if (!count) { state->complete = true; break; }
	}
}

static bool send_once(FmReporter *r, const char *payload, uint64_t generation) {
	if (transfer_cancelled(r, generation)) return false;
	size_t length = strlen(payload);
	if (length > (size_t)LONG_MAX) return false;
	CFHTTPMessageRef request = CFHTTPMessageCreateRequest(kCFAllocatorDefault,
		CFSTR("POST"), r->url, kCFHTTPVersion1_1);
	CFDataRef body = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)payload, (CFIndex)length);
	if (!request || !body) {
		if (request) CFRelease(request);
		if (body) CFRelease(body);
		return false;
	}
	CFHTTPMessageSetHeaderFieldValue(request, CFSTR("Content-Type"), CFSTR("application/json"));
	if (r->authorization)
		CFHTTPMessageSetHeaderFieldValue(request, CFSTR("Authorization"), r->authorization);
	CFHTTPMessageSetBody(request, body);
	CFRelease(body);
	CFReadStreamRef stream = CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request);
	CFRelease(request);
	if (!stream) return false;
	/* Keep normal platform certificate/hostname verification. In particular, do
	 * not install permissive SSL settings for HTTPS on jailbroken devices. */
	bool configured = CFReadStreamSetProperty(stream,
		kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanFalse);
	(void)CFReadStreamSetProperty(stream,
		kCFStreamPropertyHTTPAttemptPersistentConnection, kCFBooleanTrue);
	HttpReadState state = {false, false};
	CFStreamClientContext context = {0, &state, NULL, NULL, NULL};
	CFOptionFlags events = kCFStreamEventHasBytesAvailable |
		kCFStreamEventErrorOccurred | kCFStreamEventEndEncountered;
	configured = configured && CFReadStreamSetClient(stream, events, receive_http, &context);
	CFRunLoopRef loop = CFRunLoopGetCurrent();
	double deadline = monotonic_now() + r->config->report_timeout_seconds;
	if (configured) {
		CFReadStreamScheduleWithRunLoop(stream, loop, kCFRunLoopDefaultMode);
		if (!CFReadStreamOpen(stream)) state.failed = true;
		while (!state.complete && !state.failed && !transfer_cancelled(r, generation) && monotonic_now() < deadline)
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
	}
	CFHTTPMessageRef response = (CFHTTPMessageRef)CFReadStreamCopyProperty(stream,
		kCFStreamPropertyHTTPResponseHeader);
	bool delivered = configured && state.complete && !state.failed && response &&
		CFGetTypeID(response) == CFHTTPMessageGetTypeID() &&
		CFHTTPMessageGetResponseStatusCode(response) == 200;
	if (response) CFRelease(response);
	CFReadStreamSetClient(stream, kCFStreamEventNone, NULL, NULL);
	if (configured) CFReadStreamUnscheduleFromRunLoop(stream, loop, kCFRunLoopDefaultMode);
	CFReadStreamClose(stream);
	CFRelease(stream);
	return delivered;
}
#else
/* Consume response bodies without allocating, logging, or interpreting them. */
static size_t discard_response(char *data, size_t size, size_t count, void *context) {
	(void)data; (void)context;
	if (size && count > SIZE_MAX / size) return 0;
	return size * count;
}

static bool add_header(FmReporter *r, const char *value) {
	struct curl_slist *headers = curl_slist_append(r->headers, value);
	if (!headers) return false;
	r->headers = headers; return true;
}

/* Newer libcurl can detach an outstanding resolver during shutdown. Earlier
 * threaded resolvers may still wait for the OS even after our drain deadline. */
static void prepare_shutdown(CURL *easy) {
#if LIBCURL_VERSION_NUM >= 0x075700
	const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
	if (version && version->version_num >= 0x075700)
		(void)curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);
#else
	(void)easy;
#endif
}

static void transport_cleanup(FmReporter *r) {
	if (r->stopping && r->easy) prepare_shutdown(r->easy);
	if (r->multi) curl_multi_cleanup(r->multi);
	if (r->easy) curl_easy_cleanup(r->easy);
	curl_slist_free_all(r->headers);
	r->multi = NULL; r->easy = NULL; r->headers = NULL;
}

static bool transport_setup(FmReporter *r, char *error, size_t error_size) {
	r->easy = curl_easy_init(); r->multi = curl_multi_init();
	if (!r->easy || !r->multi || !add_header(r, "Content-Type: application/json") ||
		!add_header(r, "Expect:")) {
		set_error(error, error_size, "cannot allocate HTTP transport"); return false;
	}
	if (r->config->report_token && *r->config->report_token) {
		const char *prefix = "Authorization: Bearer ";
		size_t length = strlen(r->config->report_token), prefix_length = strlen(prefix);
		if (length > SIZE_MAX - prefix_length - 1) {
			set_error(error, error_size, "bearer token is too long"); return false;
		}
		char *header = malloc(prefix_length + length + 1);
		if (!header) { set_error(error, error_size, "cannot allocate bearer header"); return false; }
		memcpy(header, prefix, prefix_length);
		memcpy(header + prefix_length, r->config->report_token, length + 1);
		bool added = add_header(r, header); free(header);
		if (!added) { set_error(error, error_size, "cannot allocate bearer header"); return false; }
	}
	CURLcode result;
#define SETOPT(option, value) do { \
	result = curl_easy_setopt(r->easy, option, value); \
	if (result != CURLE_OK) { \
		set_error(error, error_size, "cannot configure HTTP transport: %s", curl_easy_strerror(result)); \
		return false; \
	} \
} while (0)
	SETOPT(CURLOPT_URL, r->config->report_url);
	SETOPT(CURLOPT_POST, 1L);
	SETOPT(CURLOPT_HTTPHEADER, r->headers);
	SETOPT(CURLOPT_NOSIGNAL, 1L);
	SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
	SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
	SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
	SETOPT(CURLOPT_CONNECTTIMEOUT, (long)r->config->report_timeout_seconds);
	SETOPT(CURLOPT_TIMEOUT, (long)r->config->report_timeout_seconds);
	SETOPT(CURLOPT_WRITEFUNCTION, discard_response);
	SETOPT(CURLOPT_WRITEDATA, (void *)NULL);
#if LIBCURL_VERSION_NUM >= 0x075500
	SETOPT(CURLOPT_PROTOCOLS_STR, "http,https");
#else
	SETOPT(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
#undef SETOPT
	return true;
}

/* Reuse both handles across events, preserving keep-alive connections. The
 * multi API bounds waits so stop can cancel even a stalled connect or TLS peer. */
static bool send_once(FmReporter *r, const char *payload, uint64_t generation) {
	if (transfer_cancelled(r, generation)) return false;
	size_t length = strlen(payload);
	curl_off_t body_size = (curl_off_t)length;
	if (body_size < 0 || (size_t)body_size != length) return false;
	if (curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, payload) != CURLE_OK ||
		curl_easy_setopt(r->easy, CURLOPT_POSTFIELDSIZE_LARGE, body_size) != CURLE_OK) {
		curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, (void *)NULL);
		return false;
	}
	if (curl_multi_add_handle(r->multi, r->easy) != CURLM_OK) {
		curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, (void *)NULL);
		return false;
	}
	double deadline = monotonic_now() + r->config->report_timeout_seconds;
	CURLcode transfer = CURLE_FAILED_INIT;
	bool completed = false;
	for (;;) {
		if (transfer_cancelled(r, generation) || monotonic_now() >= deadline) break;
		int running = 0;
		CURLMcode status;
		do { status = curl_multi_perform(r->multi, &running); }
		while (status == CURLM_CALL_MULTI_PERFORM);
		if (status != CURLM_OK) break;
		int remaining;
		CURLMsg *message;
		while ((message = curl_multi_info_read(r->multi, &remaining))) {
			if (message->msg == CURLMSG_DONE && message->easy_handle == r->easy) {
				transfer = message->data.result; completed = true;
			}
		}
		if (completed || !running) break;
		int ready = 0;
		if (curl_multi_wait(r->multi, NULL, 0, 100, &ready) != CURLM_OK) break;
		/* curl_multi_wait returns immediately when DNS has no socket to poll. */
		if (!ready) poll(NULL, 0, 10);
	}
	long http = 0;
	CURLcode info = curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &http);
	pthread_mutex_lock(&r->mutex);
	bool stopping = r->stopping;
	pthread_mutex_unlock(&r->mutex);
	if (stopping) prepare_shutdown(r->easy);
	CURLMcode removed = curl_multi_remove_handle(r->multi, r->easy);
	/* POSTFIELDS borrows queued memory; clear it before the queue item is freed. */
	curl_easy_setopt(r->easy, CURLOPT_POSTFIELDS, (void *)NULL);
	return completed && transfer == CURLE_OK && info == CURLE_OK &&
		removed == CURLM_OK && http == 200;
}
#endif

/* Poll a monotonic deadline so wall-clock adjustments cannot delay shutdown. */
static void wait_seconds(FmReporter *r, double seconds, uint64_t generation) {
	double until = monotonic_now() + seconds;
	for (;;) {
		pthread_mutex_lock(&r->mutex);
		double now = monotonic_now(), remaining = until - now;
		if (r->paused || generation != r->cancel_generation) remaining = 0;
		if (r->stopping && r->stop_deadline - now < remaining) remaining = r->stop_deadline - now;
		pthread_mutex_unlock(&r->mutex);
		if (remaining <= 0) break;
		int milliseconds = remaining >= 0.025 ? 25 : (int)(remaining * 1000.0) + 1;
		poll(NULL, 0, milliseconds);
	}
}

static void *report_worker(void *arg) {
	FmReporter *r = arg;
	/* EPIPE must never terminate the monitor. Only this worker blocks SIGPIPE. */
	sigset_t blocked; sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE); sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &blocked, NULL);
	for (;;) {
		pthread_mutex_lock(&r->mutex);
		while ((!r->count || r->paused) && !r->stopping) pthread_cond_wait(&r->changed, &r->mutex);
		if (r->stopping && (r->paused || !r->count || monotonic_now() >= r->stop_deadline)) { pthread_mutex_unlock(&r->mutex); break; }
		char *payload = r->queue[r->head];
		r->queue[r->head] = NULL; r->head = (r->head + 1) % r->capacity; r->count--;
		r->in_flight = true; r->discard_active = false;
		uint64_t generation = r->cancel_generation;
		pthread_mutex_unlock(&r->mutex);
		bool delivered = false;
		for (int attempt = 0; attempt <= r->config->report_retries && !transfer_cancelled(r, generation); attempt++) {
			if (send_once(r, payload, generation)) { delivered = true; break; }
			if (attempt < r->config->report_retries && !transfer_cancelled(r, generation))
				wait_seconds(r, r->config->retry_delay_seconds, generation);
		}
		pthread_mutex_lock(&r->mutex);
		bool interrupted = generation != r->cancel_generation;
		if (delivered) r->delivered++;
		else if (interrupted) {
			/* Producers may have filled the pending queue before pause. Never
			 * exceed its configured capacity when retaining the active item. */
			if (!r->discard_active && !r->stopping && r->count < r->capacity) {
				r->head = (r->head + r->capacity - 1) % r->capacity;
				r->queue[r->head] = payload; r->count++; payload = NULL;
			} else r->dropped++;
		} else r->failed++;
		free(payload); r->in_flight = false;
		uint64_t failures = r->failed;
		pthread_cond_broadcast(&r->changed); pthread_mutex_unlock(&r->mutex);
		if (!delivered && !interrupted && (failures == 1 || (failures & (failures - 1)) == 0)) fprintf(stderr, "fsmod: HTTP delivery failed; failed=%" PRIu64 " (best effort; no persistent queue)\n", failures);
	}
	/* TLS/socket cleanup also runs with this worker's SIGPIPE mask in effect. */
	transport_cleanup(r);
	return NULL;
}

FmReporter *fm_reporter_start(const FmConfig *config, char *error, size_t error_size) {
	if (error && error_size) *error = 0;
	if (!config || !valid_url(config->report_url) || config->queue_capacity < 1 || config->queue_capacity > 1000000 ||
		config->report_timeout_seconds < 1 || config->report_timeout_seconds > 3600 ||
		config->report_retries < 0 || config->report_retries > 100 || config->retry_delay_seconds < 0 ||
		config->retry_delay_seconds > 3600 || config->shutdown_timeout_seconds < 0 || config->shutdown_timeout_seconds > 3600) {
		set_error(error, error_size, "invalid HTTP reporter configuration"); return NULL;
	}
	if (!valid_token(config->report_token)) {
		set_error(error, error_size, "bearer token contains invalid control characters"); return NULL;
	}
#if !FM_USE_CFNETWORK
	if (pthread_once(&curl_once, initialize_curl) || curl_init_result != CURLE_OK) {
		set_error(error, error_size, "cannot initialize libcurl"); return NULL;
	}
	const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
	if (!version || version->version_num < 0x072000 || !(version->features & CURL_VERSION_ASYNCHDNS)) {
		set_error(error, error_size, "libcurl >= 7.32.0 with asynchronous DNS is required"); return NULL;
	}
	if (!strncasecmp(config->report_url, "https://", 8) && !(version->features & CURL_VERSION_SSL)) {
		set_error(error, error_size, "libcurl HTTPS support is required"); return NULL;
	}
#endif
	if (pthread_once(&host_info_once, collect_host_info)) {
		set_error(error, error_size, "cannot initialize host metadata"); return NULL;
	}
	FmReporter *r = calloc(1, sizeof(*r));
	if (!r) { set_error(error, error_size, "cannot allocate reporter"); return NULL; }
	r->config = config; r->capacity = (size_t)config->queue_capacity;
	r->queue = calloc(r->capacity, sizeof(*r->queue));
	if (!r->queue) { set_error(error, error_size, "cannot allocate report queue"); free(r); return NULL; }
	if (!transport_setup(r, error, error_size)) {
		transport_cleanup(r); free(r->queue); free(r); return NULL;
	}
	int status = pthread_mutex_init(&r->mutex, NULL); bool mutex_ready = !status, cond_ready = false;
	if (!status) { status = pthread_cond_init(&r->changed, NULL); cond_ready = !status; }
	if (!status) status = pthread_create(&r->worker, NULL, report_worker, r);
	if (status) {
		set_error(error, error_size, "cannot start reporter thread: %s", strerror(status));
		if (cond_ready) pthread_cond_destroy(&r->changed);
		if (mutex_ready) pthread_mutex_destroy(&r->mutex);
		transport_cleanup(r);
		free(r->queue); free(r); return NULL;
	}
	return r;
}

bool fm_reporter_submit(FmReporter *r, const FileMonitor *fm, const FileMonitorEvent *ev) {
	if (!r) return false;
	pthread_mutex_lock(&r->mutex);
	bool paused = r->paused; pthread_mutex_unlock(&r->mutex);
	if (paused) return false;
	char *payload = fm_event_json(fm, ev); pthread_mutex_lock(&r->mutex);
	if (r->paused) { pthread_mutex_unlock(&r->mutex); free(payload); return false; }
	if (!payload || r->stopping || r->count == r->capacity) {
		uint64_t dropped = ++r->dropped; pthread_mutex_unlock(&r->mutex); free(payload);
		if (dropped == 1 || (dropped & (dropped - 1)) == 0) fprintf(stderr, "fsmod: report queue unavailable/full; dropped=%" PRIu64 " (new events dropped)\n", dropped);
		return false;
	}
	r->queue[(r->head + r->count) % r->capacity] = payload; r->count++;
	pthread_cond_signal(&r->changed); pthread_mutex_unlock(&r->mutex); return true;
}

bool fm_reporter_pause(FmReporter *r, bool paused, bool discard_pending, unsigned timeout_seconds) {
	if (!r) return false;
	double deadline = monotonic_now() + timeout_seconds;
	pthread_mutex_lock(&r->mutex);
	if (r->stopping) { pthread_mutex_unlock(&r->mutex); return false; }
	r->paused = paused;
	if (paused) {
		r->cancel_generation++;
		if (discard_pending) {
			r->discard_active = true;
			for (size_t i = 0; i < r->count; i++) {
				size_t index = (r->head + i) % r->capacity;
				free(r->queue[index]); r->queue[index] = NULL;
			}
			r->dropped += r->count; r->head = r->count = 0;
		}
	}
	pthread_cond_broadcast(&r->changed);
	/* Darwin condition variables use wall-clock deadlines. Poll briefly against
	 * CLOCK_MONOTONIC instead so changing the device clock cannot extend this
	 * acknowledgement timeout. Only the worker touches transport objects. */
	while (paused && r->in_flight && monotonic_now() < deadline) {
		pthread_mutex_unlock(&r->mutex); poll(NULL, 0, 10); pthread_mutex_lock(&r->mutex);
	}
	bool ready = !paused || !r->in_flight;
	pthread_mutex_unlock(&r->mutex); return ready;
}

void fm_reporter_status(FmReporter *r, FmReporterStatus *status) {
	if (!status) return;
	*status = (FmReporterStatus){0};
	if (!r) return;
	pthread_mutex_lock(&r->mutex);
	status->queued = r->count; status->in_flight = r->in_flight; status->paused = r->paused;
	status->delivered = r->delivered; status->failed = r->failed; status->dropped = r->dropped;
	pthread_mutex_unlock(&r->mutex);
}

void fm_reporter_stop(FmReporter *r) {
	if (!r) return;
	pthread_mutex_lock(&r->mutex);
	r->stopping = true; r->stop_deadline = monotonic_now() + r->config->shutdown_timeout_seconds;
	pthread_cond_broadcast(&r->changed); pthread_mutex_unlock(&r->mutex); pthread_join(r->worker, NULL);
	for (size_t i = 0; i < r->count; i++) free(r->queue[(r->head + i) % r->capacity]);
	r->dropped += r->count;
	fprintf(stderr, "fsmod: report stopped: delivered=%" PRIu64 " failed=%" PRIu64 " dropped=%" PRIu64 "; undelivered events are not persisted\n", r->delivered, r->failed, r->dropped);
	transport_cleanup(r);
	pthread_cond_destroy(&r->changed); pthread_mutex_destroy(&r->mutex); free(r->queue); free(r);
}
#endif
