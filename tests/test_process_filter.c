/* Run the production callback with in-memory delivery and logging probes. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <assert.h>

#define main fsmod_test_main
#define fm_reporter_submit filter_test_submit
#define fm_reporter_is_internal filter_test_is_internal
#define fm_event_json filter_test_json
#include "../main.c"
#undef main
#undef fm_reporter_submit
#undef fm_reporter_is_internal
#undef fm_event_json

static size_t submitted;
static size_t serialized;

bool filter_test_submit(FmReporter *instance, const FileMonitor *monitor, const FileMonitorEvent *event) {
	assert(instance && monitor && event);
	submitted++;
	return true;
}

bool filter_test_is_internal(FmReporter *instance, const FileMonitorEvent *event) {
	assert(instance && event);
	return false;
}

char *filter_test_json(const FileMonitor *monitor, const FileMonitorEvent *event) {
	assert(monitor && event);
	serialized++;
	return strdup("{}");
}

static void check_event(FileMonitor *monitor, FileMonitorEvent *event, const char *process, bool excluded) {
	size_t previous_submitted = submitted;
	size_t previous_serialized = serialized;
	event->proc = process;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted + (excluded ? 0 : 1));
	assert(serialized == previous_serialized + (excluded ? 0 : 1));
}

static void check_typed_event(FileMonitor *monitor, FileMonitorEvent *event, int type,
		const char *file, const char *newfile, bool excluded) {
	event->type = type;
	event->file = file;
	event->newfile = newfile;
	check_event(monitor, event, "other", excluded);
}

static void test_watch_event_callback(FileMonitor *monitor, FileMonitorEvent *event) {
	const char *created = "/process-filter-test/create-only/item";
	const char *modified = "/process-filter-test/modify-only/item";
	const char *ignored = "/process-filter-test/create-only/ignored/item";
	size_t previous_submitted, previous_serialized;
	uint64_t *masks;
	/* Test the production callback with independent rules on two roots. The
	 * probes count both HTTP submission and local JSON serialization. */
	free(config.watch_paths[0]);
	free(config.watch_paths);
	free(config.watch_event_masks);
	config.watch_paths = calloc(2, sizeof(*config.watch_paths));
	config.watch_event_masks = calloc(2, sizeof(*config.watch_event_masks));
	assert(config.watch_paths && config.watch_event_masks);
	config.watch_paths[0] = strdup("/process-filter-test/create-only");
	config.watch_paths[1] = strdup("/process-filter-test/modify-only");
	assert(config.watch_paths[0] && config.watch_paths[1]);
	config.watch_count = 2;
	config.watch_event_masks[0] = (UINT64_C(1) << FSE_CREATE_FILE) | (UINT64_C(1) << FSE_CREATE_DIR);
	config.watch_event_masks[1] = UINT64_C(1) << FSE_CONTENT_MODIFIED;
	free(config.exclude_paths[0]);
	config.exclude_paths[0] = strdup("/process-filter-test/create-only/ignored");
	assert(config.exclude_paths[0]);
	config.log_events = true;
	check_typed_event(monitor, event, FSE_CREATE_FILE, created, NULL, false);
	check_typed_event(monitor, event, FSE_CREATE_DIR, created, NULL, false);
	check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, created, NULL, true);
	check_typed_event(monitor, event, FSE_STAT_CHANGED, created, NULL, true);
	check_typed_event(monitor, event, FSE_DELETE, created, NULL, true);
	check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, modified, NULL, false);
	check_typed_event(monitor, event, FSE_CREATE_FILE, modified, NULL, true);
	check_typed_event(monitor, event, FSE_OPEN, modified, NULL, true);
	check_typed_event(monitor, event, FSE_UNKNOWN, modified, NULL, true);
	check_typed_event(monitor, event, FSE_EVENTS_DROPPED, modified, NULL, true);
	check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, created, modified, false);
	check_typed_event(monitor, event, FSE_CREATE_FILE, modified, created, false);
	check_typed_event(monitor, event, FSE_RENAME, created, modified, true);
	check_typed_event(monitor, event, FSE_CREATE_FILE, ignored, modified, true);
	check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, modified, ignored, true);
	check_typed_event(monitor, event, FSE_CREATE_FILE, "/outside", NULL, true);
	check_typed_event(monitor, event, FSE_CREATE_FILE, created, NULL, false);
	check_event(monitor, event, "zsh", true);
	/* Turning event logging off does not disable the HTTP event-type rule. */
	config.log_events = false;
	previous_submitted = submitted;
	previous_serialized = serialized;
	event->proc = "other";
	event->type = FSE_DELETE;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted && serialized == previous_serialized);
	event->type = FSE_CREATE_FILE;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted + 1 && serialized == previous_serialized);
	/* Existing callers which construct configs without masks retain all. */
	config.log_events = true;
	masks = config.watch_event_masks;
	config.watch_event_masks = NULL;
	check_typed_event(monitor, event, FSE_DELETE, created, NULL, false);
	config.watch_event_masks = masks;
}

static void check_uid_event(FileMonitor *monitor, FileMonitorEvent *event, uint32_t uid,
		bool available, bool excluded) {
	/* Preserve the complete native 32-bit UID range through the callback. */
	event->uid = uid;
	event->uid_available = available;
	check_event(monitor, event, "other", excluded);
}

static void test_uid_callback(FileMonitor *monitor, FileMonitorEvent *event) {
	const char *created = "/process-filter-test/create-only/item";
	size_t previous_submitted, previous_serialized;
	assert(!config.watch_uids && !config.watch_uid_count);
	assert(!config.exclude_uids && !config.exclude_uid_count);
	config.log_events = true;
	event->file = created;
	event->newfile = NULL;
	event->type = FSE_CREATE_FILE;
	/* No UID rules preserves events from backends without UID attribution. */
	check_uid_event(monitor, event, 0, false, false);
	check_uid_event(monitor, event, 0, true, false);
	check_uid_event(monitor, event, 501, true, false);
	config.watch_uids = calloc(3, sizeof(*config.watch_uids));
	config.exclude_uids = calloc(2, sizeof(*config.exclude_uids));
	assert(config.watch_uids && config.exclude_uids);
	config.watch_uids[0] = 0;
	config.watch_uids[1] = 501;
	config.watch_uids[2] = UINT32_MAX;
	config.watch_uid_count = 1;
	check_uid_event(monitor, event, 0, true, false);
	check_uid_event(monitor, event, 501, true, true);
	check_uid_event(monitor, event, 0, false, true);
	config.watch_uid_count = 3;
	check_uid_event(monitor, event, 501, true, false);
	check_uid_event(monitor, event, UINT32_MAX, true, false);
	check_uid_event(monitor, event, 502, true, true);
	check_uid_event(monitor, event, 501, false, true);
	/* An exclusion wins even when the UID is in the allowlist. */
	config.exclude_uids[0] = 501;
	config.exclude_uid_count = 1;
	check_uid_event(monitor, event, 501, true, true);
	check_uid_event(monitor, event, 0, true, false);
	check_uid_event(monitor, event, UINT32_MAX, true, false);
	/* Passing the UID rule never bypasses process, path or event-type rules. */
	check_event(monitor, event, "zsh", true);
	event->proc = "other";
	event->file = "/process-filter-test/create-only/ignored/item";
	check_uid_event(monitor, event, 0, true, true);
	event->file = "/outside";
	check_uid_event(monitor, event, 0, true, true);
	event->file = created;
	event->type = FSE_DELETE;
	check_uid_event(monitor, event, 0, true, true);
	event->type = FSE_CREATE_FILE;
	/* The HTTP filter remains active when local event logging is disabled. */
	config.log_events = false;
	previous_submitted = submitted;
	previous_serialized = serialized;
	check_uid_event(monitor, event, 501, true, true);
	event->uid = 0;
	event->uid_available = true;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted + 1 && serialized == previous_serialized);
	config.log_events = true;
	/* Exclusion-only configurations keep known users outside the blacklist,
	 * but cannot safely decide an event with unavailable attribution. */
	config.watch_uid_count = 0;
	config.exclude_uids[0] = 0;
	config.exclude_uids[1] = UINT32_MAX;
	config.exclude_uid_count = 2;
	check_uid_event(monitor, event, 0, true, true);
	check_uid_event(monitor, event, UINT32_MAX, true, true);
	check_uid_event(monitor, event, 501, true, false);
	check_uid_event(monitor, event, 0, false, true);
	config.exclude_uid_count = 0;
	check_uid_event(monitor, event, 0, false, false);
}

static void check_gid_event(FileMonitor *monitor, FileMonitorEvent *event, uint32_t gid,
		bool available, bool excluded) {
	event->gid = gid;
	event->gid_available = available;
	check_event(monitor, event, "other", excluded);
}

static void test_gid_callback(FileMonitor *monitor, FileMonitorEvent *event) {
	const char *created = "/process-filter-test/create-only/item";
	size_t previous_submitted, previous_serialized;
	assert(!config.watch_gids && !config.watch_gid_count);
	assert(!config.exclude_gids && !config.exclude_gid_count);
	/* Start this dimension independently of the UID cases above. */
	free(config.watch_uids); config.watch_uids = NULL; config.watch_uid_count = 0;
	free(config.exclude_uids); config.exclude_uids = NULL; config.exclude_uid_count = 0;
	config.log_events = true;
	event->file = created;
	event->newfile = NULL;
	event->type = FSE_CREATE_FILE;
	/* GID attribution is independent of UID attribution. */
	event->uid = 501;
	event->uid_available = false;
	check_gid_event(monitor, event, 0, false, false);
	check_gid_event(monitor, event, 501, true, false);
	config.watch_gids = calloc(3, sizeof(*config.watch_gids));
	config.exclude_gids = calloc(2, sizeof(*config.exclude_gids));
	assert(config.watch_gids && config.exclude_gids);
	config.watch_gids[0] = 0;
	config.watch_gids[1] = 501;
	config.watch_gids[2] = UINT32_MAX;
	config.watch_gid_count = 1;
	check_gid_event(monitor, event, 0, true, false);
	check_gid_event(monitor, event, 501, true, true);
	check_gid_event(monitor, event, 0, false, true);
	config.watch_gid_count = 3;
	check_gid_event(monitor, event, 501, true, false);
	check_gid_event(monitor, event, UINT32_MAX, true, false);
	check_gid_event(monitor, event, 502, true, true);
	check_gid_event(monitor, event, 501, false, true);
	/* GID exclusion takes precedence over the allowlist. */
	config.exclude_gids[0] = 501;
	config.exclude_gid_count = 1;
	check_gid_event(monitor, event, 501, true, true);
	check_gid_event(monitor, event, 0, true, false);
	check_gid_event(monitor, event, UINT32_MAX, true, false);
	/* Passing the GID rule still requires all other filters to pass. */
	check_event(monitor, event, "zsh", true);
	event->proc = "other";
	event->file = "/process-filter-test/create-only/ignored/item";
	check_gid_event(monitor, event, 0, true, true);
	event->file = "/outside";
	check_gid_event(monitor, event, 0, true, true);
	event->file = created;
	event->type = FSE_DELETE;
	check_gid_event(monitor, event, 0, true, true);
	event->type = FSE_CREATE_FILE;
	/* An active GID rule rejects missing GID metadata, including GID zero. */
	check_gid_event(monitor, event, 0, false, true);
	/* UID-only filtering does not require GID and vice versa. */
	config.watch_gid_count = 0;
	config.exclude_gid_count = 0;
	free(config.watch_gids); config.watch_gids = NULL;
	free(config.exclude_gids); config.exclude_gids = NULL;
	config.watch_uids = calloc(1, sizeof(*config.watch_uids));
	assert(config.watch_uids);
	config.watch_uids[0] = 501; config.watch_uid_count = 1;
	event->uid = 501; event->uid_available = true;
	check_gid_event(monitor, event, 0, false, false);
	/* Both lists are ANDed; an event must satisfy each active dimension. */
	config.watch_gids = calloc(1, sizeof(*config.watch_gids));
	assert(config.watch_gids);
	config.watch_gids[0] = 82; config.watch_gid_count = 1;
	event->uid = 501; event->uid_available = true;
	check_gid_event(monitor, event, 82, true, false);
	check_gid_event(monitor, event, 0, true, true);
	event->uid = 502;
	check_gid_event(monitor, event, 82, true, true);
	/* The HTTP filter remains active when local event logging is disabled. */
	config.log_events = false;
	previous_submitted = submitted;
	previous_serialized = serialized;
	event->uid = 501; event->uid_available = true;
	event->gid = 82; event->gid_available = true; event->proc = "other";
	(void)callback(monitor, event);
	assert(submitted == previous_submitted + 1 && serialized == previous_serialized);
	config.log_events = true;
}

static void check_ppid_event(FileMonitor *monitor, FileMonitorEvent *event, int ppid,
		bool excluded) {
	event->ppid = ppid;
	check_event(monitor, event, "other", excluded);
}

static void test_ppid_callback(FileMonitor *monitor, FileMonitorEvent *event) {
	const char *created = "/process-filter-test/create-only/item";
	size_t previous_submitted, previous_serialized;
	assert(!config.watch_ppids && !config.watch_ppid_count);
	assert(!config.exclude_ppids && !config.exclude_ppid_count);
	/* Parent filtering works without file owner/group attribution. */
	free(config.watch_uids); config.watch_uids = NULL; config.watch_uid_count = 0;
	free(config.exclude_uids); config.exclude_uids = NULL; config.exclude_uid_count = 0;
	free(config.watch_gids); config.watch_gids = NULL; config.watch_gid_count = 0;
	free(config.exclude_gids); config.exclude_gids = NULL; config.exclude_gid_count = 0;
	config.log_events = true;
	event->file = created;
	event->newfile = NULL;
	event->type = FSE_CREATE_FILE;
	event->uid_available = false;
	event->gid_available = false;
	check_ppid_event(monitor, event, 0, false);
	check_ppid_event(monitor, event, -1, false);
	check_ppid_event(monitor, event, 1, false);
	check_ppid_event(monitor, event, INT_MAX, false);
	config.watch_ppids = calloc(3, sizeof(*config.watch_ppids));
	config.exclude_ppids = calloc(2, sizeof(*config.exclude_ppids));
	assert(config.watch_ppids && config.exclude_ppids);
	config.watch_ppids[0] = 1;
	config.watch_ppids[1] = 42;
	config.watch_ppids[2] = INT_MAX;
	config.watch_ppid_count = 1;
	check_ppid_event(monitor, event, 1, false);
	check_ppid_event(monitor, event, 42, true);
	check_ppid_event(monitor, event, 0, true);
	check_ppid_event(monitor, event, -1, true);
	config.watch_ppid_count = 3;
	check_ppid_event(monitor, event, 42, false);
	check_ppid_event(monitor, event, INT_MAX, false);
	check_ppid_event(monitor, event, 43, true);
	/* An event's own PID must not stand in for its direct parent PID. */
	event->pid = 42;
	check_ppid_event(monitor, event, 43, true);
	event->pid = 123456;
	check_ppid_event(monitor, event, 42, false);
	config.exclude_ppids[0] = 42;
	config.exclude_ppid_count = 1;
	check_ppid_event(monitor, event, 42, true);
	check_ppid_event(monitor, event, 1, false);
	check_ppid_event(monitor, event, INT_MAX, false);
	/* A matching parent cannot bypass process, path or event-type rules. */
	check_event(monitor, event, "zsh", true);
	event->proc = "other";
	event->file = "/process-filter-test/create-only/ignored/item";
	check_ppid_event(monitor, event, 1, true);
	event->file = "/outside";
	check_ppid_event(monitor, event, 1, true);
	event->file = created;
	event->type = FSE_DELETE;
	check_ppid_event(monitor, event, 1, true);
	event->type = FSE_CREATE_FILE;
	/* Exclusion-only rules also reject unknown parents; no rules keep them. */
	config.watch_ppid_count = 0;
	config.exclude_ppids[0] = 1;
	config.exclude_ppids[1] = INT_MAX;
	config.exclude_ppid_count = 2;
	check_ppid_event(monitor, event, 1, true);
	check_ppid_event(monitor, event, INT_MAX, true);
	check_ppid_event(monitor, event, 42, false);
	check_ppid_event(monitor, event, 0, true);
	check_ppid_event(monitor, event, -1, true);
	config.exclude_ppid_count = 0;
	check_ppid_event(monitor, event, 0, false);
	check_ppid_event(monitor, event, -1, false);
	/* All three dimensions are independent AND conditions. */
	config.watch_uid_count = 1;
	config.watch_uids = calloc(1, sizeof(*config.watch_uids));
	config.watch_gid_count = 1;
	config.watch_gids = calloc(1, sizeof(*config.watch_gids));
	assert(config.watch_uids && config.watch_gids);
	config.watch_uids[0] = 501;
	config.watch_gids[0] = 82;
	event->uid = 501; event->uid_available = true;
	event->gid = 82; event->gid_available = true;
	check_ppid_event(monitor, event, 0, false);
	config.watch_ppid_count = 3;
	check_ppid_event(monitor, event, 42, false);
	check_ppid_event(monitor, event, 43, true);
	event->uid = 502;
	check_ppid_event(monitor, event, 42, true);
	event->uid = 501;
	event->gid = 83;
	check_ppid_event(monitor, event, 42, true);
	event->gid = 82;
	event->uid_available = false;
	check_ppid_event(monitor, event, 42, true);
	event->uid_available = true;
	event->gid_available = false;
	check_ppid_event(monitor, event, 42, true);
	event->gid_available = true;
	/* HTTP filtering remains active when event logging is disabled. */
	config.log_events = false;
	previous_submitted = submitted;
	previous_serialized = serialized;
	event->ppid = 43;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted && serialized == previous_serialized);
	event->ppid = 42;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted + 1 && serialized == previous_serialized);
	config.log_events = true;
}

static void test_collection_roots(void) {
	char *nested_first[] = {"/Library/dpkg/info", "/Library", "/Library-sibling", "/Library/dpkg/info/cache/keep"};
	char *ancestor_first[] = {"/Library", "/Library/dpkg/info", "/Library-sibling", "/Library/dpkg/info/cache/keep"};
	const char *roots[4];
	FmConfig scope = {0};
	size_t i;
	for (i = 0; i < 2; i++) {
		scope.watch_paths = i ? ancestor_first : nested_first;
		scope.watch_count = 4;
		assert(configured_collection_roots(&scope, roots) == 2);
		assert(!strcmp(roots[0], "/Library"));
		assert(!strcmp(roots[1], "/Library-sibling"));
		/* Logical exception rules remain in the configuration; only the
		 * backend subscription is reduced to non-overlapping ancestor roots. */
		assert(scope.watch_count == 4);
		assert(!strcmp(scope.watch_paths[i ? 1 : 0], "/Library/dpkg/info"));
	}
	/* fanotify uses mount marks: an explicit child may be on another mount,
	 * which the ancestor mount's mark cannot cover. */
	scope.backend = "fanotify";
	assert(configured_collection_roots(&scope, roots) == 4);
	for (i = 0; i < scope.watch_count; i++) assert(roots[i] == scope.watch_paths[i]);
	scope.backend = NULL;
	scope.watch_paths = nested_first;
	scope.watch_count = 1;
	assert(configured_collection_roots(&scope, roots) == 1);
	assert(!strcmp(roots[0], "/Library/dpkg/info"));
	scope.watch_count = 0;
	assert(configured_collection_roots(&scope, roots) == 0);
}

static void test_nested_path_callback(FileMonitor *monitor, FileMonitorEvent *event) {
	const char *ordinary = "/process-filter-test/ordinary";
	const char *included = "/process-filter-test/dpkg/info/item";
	const char *excluded = "/process-filter-test/dpkg/other";
	const char *blocked = "/process-filter-test/dpkg/info/cache/item";
	const char *retained = "/process-filter-test/dpkg/info/cache/keep/item";
	size_t i, previous_submitted, previous_serialized;
	char *temporary_path;
	uint64_t temporary_mask;
	/* Start independently of UID/GID/PPID rules tested above. */
	fm_config_free(&config);
	fm_config_init(&config);
	config.watch_count = 3;
	config.watch_paths = calloc(config.watch_count, sizeof(*config.watch_paths));
	config.watch_event_masks = calloc(config.watch_count, sizeof(*config.watch_event_masks));
	config.exclude_count = 2;
	config.exclude_paths = calloc(config.exclude_count, sizeof(*config.exclude_paths));
	assert(config.watch_paths && config.watch_event_masks && config.exclude_paths);
	config.watch_paths[0] = strdup("/process-filter-test");
	config.watch_paths[1] = strdup("/process-filter-test/dpkg/info");
	config.watch_paths[2] = strdup("/process-filter-test/dpkg/info/cache/keep");
	config.exclude_paths[0] = strdup("/process-filter-test/dpkg");
	config.exclude_paths[1] = strdup("/process-filter-test/dpkg/info/cache");
	for (i = 0; i < config.watch_count; i++) assert(config.watch_paths[i]);
	for (i = 0; i < config.exclude_count; i++) assert(config.exclude_paths[i]);
	config.watch_event_masks[0] = UINT64_C(1) << FSE_CREATE_FILE;
	config.watch_event_masks[1] = (UINT64_C(1) << FSE_CONTENT_MODIFIED) |
		(UINT64_C(1) << FSE_RENAME) | (UINT64_C(1) << FSE_CLONE);
	config.watch_event_masks[2] = FM_WATCH_EVENTS_ALL;
	config.log_events = true;
	/* Every assertion checks both the HTTP submission and local log probes.
	 * Reverse watch/exclusion ordering to exercise callback order independence. */
	for (i = 0; i < 2; i++) {
		check_typed_event(monitor, event, FSE_CREATE_FILE, ordinary, NULL, false);
		check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, ordinary, NULL, true);
		check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, included, NULL, false);
		check_typed_event(monitor, event, FSE_CREATE_FILE, included, NULL, true);
		check_typed_event(monitor, event, FSE_CREATE_FILE, excluded, NULL, true);
		check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, blocked, NULL, true);
		check_typed_event(monitor, event, FSE_DELETE, retained, NULL, false);
		check_typed_event(monitor, event, FSE_CREATE_FILE, "/process-filter-test/dpkg/info-sibling/item", NULL, true);
		check_typed_event(monitor, event, FSE_CREATE_FILE, "/process-filter-test/dpkg-sibling/item", NULL, false);
		check_typed_event(monitor, event, FSE_CREATE_FILE, "/process-filter-test-sibling/item", NULL, true);
		check_typed_event(monitor, event, FSE_RENAME, ordinary, included, false);
		check_typed_event(monitor, event, FSE_RENAME, included, ordinary, false);
		check_typed_event(monitor, event, FSE_RENAME, ordinary, ordinary, true);
		check_typed_event(monitor, event, FSE_CLONE, "/outside", included, false);
		check_typed_event(monitor, event, FSE_CLONE, included, "/outside", false);
		check_typed_event(monitor, event, FSE_RENAME, excluded, included, true);
		check_typed_event(monitor, event, FSE_RENAME, included, excluded, true);
		check_typed_event(monitor, event, FSE_CLONE, blocked, retained, true);
		check_typed_event(monitor, event, FSE_CLONE, retained, blocked, true);
		temporary_path = config.watch_paths[0];
		config.watch_paths[0] = config.watch_paths[2];
		config.watch_paths[2] = temporary_path;
		temporary_mask = config.watch_event_masks[0];
		config.watch_event_masks[0] = config.watch_event_masks[2];
		config.watch_event_masks[2] = temporary_mask;
		temporary_path = config.exclude_paths[0];
		config.exclude_paths[0] = config.exclude_paths[1];
		config.exclude_paths[1] = temporary_path;
	}
	/* An exact exclusion wins at info while the deeper keep watch still wins. */
	free(config.exclude_paths[0]);
	config.exclude_paths[0] = strdup("/process-filter-test/dpkg/info");
	assert(config.exclude_paths[0]);
	check_typed_event(monitor, event, FSE_CONTENT_MODIFIED, included, NULL, true);
	check_typed_event(monitor, event, FSE_DELETE, retained, NULL, false);
	/* The same nested-path decision still governs HTTP with local logs off. */
	config.log_events = false;
	previous_submitted = submitted;
	previous_serialized = serialized;
	event->type = FSE_DELETE; event->file = retained; event->newfile = NULL;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted + 1 && serialized == previous_serialized);
	event->type = FSE_CONTENT_MODIFIED; event->file = included;
	(void)callback(monitor, event);
	assert(submitted == previous_submitted + 1 && serialized == previous_serialized);
}

int main(void) {
	char path[] = "/tmp/fsmod-process-filter-XXXXXX";
	char error[256];
	int fd = mkstemp(path);
	FILE *stream;
	FileMonitor monitor = {0};
	FileMonitorEvent event = {
		.pid = 123456, .ppid = 123455, .type = FSE_CREATE_FILE,
		.file = "/process-filter-test/change.txt"
	};
	assert(fd >= 0);
	stream = fdopen(fd, "w");
	assert(stream);
	assert(fputs("report_url=http://127.0.0.1:1/events\nwatch_path=/\n"
#if __APPLE__
		"backend=devfsev\n"
#else
		"backend=fanotify\n"
#endif
		"log_events=true\nexclude_process=zsh\nexclude_process=cfprefsd\n", stream) >= 0);
	assert(fclose(stream) == 0);
	fm_config_init(&config);
	assert(fm_config_load(&config, path, error, sizeof(error)));
	configured = true;
	/* Never dereferenced: the probes replace every callback use of reporter. */
	reporter = (FmReporter *)&submitted;
	check_event(&monitor, &event, "zsh", true);
	check_event(&monitor, &event, "cfprefsd", true);
	check_event(&monitor, &event, "Zsh", false);
	check_event(&monitor, &event, "zsh-helper", false);
	check_event(&monitor, &event, "my-zsh", false);
	check_event(&monitor, &event, "", false);
	check_event(&monitor, &event, NULL, false);
	/* Renames obey the same process rule before either output path. */
	event.type = FSE_RENAME;
	event.newfile = "/process-filter-test/renamed.txt";
	check_event(&monitor, &event, "zsh", true);
	check_event(&monitor, &event, "other", false);
	/* Clone callbacks obey both process exclusion and either-side path exclusion. */
	event.type = FSE_CLONE;
	check_event(&monitor, &event, "zsh", true);
	check_event(&monitor, &event, "other", false);
	assert(!config.exclude_paths && !config.exclude_count);
	config.exclude_paths = calloc(1, sizeof(*config.exclude_paths));
	assert(config.exclude_paths);
	config.exclude_paths[0] = strdup("/process-filter-test/ignored");
	assert(config.exclude_paths[0]);
	config.exclude_count = 1;
	event.file = "/process-filter-test/ignored/source";
	check_event(&monitor, &event, "other", true);
	event.file = "/process-filter-test/change.txt";
	event.newfile = "/process-filter-test/ignored/destination";
	check_event(&monitor, &event, "other", true);
	event.newfile = "/process-filter-test/cloned.txt";
	/* Rules remain active when local event logging is disabled. */
	config.log_events = false;
	size_t previous_submitted = submitted, previous_serialized = serialized;
	event.proc = "zsh";
	(void)callback(&monitor, &event);
	assert(submitted == previous_submitted && serialized == previous_serialized);
	event.proc = "other";
	(void)callback(&monitor, &event);
	assert(submitted == previous_submitted + 1 && serialized == previous_serialized);
	test_watch_event_callback(&monitor, &event);
	test_uid_callback(&monitor, &event);
	test_gid_callback(&monitor, &event);
	test_ppid_callback(&monitor, &event);
	test_collection_roots();
	test_nested_path_callback(&monitor, &event);
	reporter = NULL;
	configured = false;
	fm_config_free(&config);
	assert(!config.watch_ppids && !config.watch_ppid_count);
	assert(!config.exclude_ppids && !config.exclude_ppid_count);
	assert(unlink(path) == 0);
	puts("process callback filter tests passed");
	return 0;
}
