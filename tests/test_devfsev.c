/* Exercise the actual /dev/fsevents wire layout without a kernel device. */
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* A stream read may finish anywhere, including in a TLV header or scalar. */
static size_t stream_reads;
static size_t stream_read_limit = 3;
static ssize_t fragmented_read(int fd, void *buffer, size_t length) {
	stream_reads++;
	if (stream_read_limit && length > stream_read_limit) length = stream_read_limit;
	return read(fd, buffer, length);
}

#define read fragmented_read
#include "../backend/devfsev.c"
#undef read

const char *get_proc_name(int pid, int *ppid) {
	assert(pid == 1234);
	*ppid = 4321;
	return "wire-test";
}

typedef struct {
	uint8_t bytes[1024];
	size_t length;
} Record;

static void append(Record *record, const void *value, size_t length) {
	assert(length <= sizeof(record->bytes) - record->length);
	if (length) memcpy(record->bytes + record->length, value, length);
	record->length += length;
}

static Record begin_record(int32_t type) {
	Record record = {0};
	int32_t pid = 1234;
	append(&record, &type, sizeof(type));
	append(&record, &pid, sizeof(pid));
	return record;
}

static void argument(Record *record, uint16_t type, const void *value, uint16_t length) {
	append(record, &type, sizeof(type));
	append(record, &length, sizeof(length));
	append(record, value, length);
}

static void scalar32(Record *record, uint16_t type, uint32_t value) {
	argument(record, type, &value, sizeof(value));
}

static void scalar64(Record *record, uint16_t type, uint64_t value) {
	argument(record, type, &value, sizeof(value));
}

static void path(Record *record, uint16_t type, const char *value) {
	argument(record, type, value, (uint16_t)(strlen(value) + 1));
}

static void done(Record *record) {
	uint16_t marker = FSE_ARG_DONE;
	append(record, &marker, sizeof(marker));
}

static const uint64_t kernel_time = UINT64_C(0xfedcba9876543210);

static void metadata(Record *record, bool destination) {
	scalar32(record, FSE_ARG_DEV, (uint32_t)makedev(destination ? 4 : 1, destination ? 9 : 3));
	scalar64(record, FSE_ARG_INO, destination ? 8 : UINT64_MAX);
	scalar32(record, FSE_ARG_MODE, destination ? 040755 : 0100644);
	scalar32(record, FSE_ARG_UID, destination ? 502 : 501);
	scalar32(record, FSE_ARG_GID, destination ? 82 : 502);
}

static Record create_record(uint64_t timestamp, bool include_timestamp) {
	Record record = begin_record(FSE_CREATE_FILE | (FSE_COMBINED_EVENTS << FSE_FLAG_SHIFT));
	path(&record, FSE_ARG_STRING, "/private/var/mobile/probe.txt");
	metadata(&record, false);
	if (include_timestamp) scalar64(&record, FSE_ARG_INT64, timestamp);
	done(&record);
	return record;
}

static void check_source(const FileMonitorEvent *event) {
	assert(event->pid == 1234);
	assert(event->inode == UINT64_MAX);
	assert(event->dev_major == 1 && event->dev_minor == 3);
	assert(event->mode == 0100644);
	assert(event->uid == 501 && event->gid == 502);
	assert(event->uid_available && event->gid_available);
}

static void test_create_and_timestamps(void) {
	Record record = create_record(kernel_time, true);
	FileMonitorEvent event;
	assert(parse_record(record.bytes, record.length, &event) == (int)record.length);
	check_source(&event);
	assert(event.type == FSE_CREATE_FILE);
	assert(!strcmp(event.file, "/private/var/mobile/probe.txt"));
	assert(!event.newfile);
	assert(event.tstamp == kernel_time && event.tstamp > UINT64_C(9007199254740992));
	/* No partial record, even one missing only DONE's last byte, is accepted. */
	for (size_t length = 0; length < record.length; length++) {
		assert(parse_record(record.bytes, length, &event) == 0);
	}
	record = create_record(0, true);
	assert(parse_record(record.bytes, record.length, &event) == (int)record.length);
	assert(event.tstamp == 0);
	record = create_record(0, false);
	assert(parse_record(record.bytes, record.length, &event) == (int)record.length);
	assert(event.tstamp == 0);
	/* INT64 is not an inode argument; the last INT64 is XNU's timestamp. */
	record = begin_record(FSE_CREATE_FILE);
	path(&record, FSE_ARG_PATH, "/probe");
	scalar32(&record, FSE_ARG_INO, UINT32_MAX);
	scalar64(&record, FSE_ARG_INT64, 17);
	scalar64(&record, FSE_ARG_INT64, kernel_time);
	done(&record);
	assert(parse_record(record.bytes, record.length, &event) == (int)record.length);
	assert(event.inode == UINT32_MAX && event.tstamp == kernel_time);
}

static Record rename_record(int32_t type, bool destination_metadata) {
	Record record = begin_record(type);
	path(&record, FSE_ARG_STRING, "/source");
	metadata(&record, false);
	path(&record, FSE_ARG_STRING, "/destination");
	if (destination_metadata) metadata(&record, true);
	scalar64(&record, FSE_ARG_INT64, kernel_time);
	done(&record);
	return record;
}

static void test_two_path_events(void) {
	const int32_t types[] = {FSE_RENAME, FSE_EXCHANGE, FSE_CLONE};
	FileMonitorEvent event;
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		for (int destination_metadata = 0; destination_metadata <= 1; destination_metadata++) {
			Record record = rename_record(types[i], destination_metadata != 0);
			assert(parse_record(record.bytes, record.length, &event) == (int)record.length);
			check_source(&event);
			assert(event.type == types[i]);
			assert(!strcmp(event.file, "/source") && !strcmp(event.newfile, "/destination"));
			assert(event.tstamp == kernel_time);
			for (size_t length = 0; length < record.length; length++) {
				assert(parse_record(record.bytes, length, &event) == 0);
			}
		}
	}
}

static void finfo_uid(Record *record, bool destination, uint32_t uid) {
	/* Pack explicitly: a native C structure could insert ABI padding. */
	uint8_t packed[24];
	uint64_t inode = destination ? 8 : UINT64_MAX;
	uint32_t device = (uint32_t)makedev(destination ? 4 : 1, destination ? 9 : 3);
	uint32_t mode = destination ? 040755 : 0100644;
	uint32_t gid = destination ? 82 : 502;
	memcpy(packed, &inode, 8);
	memcpy(packed + 8, &device, 4);
	memcpy(packed + 12, &mode, 4);
	memcpy(packed + 16, &uid, 4);
	memcpy(packed + 20, &gid, 4);
	argument(record, FSE_ARG_FINFO, packed, sizeof(packed));
}

static void finfo(Record *record, bool destination) {
	finfo_uid(record, destination, destination ? 502 : 501);
}

static void test_uid_availability(void) {
	const uint32_t uids[] = {0, 501, UINT32_MAX};
	const int32_t pair_types[] = {FSE_RENAME, FSE_EXCHANGE, FSE_CLONE};
	FileMonitorEvent event = {0};
	Record absent = begin_record(FSE_CREATE_FILE);
	path(&absent, FSE_ARG_PATH, "/uid-absent");
	done(&absent);
	for (size_t i = 0; i < sizeof(uids) / sizeof(uids[0]); i++) {
		for (int packed = 0; packed <= 1; packed++) {
			Record present = begin_record(FSE_CREATE_FILE);
			path(&present, FSE_ARG_PATH, "/uid-present");
			if (packed) finfo_uid(&present, false, uids[i]);
			else scalar32(&present, FSE_ARG_UID, uids[i]);
			done(&present);
			assert(parse_record(present.bytes, present.length, &event) == (int)present.length);
			assert(event.uid_available && event.uid == uids[i]);
			assert(event.gid_available == (packed != 0));
			if (packed) assert(event.gid == 502);
			/* Reusing an event must clear both a nonzero UID and a true
			 * availability flag; an absent UID is not an observed root. */
			assert(parse_record(absent.bytes, absent.length, &event) == (int)absent.length);
			assert(!event.uid_available && event.uid == 0 && !event.gid_available && event.gid == 0);
			assert(parse_record(present.bytes, present.length, &event) == (int)present.length);
			assert(event.uid_available && event.uid == uids[i]);
			/* Separate records in the same input buffer have no shared UID. */
			Record combined = {0};
			append(&combined, present.bytes, present.length);
			append(&combined, absent.bytes, absent.length);
			append(&combined, present.bytes, present.length);
			int used = parse_record(combined.bytes, combined.length, &event);
			assert(used == (int)present.length && event.uid_available && event.uid == uids[i]);
			int next = parse_record(combined.bytes + used, combined.length - (size_t)used, &event);
			assert(next == (int)absent.length && !event.uid_available && event.uid == 0);
			used += next;
			assert(parse_record(combined.bytes + used, combined.length - (size_t)used, &event) == (int)present.length);
			assert(event.uid_available && event.uid == uids[i]);
		}
	}
	/* Destination metadata must never be attributed to the source path,
	 * whether the destination UID is root, mobile, or a full-width UID. */
	for (size_t type = 0; type < sizeof(pair_types) / sizeof(pair_types[0]); type++) {
		for (size_t i = 0; i < sizeof(uids) / sizeof(uids[0]); i++) {
			for (int packed = 0; packed <= 1; packed++) {
				Record pair = begin_record(pair_types[type]);
				path(&pair, FSE_ARG_PATH, "/source");
				path(&pair, FSE_ARG_PATH, "/destination");
				if (packed) finfo_uid(&pair, true, uids[i]);
				else scalar32(&pair, FSE_ARG_UID, uids[i]);
				done(&pair);
				assert(parse_record(pair.bytes, pair.length, &event) == (int)pair.length);
				assert(!event.uid_available && event.uid == 0);
				assert(!strcmp(event.file, "/source") && !strcmp(event.newfile, "/destination"));
			}
		}
	}
	/* Early/incomplete parses also reset stale attribution before returning. */
	event.uid = 501; event.uid_available = true;
	assert(parse_record(absent.bytes, 0, &event) == 0);
	assert(!event.uid_available && event.uid == 0);
}

static void test_gid_availability(void) {
	const uint32_t gids[] = {0, 82, UINT32_MAX};
	const int32_t pair_types[] = {FSE_RENAME, FSE_EXCHANGE, FSE_CLONE};
	FileMonitorEvent event = {0};
	Record absent = begin_record(FSE_CREATE_FILE);
	path(&absent, FSE_ARG_PATH, "/gid-absent");
	done(&absent);
	for (size_t i = 0; i < sizeof(gids) / sizeof(gids[0]); i++) {
		for (int packed = 0; packed <= 1; packed++) {
			Record present = begin_record(FSE_CREATE_FILE);
			path(&present, FSE_ARG_PATH, "/gid-present");
			if (packed) finfo_uid(&present, false, 501);
			else scalar32(&present, FSE_ARG_GID, gids[i]);
			if (packed) {
				/* Replace the packed GID while retaining the explicit UID. */
				uint32_t packed_gid = gids[i];
				memcpy(present.bytes + present.length - 4, &packed_gid, sizeof(packed_gid));
			}
			done(&present);
			assert(parse_record(present.bytes, present.length, &event) == (int)present.length);
			assert(event.gid_available && event.gid == gids[i]);
			/* A real GID zero is distinct from unavailable metadata. */
			assert(parse_record(absent.bytes, absent.length, &event) == (int)absent.length);
			assert(!event.gid_available && event.gid == 0);
			assert(parse_record(present.bytes, present.length, &event) == (int)present.length);
			assert(event.gid_available && event.gid == gids[i]);
		}
	}
	/* Source metadata wins for two-path events; destination GID must not leak. */
	for (size_t type = 0; type < sizeof(pair_types) / sizeof(pair_types[0]); type++) {
		for (size_t i = 0; i < sizeof(gids) / sizeof(gids[0]); i++) {
			Record pair = begin_record(pair_types[type]);
			path(&pair, FSE_ARG_PATH, "/source");
			scalar32(&pair, FSE_ARG_GID, 501);
			path(&pair, FSE_ARG_PATH, "/destination");
			scalar32(&pair, FSE_ARG_GID, gids[i]);
			done(&pair);
			assert(parse_record(pair.bytes, pair.length, &event) == (int)pair.length);
			assert(event.gid_available && event.gid == 501);
			assert(!strcmp(event.file, "/source") && !strcmp(event.newfile, "/destination"));
		}
	}
	/* Early/incomplete parses clear stale GID attribution independently. */
	event.gid = 501; event.gid_available = true;
	assert(parse_record(absent.bytes, 0, &event) == 0);
	assert(!event.gid_available && event.gid == 0);
}

static void test_compact_finfo_and_extensions(void) {
	const int32_t types[] = {FSE_RENAME, FSE_CLONE};
	const uint8_t extension[] = {1, 0, 2, 0, 3};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		Record record = begin_record(types[i]);
		FileMonitorEvent event;
		path(&record, FSE_ARG_PATH, "/source");
		finfo(&record, false);
		argument(&record, 0x4321, extension, sizeof(extension));
		path(&record, FSE_ARG_PATH, "/destination");
		finfo(&record, true);
		scalar64(&record, FSE_ARG_INT64, kernel_time);
		done(&record);
		assert(parse_record(record.bytes, record.length, &event) == (int)record.length);
		check_source(&event);
		assert(event.type == types[i]);
		assert(!strcmp(event.file, "/source") && !strcmp(event.newfile, "/destination"));
		assert(event.tstamp == kernel_time);
	}
}

static void test_event_subscription(void) {
	int8_t events[FSE_MAX_EVENTS];
	memset(events, 0x7f, sizeof(events));
	event_subscription(events);
	assert(FSE_CLONE == 14 && FSE_LEGACY_MAX_EVENTS == 11 && FSE_MAX_EVENTS == 15);
	for (int type = 0; type < FSE_MAX_EVENTS; type++) {
		assert(events[type] == (type < FSE_LEGACY_MAX_EVENTS || type == FSE_CLONE ? FSE_REPORT : FSE_IGNORE));
	}
}

static void test_dropped_and_boundaries(void) {
	Record dropped = begin_record(FSE_EVENTS_DROPPED);
	FileMonitorEvent event;
	done(&dropped);
	assert(dropped.length == 10);
	assert(parse_record(dropped.bytes, dropped.length, &event) == 10);
	assert(event.type == FSE_EVENTS_DROPPED && !event.file && !event.tstamp);
	for (size_t length = 0; length < dropped.length; length++) {
		assert(parse_record(dropped.bytes, length, &event) == 0);
	}
	Record first = create_record(kernel_time, true);
	Record second = rename_record(FSE_RENAME, true);
	Record combined = {0};
	append(&combined, first.bytes, first.length);
	append(&combined, dropped.bytes, dropped.length);
	append(&combined, second.bytes, second.length);
	int used = parse_record(combined.bytes, combined.length, &event);
	assert(used == (int)first.length && event.type == FSE_CREATE_FILE);
	int next = parse_record(combined.bytes + used, combined.length - (size_t)used, &event);
	assert(next == 10 && event.type == FSE_EVENTS_DROPPED);
	used += next;
	assert(parse_record(combined.bytes + used, combined.length - (size_t)used, &event) == (int)second.length);
	assert(event.type == FSE_RENAME && event.tstamp == kernel_time);
}

static void invalid_argument(uint16_t type, uint16_t length) {
	Record record = begin_record(FSE_CREATE_FILE);
	FileMonitorEvent event;
	uint8_t value[32] = {0};
	path(&record, FSE_ARG_STRING, "/probe");
	argument(&record, type, value, length);
	done(&record);
	assert(parse_record(record.bytes, record.length, &event) == -1);
}

static void test_bad_lengths_and_paths(void) {
	const uint16_t scalars[] = {FSE_ARG_DEV, FSE_ARG_MODE, FSE_ARG_UID, FSE_ARG_GID};
	const uint16_t bad32[] = {0, 1, 3, 5, 8};
	for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
		for (size_t j = 0; j < sizeof(bad32) / sizeof(bad32[0]); j++) invalid_argument(scalars[i], bad32[j]);
	}
	const uint16_t bad64[] = {0, 1, 4, 7, 9};
	for (size_t i = 0; i < sizeof(bad64) / sizeof(bad64[0]); i++) invalid_argument(FSE_ARG_INT64, bad64[i]);
	const uint16_t bad_ino[] = {0, 1, 3, 5, 7, 9};
	for (size_t i = 0; i < sizeof(bad_ino) / sizeof(bad_ino[0]); i++) invalid_argument(FSE_ARG_INO, bad_ino[i]);
	invalid_argument(FSE_ARG_FINFO, 0);
	invalid_argument(FSE_ARG_FINFO, 23);
	invalid_argument(FSE_ARG_FINFO, 25);
	const struct { const char *value; uint16_t length; } bad_paths[] = {
		{"", 0}, {"/unterminated", 13}, {"/a\0b", 5}
	};
	for (size_t i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); i++) {
		Record record = begin_record(FSE_CREATE_FILE);
		FileMonitorEvent event;
		argument(&record, FSE_ARG_STRING, bad_paths[i].value, bad_paths[i].length);
		done(&record);
		assert(parse_record(record.bytes, record.length, &event) == -1);
	}
	/* Apple can emit a placeholder when no absolute path is available. */
	Record record = begin_record(FSE_CREATE_FILE);
	FileMonitorEvent event;
	path(&record, FSE_ARG_STRING, "NOPATH");
	done(&record);
	assert(parse_record(record.bytes, record.length, &event) == (int)record.length);
	assert(!strcmp(event.file, "NOPATH"));
}

typedef struct {
	int fd;
	const Record *stream;
} Writer;

static void *write_fragments(void *context) {
	Writer *writer = context;
	size_t offset = 0;
	while (offset < writer->stream->length) {
		size_t length = offset % 11 + 1;
		if (length > writer->stream->length - offset) length = writer->stream->length - offset;
		ssize_t written = write(writer->fd, writer->stream->bytes + offset, length);
		if (written < 0 && errno == EINTR) continue;
		assert(written > 0);
		offset += (size_t)written;
	}
	assert(shutdown(writer->fd, SHUT_WR) == 0);
	return NULL;
}

static unsigned int callbacks;
static bool capture_event(FileMonitor *monitor, FileMonitorEvent *event) {
	check_source(event);
	assert(!strcmp(event->proc, "wire-test") && event->ppid == 4321);
	if (callbacks == 0) {
		assert(event->type == FSE_CREATE_FILE && event->tstamp == kernel_time);
		assert(!strcmp(event->file, "/private/var/mobile/probe.txt") && !event->newfile);
	} else if (callbacks == 1) {
		assert(event->type == FSE_RENAME && event->tstamp == kernel_time);
		assert(!strcmp(event->file, "/source") && !strcmp(event->newfile, "/destination"));
	} else if (callbacks == 2) {
		assert(event->type == FSE_CLONE && event->tstamp == kernel_time);
		assert(!strcmp(event->file, "/source") && !strcmp(event->newfile, "/destination"));
	} else {
		assert(callbacks == 3 && event->type == FSE_CREATE_FILE && !event->tstamp);
		monitor->running = 0;
	}
	callbacks++;
	return true;
}

static void test_stream_loop(void) {
	Record first = create_record(kernel_time, true);
	Record second = rename_record(FSE_RENAME, true);
	Record third = rename_record(FSE_CLONE, true);
	Record fourth = create_record(0, false);
	Record dropped = begin_record(FSE_EVENTS_DROPPED);
	Record stream = {0};
	done(&dropped);
	append(&stream, first.bytes, first.length);
	append(&stream, dropped.bytes, dropped.length);
	append(&stream, second.bytes, second.length);
	append(&stream, third.bytes, third.length);
	append(&stream, fourth.bytes, fourth.length);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	Writer writer = {.fd = sockets[1], .stream = &stream};
	pthread_t thread;
	assert(pthread_create(&thread, NULL, write_fragments, &writer) == 0);
	FileMonitor monitor = {.fd = sockets[0], .running = 1};
	assert(fm_loop(&monitor, capture_event));
	assert(callbacks == 4 && stream_reads >= stream.length / 3);
	assert(pthread_join(thread, NULL) == 0);
	assert(fm_end(&monitor) && monitor.fd == -1);
	assert(close(sockets[1]) == 0);
}

enum { BURST_EVENTS = 2048 };

typedef struct {
	int fd;
	const uint8_t *bytes;
	size_t length;
} BurstWriter;

static void *write_burst(void *context) {
	BurstWriter *writer = context;
	size_t offset = 0;
	while (offset < writer->length) {
		/* A large write may still be split at arbitrary socket boundaries. */
		ssize_t written = write(writer->fd, writer->bytes + offset, writer->length - offset);
		if (written < 0 && errno == EINTR) continue;
		assert(written > 0);
		offset += (size_t)written;
	}
	assert(shutdown(writer->fd, SHUT_WR) == 0);
	return NULL;
}

static unsigned int burst_callbacks;
static bool capture_burst_event(FileMonitor *monitor, FileMonitorEvent *event) {
	char expected_path[128];
	assert(burst_callbacks < BURST_EVENTS);
	assert(snprintf(expected_path, sizeof(expected_path),
		"/private/var/mobile/burst-%04u.txt", burst_callbacks) > 0);
	check_source(event);
	assert(!strcmp(event->proc, "wire-test") && event->ppid == 4321);
	assert(event->type == (burst_callbacks % 2 ? FSE_CONTENT_MODIFIED : FSE_CREATE_FILE));
	assert(!strcmp(event->file, expected_path) && !event->newfile);
	assert(event->tstamp == kernel_time + burst_callbacks);
	if (++burst_callbacks == BURST_EVENTS) monitor->running = 0;
	return true;
}

static void test_burst_stream_loop(void) {
	/* This exercises decoder carry-over, not real kernel overflow behavior. */
	const size_t capacity = (size_t)BURST_EVENTS * sizeof(((Record *)0)->bytes);
	uint8_t *bytes = malloc(capacity);
	assert(bytes);
	size_t length = 0;
	for (unsigned int index = 0; index < BURST_EVENTS; index++) {
		char filename[128];
		assert(snprintf(filename, sizeof(filename),
			"/private/var/mobile/burst-%04u.txt", index) > 0);
		Record record = begin_record(index % 2 ? FSE_CONTENT_MODIFIED : FSE_CREATE_FILE);
		path(&record, FSE_ARG_STRING, filename);
		metadata(&record, false);
		scalar64(&record, FSE_ARG_INT64, kernel_time + index);
		done(&record);
		assert(record.length <= capacity - length);
		memcpy(bytes + length, record.bytes, record.length);
		length += record.length;
	}
	assert(length > 2 * FM_BUFSIZE);
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	BurstWriter writer = {.fd = sockets[1], .bytes = bytes, .length = length};
	pthread_t thread;
	stream_read_limit = 0;
	stream_reads = 0;
	burst_callbacks = 0;
	assert(pthread_create(&thread, NULL, write_burst, &writer) == 0);
	FileMonitor monitor = {.fd = sockets[0], .running = 1};
	assert(fm_loop(&monitor, capture_burst_event));
	assert(burst_callbacks == BURST_EVENTS && stream_reads >= 3);
	assert(pthread_join(thread, NULL) == 0);
	assert(fm_end(&monitor) && monitor.fd == -1);
	assert(close(sockets[1]) == 0);
	free(bytes);
	stream_read_limit = 3;
}

int main(void) {
	test_create_and_timestamps();
	test_two_path_events();
	test_compact_finfo_and_extensions();
	test_uid_availability();
	test_gid_availability();
	test_event_subscription();
	test_dropped_and_boundaries();
	test_bad_lengths_and_paths();
	test_stream_loop();
	test_burst_stream_loop();
	puts("devfsev tests passed");
	return 0;
}
