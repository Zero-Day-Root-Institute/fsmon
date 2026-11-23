#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "config.h"
#include "backend/devfsev.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum config_key {
	KEY_REPORT_URL, KEY_REPORT_TOKEN, KEY_BACKEND, KEY_PID_FILE, KEY_LOG_FILE,
	KEY_REPORT_TIMEOUT, KEY_REPORT_RETRIES, KEY_RETRY_DELAY, KEY_SHUTDOWN_TIMEOUT,
	KEY_QUEUE_CAPACITY, KEY_DAEMONIZE, KEY_LOG_EVENTS, KEY_WATCH_PATH, KEY_EXCLUDE_PATH,
	KEY_EXCLUDE_PROCESS, KEY_WATCH_EVENTS, KEY_WATCH_UID, KEY_EXCLUDE_UID,
	KEY_WATCH_GID, KEY_EXCLUDE_GID,
	KEY_WATCH_PPID, KEY_EXCLUDE_PPID,
	KEY_UNKNOWN
};

static void config_error(char *err, size_t size, size_t line, const char *format, ...) {
	va_list ap;
	int prefix = 0;
	if (!err || !size) {
		return;
	}
	if (line) {
		prefix = snprintf(err, size, "line %zu: ", line);
		if (prefix < 0 || (size_t)prefix >= size) {
			err[size - 1] = '\0';
			return;
		}
	}
	va_start(ap, format);
	(void)vsnprintf(err + prefix, size - (size_t)prefix, format, ap);
	va_end(ap);
}

static char *config_strdup(const char *value) {
	size_t length = strlen(value) + 1;
	char *copy = malloc(length);
	if (copy) {
		memcpy(copy, value, length);
	}
	return copy;
}

void fm_config_init(FmConfig *config) {
	if (!config) {
		return;
	}
	memset(config, 0, sizeof(*config));
#if TARGET_IOS
	config->backend = config_strdup("devfsev");
	/* iOS services stay in the foreground and are supervised by launchd. */
	config->daemonize = false;
#elif __APPLE__
	config->backend = config_strdup("fsevapi");
#else
	config->backend = config_strdup("inotify");
#endif
	config->pid_file = config_strdup("/tmp/fsmod.pid");
	config->log_file = config_strdup("/tmp/fsmod.log");
	config->report_timeout_seconds = 10;
	config->report_retries = 3;
	config->retry_delay_seconds = 2;
	config->shutdown_timeout_seconds = 15;
	config->queue_capacity = 1024;
#if !TARGET_IOS
	config->daemonize = true;
#endif
	config->log_events = false;
}

void fm_config_free(FmConfig *config) {
	size_t i;
	if (!config) {
		return;
	}
	free(config->report_url);
	free(config->report_token);
	free(config->backend);
	free(config->pid_file);
	free(config->log_file);
	for (i = 0; i < config->watch_count; i++) {
		free(config->watch_paths[i]);
	}
	free(config->watch_paths);
	free(config->watch_event_masks);
	for (i = 0; i < config->warning_count; i++) {
		free(config->warnings[i]);
	}
	free(config->warnings);
	for (i = 0; i < config->exclude_count; i++) {
		free(config->exclude_paths[i]);
	}
	free(config->exclude_paths);
	for (i = 0; i < config->exclude_process_count; i++) {
		free(config->exclude_processes[i]);
	}
	free(config->exclude_processes);
	free(config->watch_uids);
	free(config->exclude_uids);
	free(config->watch_gids);
	free(config->exclude_gids);
	free(config->watch_ppids);
	free(config->exclude_ppids);
	memset(config, 0, sizeof(*config));
}

bool fm_path_matches(const char *path, const char *root) {
	size_t length;
	if (!path || !root || path[0] != '/' || root[0] != '/') {
		return false;
	}
	length = strlen(root);
	while (length > 1 && root[length - 1] == '/') {
		length--;
	}
	if (length == 1) {
		return true;
	}
	return !strncmp(path, root, length) && (path[length] == '\0' || path[length] == '/');
}

typedef struct {
	size_t watch;
	bool excluded;
} PathRule;

/* More specific paths override ancestors; an exclusion wins an exact tie.
 * Resolve each endpoint independently so a still-excluded rename/clone side
 * cannot be rescued by an unrelated allowed side. */
static PathRule resolve_path_rule(const FmConfig *config, const char *path) {
	PathRule rule = { .watch = SIZE_MAX, .excluded = false };
	size_t i, watch_length = 0, exclude_length = 0;
	if (!config || !path || path[0] != '/') return rule;
	for (i = 0; i < config->watch_count; i++) {
		if (fm_path_matches(path, config->watch_paths[i])) {
			size_t length = strlen(config->watch_paths[i]);
			while (length > 1 && config->watch_paths[i][length - 1] == '/') length--;
			if (length > watch_length) {
				rule.watch = i;
				watch_length = length;
			}
		}
	}
	for (i = 0; i < config->exclude_count; i++) {
		if (fm_path_matches(path, config->exclude_paths[i])) {
			size_t length = strlen(config->exclude_paths[i]);
			while (length > 1 && config->exclude_paths[i][length - 1] == '/') length--;
			if (length > exclude_length) exclude_length = length;
		}
	}
	rule.excluded = exclude_length && exclude_length >= watch_length;
	return rule;
}

bool fm_config_matches(const FmConfig *config, const char *file, const char *newfile) {
	PathRule source = resolve_path_rule(config, file);
	PathRule destination = resolve_path_rule(config, newfile);
	return !source.excluded && !destination.excluded &&
		(source.watch != SIZE_MAX || destination.watch != SIZE_MAX);
}

bool fm_config_excludes_process(const FmConfig *config, const char *process) {
	size_t i;
	if (!config || !process || !*process) return false;
	for (i = 0; i < config->exclude_process_count; i++) {
		if (!strcmp(process, config->exclude_processes[i])) return true;
	}
	return false;
}

static bool matches_identity(uint32_t value, bool available, const uint32_t *watched,
		size_t watch_count, const uint32_t *excluded, size_t exclude_count) {
	size_t i;
	if (!watch_count && !exclude_count) return true;
	if (!available) return false;
	for (i = 0; i < exclude_count; i++) {
		if (value == excluded[i]) return false;
	}
	if (!watch_count) return true;
	for (i = 0; i < watch_count; i++) {
		if (value == watched[i]) return true;
	}
	return false;
}

bool fm_config_matches_uid(const FmConfig *config, uint32_t uid, bool uid_available) {
	return config && matches_identity(uid, uid_available, config->watch_uids,
		config->watch_uid_count, config->exclude_uids, config->exclude_uid_count);
}

bool fm_config_matches_gid(const FmConfig *config, uint32_t gid, bool gid_available) {
	return config && matches_identity(gid, gid_available, config->watch_gids,
		config->watch_gid_count, config->exclude_gids, config->exclude_gid_count);
}

bool fm_config_matches_ppid(const FmConfig *config, int ppid) {
	return config && matches_identity((uint32_t)ppid, ppid > 0, config->watch_ppids,
		config->watch_ppid_count, config->exclude_ppids, config->exclude_ppid_count);
}

/* FSE_OPEN/UNKNOWN are negative, while dropped-event markers are large.
 * Only concrete event types have bits; all preserves the previous behavior. */
static uint64_t event_bit(int type) {
	if (type >= 0 && type < FSE_MAX_EVENTS) return UINT64_C(1) << type;
	switch (type) {
	case FSE_CLOSE: return UINT64_C(1) << 20;
	case FSE_CLOSE_WRITABLE: return UINT64_C(1) << 21;
	case FSE_OPEN: return UINT64_C(1) << 22;
	case FSE_UNKNOWN: return UINT64_C(1) << 23;
	default: return 0;
	}
}

bool fm_config_matches_event(const FmConfig *config, const char *file, const char *newfile, int type) {
	PathRule rules[] = { resolve_path_rule(config, file), resolve_path_rule(config, newfile) };
	size_t i;
	if (rules[0].excluded || rules[1].excluded) return false;
	for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		uint64_t mask;
		if (rules[i].watch == SIZE_MAX) continue;
		mask = config->watch_event_masks ? config->watch_event_masks[rules[i].watch] : FM_WATCH_EVENTS_ALL;
		if (mask == FM_WATCH_EVENTS_ALL || (mask & event_bit(type))) return true;
	}
	return false;
}

static char *trim(char *value) {
	char *end;
	while (*value == ' ' || *value == '\t') {
		value++;
	}
	end = value + strlen(value);
	while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	*end = '\0';
	return value;
}

static bool parse_watch_events(uint64_t *mask, char *value, size_t line, char *err, size_t err_size) {
	static const struct { const char *name; int type; } events[] = {
		{"FSE_CREATE_FILE", FSE_CREATE_FILE}, {"FSE_CREATE_DIR", FSE_CREATE_DIR},
		{"FSE_DELETE", FSE_DELETE}, {"FSE_STAT_CHANGED", FSE_STAT_CHANGED},
		{"FSE_RENAME", FSE_RENAME}, {"FSE_CONTENT_MODIFIED", FSE_CONTENT_MODIFIED},
		{"FSE_EXCHANGE", FSE_EXCHANGE}, {"FSE_FINDER_INFO_CHANGED", FSE_FINDER_INFO_CHANGED},
		{"FSE_CHOWN", FSE_CHOWN}, {"FSE_XATTR_MODIFIED", FSE_XATTR_MODIFIED},
		{"FSE_XATTR_REMOVED", FSE_XATTR_REMOVED}, {"FSE_DOCID_CREATED", FSE_DOCID_CREATED},
		{"FSE_DOCID_CHANGED", FSE_DOCID_CHANGED}, {"FSE_UNMOUNT_PENDING", FSE_UNMOUNT_PENDING},
		{"FSE_CLONE", FSE_CLONE}, {"FSE_OPEN", FSE_OPEN},
		{"FSE_CLOSE", FSE_CLOSE}, {"FSE_CLOSE_WRITABLE", FSE_CLOSE_WRITABLE},
		{"FSE_UNKNOWN", FSE_UNKNOWN}
	};
	char *token = value;
	*mask = 0;
	if (!strcmp(value, "all")) {
		*mask = FM_WATCH_EVENTS_ALL;
		return true;
	}
	for (;;) {
		char *comma = strchr(token, ',');
		uint64_t bits = 0;
		size_t i;
		if (comma) *comma = '\0';
		token = trim(token);
		if (!strcmp(token, "create")) bits = event_bit(FSE_CREATE_FILE) | event_bit(FSE_CREATE_DIR);
		else if (!strcmp(token, "modify")) {
			bits = event_bit(FSE_CONTENT_MODIFIED) | event_bit(FSE_STAT_CHANGED) | event_bit(FSE_CHOWN) |
				event_bit(FSE_XATTR_MODIFIED) | event_bit(FSE_XATTR_REMOVED) | event_bit(FSE_FINDER_INFO_CHANGED);
		} else if (!strcmp(token, "delete")) bits = event_bit(FSE_DELETE);
		else if (!strcmp(token, "rename")) bits = event_bit(FSE_RENAME);
		else if (!strcmp(token, "clone")) bits = event_bit(FSE_CLONE);
		else if (!strcmp(token, "exchange")) bits = event_bit(FSE_EXCHANGE);
		else {
			for (i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
				if (!strcmp(token, events[i].name)) { bits = event_bit(events[i].type); break; }
			}
		}
		if (!bits) {
			config_error(err, err_size, line, "invalid watch_events item '%s'; use all alone, create, modify, delete, rename, clone, exchange or an exact FSE_* event name", token);
			return false;
		}
		*mask |= bits;
		if (!comma) return true;
		token = comma + 1;
	}
}

/* 1 = line read, 0 = EOF, -1 = invalid input/read error. */
static int read_line(FILE *stream, char *buffer, size_t line_number, char *err, size_t err_size) {
	size_t length = 0;
	int ch;
	while ((ch = fgetc(stream)) != EOF && ch != '\n') {
		if (!ch) {
			config_error(err, err_size, line_number, "NUL bytes are not permitted");
			return -1;
		}
		if (length == FM_CONFIG_MAX_LINE + 1) {
			config_error(err, err_size, line_number, "line exceeds %d bytes", FM_CONFIG_MAX_LINE);
			return -1;
		}
		buffer[length++] = (char)ch;
	}
	if (ch == EOF && ferror(stream)) {
		config_error(err, err_size, line_number, "cannot read config: %s", strerror(errno));
		return -1;
	}
	if (ch == EOF && !length) {
		return 0;
	}
	if (ch == '\n' && length && buffer[length - 1] == '\r') {
		length--;
	}
	if (length > FM_CONFIG_MAX_LINE) {
		config_error(err, err_size, line_number, "line exceeds %d bytes", FM_CONFIG_MAX_LINE);
		return -1;
	}
	buffer[length] = '\0';
	if (strchr(buffer, '\r')) {
		config_error(err, err_size, line_number, "embedded carriage returns are not permitted");
		return -1;
	}
	return 1;
}

static enum config_key key_id(const char *key) {
	static const char *const keys[] = {
		"report_url", "report_token", "backend", "pid_file", "log_file",
		"report_timeout_seconds", "report_retries", "retry_delay_seconds",
		"shutdown_timeout_seconds", "queue_capacity", "daemonize", "log_events",
		"watch_path", "exclude_path", "exclude_process", "watch_events",
		"watch_uid", "exclude_uid", "watch_gid", "exclude_gid",
		"watch_ppid", "exclude_ppid"
	};
	size_t i;
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (!strcmp(key, keys[i])) {
			return (enum config_key)i;
		}
	}
	return KEY_UNKNOWN;
}

static bool has_control(const char *value) {
	const unsigned char *p = (const unsigned char *)value;
	for (; *p; p++) {
		if (*p < 0x20 || *p == 0x7f) {
			return true;
		}
	}
	return false;
}

static bool valid_url(const char *value) {
	const char *authority, *end, *p, *host_end;
	if (!strncasecmp(value, "https://", 8)) {
		authority = value + 8;
	} else if (!strncasecmp(value, "http://", 7)) {
		authority = value + 7;
	} else {
		return false;
	}
	for (p = value; *p; p++) {
		if ((unsigned char)*p <= 0x20 || *p == '\x7f' || *p == '\\') {
			return false;
		}
	}
	end = authority + strcspn(authority, "/?#");
	if (end == authority || memchr(authority, '@', (size_t)(end - authority))) {
		return false;
	}
	if (*authority == '[') {
		host_end = memchr(authority, ']', (size_t)(end - authority));
		if (!host_end || host_end == authority + 1) {
			return false;
		}
		host_end++;
	} else {
		host_end = authority;
		while (host_end < end && *host_end != ':') {
			if (*host_end == '[' || *host_end == ']') {
				return false;
			}
			host_end++;
		}
		if (host_end == authority) {
			return false;
		}
	}
	if (host_end < end) {
		unsigned long port = 0;
		if (*host_end++ != ':' || host_end == end) {
			return false;
		}
		for (p = host_end; p < end; p++) {
			if (*p < '0' || *p > '9') {
				return false;
			}
			port = port * 10 + (unsigned long)(*p - '0');
			if (port > 65535) {
				return false;
			}
		}
		if (!port) {
			return false;
		}
	}
	return true;
}

static bool valid_backend(const char *value) {
#if TARGET_IOS
	return !strcmp(value, "devfsev") || !strcmp(value, "kqueue") || !strcmp(value, "kdebug");
#elif __APPLE__
	return !strcmp(value, "devfsev") || !strcmp(value, "fsevapi") ||
		!strcmp(value, "kqueue") || !strcmp(value, "kdebug");
#else
	return !strcmp(value, "inotify") || !strcmp(value, "fanotify");
#endif
}

static bool set_string(char **destination, const char *value, size_t line, char *err, size_t err_size) {
	char *copy = config_strdup(value);
	if (!copy) {
		config_error(err, err_size, line, "out of memory");
		return false;
	}
	free(*destination);
	*destination = copy;
	return true;
}

static bool set_number(int *destination, const char *value, int minimum, int maximum,
		const char *key, size_t line, char *err, size_t err_size) {
	const char *p;
	char *end;
	long number;
	for (p = value; *p; p++) {
		if (*p < '0' || *p > '9') {
			break;
		}
	}
	errno = 0;
	number = strtol(value, &end, 10);
	if (!*value || *p || errno == ERANGE || *end || number < minimum || number > maximum) {
		config_error(err, err_size, line, "%s must be an integer from %d to %d", key, minimum, maximum);
		return false;
	}
	*destination = (int)number;
	return true;
}

static bool set_boolean(bool *destination, const char *value, const char *key,
		size_t line, char *err, size_t err_size) {
	if (!strcmp(value, "true")) {
		*destination = true;
	} else if (!strcmp(value, "false")) {
		*destination = false;
	} else {
		config_error(err, err_size, line, "%s must be true or false", key);
		return false;
	}
	return true;
}

static bool warn_missing_watch(FmConfig *config, const char *value, size_t line,
		char *err, size_t err_size) {
	size_t length = strlen(value) + 128;
	char *message = malloc(length);
	char **warnings;
	if (!message) {
		config_error(err, err_size, line, "out of memory");
		return false;
	}
	(void)snprintf(message, length,
		"line %zu: watch_path '%s' does not exist; ignored until restart", line, value);
	warnings = realloc(config->warnings, (config->warning_count + 1) * sizeof(*warnings));
	if (!warnings) {
		free(message);
		config_error(err, err_size, line, "out of memory");
		return false;
	}
	config->warnings = warnings;
	config->warnings[config->warning_count++] = message;
	return true;
}

void fm_config_log_warnings(const FmConfig *config) {
	size_t i;
	for (i = 0; config && i < config->warning_count; i++) {
		fprintf(stderr, "Configuration warning: %s\n", config->warnings[i]);
	}
	if (config && config->warning_count && !config->watch_count) {
		fprintf(stderr, "Configuration warning: no existing watch paths; monitoring is idle until restart\n");
	}
}

static bool append_path(FmConfig *config, char ***paths, size_t *count, const char *value, bool watch, bool must_exist,
		size_t line, char *err, size_t err_size) {
	char *normalized;
	char **resized;
	struct stat st;
	size_t i;
	if (value[0] != '/') {
		config_error(err, err_size, line, "%s must be an absolute path", watch ? "watch_path" : "exclude_path");
		return false;
	}
	if (watch && *count >= FM_CONFIG_MAX_WATCH_PATHS) {
		config_error(err, err_size, line, "at most %d watch paths are permitted", FM_CONFIG_MAX_WATCH_PATHS);
		return false;
	}
	normalized = must_exist ? realpath(value, NULL) : config_strdup(value);
	if (!normalized) {
		if (must_exist && watch && (errno == ENOENT || errno == ENOTDIR)) {
			return warn_missing_watch(config, value, line, err, err_size);
		}
		config_error(err, err_size, line, "cannot resolve path '%s': %s", value, strerror(errno));
		return false;
	}
	if (must_exist && stat(normalized, &st)) {
		int saved = errno;
		free(normalized);
		if (watch && (saved == ENOENT || saved == ENOTDIR)) {
			return warn_missing_watch(config, value, line, err, err_size);
		}
		config_error(err, err_size, line, "cannot inspect path '%s': %s", value, strerror(saved));
		return false;
	}
	if (must_exist && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
		config_error(err, err_size, line, "path '%s' must be an existing regular file or directory", value);
		free(normalized);
		return false;
	}
	for (i = 0; i < *count; i++) {
		if (watch && !strcmp(normalized, (*paths)[i])) {
			config_error(err, err_size, line, "duplicate watch path: '%s'", normalized);
			free(normalized);
			return false;
		}
		if (!watch && !strcmp(normalized, (*paths)[i])) {
			free(normalized);
			return true;
		}
	}
	if (*count >= (size_t)-1 / sizeof(**paths)) {
		config_error(err, err_size, line, "too many paths");
		free(normalized);
		return false;
	}
	resized = realloc(*paths, (*count + 1) * sizeof(**paths));
	if (!resized) {
		config_error(err, err_size, line, "out of memory");
		free(normalized);
		return false;
	}
	resized[*count] = normalized;
	*paths = resized;
	(*count)++;
	return true;
}

static bool append_identity(uint32_t **ids, size_t *count, const char *value,
		const char *key, const char *kind, uint32_t minimum, uint32_t maximum,
		size_t line, char *err, size_t err_size) {
	const unsigned char *p = (const unsigned char *)value;
	uint32_t id = 0;
	uint32_t *resized;
	size_t i;
	if (!*p) goto invalid;
	for (; *p; p++) {
		if (*p < '0' || *p > '9' || id > (UINT32_MAX - (*p - '0')) / 10) goto invalid;
		id = id * 10 + (*p - '0');
	}
	if (id < minimum || id > maximum) goto invalid;
	for (i = 0; i < *count; i++) {
		if ((*ids)[i] == id) return true;
	}
	if (*count >= SIZE_MAX / sizeof(**ids)) {
		config_error(err, err_size, line, "too many %s entries", key);
		return false;
	}
	resized = realloc(*ids, (*count + 1) * sizeof(**ids));
	if (!resized) {
		config_error(err, err_size, line, "out of memory");
		return false;
	}
	resized[(*count)++] = id;
	*ids = resized;
	return true;
invalid:
	config_error(err, err_size, line, "%s must be a decimal %s from %" PRIu32 " to %" PRIu32 " (one per line)",
		key, kind, minimum, maximum);
	return false;
}

static bool append_process(char ***processes, size_t *count, const char *value,
		size_t line, char *err, size_t err_size) {
	char **resized;
	size_t i;
	if (!*value || strchr(value, '/') || strchr(value, '*') || strchr(value, '?') ||
		strchr(value, '[') || strchr(value, ']')) {
		config_error(err, err_size, line,
			"exclude_process must be a non-empty process name without '/', '*', '?', '[' or ']'");
		return false;
	}
	for (i = 0; i < *count; i++) {
		if (!strcmp((*processes)[i], value)) return true;
	}
	if (*count >= (size_t)-1 / sizeof(**processes)) {
		config_error(err, err_size, line, "too many excluded processes");
		return false;
	}
	resized = realloc(*processes, (*count + 1) * sizeof(**processes));
	if (!resized) {
		config_error(err, err_size, line, "out of memory");
		return false;
	}
	resized[*count] = config_strdup(value);
	if (!resized[*count]) {
		config_error(err, err_size, line, "out of memory");
		*processes = resized;
		return false;
	}
	*processes = resized;
	(*count)++;
	return true;
}

/* Resolve only the parent: the runtime file is allowed not to exist, and a
 * final-component symlink must remain visible to the service's O_NOFOLLOW. */
static bool normalize_runtime_path(char **value, const char *key, char *err, size_t err_size) {
	char *copy = config_strdup(*value);
	char *separator, *parent, *normalized;
	const char *name;
	size_t parent_length, name_length;
	struct stat st;
	if (!copy) {
		config_error(err, err_size, 0, "out of memory");
		return false;
	}
	separator = strrchr(copy, '/');
	name = separator ? separator + 1 : copy;
	if (!separator || !*name || !strcmp(name, ".") || !strcmp(name, "..")) {
		config_error(err, err_size, 0, "%s must name a file in an existing absolute directory", key);
		free(copy);
		return false;
	}
	*separator = '\0';
	parent = realpath(*copy ? copy : "/", NULL);
	if (!parent || stat(parent, &st) || !S_ISDIR(st.st_mode)) {
		config_error(err, err_size, 0, "cannot resolve %s parent directory", key);
		free(parent);
		free(copy);
		return false;
	}
	parent_length = strlen(parent);
	name_length = strlen(name);
	normalized = malloc(parent_length + name_length + 2);
	if (!normalized) {
		config_error(err, err_size, 0, "out of memory");
		free(parent);
		free(copy);
		return false;
	}
	(void)snprintf(normalized, parent_length + name_length + 2, "%s%s%s",
		parent, parent_length == 1 ? "" : "/", name);
	free(parent);
	free(copy);
	free(*value);
	*value = normalized;
	return true;
}

static bool validate_watch_coverage(const FmConfig *config, char *err, size_t err_size) {
	size_t i, j, first_exclusion = 0;
	for (i = 0; i < config->watch_count; i++) {
		PathRule rule = resolve_path_rule(config, config->watch_paths[i]);
		if (!rule.excluded) return true;
		for (j = 0; j < config->exclude_count; j++) {
			if (fm_path_matches(config->watch_paths[i], config->exclude_paths[j])) {
				if (i == 0) first_exclusion = j;
				break;
			}
		}
	}
	config_error(err, err_size, 0,
		"all watch paths are covered by exclude_path: '%s' is excluded by '%s'",
		config->watch_paths[0], config->exclude_paths[first_exclusion]);
	return false;
}

static bool validate_runtime_paths(FmConfig *config, char *err, size_t err_size) {
	struct stat pid_stat, log_stat, watch_stat;
	bool pid_exists, log_exists;
	size_t i;
	if (!normalize_runtime_path(&config->pid_file, "pid_file", err, err_size) ||
		!normalize_runtime_path(&config->log_file, "log_file", err, err_size)) {
		return false;
	}
	pid_exists = stat(config->pid_file, &pid_stat) == 0;
	log_exists = stat(config->log_file, &log_stat) == 0;
	if (!strcmp(config->pid_file, config->log_file) ||
		(pid_exists && log_exists && pid_stat.st_dev == log_stat.st_dev && pid_stat.st_ino == log_stat.st_ino)) {
		config_error(err, err_size, 0, "pid_file and log_file must name different files");
		return false;
	}
	for (i = 0; i < config->watch_count; i++) {
		if (stat(config->watch_paths[i], &watch_stat) || !S_ISREG(watch_stat.st_mode)) {
			continue;
		}
		if (!strcmp(config->watch_paths[i], config->pid_file) ||
			!strcmp(config->watch_paths[i], config->log_file) ||
			(pid_exists && pid_stat.st_dev == watch_stat.st_dev && pid_stat.st_ino == watch_stat.st_ino) ||
			(log_exists && log_stat.st_dev == watch_stat.st_dev && log_stat.st_ino == watch_stat.st_ino)) {
			config_error(err, err_size, 0, "watch_path must not be the PID or log file");
			return false;
		}
	}
	return true;
}

static bool config_load(FmConfig *config, const char *filename, bool control, char *err, size_t err_size) {
	FmConfig parsed;
	FILE *stream;
	char line[FM_CONFIG_MAX_LINE + 2];
	size_t line_number = 0, watch_entries = 0;
	size_t current_watch = SIZE_MAX;
	bool watch_events_seen = false;
	unsigned int seen = 0;
	int result;
	if (err && err_size) {
		err[0] = '\0';
	}
	if (!config || !filename || !*filename) {
		config_error(err, err_size, 0, "config and filename are required");
		return false;
	}
	stream = fopen(filename, "r");
	if (!stream) {
		config_error(err, err_size, 0, "cannot open config '%s': %s", filename, strerror(errno));
		return false;
	}
	fm_config_init(&parsed);
	if (!parsed.backend || !parsed.pid_file || !parsed.log_file) {
		config_error(err, err_size, 0, "out of memory");
		goto fail;
	}
	while ((result = read_line(stream, line, ++line_number, err, err_size)) > 0) {
		char *key = trim(line);
		char *separator, *value;
		enum config_key id;
		bool ok = true;
		if (!*key || *key == '#') {
			continue;
		}
		separator = strchr(key, '=');
		if (!separator) {
			config_error(err, err_size, line_number, "expected key=value");
			goto fail;
		}
		*separator = '\0';
		key = trim(key);
		value = trim(separator + 1);
		id = key_id(key);
		if (id == KEY_UNKNOWN) {
			config_error(err, err_size, line_number, "unknown config key '%s'", key);
			goto fail;
		}
		if (id < KEY_WATCH_PATH && (seen & (1U << id))) {
			config_error(err, err_size, line_number, "duplicate config key '%s'", key);
			goto fail;
		}
		if (has_control(value) || (!*value && id != KEY_REPORT_TOKEN)) {
			config_error(err, err_size, line_number, "invalid or empty value for '%s'", key);
			goto fail;
		}
		seen |= 1U << id;
		switch (id) {
		case KEY_REPORT_URL:
			if (!valid_url(value)) {
				config_error(err, err_size, line_number, "report_url must be an http/https URL without credentials, whitespace or control characters");
				ok = false;
			} else {
				ok = set_string(&parsed.report_url, value, line_number, err, err_size);
			}
			break;
		case KEY_REPORT_TOKEN:
			ok = set_string(&parsed.report_token, value, line_number, err, err_size);
			break;
		case KEY_BACKEND:
			if (!valid_backend(value)) {
				config_error(err, err_size, line_number, "unsupported backend '%s' for this platform", value);
				ok = false;
			} else {
				ok = set_string(&parsed.backend, value, line_number, err, err_size);
			}
			break;
		case KEY_PID_FILE:
		case KEY_LOG_FILE:
			if (value[0] != '/') {
				config_error(err, err_size, line_number, "%s must be an absolute path", key);
				ok = false;
			} else {
				ok = set_string(id == KEY_PID_FILE ? &parsed.pid_file : &parsed.log_file, value, line_number, err, err_size);
			}
			break;
		case KEY_REPORT_TIMEOUT:
			ok = set_number(&parsed.report_timeout_seconds, value, 1, 3600, key, line_number, err, err_size);
			break;
		case KEY_REPORT_RETRIES:
			ok = set_number(&parsed.report_retries, value, 0, 100, key, line_number, err, err_size);
			break;
		case KEY_RETRY_DELAY:
			ok = set_number(&parsed.retry_delay_seconds, value, 0, 3600, key, line_number, err, err_size);
			break;
		case KEY_SHUTDOWN_TIMEOUT:
			ok = set_number(&parsed.shutdown_timeout_seconds, value, 1, 3600, key, line_number, err, err_size);
			break;
		case KEY_QUEUE_CAPACITY:
			ok = set_number(&parsed.queue_capacity, value, 1, 1000000, key, line_number, err, err_size);
			break;
		case KEY_DAEMONIZE:
			ok = set_boolean(&parsed.daemonize, value, key, line_number, err, err_size);
			break;
		case KEY_LOG_EVENTS:
			ok = set_boolean(&parsed.log_events, value, key, line_number, err, err_size);
			break;
		case KEY_WATCH_PATH:
			current_watch = SIZE_MAX;
			watch_events_seen = false;
			if (++watch_entries > FM_CONFIG_MAX_WATCH_PATHS) {
				config_error(err, err_size, line_number, "at most %d watch paths are permitted", FM_CONFIG_MAX_WATCH_PATHS);
				ok = false;
			} else {
				size_t previous_count = parsed.watch_count;
				ok = append_path(&parsed, &parsed.watch_paths, &parsed.watch_count, value, true, !control, line_number, err, err_size);
				if (ok && parsed.watch_count > previous_count) {
					uint64_t *masks = realloc(parsed.watch_event_masks, parsed.watch_count * sizeof(*masks));
					if (!masks) {
						config_error(err, err_size, line_number, "out of memory");
						ok = false;
					} else {
						parsed.watch_event_masks = masks;
						current_watch = previous_count;
						masks[current_watch] = FM_WATCH_EVENTS_ALL;
					}
				}
			}
			break;
		case KEY_WATCH_EVENTS: {
			uint64_t mask;
			if (!watch_entries || watch_events_seen) {
				config_error(err, err_size, line_number, "watch_events requires a preceding watch_path and may appear only once per watch_path");
				ok = false;
			} else {
				ok = parse_watch_events(&mask, value, line_number, err, err_size);
				if (ok && current_watch != SIZE_MAX) parsed.watch_event_masks[current_watch] = mask;
				watch_events_seen = true;
			}
			break;
		}
		case KEY_EXCLUDE_PATH:
			ok = append_path(&parsed, &parsed.exclude_paths, &parsed.exclude_count, value, false, !control, line_number, err, err_size);
			break;
		case KEY_EXCLUDE_PROCESS:
			ok = append_process(&parsed.exclude_processes, &parsed.exclude_process_count, value,
				line_number, err, err_size);
			break;
		case KEY_WATCH_UID:
			ok = append_identity(&parsed.watch_uids, &parsed.watch_uid_count, value, key, "UID",
				0, UINT32_MAX, line_number, err, err_size);
			break;
		case KEY_EXCLUDE_UID:
			ok = append_identity(&parsed.exclude_uids, &parsed.exclude_uid_count, value, key, "UID",
				0, UINT32_MAX, line_number, err, err_size);
			break;
		case KEY_WATCH_GID:
			ok = append_identity(&parsed.watch_gids, &parsed.watch_gid_count, value, key, "GID",
				0, UINT32_MAX, line_number, err, err_size);
			break;
		case KEY_EXCLUDE_GID:
			ok = append_identity(&parsed.exclude_gids, &parsed.exclude_gid_count, value, key, "GID",
				0, UINT32_MAX, line_number, err, err_size);
			break;
		case KEY_WATCH_PPID:
			ok = append_identity(&parsed.watch_ppids, &parsed.watch_ppid_count, value, key, "PPID",
				1, INT32_MAX, line_number, err, err_size);
			break;
		case KEY_EXCLUDE_PPID:
			ok = append_identity(&parsed.exclude_ppids, &parsed.exclude_ppid_count, value, key, "PPID",
				1, INT32_MAX, line_number, err, err_size);
			break;
		case KEY_UNKNOWN:
			ok = false;
			break;
		}
		if (!ok) {
			goto fail;
		}
	}
	if (result < 0) {
		goto fail;
	}
	if (!parsed.report_url || !watch_entries) {
		config_error(err, err_size, 0, "report_url and at least one watch_path are required");
		goto fail;
	}
	/* Stop/control commands must still work with an unusable watch scope. */
	if (!control && parsed.watch_count && !validate_watch_coverage(&parsed, err, err_size)) {
		goto fail;
	}
	if (!validate_runtime_paths(&parsed, err, err_size)) {
		goto fail;
	}
	if (fclose(stream)) {
		config_error(err, err_size, 0, "cannot close config: %s", strerror(errno));
		fm_config_free(&parsed);
		return false;
	}
	fm_config_free(config);
	*config = parsed;
	return true;
fail:
	(void)fclose(stream);
	fm_config_free(&parsed);
	return false;
}

bool fm_config_load(FmConfig *config, const char *filename, char *err, size_t err_size) {
	return config_load(config, filename, false, err, err_size);
}

bool fm_config_load_control(FmConfig *config, const char *filename, char *err, size_t err_size) {
	return config_load(config, filename, true, err, err_size);
}
