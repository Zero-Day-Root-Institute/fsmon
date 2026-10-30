#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "report.h"

/* Keep reporter tests independent from platform-specific utility code. */
const char *fm_typestr(int type) {
	switch (type) {
	case FSE_CREATE_FILE: return "FSE_CREATE_FILE";
	case FSE_RENAME: return "FSE_RENAME";
	case FSE_CLONE: return "FSE_CLONE";
	default: return "FSE_UNKNOWN";
	}
}

static double now_seconds(void) {
	struct timespec ts; assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static void pause_ms(long ms) {
	struct timespec pause = {ms / 1000, (ms % 1000) * 1000000};
	while (nanosleep(&pause, &pause) && errno == EINTR) {}
}

static void assert_envelope(const char *json);

#define MAX_REQUESTS 16
#define MAX_HEADERS 16384
#define MAX_BODY 65536

typedef struct {
	char path[256];
	char authorization[256];
	char content_type[128];
	char *body;
} HttpRequest;

typedef struct {
	int listener;
	pthread_t thread;
	pthread_mutex_t mutex;
	bool stopping;
	bool hang;
	int first_status;
	int next_status;
	size_t request_count;
	size_t connections;
	HttpRequest requests[MAX_REQUESTS];
	char url[128];
} HttpServer;

static bool server_stopping(HttpServer *server) {
	assert(pthread_mutex_lock(&server->mutex) == 0);
	bool stopping = server->stopping;
	assert(pthread_mutex_unlock(&server->mutex) == 0);
	return stopping;
}

static bool server_hanging(HttpServer *server) {
	assert(pthread_mutex_lock(&server->mutex) == 0);
	bool hang = server->hang;
	assert(pthread_mutex_unlock(&server->mutex) == 0);
	return hang;
}

static void nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0); assert(flags >= 0);
	assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

/* All server I/O is bounded so a failed reporter cannot hang the test suite. */
static bool ready(HttpServer *server, int fd, short events, double deadline) {
	while (!server_stopping(server) && now_seconds() < deadline) {
		struct pollfd pending = {fd, events, 0};
		int count = poll(&pending, 1, 50);
		if (count > 0) return (pending.revents & (events | POLLHUP | POLLERR)) != 0;
		if (count < 0 && errno != EINTR) return false;
	}
	return false;
}

static bool send_response(HttpServer *server, int fd, int status) {
	const char *body = status == 204 ? "" : "{\"code\":200,\"message\":\"ok\"}";
	char response[512];
	int size = snprintf(response, sizeof(response),
		"HTTP/1.1 %d Test\r\nContent-Type: application/json\r\n"
		"Content-Length: %zu\r\nConnection: keep-alive\r\n%s\r\n%s", status,
		strlen(body), status == 302 ? "Location: /unexpected\r\n" : "", body);
	assert(size > 0 && (size_t)size < sizeof(response));
	size_t sent = 0; double deadline = now_seconds() + 3.0;
	while (sent < (size_t)size && ready(server, fd, POLLOUT, deadline)) {
		ssize_t count = send(fd, response + sent, (size_t)size - sent, 0);
		if (count > 0) sent += (size_t)count;
		else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
	}
	return sent == (size_t)size;
}

static bool header_value(const char *headers, const char *name, char *out, size_t capacity) {
	size_t length = strlen(name);
	const char *line = strstr(headers, "\r\n"); assert(line);
	while (*(line += 2)) {
		const char *end = strstr(line, "\r\n"); assert(end);
		if (end == line) break;
		if (!strncasecmp(line, name, length) && line[length] == ':') {
			const char *value = line + length + 1;
			while (value < end && (*value == ' ' || *value == '\t')) value++;
			assert((size_t)(end - value) < capacity);
			memcpy(out, value, (size_t)(end - value)); out[end - value] = 0;
			return true;
		}
		line = end;
	}
	return false;
}

static bool receive_request(HttpServer *server, int fd, HttpRequest *request) {
	char wire[MAX_HEADERS + MAX_BODY + 1]; size_t used = 0, header_size = 0, body_size = 0;
	double deadline = now_seconds() + 6.0;
	while (ready(server, fd, POLLIN, deadline)) {
		ssize_t count = recv(fd, wire + used, sizeof(wire) - used - 1, 0);
		if (count == 0) return false;
		if (count < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
			return false;
		}
		used += (size_t)count; wire[used] = 0;
		if (!header_size) {
			char *end = strstr(wire, "\r\n\r\n");
			if (!end) { assert(used < MAX_HEADERS); continue; }
			header_size = (size_t)(end - wire) + 4; assert(header_size <= MAX_HEADERS);
			char method[16], version[16], length[32] = {0};
			assert(sscanf(wire, "%15s %255s %15s", method, request->path, version) == 3);
			assert(!strcmp(method, "POST") && !strcmp(version, "HTTP/1.1"));
			assert(header_value(wire, "Content-Length", length, sizeof(length)));
			char *after; errno = 0; unsigned long long parsed = strtoull(length, &after, 10);
			assert(!errno && *length && !*after && parsed <= MAX_BODY); body_size = (size_t)parsed;
			header_value(wire, "Authorization", request->authorization, sizeof(request->authorization));
			assert(header_value(wire, "Content-Type", request->content_type, sizeof(request->content_type)));
			assert(!strcmp(request->content_type, "application/json"));
			/* The reporter suppresses Expect, so no interim response is required. */
			char expect[64]; assert(!header_value(wire, "Expect", expect, sizeof(expect)) || !*expect);
		}
		if (used >= header_size + body_size) {
			assert(used == header_size + body_size);
			request->body = malloc(body_size + 1); assert(request->body);
			memcpy(request->body, wire + header_size, body_size); request->body[body_size] = 0;
			assert_envelope(request->body); return true;
		}
		assert(used < sizeof(wire) - 1);
	}
	return false;
}

static void *server_worker(void *arg) {
	HttpServer *server = arg;
	while (!server_stopping(server)) {
		if (!ready(server, server->listener, POLLIN, now_seconds() + 1.0)) continue;
		int client = accept(server->listener, NULL, NULL);
		if (client < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR); continue; }
		nonblocking(client);
		assert(pthread_mutex_lock(&server->mutex) == 0); server->connections++;
		assert(pthread_mutex_unlock(&server->mutex) == 0);
		while (!server_stopping(server)) {
			HttpRequest request = {0};
			if (!receive_request(server, client, &request)) break;
			assert(pthread_mutex_lock(&server->mutex) == 0);
			size_t index = server->request_count; assert(index < MAX_REQUESTS);
			server->requests[index] = request; server->request_count++;
			assert(pthread_mutex_unlock(&server->mutex) == 0);
			if (server_hanging(server)) {
				/* Hold the HTTP response until timeout, cancellation, or server stop. */
				while (server_hanging(server) && !server_stopping(server)) pause_ms(10);
				if (server_stopping(server)) break;
			}
			if (!send_response(server, client, index ? server->next_status : server->first_status)) break;
		}
		close(client);
	}
	return NULL;
}

static void server_start(HttpServer *server, int first_status, int next_status, bool hang) {
	memset(server, 0, sizeof(*server)); server->first_status = first_status; server->next_status = next_status; server->hang = hang;
	assert(pthread_mutex_init(&server->mutex, NULL) == 0);
	server->listener = socket(AF_INET, SOCK_STREAM, 0); assert(server->listener >= 0);
	int enabled = 1; assert(setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0);
	struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(bind(server->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	socklen_t length = sizeof(address); assert(getsockname(server->listener, (struct sockaddr *)&address, &length) == 0);
	assert(listen(server->listener, 8) == 0); nonblocking(server->listener);
	snprintf(server->url, sizeof(server->url), "http://127.0.0.1:%u/events", (unsigned)ntohs(address.sin_port));
	assert(pthread_create(&server->thread, NULL, server_worker, server) == 0);
}

static void wait_requests(HttpServer *server, size_t expected) {
	double deadline = now_seconds() + 3.0; size_t count;
	do {
		assert(pthread_mutex_lock(&server->mutex) == 0); count = server->request_count;
		assert(pthread_mutex_unlock(&server->mutex) == 0);
		if (count >= expected) return;
		pause_ms(10);
	} while (now_seconds() < deadline);
	assert(count >= expected);
}

static void server_stop(HttpServer *server) {
	assert(pthread_mutex_lock(&server->mutex) == 0); server->stopping = true;
	assert(pthread_mutex_unlock(&server->mutex) == 0);
	assert(pthread_join(server->thread, NULL) == 0); close(server->listener);
	assert(pthread_mutex_destroy(&server->mutex) == 0);
}

static void server_free(HttpServer *server) {
	for (size_t i = 0; i < server->request_count; i++) free(server->requests[i].body);
}

static FmConfig config_for_test(HttpServer *server) {
	FmConfig config = {0};
	config.report_url = server ? server->url : "http://127.0.0.1:1/events";
	config.report_timeout_seconds = 2; config.report_retries = 1; config.queue_capacity = 2;
	config.shutdown_timeout_seconds = 4; return config;
}

static void assert_envelope(const char *json) {
	/* Exact prefix rules out extra root keys and fixes data as an object. */
	const char *prefix = "{\"source\":\"fsmod\",\"data\":{";
	assert(!strncmp(json, prefix, strlen(prefix)));
	const char *p = json + strlen(prefix);
	int depth = 1; bool quoted = false, escaped = false;
	for (; *p; p++) {
		assert((unsigned char)*p >= 0x20);
		if (quoted) {
			if (escaped) escaped = false;
			else if (*p == 92) escaped = true;
			else if (*p == '"') quoted = false;
		} else if (*p == '"') quoted = true;
		else if (*p == '{') depth++;
		else if (*p == '}' && --depth == 0) break;
	}
	assert(*p == '}' && !quoted && !escaped);
	assert(p[1] == '}' && p[2] == 0);
}

/* Return the exact serialized nullable-string token, including its quotes.
 * Host metadata lives inside data and must never be numeric UIDs. */
static const char *metadata_token(const char *json, const char *key, size_t *length) {
	char field[80];
	assert(snprintf(field, sizeof(field), "\"%s\":", key) > 0);
	const char *value = strstr(json, field); assert(value);
	assert(!strstr(value + strlen(field), field));
	value += strlen(field);
	const char *end = value;
	if (*end == '"') {
		end++;
		while (*end && *end != '"') {
			assert((unsigned char)*end >= 0x20);
			if (*end == 92) {
				end++; assert(*end && strchr("\"\\/bfnrtu", *end));
				if (*end == 'u') {
					for (int i = 0; i < 4; i++) { end++; assert(*end && strchr("0123456789abcdefABCDEF", *end)); }
				}
			}
			end++;
		}
		assert(*end == '"'); end++;
	} else {
		assert(!strncmp(end, "null", 4)); end += 4;
	}
	assert(*end == ',' || *end == '}');
	*length = (size_t)(end - value);
	return value;
}

static void assert_host_metadata(const char *json) {
	assert_envelope(json);
	static const char *fields[] = {"uuid", "os_name", "os_version", "kernel_version"};
	size_t length;
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		const char *value = metadata_token(json, fields[i], &length);
		assert(value[0] != '"' || length > 2);
	}
	const char *id = metadata_token(json, "uuid", &length);
	if (*id == '"') {
#if __APPLE__
		assert(length == 38);
		bool nonzero = false;
		for (size_t i = 0; i < 36; i++) {
			if (i == 8 || i == 13 || i == 18 || i == 23) assert(id[i + 1] == '-');
			else {
				assert(strchr("0123456789ABCDEF", id[i + 1]));
				if (id[i + 1] != '0') nonzero = true;
			}
		}
		assert(nonzero);
#else
		assert(length == 34);
		for (size_t i = 1; i <= 32; i++) assert(strchr("0123456789abcdef", id[i]));
		assert(strspn(id + 1, "0") != 32);
#endif
	}
	struct utsname host;
	if (!uname(&host)) {
		const char *name = metadata_token(json, "os_name", &length);
		assert(*name == '"' && length > 2);
		const char *kernel = metadata_token(json, "kernel_version", &length);
		assert(*kernel == '"' && length == strlen(host.release) + 2);
		assert(!memcmp(kernel + 1, host.release, length - 2));
#if __APPLE__
		assert(strstr(json, "\"os_name\":\"macOS\""));
#endif
	}
}

static void test_host_metadata(void) {
	/* Unknown metadata is represented as JSON null, never as an empty string. */
	size_t null_size;
	const char *null_value = metadata_token("{\"uuid\":null}", "uuid", &null_size);
	assert(null_size == 4 && !memcmp(null_value, "null", 4));
	FileMonitor fm = {0}; fm.backend.name = "test";
	FileMonitorEvent ev = {0}; ev.file = "/metadata/first"; ev.type = FSE_CREATE_FILE;
	/* Deliberately differ from the monitor's account: uid belongs to the event. */
	ev.uid = getuid() == 424242 ? 424243 : 424242; ev.gid = 424244;
	char *first = fm_event_json(&fm, &ev); assert(first); assert_host_metadata(first);
	char identity[80];
	snprintf(identity, sizeof(identity), "\"uid\":%u,\"gid\":%d", (unsigned)ev.uid, ev.gid);
	assert(strstr(first, identity));
	assert(!strstr(first, "\"uid\":\""));
	static const char *fields[] = {"uuid", "os_name", "os_version", "kernel_version"};
	for (int event = 0; event < 3; event++) {
		ev.file = "/metadata/next"; ev.uid = event; ev.gid = event + 100;
		char *next = fm_event_json(&fm, &ev); assert(next); assert_host_metadata(next);
		snprintf(identity, sizeof(identity), "\"uid\":%u,\"gid\":%d", (unsigned)ev.uid, ev.gid);
		assert(strstr(next, identity));
		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			size_t before_size, after_size;
			const char *before = metadata_token(first, fields[i], &before_size);
			const char *after = metadata_token(next, fields[i], &after_size);
			assert(before_size == after_size && !memcmp(before, after, before_size));
		}
		free(next);
	}
	free(first);
}

static void test_json(void) {
	FileMonitor fm = {0}; fm.backend.name = "devfsev"; fm.fileonly = true;
	char filename[] = {'/', 'x', '/', (char)0xe4, (char)0xb8, (char)0xad, '"', 92, 10, 9, 1, 0};
	FileMonitorEvent ev = {0};
	ev.file = filename; ev.newfile = "/new"; ev.proc = "process";
	ev.type = FSE_RENAME; ev.uid = 0; ev.gid = 0; ev.inode = UINT64_MAX; ev.tstamp = UINT64_MAX;
	char *json = fm_event_json(&fm, &ev); assert(json);
	assert_envelope(json);
	assert(strstr(json, "/x/")); assert(strstr(json, "\"newfile\":\"/new\""));
	assert(strstr(json, "\"uid\":0,\"gid\":0"));
	assert(strstr(json, "\"inode\":18446744073709551615"));
	assert(strstr(json, "\"kernel_timestamp\":18446744073709551615"));
	char utf8[] = {(char)0xe4, (char)0xb8, (char)0xad, 0}; assert(strstr(json, utf8));
	char escaped_quote[] = {92, '"', 0}, escaped_slash[] = {92, 92, 0};
	char escaped_nl[] = {92, 'u', '0', '0', '0', 'a', 0};
	char escaped_tab[] = {92, 'u', '0', '0', '0', '9', 0};
	char escaped_one[] = {92, 'u', '0', '0', '0', '1', 0};
	assert(strstr(json, escaped_quote) && strstr(json, escaped_slash));
	assert(strstr(json, escaped_nl) && strstr(json, escaped_tab) && strstr(json, escaped_one));
	assert(!strchr(json, 10) && !strchr(json, 9));
	char *observed = strstr(json, "\"observed_at\":\""); assert(observed);
	observed += strlen("\"observed_at\":\"");
	assert(observed[4] == '-' && observed[10] == 'T' && observed[23] == 'Z');
	assert(ev.file == filename && filename[0] == '/'); free(json);
	/* Full-width UIDs remain unsigned JSON numbers without changing fields. */
	ev.uid = UINT32_MAX; ev.uid_available = true;
	ev.gid = UINT32_MAX; ev.gid_available = true;
	json = fm_event_json(&fm, &ev); assert(json);
	assert_envelope(json);
	assert(strstr(json, "\"uid\":4294967295,\"gid\":4294967295"));
	assert(!strstr(json, "\"uid\":-1") && !strstr(json, "\"uid\":\""));
	assert(!strstr(json, "\"gid\":-1") && !strstr(json, "\"gid\":\""));
	free(json);
	/* Native clone events report the source and destination as distinct paths. */
	ev.type = FSE_CLONE; ev.file = "/bin/launchctl"; ev.newfile = "/sbin/test";
	json = fm_event_json(&fm, &ev); assert(json);
	assert_envelope(json);
	assert(strstr(json, "\"type\":\"FSE_CLONE\""));
	assert(strstr(json, "\"filename\":\"/bin/launchctl\""));
	assert(strstr(json, "\"newfile\":\"/sbin/test\""));
	assert(strstr(json, "\"inode\":18446744073709551615")); free(json);
	ev.file = NULL; ev.newfile = NULL; ev.proc = NULL;
	json = fm_event_json(&fm, &ev); assert(json);
	assert_envelope(json);
	assert(strstr(json, "\"filename\":null") && strstr(json, "\"newfile\":null")); free(json);
	char invalid[] = {'/', (char)0xff, (char)0xe2, 0}; ev.file = invalid;
	json = fm_event_json(&fm, &ev); assert(json);
	assert_envelope(json);
	char ff[] = {92, 'u', '0', '0', 'f', 'f', 0}; assert(strstr(json, ff)); free(json);
	assert(!fm_event_json(NULL, &ev)); assert(!fm_event_json(&fm, NULL));
}

static void test_delivery(void) {
	HttpServer server; server_start(&server, 200, 200, false);
	char error[256]; FmConfig config = config_for_test(&server); config.report_token = "test-secret";
	/* A rejected JSON acknowledgement would retry and break the request count. */
	FileMonitor fm = {0}; fm.backend.name = "test"; fm.fileonly = true;
	char path[] = "/original/first"; FileMonitorEvent ev = {0}; ev.file = path; ev.type = FSE_CREATE_FILE;
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	FileMonitorEvent internal = {0}; internal.pid = (int)getpid(); assert(fm_reporter_is_internal(reporter, &internal));
	internal.pid = 0; internal.ppid = (int)getpid(); assert(fm_reporter_is_internal(reporter, &internal));
	internal.ppid = 0; internal.file = "/some/application/file"; assert(!fm_reporter_is_internal(reporter, &internal));
	assert(!fm_reporter_is_internal(NULL, &internal) && !fm_reporter_is_internal(reporter, NULL));
	assert(fm_reporter_submit(reporter, &fm, &ev)); path[1] = 'X';
	ev.file = "/original/second"; assert(fm_reporter_submit(reporter, &fm, &ev));
	fm_reporter_stop(reporter); server_stop(&server);
	assert(server.request_count == 2);
#if defined(FSMOD_CFNETWORK_TEST)
	/* CFNetwork owns connection pooling and may choose a fresh socket per stream. */
	assert(server.connections >= 1 && server.connections <= server.request_count);
#else
	assert(server.connections == 1);
#endif
	char *body = server.requests[0].body, *second = server.requests[1].body;
	for (size_t i = 0; i < server.request_count; i++) {
		assert(!strcmp(server.requests[i].path, "/events"));
		assert(!strcmp(server.requests[i].authorization, "Bearer test-secret"));
	}
	assert(strstr(body, "\"filename\":\"/original/first\""));
	assert(strstr(second, "\"filename\":\"/original/second\""));
	assert(!strstr(body, "/original/second") && !strstr(second, "/original/first"));
	assert(!strstr(body, "/Xriginal") && !strstr(second, "/Xriginal"));
	assert_host_metadata(body); assert_host_metadata(second); server_free(&server);
}

static void test_retry(int first_status, int next_status) {
	HttpServer server; server_start(&server, first_status, next_status, false);
	char error[256]; FmConfig config = config_for_test(&server);
	FileMonitor fm = {0}; fm.backend.name = "test"; FileMonitorEvent ev = {0}; ev.file = "/retry";
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	assert(fm_reporter_submit(reporter, &fm, &ev)); fm_reporter_stop(reporter); server_stop(&server);
	assert(server.request_count == 2);
	char *body = server.requests[0].body, *again = server.requests[1].body;
	assert(strstr(body, "\"filename\":\"/retry\""));
	/* A retry resends the same queued event, including its observation time. */
	assert(!strcmp(body, again));
	for (size_t i = 0; i < server.request_count; i++) {
		assert(!strcmp(server.requests[i].path, "/events"));
		assert(!server.requests[i].authorization[0]);
	}
	server_free(&server);
}

static void test_response_timeout(void) {
	HttpServer server; server_start(&server, 200, 200, true);
	char error[256]; FmConfig config = config_for_test(&server);
	config.report_timeout_seconds = 1; config.report_retries = 0;
	FileMonitor fm = {0}; fm.backend.name = "test"; FileMonitorEvent ev = {0}; ev.file = "/timeout";
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	double start = now_seconds(); assert(fm_reporter_submit(reporter, &fm, &ev)); wait_requests(&server, 1);
	fm_reporter_stop(reporter); double elapsed = now_seconds() - start;
	assert(elapsed >= 0.7 && elapsed < 2.5);
	server_stop(&server); assert(server.request_count == 1); server_free(&server);
}

static void test_bounded_stop(void) {
	HttpServer server; server_start(&server, 200, 200, true);
	char error[256]; FmConfig config = config_for_test(&server);
	config.report_timeout_seconds = 30; config.shutdown_timeout_seconds = 0;
	FileMonitor fm = {0}; fm.backend.name = "test"; FileMonitorEvent ev = {0}; ev.file = "/bounded";
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	assert(fm_reporter_submit(reporter, &fm, &ev)); wait_requests(&server, 1);
	assert(fm_reporter_submit(reporter, &fm, &ev)); assert(fm_reporter_submit(reporter, &fm, &ev));
	double start = now_seconds(); assert(!fm_reporter_submit(reporter, &fm, &ev)); assert(now_seconds() - start < 0.5);
	start = now_seconds(); fm_reporter_stop(reporter); assert(now_seconds() - start < 1.0);
	server_stop(&server); assert(server.request_count == 1); server_free(&server);
}

static void wait_delivered(FmReporter *reporter, uint64_t expected) {
	double deadline = now_seconds() + 3.0; FmReporterStatus status;
	do {
		fm_reporter_status(reporter, &status);
		if (status.delivered >= expected) return;
		pause_ms(10);
	} while (now_seconds() < deadline);
	assert(status.delivered >= expected);
}

static void test_pause_retain(void) {
	HttpServer server; server_start(&server, 200, 200, true);
	char error[256]; FmConfig config = config_for_test(&server);
	config.report_timeout_seconds = 30; config.queue_capacity = 4;
	FileMonitor fm = {0}; fm.backend.name = "test";
	FileMonitorEvent ev = {0}; ev.file = "/interrupted";
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	assert(fm_reporter_submit(reporter, &fm, &ev)); wait_requests(&server, 1);
	ev.file = "/queued/first"; assert(fm_reporter_submit(reporter, &fm, &ev));
	ev.file = "/queued/second"; assert(fm_reporter_submit(reporter, &fm, &ev));
	/* A zero deadline must report that the active transfer is not quiescent,
	 * while immediately closing the submission gate. Repeating pause waits. */
	assert(!fm_reporter_pause(reporter, true, false, 0));
	FmReporterStatus status; fm_reporter_status(reporter, &status); assert(status.paused);
	double start = now_seconds(); assert(fm_reporter_pause(reporter, true, false, 2));
	assert(now_seconds() - start < 2.0);
	fm_reporter_status(reporter, &status);
	assert(status.paused && !status.in_flight && status.queued == 3);
	assert(status.failed == 0 && status.delivered == 0 && status.dropped == 0);
	ev.file = "/suppressed"; assert(!fm_reporter_submit(reporter, &fm, &ev));
	pause_ms(150);
	assert(pthread_mutex_lock(&server.mutex) == 0);
	assert(server.request_count == 1); server.hang = false;
	assert(pthread_mutex_unlock(&server.mutex) == 0);
	assert(fm_reporter_pause(reporter, false, false, 0)); wait_delivered(reporter, 3);
	fm_reporter_status(reporter, &status);
	assert(!status.paused && !status.in_flight && status.queued == 0);
	assert(status.failed == 0 && status.delivered == 3 && status.dropped == 0);
	fm_reporter_stop(reporter); server_stop(&server);
	assert(server.request_count == 4);
	assert(!strcmp(server.requests[0].body, server.requests[1].body));
	assert(strstr(server.requests[2].body, "\"filename\":\"/queued/first\""));
	assert(strstr(server.requests[3].body, "\"filename\":\"/queued/second\""));
	server_free(&server);
}

static void test_pause_discard_and_stop(void) {
	HttpServer server; server_start(&server, 200, 200, true);
	char error[256]; FmConfig config = config_for_test(&server);
	config.report_timeout_seconds = 30; config.shutdown_timeout_seconds = 30;
	FileMonitor fm = {0}; fm.backend.name = "test";
	FileMonitorEvent ev = {0}; ev.file = "/discard";
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	assert(fm_reporter_submit(reporter, &fm, &ev)); wait_requests(&server, 1);
	assert(fm_reporter_submit(reporter, &fm, &ev)); assert(fm_reporter_submit(reporter, &fm, &ev));
	assert(fm_reporter_pause(reporter, true, true, 2));
	FmReporterStatus status; fm_reporter_status(reporter, &status);
	assert(status.paused && !status.in_flight && status.queued == 0);
	assert(status.failed == 0 && status.delivered == 0 && status.dropped == 3);
	double start = now_seconds(); fm_reporter_stop(reporter); assert(now_seconds() - start < 1.0);
	server_stop(&server); assert(server.request_count == 1); server_free(&server);
	/* Retained paused work is discarded on stop without sending or waiting for
	 * the normal shutdown drain deadline. */
	server_start(&server, 200, 200, true); config = config_for_test(&server);
	config.report_timeout_seconds = 30; config.shutdown_timeout_seconds = 30;
	reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	assert(fm_reporter_submit(reporter, &fm, &ev)); wait_requests(&server, 1);
	assert(fm_reporter_submit(reporter, &fm, &ev));
	assert(fm_reporter_pause(reporter, true, false, 2));
	fm_reporter_status(reporter, &status); assert(status.queued == 2 && !status.in_flight);
	start = now_seconds(); fm_reporter_stop(reporter); assert(now_seconds() - start < 1.0);
	server_stop(&server); assert(server.request_count == 1); server_free(&server);
}

static void test_pause_full_queue(void) {
	HttpServer server; server_start(&server, 200, 200, true);
	char error[256]; FmConfig config = config_for_test(&server); config.report_timeout_seconds = 30;
	FileMonitor fm = {0}; fm.backend.name = "test"; FileMonitorEvent ev = {0}; ev.file = "/full";
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	assert(fm_reporter_submit(reporter, &fm, &ev)); wait_requests(&server, 1);
	assert(fm_reporter_submit(reporter, &fm, &ev)); assert(fm_reporter_submit(reporter, &fm, &ev));
	assert(fm_reporter_pause(reporter, true, false, 2));
	FmReporterStatus status; fm_reporter_status(reporter, &status);
	/* An interrupted active item cannot overflow an already full queue. */
	assert(status.paused && !status.in_flight && status.queued == 2 && status.dropped == 1);
	assert(status.failed == 0 && status.delivered == 0);
	fm_reporter_stop(reporter); server_stop(&server); assert(server.request_count == 1); server_free(&server);
}

static void test_pause_retry_wait(void) {
	HttpServer server; server_start(&server, 503, 200, false);
	char error[256]; FmConfig config = config_for_test(&server); config.retry_delay_seconds = 30;
	FileMonitor fm = {0}; fm.backend.name = "test"; FileMonitorEvent ev = {0}; ev.file = "/retry-wait";
	FmReporter *reporter = fm_reporter_start(&config, error, sizeof(error)); assert(reporter);
	assert(fm_reporter_submit(reporter, &fm, &ev)); wait_requests(&server, 1); pause_ms(100);
	double start = now_seconds(); assert(fm_reporter_pause(reporter, true, false, 2));
	assert(now_seconds() - start < 2.0);
	FmReporterStatus status; fm_reporter_status(reporter, &status);
	assert(status.queued == 1 && !status.in_flight && status.failed == 0);
	assert(fm_reporter_pause(reporter, false, false, 0)); wait_delivered(reporter, 1);
	fm_reporter_stop(reporter); server_stop(&server); assert(server.request_count == 2);
	assert(!strcmp(server.requests[0].body, server.requests[1].body)); server_free(&server);
}

int main(void) {
	signal(SIGPIPE, SIG_IGN); test_json(); test_host_metadata();
	const char *path = getenv("PATH"); char *original_path = path ? strdup(path) : NULL;
	/* No executable can be found: all requests must run inside this process. */
	assert(setenv("PATH", "", 1) == 0);
	/* Keep inherited proxy settings from redirecting loopback test traffic. */
	assert(setenv("no_proxy", "127.0.0.1", 1) == 0);
	test_delivery(); test_retry(503, 200); test_retry(302, 302);
	test_retry(201, 201); test_retry(204, 204); test_response_timeout(); test_bounded_stop();
	test_pause_retain(); test_pause_discard_and_stop(); test_pause_full_queue(); test_pause_retry_wait();
	FmConfig config = config_for_test(NULL); char error[256]; config.report_url = "file:///etc/passwd";
	assert(!fm_reporter_start(&config, error, sizeof(error)) && *error);
	config = config_for_test(NULL); config.report_url = "ftp://127.0.0.1/events";
	assert(!fm_reporter_start(&config, error, sizeof(error)) && *error);
	config = config_for_test(NULL); config.report_token = "bad\r\nheader";
	assert(!fm_reporter_start(&config, error, sizeof(error)) && *error);
	if (original_path) { assert(setenv("PATH", original_path, 1) == 0); free(original_path); } else unsetenv("PATH");
	puts("report tests passed"); return 0;
}
