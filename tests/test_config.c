#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "config.h"
#include "backend/devfsev.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_bytes(const char *path, const void *data, size_t length) {
	FILE *stream = fopen(path, "wb");
	assert(stream);
	assert(fwrite(data, 1, length, stream) == length);
	assert(fclose(stream) == 0);
}

static void write_config(const char *path, const char *root, const char *extra) {
	FILE *stream = fopen(path, "wb");
	assert(stream);
	assert(fprintf(stream, "report_url=https://example.com/events\nwatch_path=%s\n%s", root, extra) > 0);
	assert(fclose(stream) == 0);
}

static void rejects(FmConfig *config, const char *path, const char *root, const char *extra) {
	char err[256];
	char *original_url = config->report_url;
	size_t original_count = config->watch_count;
	char **original_processes = config->exclude_processes;
	size_t original_process_count = config->exclude_process_count;
	uint32_t *original_watch_uids = config->watch_uids;
	size_t original_watch_uid_count = config->watch_uid_count;
	uint32_t *original_exclude_uids = config->exclude_uids;
	size_t original_exclude_uid_count = config->exclude_uid_count;
	uint32_t original_watch_uid = original_watch_uid_count ? original_watch_uids[0] : 0;
	uint32_t original_exclude_uid = original_exclude_uid_count ? original_exclude_uids[0] : 0;
	uint32_t *original_watch_ppids = config->watch_ppids;
	size_t original_watch_ppid_count = config->watch_ppid_count;
	uint32_t *original_exclude_ppids = config->exclude_ppids;
	size_t original_exclude_ppid_count = config->exclude_ppid_count;
	uint32_t original_watch_ppid = original_watch_ppid_count ? original_watch_ppids[0] : 0;
	uint32_t original_exclude_ppid = original_exclude_ppid_count ? original_exclude_ppids[0] : 0;
	char **original_warnings = config->warnings;
	size_t original_warning_count = config->warning_count;
	uint64_t *original_masks = config->watch_event_masks;
	uint64_t original_first_mask = original_masks && original_count ? original_masks[0] : 0;
	write_config(path, root, extra);
	assert(!fm_config_load(config, path, err, sizeof(err)));
	assert(*err);
	assert(config->report_url == original_url);
	assert(config->watch_count == original_count);
	assert(config->exclude_processes == original_processes);
	assert(config->exclude_process_count == original_process_count);
	assert(config->watch_uids == original_watch_uids);
	assert(config->watch_uid_count == original_watch_uid_count);
	assert(config->exclude_uids == original_exclude_uids);
	assert(config->exclude_uid_count == original_exclude_uid_count);
	if (original_watch_uid_count) assert(config->watch_uids[0] == original_watch_uid);
	if (original_exclude_uid_count) assert(config->exclude_uids[0] == original_exclude_uid);
	assert(config->watch_ppids == original_watch_ppids);
	assert(config->watch_ppid_count == original_watch_ppid_count);
	assert(config->exclude_ppids == original_exclude_ppids);
	assert(config->exclude_ppid_count == original_exclude_ppid_count);
	if (original_watch_ppid_count) assert(config->watch_ppids[0] == original_watch_ppid);
	if (original_exclude_ppid_count) assert(config->exclude_ppids[0] == original_exclude_ppid);
	assert(config->warnings == original_warnings);
	assert(config->warning_count == original_warning_count);
	assert(config->watch_event_masks == original_masks);
	if (original_masks && original_count) assert(original_masks[0] == original_first_mask);
}

static const int concrete_events[] = {
	FSE_CREATE_FILE, FSE_DELETE, FSE_STAT_CHANGED, FSE_RENAME, FSE_CONTENT_MODIFIED,
	FSE_EXCHANGE, FSE_FINDER_INFO_CHANGED, FSE_CREATE_DIR, FSE_CHOWN, FSE_XATTR_MODIFIED,
	FSE_XATTR_REMOVED, FSE_DOCID_CREATED, FSE_DOCID_CHANGED, FSE_UNMOUNT_PENDING, FSE_CLONE,
	FSE_OPEN, FSE_CLOSE, FSE_CLOSE_WRITABLE, FSE_UNKNOWN
};

static void assert_event_set(const FmConfig *config, const char *path, const int *allowed, size_t count) {
	size_t i, j;
	for (i = 0; i < sizeof(concrete_events) / sizeof(concrete_events[0]); i++) {
		bool expected = false;
		for (j = 0; j < count; j++) {
			if (concrete_events[i] == allowed[j]) expected = true;
		}
		assert(fm_config_matches_event(config, path, NULL, concrete_events[i]) == expected);
		assert(fm_config_matches_event(config, "/outside-watch-events", path, concrete_events[i]) == expected);
	}
	assert(!fm_config_matches_event(config, path, NULL, FSE_INVALID));
	assert(!fm_config_matches_event(config, path, NULL, FSE_EVENTS_DROPPED));
	assert(!fm_config_matches_event(config, path, NULL, INT_MIN));
	assert(!fm_config_matches_event(config, path, NULL, INT_MAX));
}

static void test_watch_events(FmConfig *config, const char *path, const char *root,
		const char *child, const char *file, const char *directory) {
	static const struct { const char *name; int types[6]; size_t count; } groups[] = {
		{"create", {FSE_CREATE_FILE, FSE_CREATE_DIR}, 2},
		{"modify", {FSE_CONTENT_MODIFIED, FSE_STAT_CHANGED, FSE_CHOWN, FSE_XATTR_MODIFIED,
			FSE_XATTR_REMOVED, FSE_FINDER_INFO_CHANGED}, 6},
		{"delete", {FSE_DELETE}, 1}, {"rename", {FSE_RENAME}, 1},
		{"clone", {FSE_CLONE}, 1}, {"exchange", {FSE_EXCHANGE}, 1}
	};
	static const struct { const char *name; int type; } exact[] = {
		{"FSE_CREATE_FILE", FSE_CREATE_FILE}, {"FSE_CREATE_DIR", FSE_CREATE_DIR},
		{"FSE_DELETE", FSE_DELETE}, {"FSE_STAT_CHANGED", FSE_STAT_CHANGED},
		{"FSE_RENAME", FSE_RENAME}, {"FSE_CONTENT_MODIFIED", FSE_CONTENT_MODIFIED},
		{"FSE_EXCHANGE", FSE_EXCHANGE}, {"FSE_FINDER_INFO_CHANGED", FSE_FINDER_INFO_CHANGED},
		{"FSE_CHOWN", FSE_CHOWN}, {"FSE_XATTR_MODIFIED", FSE_XATTR_MODIFIED},
		{"FSE_XATTR_REMOVED", FSE_XATTR_REMOVED}, {"FSE_DOCID_CREATED", FSE_DOCID_CREATED},
		{"FSE_DOCID_CHANGED", FSE_DOCID_CHANGED}, {"FSE_UNMOUNT_PENDING", FSE_UNMOUNT_PENDING},
		{"FSE_CLONE", FSE_CLONE}, {"FSE_OPEN", FSE_OPEN}, {"FSE_CLOSE", FSE_CLOSE},
		{"FSE_CLOSE_WRITABLE", FSE_CLOSE_WRITABLE}, {"FSE_UNKNOWN", FSE_UNKNOWN}
	};
	static const char *bad[] = {
		"", "   ", ",create", "create,", "create,,modify", "create, ,modify",
		"all,create", "create,all", "all,all", "ALL", "Create", "fse_create_file",
		"FSE_ALL_EVENTS", "FSE_EVENTS_DROPPED", "FSE_INVALID", "FSE_UNKNOWN_EVENT",
		"0", "create modify", "create;modify", "*"
	};
	char text[16384], missing[2048], nested[2048], err[256];
	size_t i;
	uint64_t *masks;
	const int create_delete[] = {FSE_CREATE_FILE, FSE_CREATE_DIR, FSE_DELETE};
	assert(snprintf(missing, sizeof(missing), "%s/missing-events", directory) > 0);
	assert(snprintf(nested, sizeof(nested), "%s/event-child", child) > 0);
	/* Omission and explicit all both preserve legacy event behavior, including
	 * negative/unknown types and dropped markers without invalid bit shifts. */
	for (i = 0; i < 2; i++) {
		write_config(path, root, i ? "watch_events= all \n" : "");
		assert(fm_config_load(config, path, err, sizeof(err)));
		assert(config->watch_event_masks && config->watch_event_masks[0] == FM_WATCH_EVENTS_ALL);
		assert(fm_config_matches_event(config, root, NULL, FSE_INVALID));
		assert(fm_config_matches_event(config, root, NULL, FSE_OPEN));
		assert(fm_config_matches_event(config, root, NULL, FSE_UNKNOWN));
		assert(fm_config_matches_event(config, root, NULL, FSE_EVENTS_DROPPED));
		assert(fm_config_matches_event(config, root, NULL, INT_MIN));
		assert(fm_config_matches_event(config, root, NULL, INT_MAX));
	}
	assert(!fm_config_matches_event(NULL, root, NULL, FSE_CREATE_FILE));
	assert(!fm_config_matches_event(config, NULL, NULL, FSE_CREATE_FILE));
	assert(!fm_config_matches_event(config, "/outside-watch-events", NULL, FSE_CREATE_FILE));
	for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		assert(snprintf(text, sizeof(text), "watch_events=%s\n", groups[i].name) > 0);
		write_config(path, root, text);
		assert(fm_config_load(config, path, err, sizeof(err)));
		assert_event_set(config, root, groups[i].types, groups[i].count);
	}
	for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
		assert(snprintf(text, sizeof(text), "watch_events=%s\n", exact[i].name) > 0);
		write_config(path, root, text);
		assert(fm_config_load(config, path, err, sizeof(err)));
		assert_event_set(config, root, &exact[i].type, 1);
	}
	/* Whitespace and duplicate tokens are harmless; mixed groups are unions. */
	write_config(path, root, "watch_events= create , FSE_DELETE ,create,FSE_CREATE_FILE \n");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert_event_set(config, root, create_delete, sizeof(create_delete) / sizeof(create_delete[0]));
	assert(fm_config_matches(config, root, NULL));
	assert(!fm_config_matches_event(config, root, NULL, FSE_RENAME));
	/* A manually initialized older config without masks still means all. */
	masks = config->watch_event_masks;
	config->watch_event_masks = NULL;
	assert(fm_config_matches_event(config, root, NULL, INT_MAX));
	config->watch_event_masks = masks;
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		assert(snprintf(text, sizeof(text), "watch_events=%s\n", bad[i]) > 0);
		rejects(config, path, root, text);
	}
	rejects(config, path, root, "watch_events=create\nwatch_events=modify\n");
	assert(snprintf(text, sizeof(text),
		"report_url=https://example.com/events\nwatch_events=create\nwatch_path=%s\n", root) > 0);
	write_bytes(path, text, strlen(text));
	assert(!fm_config_load(config, path, err, sizeof(err)));
	assert(strstr(err, "line 2:") && strstr(err, "preceding watch_path"));
	assert(!fm_config_load_control(config, path, err, sizeof(err)));
	/* Intervening global settings/comments do not change the latest root. */
	assert(snprintf(text, sizeof(text),
		"log_events=true\n# still the first root\nwatch_events=rename,clone,exchange\n"
		"watch_path=%s\nwatch_events=create\n", file) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 2);
	assert(fm_config_matches_event(config, root, file, FSE_RENAME));
	assert(fm_config_matches_event(config, file, root, FSE_CLONE));
	assert(fm_config_matches_event(config, file, root, FSE_EXCHANGE));
	assert(fm_config_matches_event(config, root, file, FSE_CREATE_FILE));
	assert(!fm_config_matches_event(config, root, NULL, FSE_CREATE_FILE));
	assert(!fm_config_matches_event(config, file, NULL, FSE_RENAME));
	assert(!fm_config_matches_event(config, root, file, FSE_DELETE));
	/* Either-side exclusions override a root which would allow the event. */
	assert(snprintf(text, sizeof(text),
		"watch_events=rename,clone,exchange\nwatch_path=%s\nwatch_events=create\nexclude_path=%s\n", file, child) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!fm_config_matches_event(config, nested, file, FSE_RENAME));
	assert(!fm_config_matches_event(config, file, nested, FSE_CREATE_FILE));
	/* A skipped root owns and validates its own rule; it must never overwrite
	 * the previous valid root, and the next root starts again with all. */
	assert(snprintf(text, sizeof(text),
		"watch_events=create\nwatch_path=%s\nwatch_events=modify\nwatch_path=%s\n", missing, file) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 2 && config->warning_count == 1);
	assert(config->watch_event_masks[1] == FM_WATCH_EVENTS_ALL);
	assert(fm_config_matches_event(config, root, NULL, FSE_CREATE_FILE));
	assert(!fm_config_matches_event(config, root, NULL, FSE_CONTENT_MODIFIED));
	assert(fm_config_matches_event(config, file, NULL, FSE_CONTENT_MODIFIED));
	assert(!fm_config_matches_event(config, missing, NULL, FSE_CONTENT_MODIFIED));
	/* Control mode retains all roots but preserves exactly the same rule
	 * associations, so stop remains usable after watched paths disappear. */
	assert(fm_config_load_control(config, path, err, sizeof(err)));
	assert(config->watch_count == 3 && config->warning_count == 0);
	assert(fm_config_matches_event(config, missing, NULL, FSE_CONTENT_MODIFIED));
	assert(!fm_config_matches_event(config, missing, NULL, FSE_CREATE_FILE));
	assert(config->watch_event_masks[2] == FM_WATCH_EVENTS_ALL);
	assert(snprintf(text, sizeof(text), "watch_path=%s\nwatch_events=unknown\n", missing) > 0);
	rejects(config, path, root, text);
	assert(!fm_config_load_control(config, path, err, sizeof(err)));
	assert(snprintf(text, sizeof(text),
		"watch_path=%s\nwatch_events=create\nwatch_events=delete\n", missing) > 0);
	rejects(config, path, root, text);
	assert(snprintf(text, sizeof(text), "watch_events=modify\n") > 0);
	write_config(path, missing, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 0 && config->watch_event_masks == NULL && config->warning_count == 1);
	assert(!fm_config_matches_event(config, missing, NULL, FSE_CONTENT_MODIFIED));
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 1 && config->watch_event_masks[0] == FM_WATCH_EVENTS_ALL);
}

static void test_nested_watch_paths(FmConfig *config, const char *path, const char *root) {
	char excluded[2048], included[2048], cache[2048], kept[2048];
	char ordinary[2048], item[2048], blocked[2048], retained[2048], sibling[2048];
	char outer_rule[4096], inner_rule[4096], exclusions[8192], text[16384], err[256];
	const char *parts[3];
	size_t order, control;
	assert(snprintf(excluded, sizeof(excluded), "%s/dpkg", root) > 0);
	assert(snprintf(included, sizeof(included), "%s/info", excluded) > 0);
	assert(snprintf(cache, sizeof(cache), "%s/cache", included) > 0);
	assert(snprintf(kept, sizeof(kept), "%s/keep", cache) > 0);
	assert(mkdir(excluded, 0700) == 0);
	assert(mkdir(included, 0700) == 0);
	assert(mkdir(cache, 0700) == 0);
	assert(mkdir(kept, 0700) == 0);
	assert(snprintf(ordinary, sizeof(ordinary), "%s/ordinary", root) > 0);
	assert(snprintf(item, sizeof(item), "%s/item", included) > 0);
	assert(snprintf(blocked, sizeof(blocked), "%s/item", cache) > 0);
	assert(snprintf(retained, sizeof(retained), "%s/item", kept) > 0);
	assert(snprintf(sibling, sizeof(sibling), "%s-sibling/item", included) > 0);
	assert(snprintf(outer_rule, sizeof(outer_rule), "watch_path=%s\nwatch_events=create\n", root) > 0);
	assert(snprintf(inner_rule, sizeof(inner_rule),
		"watch_path=%s\nwatch_events=modify,rename,clone\nwatch_path=%s\n", included, kept) > 0);
	assert(snprintf(exclusions, sizeof(exclusions), "exclude_path=%s\nexclude_path=%s\n", excluded, cache) > 0);
	/* The most specific path wins, independent of input order. Event masks
	 * belong only to that watch; the deeper watch without a mask starts at all. */
	for (order = 0; order < 3; order++) {
		parts[order] = outer_rule;
		parts[(order + 1) % 3] = inner_rule;
		parts[(order + 2) % 3] = exclusions;
		assert(snprintf(text, sizeof(text), "report_url=https://example.com/events\n%s%s%s",
			parts[0], parts[1], parts[2]) > 0);
		write_bytes(path, text, strlen(text));
		for (control = 0; control < 2; control++) {
			assert(control ? fm_config_load_control(config, path, err, sizeof(err)) :
				fm_config_load(config, path, err, sizeof(err)));
			assert(config->watch_count == 3 && config->exclude_count == 2);
			assert(fm_config_matches(config, ordinary, NULL));
			assert(fm_config_matches_event(config, ordinary, NULL, FSE_CREATE_FILE));
			assert(!fm_config_matches_event(config, ordinary, NULL, FSE_CONTENT_MODIFIED));
			assert(!fm_config_matches(config, excluded, NULL));
			assert(fm_config_matches(config, included, NULL));
			assert(fm_config_matches(config, item, NULL));
			assert(fm_config_matches_event(config, item, NULL, FSE_CONTENT_MODIFIED));
			assert(!fm_config_matches_event(config, item, NULL, FSE_CREATE_FILE));
			assert(!fm_config_matches(config, blocked, NULL));
			assert(!fm_config_matches_event(config, blocked, NULL, FSE_CONTENT_MODIFIED));
			assert(fm_config_matches(config, retained, NULL));
			assert(fm_config_matches_event(config, retained, NULL, FSE_CREATE_FILE));
			assert(fm_config_matches_event(config, retained, NULL, FSE_DELETE));
			assert(!fm_config_matches(config, sibling, NULL));
			assert(!fm_config_matches_event(config, sibling, NULL, FSE_CONTENT_MODIFIED));
			/* Re-inclusion is bounded by path components, not a string prefix. */
			assert(snprintf(text, sizeof(text), "%s-sibling/item", excluded) > 0);
			assert(fm_config_matches_event(config, text, NULL, FSE_CREATE_FILE));
			/* Either included side may admit a rename/clone, but any finally
			 * excluded side vetoes it. An outside path is not an exclusion. */
			assert(fm_config_matches_event(config, ordinary, item, FSE_RENAME));
			assert(fm_config_matches_event(config, item, ordinary, FSE_RENAME));
			assert(!fm_config_matches_event(config, ordinary, ordinary, FSE_RENAME));
			assert(fm_config_matches_event(config, "/outside", item, FSE_CLONE));
			assert(fm_config_matches_event(config, item, "/outside", FSE_CLONE));
			assert(!fm_config_matches_event(config, blocked, item, FSE_RENAME));
			assert(!fm_config_matches_event(config, item, blocked, FSE_RENAME));
			assert(!fm_config_matches_event(config, sibling, retained, FSE_CLONE));
			assert(!fm_config_matches_event(config, retained, sibling, FSE_CLONE));
			assert(!fm_config_matches(config, item, blocked));
			assert(!fm_config_matches(config, blocked, item));
			assert(fm_config_matches(config, "/outside", item));
			assert(fm_config_matches(config, item, "/outside"));
			assert(!fm_config_matches(config, "/outside", "/also-outside"));
		}
	}
	/* An exact exclusion wins over an exact watch. A still deeper watch can
	 * re-include a subtree even when every ancestor is explicitly excluded. */
	assert(snprintf(text, sizeof(text),
		"watch_path=%s\nexclude_path=%s\nexclude_path=%s\n", included, root, included) > 0);
	rejects(config, path, root, text);
	assert(snprintf(text, sizeof(text),
		"watch_path=%s\nwatch_path=%s\nexclude_path=%s\nexclude_path=%s\n", included, kept, root, included) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!fm_config_matches(config, ordinary, NULL));
	assert(!fm_config_matches(config, item, NULL));
	assert(fm_config_matches(config, retained, NULL));
	/* A watched root itself is more specific than an ancestor exclusion. */
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", excluded) > 0);
	write_config(path, included, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(fm_config_matches(config, item, NULL));
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", included) > 0);
	rejects(config, path, included, text);
	assert(fm_config_load_control(config, path, err, sizeof(err)));
	assert(!fm_config_matches(config, item, NULL));
	assert(rmdir(kept) == 0);
	assert(rmdir(cache) == 0);
	assert(rmdir(included) == 0);
	assert(rmdir(excluded) == 0);
}

static void test_missing_watch_paths(FmConfig *config, const char *path, const char *root,
		const char *file, const char *directory) {
	char missing[2048], not_directory[2048], loop[2048], text[16384], err[256];
	char **warnings;
	FILE *stream;
	size_t i;
	assert(snprintf(missing, sizeof(missing), "%s/missing-watch", directory) > 0);
	assert(snprintf(not_directory, sizeof(not_directory), "%s/child", file) > 0);
	assert(snprintf(loop, sizeof(loop), "%s/loop-watch", directory) > 0);
	/* A missing directory, missing file and a non-directory parent are all
	 * skipped, with the original spelling and line number in each warning. */
	assert(snprintf(text, sizeof(text),
		"# missing-watch test\nreport_url=https://example.com/events\n"
		"watch_path=%s\nwatch_path=%s\n\nwatch_path=%s/future.txt\nwatch_path=%s\n",
		root, missing, missing, not_directory) > 0);
	write_bytes(path, text, strlen(text));
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!*err);
	assert(config->watch_count == 1 && !strcmp(config->watch_paths[0], root));
	assert(config->warning_count == 3);
	assert(strstr(config->warnings[0], "line 4:"));
	assert(strstr(config->warnings[0], missing));
	assert(strstr(config->warnings[1], "line 6:"));
	assert(snprintf(text, sizeof(text), "%s/future.txt", missing) > 0);
	assert(strstr(config->warnings[1], text));
	assert(strstr(config->warnings[2], "line 7:"));
	assert(strstr(config->warnings[2], not_directory));
	for (i = 0; i < config->warning_count; i++) {
		assert(strstr(config->warnings[i], "watch_path"));
		assert(strstr(config->warnings[i], "does not exist"));
		assert(strstr(config->warnings[i], "ignored until restart"));
	}
	assert(fm_config_matches(config, root, NULL));
	assert(!fm_config_matches(config, missing, NULL));
	assert(!fm_config_matches(config, "/outside", not_directory));
	/* A later fatal error must not overwrite an already loaded warning list,
	 * including when the failed replacement accumulated warnings first. */
	assert(snprintf(text, sizeof(text), "watch_path=%s\nunknown_key=value\n", missing) > 0);
	rejects(config, path, root, text);
	assert(config->warning_count == 3);
	assert(strstr(config->warnings[0], "line 4:"));
	/* Exclusions remain strict. Nested roots remain valid after missing ones
	 * are skipped, with the more specific root retaining its own event mask. */
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", missing) > 0);
	rejects(config, path, root, text);
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", not_directory) > 0);
	rejects(config, path, root, text);
	assert(snprintf(text, sizeof(text), "watch_path=%s\nwatch_path=%s/child\n", missing, root) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 2 && config->warning_count == 1);
	assert(fm_config_matches(config, root, NULL));
	/* Unresolvable paths for reasons other than absence remain errors. */
	assert(symlink(loop, loop) == 0);
	assert(snprintf(text, sizeof(text), "watch_path=%s\n", loop) > 0);
	rejects(config, path, root, text);
	assert(unlink(loop) == 0);
	/* All missing roots are valid, even with exclusions; the empty scope must
	 * never match the whole filesystem or an absent root created later. */
	assert(snprintf(text, sizeof(text), "watch_path=%s\nexclude_path=%s\n", not_directory, root) > 0);
	write_config(path, missing, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 0 && config->watch_paths == NULL);
	assert(config->warning_count == 2 && config->exclude_count == 1);
	assert(!fm_config_matches(config, "/", NULL));
	assert(!fm_config_matches(config, root, NULL));
	assert(!fm_config_matches(config, missing, NULL));
	assert(!fm_config_matches(config, "/outside", not_directory));
	assert(mkdir(missing, 0700) == 0);
	assert(!fm_config_matches(config, missing, NULL));
	/* Reload replaces warnings instead of accumulating them, and activates a
	 * formerly missing root only after resolving it again. */
	write_config(path, missing, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 1 && !strcmp(config->watch_paths[0], missing));
	assert(config->warning_count == 0 && config->warnings == NULL);
	assert(fm_config_matches(config, missing, NULL));
	assert(rmdir(missing) == 0);
	write_config(path, not_directory, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 0 && config->warning_count == 1);
	assert(strstr(config->warnings[0], not_directory));
	/* Count configured entries, including skipped ones, toward the 64 limit. */
	stream = fopen(path, "wb");
	assert(stream);
	assert(fprintf(stream, "report_url=https://example.com/events\nwatch_path=%s\n", root) > 0);
	for (i = 1; i < FM_CONFIG_MAX_WATCH_PATHS; i++) {
		assert(fprintf(stream, "watch_path=%s/missing-%zu\n", directory, i) > 0);
	}
	assert(fclose(stream) == 0);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 1 && config->warning_count == FM_CONFIG_MAX_WATCH_PATHS - 1);
	warnings = config->warnings;
	stream = fopen(path, "ab");
	assert(stream);
	assert(fprintf(stream, "watch_path=%s/missing-over-limit\n", directory) > 0);
	assert(fclose(stream) == 0);
	assert(!fm_config_load(config, path, err, sizeof(err)));
	assert(strstr(err, "at most"));
	assert(config->warnings == warnings);
	assert(config->watch_count == 1 && config->warning_count == FM_CONFIG_MAX_WATCH_PATHS - 1);
	stream = fopen(path, "wb");
	assert(stream);
	assert(fputs("report_url=https://example.com/events\n", stream) >= 0);
	for (i = 0; i < FM_CONFIG_MAX_WATCH_PATHS; i++) {
		assert(fprintf(stream, "watch_path=%s/missing-%zu\n", directory, i) > 0);
	}
	assert(fclose(stream) == 0);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 0 && config->warning_count == FM_CONFIG_MAX_WATCH_PATHS);
	warnings = config->warnings;
	stream = fopen(path, "ab");
	assert(stream);
	assert(fprintf(stream, "watch_path=%s/missing-over-limit\n", directory) > 0);
	assert(fclose(stream) == 0);
	assert(!fm_config_load(config, path, err, sizeof(err)));
	assert(strstr(err, "at most"));
	assert(config->watch_count == 0 && config->warnings == warnings);
	assert(config->warning_count == FM_CONFIG_MAX_WATCH_PATHS);
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_count == 1 && config->warning_count == 0 && config->warnings == NULL);
}

static void test_uid_filters(FmConfig *config, const char *path, const char *root) {
	static const char *keys[] = {"watch_uid", "exclude_uid"};
	static const char *bad[] = {
		"", "   ", "-1", "+1", "0x1f5", "0X0", "root", "mobile",
		"0,501", "0 501", "501 # user", "501.0", "1e3", "4294967296",
		"18446744073709551615", "999999999999999999999999999999999999"
	};
	char text[1024], err[256];
	size_t i, j;
	uint32_t *watch_uids, *exclude_uids;
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_uids && !config->watch_uid_count);
	assert(!config->exclude_uids && !config->exclude_uid_count);
	assert(fm_config_matches_uid(config, 0, true));
	assert(fm_config_matches_uid(config, 501, true));
	assert(fm_config_matches_uid(config, UINT32_MAX, true));
	assert(fm_config_matches_uid(config, 0, false));
	/* Numeric zero is a real root UID; unavailable attribution is separate.
	 * Decimal leading zeros and repeated lines identify the same UID. */
	write_config(path, root, "watch_uid= 0 \nwatch_uid=501\nwatch_uid=4294967295\n"
		"watch_uid=000501\nwatch_uid=0\nexclude_uid=502\nexclude_uid=502\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_uid_count == 3 && config->exclude_uid_count == 1);
	assert(config->watch_uids[0] == 0 && config->watch_uids[1] == 501);
	assert(config->watch_uids[2] == UINT32_MAX && config->exclude_uids[0] == 502);
	assert(fm_config_matches_uid(config, 0, true));
	assert(fm_config_matches_uid(config, 501, true));
	assert(fm_config_matches_uid(config, UINT32_MAX, true));
	assert(!fm_config_matches_uid(config, 502, true));
	assert(!fm_config_matches_uid(config, 503, true));
	assert(!fm_config_matches_uid(config, 0, false));
	assert(!fm_config_matches_uid(config, 501, false));
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		for (j = 0; j < sizeof(bad) / sizeof(bad[0]); j++) {
			assert(snprintf(text, sizeof(text), "%s=%s\n", keys[i], bad[j]) > 0);
			rejects(config, path, root, text);
		}
	}
	/* A failure after building both replacement lists must be transactional. */
	rejects(config, path, root, "watch_uid=777\nexclude_uid=888\nexclude_uid=-1\n");
	rejects(config, path, root, "watch_uid=777\nexclude_uid=888\nunknown_uid_option=true\n");
	watch_uids = config->watch_uids;
	exclude_uids = config->exclude_uids;
	write_config(path, root, "watch_uid=777\nexclude_uid=888\nwatch_uid=4294967296\n");
	assert(!fm_config_load_control(config, path, err, sizeof(err)));
	assert(*err && config->watch_uids == watch_uids && config->exclude_uids == exclude_uids);
	assert(config->watch_uid_count == 3 && config->exclude_uid_count == 1);
	assert(config->watch_uids[0] == 0 && config->exclude_uids[0] == 502);
	/* Control parsing uses the same numeric rules and exclusion precedence. */
	write_config(path, root, "watch_uid=0\nwatch_uid=501\nwatch_uid=4294967295\n"
		"exclude_uid=0\nexclude_uid=501\nexclude_uid=0\n");
	assert(fm_config_load_control(config, path, err, sizeof(err)));
	assert(config->watch_uid_count == 3 && config->exclude_uid_count == 2);
	assert(!fm_config_matches_uid(config, 0, true));
	assert(!fm_config_matches_uid(config, 501, true));
	assert(fm_config_matches_uid(config, UINT32_MAX, true));
	assert(!fm_config_matches_uid(config, UINT32_MAX, false));
	/* With no allowlist, exclusions still reject unknown attribution. */
	write_config(path, root, "exclude_uid=0\nexclude_uid=4294967295\nexclude_uid=0\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_uids && !config->watch_uid_count);
	assert(config->exclude_uid_count == 2 && config->exclude_uids[1] == UINT32_MAX);
	assert(!fm_config_matches_uid(config, 0, true));
	assert(!fm_config_matches_uid(config, UINT32_MAX, true));
	assert(fm_config_matches_uid(config, 501, true));
	assert(!fm_config_matches_uid(config, 0, false));
	/* Reload removes old allocations and restores default unknown handling. */
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_uids && !config->watch_uid_count);
	assert(!config->exclude_uids && !config->exclude_uid_count);
	assert(fm_config_matches_uid(config, 0, false));
	/* Leave allocations live so the caller checks free/reset and double-free. */
	write_config(path, root, "watch_uid=0\nexclude_uid=501\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
}

static void test_gid_filters(FmConfig *config, const char *path, const char *root) {
	static const char *keys[] = {"watch_gid", "exclude_gid"};
	static const char *bad[] = {
		"", "   ", "-1", "+1", "0x1f5", "0X0", "root", "mobile",
		"0,501", "0 501", "501 # group", "501.0", "1e3", "4294967296",
		"18446744073709551615", "999999999999999999999999999999999999"
	};
	char text[1024], err[256];
	size_t i, j;
	uint32_t *watch_gids, *exclude_gids;
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_gids && !config->watch_gid_count);
	assert(!config->exclude_gids && !config->exclude_gid_count);
	assert(fm_config_matches_gid(config, 0, true));
	assert(fm_config_matches_gid(config, 501, true));
	assert(fm_config_matches_gid(config, UINT32_MAX, true));
	assert(fm_config_matches_gid(config, 0, false));
	write_config(path, root, "watch_gid= 0 \nwatch_gid=501\nwatch_gid=4294967295\n"
		"watch_gid=000501\nwatch_gid=0\nexclude_gid=502\nexclude_gid=502\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_gid_count == 3 && config->exclude_gid_count == 1);
	assert(config->watch_gids[0] == 0 && config->watch_gids[1] == 501);
	assert(config->watch_gids[2] == UINT32_MAX && config->exclude_gids[0] == 502);
	assert(fm_config_matches_gid(config, 0, true));
	assert(fm_config_matches_gid(config, 501, true));
	assert(fm_config_matches_gid(config, UINT32_MAX, true));
	assert(!fm_config_matches_gid(config, 502, true));
	assert(!fm_config_matches_gid(config, 503, true));
	assert(!fm_config_matches_gid(config, 0, false));
	assert(!fm_config_matches_gid(config, 501, false));
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		for (j = 0; j < sizeof(bad) / sizeof(bad[0]); j++) {
			assert(snprintf(text, sizeof(text), "%s=%s\n", keys[i], bad[j]) > 0);
			rejects(config, path, root, text);
		}
	}
	/* A failure after building both replacement lists is transactional. */
	rejects(config, path, root, "watch_gid=777\nexclude_gid=888\nexclude_gid=-1\n");
	rejects(config, path, root, "watch_gid=777\nexclude_gid=888\nunknown_gid_option=true\n");
	watch_gids = config->watch_gids;
	exclude_gids = config->exclude_gids;
	write_config(path, root, "watch_gid=777\nexclude_gid=888\nwatch_gid=4294967296\n");
	assert(!fm_config_load_control(config, path, err, sizeof(err)));
	assert(*err && config->watch_gids == watch_gids && config->exclude_gids == exclude_gids);
	assert(config->watch_gid_count == 3 && config->exclude_gid_count == 1);
	/* Control parsing uses the same range and precedence rules. */
	write_config(path, root, "watch_gid=0\nwatch_gid=501\nwatch_gid=4294967295\n"
		"exclude_gid=0\nexclude_gid=501\nexclude_gid=0\n");
	assert(fm_config_load_control(config, path, err, sizeof(err)));
	assert(config->watch_gid_count == 3 && config->exclude_gid_count == 2);
	assert(!fm_config_matches_gid(config, 0, true));
	assert(!fm_config_matches_gid(config, 501, true));
	assert(fm_config_matches_gid(config, UINT32_MAX, true));
	assert(!fm_config_matches_gid(config, UINT32_MAX, false));
	write_config(path, root, "exclude_gid=0\nexclude_gid=4294967295\nexclude_gid=0\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_gids && !config->watch_gid_count);
	assert(config->exclude_gid_count == 2 && config->exclude_gids[1] == UINT32_MAX);
	assert(!fm_config_matches_gid(config, 0, true));
	assert(!fm_config_matches_gid(config, UINT32_MAX, true));
	assert(fm_config_matches_gid(config, 501, true));
	assert(!fm_config_matches_gid(config, 0, false));
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_gids && !config->watch_gid_count);
	assert(!config->exclude_gids && !config->exclude_gid_count);
	assert(fm_config_matches_gid(config, 0, false));
	/* Leave allocations live so reset/free paths are covered by main cleanup. */
	write_config(path, root, "watch_gid=0\nexclude_gid=501\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
}

static void test_ppid_filters(FmConfig *config, const char *path, const char *root) {
	static const char *keys[] = {"watch_ppid", "exclude_ppid"};
	static const char *bad[] = {
		"", "   ", "0", "0000", "-1", "+1", "0x2a", "launchd", "/sbin/launchd",
		"1,42", "1 42", "42 # parent", "42.0", "1e3", "2147483648", "4294967295",
		"18446744073709551615", "999999999999999999999999999999999999"
	};
	char text[1024], err[256];
	size_t i, j;
	uint32_t *watch_ppids, *exclude_ppids;
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_ppids && !config->watch_ppid_count);
	assert(!config->exclude_ppids && !config->exclude_ppid_count);
	assert(fm_config_matches_ppid(config, 0));
	assert(fm_config_matches_ppid(config, -1));
	assert(fm_config_matches_ppid(config, INT_MIN));
	assert(fm_config_matches_ppid(config, 1));
	assert(fm_config_matches_ppid(config, INT_MAX));
	/* Direct parent PIDs are positive signed integers, unlike UID/GID zero.
	 * Leading zeros and repeated lines identify the same parent PID. */
	write_config(path, root, "watch_ppid= 1 \nwatch_ppid=42\nwatch_ppid=2147483647\n"
		"watch_ppid=00042\nwatch_ppid=1\nexclude_ppid=43\nexclude_ppid=43\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->watch_ppid_count == 3 && config->exclude_ppid_count == 1);
	assert(config->watch_ppids[0] == 1 && config->watch_ppids[1] == 42);
	assert(config->watch_ppids[2] == INT_MAX && config->exclude_ppids[0] == 43);
	assert(fm_config_matches_ppid(config, 1));
	assert(fm_config_matches_ppid(config, 42));
	assert(fm_config_matches_ppid(config, INT_MAX));
	assert(!fm_config_matches_ppid(config, 43));
	assert(!fm_config_matches_ppid(config, 44));
	assert(!fm_config_matches_ppid(config, 0));
	assert(!fm_config_matches_ppid(config, -1));
	assert(!fm_config_matches_ppid(config, INT_MIN));
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		for (j = 0; j < sizeof(bad) / sizeof(bad[0]); j++) {
			assert(snprintf(text, sizeof(text), "%s=%s\n", keys[i], bad[j]) > 0);
			rejects(config, path, root, text);
			/* Control commands must reject the same invalid values without
			 * replacing the running configuration's lists. */
			watch_ppids = config->watch_ppids;
			exclude_ppids = config->exclude_ppids;
			assert(!fm_config_load_control(config, path, err, sizeof(err)));
			assert(*err && config->watch_ppids == watch_ppids && config->exclude_ppids == exclude_ppids);
			assert(config->watch_ppid_count == 3 && config->exclude_ppid_count == 1);
		}
	}
	/* Both replacement lists may be allocated before a later error occurs. */
	rejects(config, path, root, "watch_ppid=777\nexclude_ppid=888\nexclude_ppid=0\n");
	rejects(config, path, root, "watch_ppid=777\nexclude_ppid=888\nunknown_ppid_option=true\n");
	watch_ppids = config->watch_ppids;
	exclude_ppids = config->exclude_ppids;
	write_config(path, root, "watch_ppid=777\nexclude_ppid=888\nwatch_ppid=2147483648\n");
	assert(!fm_config_load_control(config, path, err, sizeof(err)));
	assert(*err && config->watch_ppids == watch_ppids && config->exclude_ppids == exclude_ppids);
	assert(config->watch_ppid_count == 3 && config->exclude_ppid_count == 1);
	assert(config->watch_ppids[0] == 1 && config->exclude_ppids[0] == 43);
	/* Exclusion wins over the allowlist in control and daemon parsing. */
	write_config(path, root, "watch_ppid=1\nwatch_ppid=42\nwatch_ppid=2147483647\n"
		"exclude_ppid=1\nexclude_ppid=42\nexclude_ppid=1\n");
	assert(fm_config_load_control(config, path, err, sizeof(err)));
	assert(config->watch_ppid_count == 3 && config->exclude_ppid_count == 2);
	assert(!fm_config_matches_ppid(config, 1));
	assert(!fm_config_matches_ppid(config, 42));
	assert(fm_config_matches_ppid(config, INT_MAX));
	assert(!fm_config_matches_ppid(config, 0));
	write_config(path, root, "exclude_ppid=1\nexclude_ppid=2147483647\nexclude_ppid=1\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_ppids && !config->watch_ppid_count);
	assert(config->exclude_ppid_count == 2 && config->exclude_ppids[1] == INT_MAX);
	assert(!fm_config_matches_ppid(config, 1));
	assert(!fm_config_matches_ppid(config, INT_MAX));
	assert(fm_config_matches_ppid(config, 42));
	assert(!fm_config_matches_ppid(config, 0));
	assert(!fm_config_matches_ppid(config, -1));
	/* Reload releases both old lists and restores unknown-parent handling. */
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(!config->watch_ppids && !config->watch_ppid_count);
	assert(!config->exclude_ppids && !config->exclude_ppid_count);
	assert(fm_config_matches_ppid(config, 0));
	assert(fm_config_matches_ppid(config, -1));
	/* Leave every identity dimension live for the caller's free/reset checks. */
	write_config(path, root, "watch_uid=0\nexclude_uid=501\nwatch_gid=0\nexclude_gid=501\n"
		"watch_ppid=1\nexclude_ppid=42\n");
	assert(fm_config_load(config, path, err, sizeof(err)));
}

static void test_process_exclusions(FmConfig *config, const char *path, const char *root) {
	char err[256];
	const char *bad[] = {"", "   ", "/bin/zsh", "bin/zsh", "zsh*", "zs?", "[z]sh", "zsh]"};
	char text[1024];
	size_t i;
#if __APPLE__
	const char *backend = "devfsev";
	const char *no_process_backend = "fsevapi";
#else
	const char *backend = "fanotify";
	const char *no_process_backend = "inotify";
#endif
	assert(snprintf(text, sizeof(text),
		"backend=%s\nexclude_process= zsh \nexclude_process=cfprefsd\n"
		"exclude_process=zsh\nexclude_process=My App\nexclude_process=ZSH\n", backend) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->exclude_process_count == 4);
	assert(!strcmp(config->exclude_processes[0], "zsh"));
	assert(!strcmp(config->exclude_processes[1], "cfprefsd"));
	assert(!strcmp(config->exclude_processes[2], "My App"));
	assert(!strcmp(config->exclude_processes[3], "ZSH"));
	assert(fm_config_excludes_process(config, "zsh"));
	assert(fm_config_excludes_process(config, "cfprefsd"));
	assert(fm_config_excludes_process(config, "My App"));
	assert(fm_config_excludes_process(config, "ZSH"));
	assert(!fm_config_excludes_process(config, "Zsh"));
	assert(!fm_config_excludes_process(config, "zsh-helper"));
	assert(!fm_config_excludes_process(config, "my-zsh"));
	assert(!fm_config_excludes_process(config, "/bin/zsh"));
	assert(!fm_config_excludes_process(config, ""));
	assert(!fm_config_excludes_process(config, NULL));
	assert(!fm_config_excludes_process(NULL, "zsh"));
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		assert(snprintf(text, sizeof(text), "backend=%s\nexclude_process=%s\n", backend, bad[i]) > 0);
		rejects(config, path, root, text);
		assert(fm_config_excludes_process(config, "zsh"));
	}
	/* Failed loads also roll back a partially constructed replacement list. */
	assert(snprintf(text, sizeof(text),
		"backend=%s\nexclude_process=launchd\nexclude_process=/bin/zsh\n", backend) > 0);
	rejects(config, path, root, text);
	assert(fm_config_excludes_process(config, "zsh"));
	assert(!fm_config_excludes_process(config, "launchd"));
	assert(snprintf(text, sizeof(text),
		"backend=%s\nexclude_process=launchd\nunknown_process_option=true\n", backend) > 0);
	rejects(config, path, root, text);
	assert(fm_config_excludes_process(config, "zsh"));
	assert(!fm_config_excludes_process(config, "launchd"));
	/* Backends without process attribution may leave the event name unknown;
	 * the configuration remains portable and the rule applies when a name exists. */
	assert(snprintf(text, sizeof(text), "backend=%s\nexclude_process=zsh\n", no_process_backend) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(fm_config_excludes_process(config, "zsh"));
	assert(fm_config_load_control(config, path, err, sizeof(err)));
	assert(fm_config_excludes_process(config, "zsh"));
	/* Reload replaces, rather than appends, the previous exclusion list. */
	assert(snprintf(text, sizeof(text), "backend=%s\nexclude_process=launchd\n", backend) > 0);
	write_config(path, root, text);
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->exclude_process_count == 1);
	assert(fm_config_excludes_process(config, "launchd"));
	assert(!fm_config_excludes_process(config, "zsh"));
	write_config(path, root, "");
	assert(fm_config_load(config, path, err, sizeof(err)));
	assert(config->exclude_process_count == 0 && config->exclude_processes == NULL);
	assert(!fm_config_excludes_process(config, "launchd"));
}

int main(void) {
	char template[] = "/tmp/fsmod-config-test-XXXXXX";
	char *created = mkdtemp(template);
	char *directory;
	char root[1024], child[1024], excluded[1024], excluded2[1024], excluded_file[1024];
	char other[1024], file[1024];
	char link_path[1024], config_file[1024], path[2048], text[16384], err[256];
	FmConfig config;
	FILE *stream;
	size_t i;
	assert(created);
	directory = realpath(created, NULL);
	assert(directory);
	assert(snprintf(root, sizeof(root), "%s/root", directory) > 0);
	assert(snprintf(child, sizeof(child), "%s/root/child", directory) > 0);
	assert(snprintf(excluded, sizeof(excluded), "%s/root/private", directory) > 0);
	assert(snprintf(excluded2, sizeof(excluded2), "%s/root/cache", directory) > 0);
	assert(snprintf(excluded_file, sizeof(excluded_file), "%s/root/secret.txt", directory) > 0);
	assert(snprintf(other, sizeof(other), "%s/other", directory) > 0);
	assert(snprintf(file, sizeof(file), "%s/other/测试.txt", directory) > 0);
	assert(snprintf(link_path, sizeof(link_path), "%s/root-link", directory) > 0);
	assert(snprintf(config_file, sizeof(config_file), "%s/fsmod.conf", directory) > 0);
	assert(mkdir(root, 0700) == 0);
	assert(mkdir(child, 0700) == 0);
	assert(mkdir(excluded, 0700) == 0);
	assert(mkdir(excluded2, 0700) == 0);
	assert(mkdir(other, 0700) == 0);
	write_bytes(file, "hello", 5);
	write_bytes(excluded_file, "secret", 6);
	assert(symlink(root, link_path) == 0);
	fm_config_init(&config);
	assert(config.daemonize && !config.log_events);
	assert(config.report_timeout_seconds == 10 && config.report_retries == 3);
	assert(config.retry_delay_seconds == 2 && config.shutdown_timeout_seconds == 15);
	assert(config.queue_capacity == 1024);
	assert(!strcmp(config.pid_file, "/tmp/fsmod.pid"));
	assert(!strcmp(config.log_file, "/tmp/fsmod.log"));
#if __APPLE__
	assert(!strcmp(config.backend, "fsevapi"));
#else
	assert(!strcmp(config.backend, "inotify"));
#endif
	assert(fm_path_matches("/a", "/a"));
	assert(fm_path_matches("/a/b", "/a"));
	assert(fm_path_matches("/a", "/a/"));
	assert(fm_path_matches("/a", "/"));
	assert(!fm_path_matches("/ab", "/a"));
	assert(!fm_path_matches("/a", "/ab"));
	assert(!fm_path_matches(NULL, "/a"));
	assert(!fm_path_matches("a", "/a"));
	assert(!fm_path_matches("/a", "a"));
	assert(snprintf(text, sizeof(text),
		" # comment\r\nreport_url = https://example.com/events?name=测试\r\n"
		"report_token=秘密-token\r\nwatch_path=%s/./\r\nwatch_path=%s\r\n"
		"exclude_path=%s\r\nexclude_path=%s\r\nexclude_path=%s\r\nexclude_path=%s\r\n"
		"daemonize=false\r\nlog_events=true\r\nreport_retries=0\r\nretry_delay_seconds=0\r\n",
		root, file, excluded, excluded2, excluded_file, excluded) > 0);
	write_bytes(config_file, text, strlen(text));
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(config.watch_count == 2 && config.exclude_count == 3);
	assert(!strcmp(config.watch_paths[0], root));
	assert(!strcmp(config.watch_paths[1], file));
	assert(!strcmp(config.report_token, "秘密-token"));
	assert(!config.daemonize && config.log_events);
	assert(config.report_retries == 0 && config.retry_delay_seconds == 0);
	assert(snprintf(path, sizeof(path), "%s/deleted.txt", root) > 0);
	assert(fm_config_matches(&config, path, NULL));
	assert(fm_config_matches(&config, "/outside/file", path));
	assert(fm_config_matches(&config, file, NULL));
	assert(!fm_config_matches(&config, "/outside/file", "/outside/new"));
	assert(!fm_config_matches(&config, path, excluded));
	assert(!fm_config_matches(&config, excluded, path));
	assert(snprintf(path, sizeof(path), "%s/nested/item", excluded2) > 0);
	assert(!fm_config_matches(&config, path, NULL));
	assert(!fm_config_matches(&config, "/outside/file", path));
	assert(!fm_config_matches(&config, path, "/outside/new"));
	assert(snprintf(path, sizeof(path), "%s-sibling/item", excluded2) > 0);
	assert(fm_config_matches(&config, path, NULL));
	assert(!fm_config_matches(&config, excluded_file, NULL));
	assert(snprintf(path, sizeof(path), "%s-sibling/file", root) > 0);
	assert(!fm_config_matches(&config, path, NULL));
	assert(snprintf(text, sizeof(text), "watch_path=%s\n", child) > 0);
	write_config(config_file, root, text);
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(config.watch_count == 2);
	assert(fm_config_matches(&config, child, NULL));
	assert(snprintf(text, sizeof(text), "watch_path=%s/./\n", root) > 0);
	rejects(&config, config_file, root, text);
	assert(snprintf(text, sizeof(text), "watch_path=%s\n", link_path) > 0);
	rejects(&config, config_file, root, text);
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", root) > 0);
	rejects(&config, config_file, root, text);
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", directory) > 0);
	write_config(config_file, root, text);
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(fm_config_matches(&config, root, NULL));
	assert(snprintf(text, sizeof(text), "exclude_path=%s/\n", link_path) > 0);
	rejects(&config, config_file, root, text);
	write_config(config_file, root, "exclude_path=/\n");
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(fm_config_matches(&config, root, NULL));
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", file) > 0);
	rejects(&config, config_file, file, text);
	assert(snprintf(text, sizeof(text),
		"watch_path=%s\nexclude_path=%s\nexclude_path=%s\n", file, root, file) > 0);
	rejects(&config, config_file, root, text);
	/* One excluded root is allowed when another root remains usable. */
	assert(snprintf(text, sizeof(text), "watch_path=%s\nexclude_path=%s\n", file, root) > 0);
	write_config(config_file, root, text);
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(!fm_config_matches(&config, root, NULL));
	assert(fm_config_matches(&config, file, NULL));
	/* A bad scope must not prevent stopping an already running service. */
	assert(snprintf(text, sizeof(text), "exclude_path=%s\n", root) > 0);
	write_config(config_file, root, text);
	assert(!fm_config_load(&config, config_file, err, sizeof(err)));
	assert(strstr(err, "all watch paths are covered"));
	assert(fm_config_load_control(&config, config_file, err, sizeof(err)));
	rejects(&config, config_file, root, "unexpected=value\n");
	rejects(&config, config_file, root, "daemonize=true\ndaemonize=false\n");
	rejects(&config, config_file, root, "report_token=secret\rinjected\n");
	rejects(&config, config_file, root, "pid_file=relative.pid\n");
	rejects(&config, config_file, root, "log_file=relative.log\n");
	rejects(&config, config_file, root, "watch_path=relative\n");
	rejects(&config, config_file, root, "backend=unknown\n");
	rejects(&config, config_file, root, "daemonize=1\n");
	rejects(&config, config_file, root, "report_timeout_seconds=0\n");
	rejects(&config, config_file, root, "report_retries=101\n");
	rejects(&config, config_file, root, "report_retries=-1\n");
	rejects(&config, config_file, root, "report_retries=+1\n");
	rejects(&config, config_file, root, "report_retries=999999999999999999999999\n");
	rejects(&config, config_file, root, "retry_delay_seconds=3601\n");
	rejects(&config, config_file, root, "shutdown_timeout_seconds=0\n");
	rejects(&config, config_file, root, "queue_capacity=1000001\n");
	{
		const char *urls[] = {"ftp://example.com/events", "https://user:password@example.com/events",
			"http:///events", "http://:123/events", "http://example.com:0/events",
			"http://example.com:65536/events", "http://example.com/a b", "http://example.com/\\file"};
		for (i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
			assert(snprintf(text, sizeof(text), "report_url=%s\nwatch_path=%s\n", urls[i], root) > 0);
			write_bytes(config_file, text, strlen(text));
			assert(!fm_config_load(&config, config_file, err, sizeof(err)));
		}
	}
	write_bytes(config_file, "report_url=https://example.com\n", strlen("report_url=https://example.com\n"));
	assert(!fm_config_load(&config, config_file, err, sizeof(err)));
	assert(snprintf(text, sizeof(text), "watch_path=%s\n", root) > 0);
	write_bytes(config_file, text, strlen(text));
	assert(!fm_config_load(&config, config_file, err, sizeof(err)));
	{
		const char bad[] = "report_token=a\0b\n";
		write_bytes(config_file, bad, sizeof(bad) - 1);
		assert(!fm_config_load(&config, config_file, err, sizeof(err)));
	}
	stream = fopen(config_file, "wb");
	assert(stream);
	for (i = 0; i <= FM_CONFIG_MAX_LINE; i++) {
		assert(fputc('x', stream) != EOF);
	}
	assert(fputc('\n', stream) != EOF);
	assert(fclose(stream) == 0);
	assert(!fm_config_load(&config, config_file, err, sizeof(err)));
	assert(strstr(err, "exceeds"));
	stream = fopen(config_file, "wb");
	assert(stream);
	assert(fputs("report_url=http://[::1]:8080/events\n", stream) >= 0);
	for (i = 0; i <= FM_CONFIG_MAX_WATCH_PATHS; i++) {
		assert(snprintf(path, sizeof(path), "%s/limit-%zu", directory, i) > 0);
		assert(mkdir(path, 0700) == 0);
		if (i < FM_CONFIG_MAX_WATCH_PATHS) {
			assert(fprintf(stream, "watch_path=%s\n", path) > 0);
		}
	}
	assert(fclose(stream) == 0);
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(config.watch_count == FM_CONFIG_MAX_WATCH_PATHS);
	stream = fopen(config_file, "ab");
	assert(stream);
	assert(fprintf(stream, "watch_path=%s/limit-%d\n", directory, FM_CONFIG_MAX_WATCH_PATHS) > 0);
	assert(fclose(stream) == 0);
	assert(!fm_config_load(&config, config_file, err, sizeof(err)));
	assert(config.watch_count == FM_CONFIG_MAX_WATCH_PATHS);
	assert(strstr(err, "at most"));
	for (i = 0; i <= FM_CONFIG_MAX_WATCH_PATHS; i++) {
		assert(snprintf(path, sizeof(path), "%s/limit-%zu", directory, i) > 0);
		assert(rmdir(path) == 0);
	}
	/* Check a valid upper bound and replacement of the previous allocations. */
	write_config(config_file, root, "report_timeout_seconds=3600\nreport_retries=100\n"
		"retry_delay_seconds=3600\nshutdown_timeout_seconds=3600\nqueue_capacity=1000000");
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(config.watch_count == 1 && config.exclude_count == 0);
	assert(config.report_token == NULL);
	assert(config.queue_capacity == 1000000);
	assert(snprintf(text, sizeof(text), "pid_file=%s/../fsmod.pid\nlog_file=%s/fsmod.log\n", child, link_path) > 0);
	write_config(config_file, root, text);
	assert(fm_config_load(&config, config_file, err, sizeof(err)));
	assert(snprintf(path, sizeof(path), "%s/fsmod.pid", root) > 0);
	assert(!strcmp(config.pid_file, path));
	assert(snprintf(path, sizeof(path), "%s/fsmod.log", root) > 0);
	assert(!strcmp(config.log_file, path));
	assert(snprintf(text, sizeof(text), "pid_file=%s/shared\nlog_file=%s/shared\n", root, link_path) > 0);
	rejects(&config, config_file, root, text);
	assert(snprintf(text, sizeof(text), "pid_file=%s\n", file) > 0);
	rejects(&config, config_file, file, text);
	assert(snprintf(path, sizeof(path), "%s/runtime-alias", other) > 0);
	assert(symlink(file, path) == 0);
	assert(snprintf(text, sizeof(text), "pid_file=%s\nlog_file=%s\n", file, path) > 0);
	rejects(&config, config_file, root, text);
	assert(unlink(path) == 0);
	assert(link(file, path) == 0);
	assert(snprintf(text, sizeof(text), "pid_file=%s\nlog_file=%s\n", file, path) > 0);
	rejects(&config, config_file, root, text);
	assert(unlink(path) == 0);
	assert(snprintf(text, sizeof(text), "pid_file=%s/no-parent/runtime.pid\n", root) > 0);
	rejects(&config, config_file, root, text);
	write_config(config_file, "/nonexistent-fsmod-watch-directory", "exclude_path=/nonexistent-fsmod-excluded-directory\n");
	assert(!fm_config_load(&config, config_file, err, sizeof(err)));
	assert(fm_config_load_control(&config, config_file, err, sizeof(err)));
	assert(!strcmp(config.watch_paths[0], "/nonexistent-fsmod-watch-directory"));
	assert(!strcmp(config.exclude_paths[0], "/nonexistent-fsmod-excluded-directory"));
	test_missing_watch_paths(&config, config_file, root, file, directory);
	test_watch_events(&config, config_file, root, child, file, directory);
	test_nested_watch_paths(&config, config_file, root);
	test_process_exclusions(&config, config_file, root);
	test_uid_filters(&config, config_file, root);
	test_gid_filters(&config, config_file, root);
	test_ppid_filters(&config, config_file, root);
	fm_config_free(&config);
	assert(config.watch_uid_count == 0 && config.watch_uids == NULL);
	assert(config.exclude_uid_count == 0 && config.exclude_uids == NULL);
	assert(config.watch_gid_count == 0 && config.watch_gids == NULL);
	assert(config.exclude_gid_count == 0 && config.exclude_gids == NULL);
	assert(config.watch_ppid_count == 0 && config.watch_ppids == NULL);
	assert(config.exclude_ppid_count == 0 && config.exclude_ppids == NULL);
	assert(config.exclude_process_count == 0 && config.exclude_processes == NULL);
	assert(config.warning_count == 0 && config.warnings == NULL);
	assert(config.watch_event_masks == NULL);
	fm_config_free(&config);
	assert(unlink(config_file) == 0);
	assert(unlink(file) == 0);
	assert(unlink(excluded_file) == 0);
	assert(unlink(link_path) == 0);
	assert(rmdir(child) == 0);
	assert(rmdir(excluded) == 0);
	assert(rmdir(excluded2) == 0);
	assert(rmdir(root) == 0);
	assert(rmdir(other) == 0);
	assert(rmdir(directory) == 0);
	free(directory);
	puts("config tests passed");
	return 0;
}
