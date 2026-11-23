#ifndef INCLUDE_FM_CONFIG_H
#define INCLUDE_FM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FM_CONFIG_MAX_WATCH_PATHS 64
#define FM_CONFIG_MAX_LINE 8192
#define FM_WATCH_EVENTS_ALL UINT64_MAX

typedef struct fm_config_t {
	char *report_url;
	char *report_token;
	char *backend;
	char *pid_file;
	char *log_file;
	char **watch_paths;
	uint64_t *watch_event_masks;
	size_t watch_count;
	char **warnings;
	size_t warning_count;
	char **exclude_paths;
	size_t exclude_count;
	char **exclude_processes;
	size_t exclude_process_count;
	uint32_t *watch_uids;
	size_t watch_uid_count;
	uint32_t *exclude_uids;
	size_t exclude_uid_count;
	uint32_t *watch_gids;
	size_t watch_gid_count;
	uint32_t *exclude_gids;
	size_t exclude_gid_count;
	uint32_t *watch_ppids;
	size_t watch_ppid_count;
	uint32_t *exclude_ppids;
	size_t exclude_ppid_count;
	int report_timeout_seconds;
	int report_retries;
	int retry_delay_seconds;
	int shutdown_timeout_seconds;
	int queue_capacity;
	bool daemonize;
	bool log_events;
} FmConfig;

/* Initialize before loading. A failed load leaves the original config intact. */
void fm_config_init(FmConfig *config);
bool fm_config_load(FmConfig *config, const char *filename, char *err, size_t err_size);
/* Parse a stop/control request even when a watched path has been removed. */
bool fm_config_load_control(FmConfig *config, const char *filename, char *err, size_t err_size);
/* Emit nonfatal startup/check diagnostics to stderr after output redirection. */
void fm_config_log_warnings(const FmConfig *config);
void fm_config_free(FmConfig *config);

/* Match a normalized absolute path at an exact path-component boundary. */
bool fm_path_matches(const char *path, const char *root);
/* The most specific watch/exclusion wins; equal paths favor exclusion.
 * A resolved exclusion on either rename/clone endpoint rejects the event. */
bool fm_config_matches(const FmConfig *config, const char *file, const char *newfile);
/* Apply only the most specific watch's event mask on each included endpoint. */
bool fm_config_matches_event(const FmConfig *config, const char *file, const char *newfile, int type);
/* Exact, case-sensitive process-name exclusion; unknown event names do not match. */
bool fm_config_excludes_process(const FmConfig *config, const char *process);
/* Match the backend's event UID. Active UID rules reject unavailable metadata;
 * exclusions take precedence over a nonempty watch list. */
bool fm_config_matches_uid(const FmConfig *config, uint32_t uid, bool uid_available);
/* Independent group-ID rule, combined with the UID rule by the callback. */
bool fm_config_matches_gid(const FmConfig *config, uint32_t gid, bool gid_available);
/* Match the event process's immediate parent PID. Active rules reject PPID <= 0. */
bool fm_config_matches_ppid(const FmConfig *config, int ppid);

#endif
