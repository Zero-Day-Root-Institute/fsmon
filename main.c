/* fsmod -- MIT - Copyright NowSecure 2015-2025 - pancake@nowsecure.com  */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#if __APPLE__
#include <mach-o/dyld.h>
#endif
#include "fsmod.h"
#include "config.h"
#include "report.h"
#include "service.h"
#include "control.h"

static FileMonitor fm = { .fd = -1 };
static bool firstnode = true;
static bool colorful = true;
static FmConfig config;
static bool configured;
static FmReporter *reporter;
static FmControl *runtime_control;

FileMonitorBackend *backends[] = {
#if __APPLE__
#if !TARGET_WATCHOS && !TARGET_IOS
	&fmb_fsevapi,
#endif
	&fmb_devfsev,
	&fmb_kqueue,
	&fmb_kdebug,
#else
	&fmb_inotify,
#if HAVE_FANOTIFY
	&fmb_fanotify,
#endif
#endif
	NULL
};

static void control_c(int sig) {
	(void)sig;
	fm.running = false;
}

static bool setup_signals() {
	bool res = true;
	fm.running = true;
	struct sigaction int_handler = {
		.sa_handler = control_c
	};
	sigemptyset (&int_handler.sa_mask);
	if (sigaction (SIGINT, &int_handler, 0) == -1) {
		eprintf ("Cannot setup the SIGINT handler\n");
		res = false;
	}
	if (sigaction (SIGTERM, &int_handler, 0) == -1) {
		eprintf ("Cannot setup the SIGTERM handler\n");
		res = false;
	}
	/* Network writes to a closed connection must not terminate monitoring. */
	struct sigaction pipe_handler = { .sa_handler = SIG_IGN };
	sigemptyset (&pipe_handler.sa_mask);
	if (sigaction (SIGPIPE, &pipe_handler, 0) == -1) res = false;
	return res;
}

static uint64_t __sys_now(void) {
	uint64_t ret;
	struct timeval now;
	gettimeofday (&now, NULL);
	ret = now.tv_sec;
	ret <<= 20;
	ret |= now.tv_usec;
	//(sizeof (now.tv_sec) == 4
	return ret;
}

static void time_ymdhms(char *buf, size_t buflen) {
	struct timeval now;
	gettimeofday (&now, NULL);
	struct tm *tm_info = localtime (&now.tv_sec);
	char time_buf[20];
	strftime (time_buf, sizeof (time_buf), "%Y%m%d-%H:%M:%S", tm_info);
	// Append milliseconds
	int millisec = now.tv_usec / 1000;
	snprintf(buf, buflen, "%s.%03d", time_buf, millisec);
}

static bool callback(FileMonitor *fm, FileMonitorEvent *ev) {
	if (!ev || !ev->file) return false;
	if (configured) {
		if (!fm_config_matches_uid (&config, ev->uid, ev->uid_available)) return false;
		if (!fm_config_matches_gid (&config, ev->gid, ev->gid_available)) return false;
		if (!fm_config_matches_ppid (&config, ev->ppid)) return false;
		if (!fm_config_matches_event (&config, ev->file, ev->newfile, ev->type)) return false;
		if (fm_config_excludes_process (&config, ev->proc)) return false;
		if (!strcmp (ev->file, "/dev/null") ||
			ev->pid == (int)getpid () || ev->ppid == (int)getpid () ||
			(reporter && fm_reporter_is_internal (reporter, ev))) return false;
		/* Avoid reporting our own log and PID-file writes, including renames. */
		if (fm_path_matches (ev->file, config.log_file) ||
			fm_path_matches (ev->file, config.pid_file) ||
			fm_control_is_internal (runtime_control, ev->file) ||
			(ev->newfile && (fm_path_matches (ev->newfile, config.log_file) ||
			fm_path_matches (ev->newfile, config.pid_file) ||
			fm_control_is_internal (runtime_control, ev->newfile)))) return false;
	}
	if (fm->child) {
		if (fm->pid && ev->pid != fm->pid) {
			if (ev->ppid != fm->pid) {
				return false;
			}
		}
	} else {
		if (fm->pid && ev->pid != fm->pid) {
			return false;
		}
	}
	if (!configured && fm->root && ev->file) {
		if (!fm_path_matches (ev->file, fm->root) &&
			(!ev->newfile || !fm_path_matches (ev->newfile, fm->root))) {
			return false;
		}
	}
	if (fm->link && ev->file) {
		if (fm_path_matches (ev->file, fm->link)) {
			return false;
		}
	}
	if (fm->proc) {
		if (!ev->proc || !strstr (ev->proc, fm->proc)) {
			return false;
		}
	}
	if (reporter) {
		if (!fm_control_event_begin (runtime_control, ev)) return false;
		/* Serialize while backend-owned strings are still valid. */
		if (fm_control_should_report (runtime_control, ev))
			(void)fm_reporter_submit (reporter, fm, ev);
		if (config.log_events) {
			char *json = fm_event_json (fm, ev);
			if (json) { puts (json); fflush (stdout); free (json); }
			else eprintf ("Cannot serialize event log\n");
		}
		fm_control_event_end (runtime_control);
		return false;
	}
	const char *display_file = ev->file;
	if (fm->fileonly) {
		const char *slash = strrchr (display_file, '/');
		if (slash) display_file = slash + 1;
	}
	if (fm->json || fm->jsonStream) {
		char *filename = fmu_jsonfilter (display_file);
		char *proc = ev->proc ? fmu_jsonfilter (ev->proc) : NULL;
		char *event = ev->event ? fmu_jsonfilter (ev->event) : NULL;
		char *newfile = ev->newfile ? fmu_jsonfilter (ev->newfile) : NULL;
		if (!filename || (ev->proc && !proc) || (ev->event && !event) || (ev->newfile && !newfile)) {
			eprintf ("Cannot allocate JSON event\n");
			free (filename); free (proc); free (event); free (newfile);
			return false;
		}
		if (fm->jsonStream) {
			firstnode = true;
		}
		printf ("%s{\"filename\":\"%s\",", (fm->jsonStream || firstnode)? "":",", filename);
		if (ev->pid) {
			printf ("\"pid\":%d,", ev->pid);
		}
		if (ev->uid && ev->gid) {
			printf ("\"uid\":%" PRIu32 ",\"gid\":%" PRIu32 ",", ev->uid, ev->gid);
		}
		firstnode = false;
		free (filename);
		if (ev->inode) {
			printf ("\"inode\":%" PRIu64 ",", ev->inode);
		}
		if (ev->tstamp) {
			uint64_t now = __sys_now ();
			printf ("\"time\":%" PRIu64 ",", now);
		}
		if (fm->show_timestamps) {
			char datetime[32];
			time_ymdhms (datetime, sizeof (datetime));
			printf ("\"datetime\":\"%s\",", datetime);
		}
		if (ev->tstamp) {
			printf ("\"timestamp\":%" PRIu64 ",", ev->tstamp);
		}
		if (ev->inode) {
			printf ("\"dev\":{\"major\":%d,\"minor\":%d},",
				ev->dev_major, ev->dev_minor);
		}
		if (ev->mode) {
			printf ("\"mode\":%d,", ev->mode);
		}
		if (ev->ppid) {
			printf ("\"ppid\":%d,", ev->ppid);
		}
		if (ev->proc && *ev->proc) {
			printf ("\"proc\":\"%s\",", proc);
		}
		if (ev->event && *ev->event) {
			printf ("\"event\":\"%s\",", event);
		}
		if (ev->newfile && *ev->newfile) {
			printf ("\"newfile\":\"%s\",", newfile);
		}
		free (proc); free (event); free (newfile);
		printf ("\"type\":\"%s\"}", fm_typestr (ev->type));
		if (fm->jsonStream) {
			printf ("\n");
			fflush (stdout);
		}
	} else {
		const char *color_begin = colorful? fm_colorstr (ev->type): "";
		const char *color_begin2 = colorful? Color_MAGENTA: "";
		const char *color_end = colorful? Color_RESET: "";
		if (fm->show_timestamps) {
			char datetime[32];
			time_ymdhms (datetime, sizeof (datetime));
			printf ("%s  ", datetime);
		}
		// TODO . show event type
		if (ev->type == FSE_RENAME || ev->type == FSE_EXCHANGE || ev->type == FSE_CLONE) {
			printf ("%s%s%s\t%d\t\"%s%s%s\"\t%s -> %s\n",
				color_begin, fm_typestr (ev->type), color_end,
				ev->pid, color_begin2, ev->proc? ev->proc: "", color_end, display_file,
				ev->newfile? ev->newfile: "?");
		} else {
			printf ("%s%s%s\t%d\t\"%s%s%s\"\t%s\n",
				color_begin, fm_typestr (ev->type), color_end,
				ev->pid, color_begin2, ev->proc? ev->proc: "", color_end, display_file);
		}
	}
	if (fm->link) {
		size_t i;
		char dst[1024];
		const char *src = ev->file;
		char *fname = strdup (ev->file);
		if (!fname) {
			eprintf ("Cannot allocate ev->file\n");
			return false;
		}
		for (i = 0; fname[i]; i++) {
			if (fname[i] == '/') {
				fname[i] = '_';
			}
		}
		if (ev->newfile) {
			src = ev->newfile;
		}
		if (is_directory (src)) {
			eprintf ("[I] Directories not copied\n");
		} else {
			snprintf (dst, sizeof (dst), "%s/%s", fm->link, fname);
			if (!copy_file (src, dst)) {
				eprintf ("[E] Error copying %s\n", dst);
			}
		}
		free (fname);
	}
	return false;
}

/* Resolve the configuration from the installed executable, never by probing
 * whether a rootful or rootless configuration happens to exist. The longer
 * /usr suffix must precede /sbin. Desktop /usr/local binaries
 * use the established /etc location. */
static bool default_config_from_executable(const char *executable, char *path, size_t capacity) {
	static const char *suffixes[] = {
		"/usr/sbin/fsmod", "/sbin/fsmod"
	};
	static const char config_suffix[] = "/etc/fsmod.conf";
	if (!executable || executable[0] != '/' || !path || !capacity) return false;
	/* Desktop make install uses /usr/local for binaries but retains /etc. */
	if (!strcmp (executable, "/usr/local/sbin/fsmod")) {
		if (sizeof (config_suffix) > capacity) return false;
		memcpy (path, config_suffix, sizeof (config_suffix));
		return true;
	}
	size_t length = strlen (executable);
	for (size_t i = 0; i < sizeof (suffixes) / sizeof (suffixes[0]); i++) {
		size_t suffix_length = strlen (suffixes[i]);
		if (length < suffix_length || strcmp (executable + length - suffix_length, suffixes[i])) continue;
		size_t prefix_length = length - suffix_length;
		if (prefix_length > capacity || sizeof (config_suffix) > capacity - prefix_length) return false;
		memcpy (path, executable, prefix_length);
		memcpy (path + prefix_length, config_suffix, sizeof (config_suffix));
		return true;
	}
	return false;
}

static bool default_config_path(char *path, size_t capacity) {
	if (!path || !capacity) return false;
#ifdef FSMOD_DEFAULT_CONFIG
	/* A compile-time override also gives startup tests an isolated fixture. */
	const char *override = FSMOD_DEFAULT_CONFIG;
	size_t length = strlen (override);
	if (length >= capacity) return false;
	memcpy (path, override, length + 1);
	return true;
#elif __APPLE__
	char executable[PATH_MAX], canonical[PATH_MAX];
	uint32_t length = sizeof (executable);
	if (_NSGetExecutablePath (executable, &length) != 0) return false;
	const char *actual = realpath (executable, canonical) ? canonical : executable;
	/* A copied developer build outside an installation directory uses /etc. */
	if (default_config_from_executable (actual, path, capacity)) return true;
#endif
	return default_config_from_executable ("/usr/sbin/fsmod", path, capacity);
}

static void help (const char *argv0) {
#if !TARGET_WATCHOS && !FSMOD_NO_REPORT
	char default_path[PATH_MAX];
	const char *default_name = default_config_path (default_path, sizeof (default_path)) ?
		default_path : "the configuration beside the installation prefix";
#else
	const char *default_name = "interactive monitoring (reporting unavailable)";
#endif
	eprintf ("Usage: %s [-Jjc] [-a sec] [-b dir] [-B name] [-p pid] [-P proc] [path]\n"
		"       %s [-C config] [-F | -D] [-S | -T]\n"
		"       fsmod [-C config] ctl status|pause|resume|run [control options]\n"
#if !TARGET_WATCHOS && !FSMOD_NO_REPORT
		" No arguments: load %s and run in the foreground.\n"
#else
		" No arguments: %s.\n"
#endif
		" -C [file] override the default daemon/reporting configuration\n"
		" -D        run configured monitor in the background\n"
		" -F        run configured monitor in the foreground (launchd/systemd)\n"
		" -S        send SIGTERM to the configured instance\n"
		" -T        validate configuration without starting monitoring\n"
		" -a [sec]  stop monitoring after N seconds (alarm)\n"
		" -b [dir]  backup files to DIR folder (EXPERIMENTAL)\n"
		" -B [name] specify an alternative backend\n"
		" -c        follow children of -p PID\n"
		" -f        show only filename (no path)\n"
		" -h        show this help\n"
		" -j        output in JSON format\n"
		" -J        output in JSON stream format\n"
		" -L        list all filemonitor backends\n"
		" -n        do not use colors\n"
		" -p [pid]  only show events from this pid (Linux: requires -B fanotify)\n"
		" -P [proc] events only from process name (Linux: requires -B fanotify)\n"
		" -t        show timestamps in default logs\n"
		" -v        show version\n"
		" [path]    only get events from this path\n"
		"Examples:\n"
		" fsmod /data\n"
		" fsmod -J / | jq -r .filename\n"
		" fsmod -B fanotify /home\n"
		, argv0, argv0, default_name);
}

static bool use_backend(const char *name) {
	size_t i;
	for (i = 0; backends[i]; i++) {
		if (!strcmp (backends[i]->name, name)) {
			fm.backend = *backends[i];
			return true;
		}
	}
	return false;
}

static void list_backends() {
	size_t i;
	for (i = 0; backends[i]; i++) {
		printf ("%s\n", backends[i]->name);
	}
}

/* Keep nested watches as filter/event-mask rules, but collect their shared
 * ancestor only once. The caller provides config->watch_count storage slots. */
static size_t configured_collection_roots(const FmConfig *cfg, const char **roots) {
	size_t i, j, count = 0;
	/* A fanotify mount mark does not cover a nested mount. Keep explicit
	 * roots so every requested mount is marked; repeated marks are merged. */
	if (cfg->backend && !strcmp (cfg->backend, "fanotify")) {
		for (i = 0; i < cfg->watch_count; i++) roots[i] = cfg->watch_paths[i];
		return cfg->watch_count;
	}
	for (i = 0; i < cfg->watch_count; i++) {
		for (j = 0; j < cfg->watch_count; j++) {
			if (i != j && fm_path_matches(cfg->watch_paths[i], cfg->watch_paths[j])) break;
		}
		if (j == cfg->watch_count) roots[count++] = cfg->watch_paths[i];
	}
	return count;
}

int main (int argc, char **argv) {
	char absroot[PATH_MAX];
	char default_path[PATH_MAX];
	const char *collection_roots[FM_CONFIG_MAX_WATCH_PATHS];
	int c, ret = 0;
	const char *config_path = NULL;
	bool foreground = false, background = false, stop = false, check = false;
	bool legacy_options = false;
	bool implicit_config = false;
	bool automatic_config = true;
#if TARGET_WATCHOS || FSMOD_NO_REPORT
	/* Preserve no-argument interactive monitoring on builds without reporting. */
	automatic_config = false;
#endif
	char error[1024] = {0};
	fm_config_init (&config);
	/* Intercept ctl before getopt can permute its long options. The explicit
	 * socket mode never needs a valid or even present monitoring config. */
	int ctl_index = 1;
	const char *ctl_config = NULL;
	if (argc > 2 && !strcmp (argv[1], "-C")) { ctl_config = argv[2]; ctl_index = 3; }
	if (ctl_index < argc && !strcmp (argv[ctl_index], "ctl")) {
		char socket_path[PATH_MAX]; const char *socket_name = NULL; bool explicit_socket = false;
		for (int i = ctl_index + 1; i < argc; i++) {
			if (!strcmp (argv[i], "--")) break;
			if (!strcmp (argv[i], "--socket")) explicit_socket = true;
		}
		if (!explicit_socket) {
			if (!ctl_config) {
				if (!default_config_path (default_path, sizeof (default_path))) {
					eprintf ("Control: cannot resolve default configuration\n"); fm_config_free (&config); return 1;
				}
				ctl_config = default_path;
			}
			if (!fm_config_load_control (&config, ctl_config, error, sizeof (error)) ||
				!fm_control_path (config.pid_file, socket_path, sizeof (socket_path))) {
				eprintf ("Control: %s\n", *error ? error : "PID path too long for control socket");
				fm_config_free (&config); return 1;
			}
			socket_name = socket_path;
		}
		int result = fm_control_cli (argc - ctl_index - 1, argv + ctl_index + 1, socket_name);
		fm_config_free (&config); return result;
	}
	if (!setup_signals ()) { fm_config_free (&config); return 1; }
#if __APPLE__
	fm.backend = fmb_devfsev;
#else
	fm.backend = fmb_inotify;
#endif

	while ((c = getopt (argc, argv, "a:chb:B:fjJlLnp:P:vtC:DFST")) != -1) {
		if (strchr ("abcBfjJnpPt", c)) legacy_options = true;
		switch (c) {
		case 'C':
			config_path = optarg;
			break;
		case 'D': background = true; break;
		case 'F': foreground = true; break;
		case 'S': stop = true; break;
		case 'T': check = true; break;
		case 'a':
			fm.alarm = atoi (optarg);
			if (fm.alarm < 1) {
				eprintf ("Invalid alarm time\n");
				return 1;
			}
			break;
		case 'b':
			fm.link = optarg;
			break;
		case 'B':
			if (!use_backend (optarg)) {
				eprintf ("Unknown backend: %s\n", optarg);
				return 1;
			}
			break;
		case 'c':
			fm.child = true;
			break;
		case 'h':
			help (argv[0]);
			return 0;
		case 'f':
			fm.fileonly = true;
			break;
		case 't':
			fm.show_timestamps = true;
			break;
		case 'j':
			fm.json = true;
			break;
		case 'J':
			fm.jsonStream = true;
			break;
		case 'l':
		case 'L':
			list_backends ();
			return 0;
		case 'n':
			colorful = false;
			break;
		case 'p':
			fm.pid = atoi (optarg);
			break;
		case 'P':
			fm.proc = optarg;
			break;
		case 'v':
			printf ("fsmod %s\n", FSMOD_VERSION);
			return 0;
		default:
			return 1;
		}
	}
	if ((foreground && background) || (stop && check) || (stop && (foreground || background))) {
		eprintf ("Cannot combine -D with -F, -S with -T, or -S with -D/-F\n");
		return 1;
	}
	if (!config_path && (foreground || background || stop || check || (automatic_config && !legacy_options && optind == argc))) {
		if (!default_config_path (default_path, sizeof (default_path))) {
			eprintf ("Cannot resolve the default configuration path; specify -C explicitly\n");
			fm_config_free (&config);
			return 1;
		}
		config_path = default_path;
		implicit_config = true;
	}
	if (config_path) {
#if TARGET_WATCHOS
		eprintf ("Configured daemon reporting is unavailable on watchOS\n");
		fm_config_free (&config);
		return 1;
#endif
		if (legacy_options || optind < argc) {
			eprintf ("In configured mode, set monitoring/output options in the configuration file\n");
			return 1;
		}
		bool loaded = stop ? fm_config_load_control (&config, config_path, error, sizeof (error)) :
			fm_config_load (&config, config_path, error, sizeof (error));
		if (!loaded) {
			eprintf ("Configuration: %s\n", error);
			fm_config_free (&config);
			return 1;
		}
		configured = true;
		if (stop) {
			bool ok = fm_service_stop (config.pid_file, error, sizeof (error));
			if (!ok) eprintf ("Stop: %s\n", error);
			fm_config_free (&config);
			return ok ? 0 : 1;
		}
		if (!use_backend (config.backend) ||
			(!strcmp (config.backend, "kqueue") || !strcmp (config.backend, "kdebug"))) {
			eprintf ("Backend '%s' is unavailable or not supported for daemon reporting\n", config.backend);
			fm_config_free (&config);
			return 1;
		}
		/* A no-argument launchd/systemd command must never double-fork, even
		 * if an older configuration still contains daemonize=true. */
		if (foreground || (implicit_config && !background)) config.daemonize = false;
#if TARGET_IOS
		/* iOS is supervised by launchd; only an explicit -D can request the
		 * unsupported detach operation and receive its existing error. */
		if (!background) config.daemonize = false;
#endif
		if (background) config.daemonize = true;
		if (check) {
			fm_config_log_warnings (&config);
			printf ("Configuration OK: backend=%s, watch_paths=%zu, daemonize=%s, warnings=%zu\n",
				config.backend, config.watch_count, config.daemonize ? "true" : "false", config.warning_count);
			fm_config_free (&config);
			return 0;
		}
		fm.roots = collection_roots;
		fm.root_count = configured_collection_roots (&config, collection_roots);
		fm.jsonStream = true;
		fm.changes_only = true;
		colorful = false;
		if (config.daemonize && !fm_service_detach (config.log_file, error, sizeof (error))) {
			eprintf ("Daemon: %s\n", error);
			ret = 1;
			goto cleanup;
		}
		if (!fm_service_lock (config.pid_file, error, sizeof (error))) {
			eprintf ("Daemon: %s\n", error);
			ret = 1;
			goto cleanup;
		}
		fm_config_log_warnings (&config);
	}
	if (optind < argc) {
		if (optind + 1 < argc) {
			eprintf ("Warning: Too many arguments passed, capturing events only from the first path.\n");
		}
		char * res = realpath (argv[optind], absroot);
		if (!res) {
			eprintf ("Invalid path\n");
			return 1;
		}
		fm.root = (const char *)absroot;
	}
	if (fm.child && !fm.pid) {
		eprintf ("-c requires -p\n");
		return 1;
	}
#if __linux__
	if ((fm.pid || fm.proc) && fm.backend.name && !strcmp (fm.backend.name, "inotify")) {
		eprintf ("Error: the inotify backend cannot filter by -p/-P because the Linux\n"
		         "       inotify API does not report the originating process. Use\n"
		         "       '-B fanotify' (requires root and Linux 2.6.37+) or drop the filter.\n");
		return 1;
	}
#endif
	if (!strcmp (fm.backend.name, "fsevapi") && (fm.pid || fm.proc || fm.child)) {
		eprintf ("The fsevapi backend cannot identify originating processes\n");
		return 1;
	}
	/* The reporter creates its private token transport before any watches.
	 * Detaching and the PID lock always precede threads or backend handles. */
	if (configured) {
		sigset_t blocked, previous;
		sigemptyset (&blocked);
		sigaddset (&blocked, SIGINT);
		sigaddset (&blocked, SIGTERM);
		sigaddset (&blocked, SIGALRM);
		int mask_error = pthread_sigmask (SIG_BLOCK, &blocked, &previous);
		if (mask_error) {
			eprintf ("Cannot block reporter signals: %s\n", strerror (mask_error));
			ret = 1;
			goto cleanup;
		}
		reporter = fm_reporter_start (&config, error, sizeof (error));
		(void)pthread_sigmask (SIG_SETMASK, &previous, NULL);
		if (!reporter) {
			eprintf ("Reporter: %s\n", error);
			ret = 1;
			goto cleanup;
		}
		runtime_control = fm_control_start (config.pid_file, reporter, &fm, error, sizeof (error));
		if (!runtime_control) {
			eprintf ("Control: %s\n", error);
			ret = 1; goto cleanup;
		}
	}
	if (!fm.running) { ret = 1; goto cleanup; }
	/* Zero configured roots must never trigger a backend's / or . fallback.
	 * Missing roots are skipped for this run; a restart rechecks them. */
	if (configured && !fm.root_count) {
		eprintf ("fsmod started: pid=%ld backend=%s roots=0 (idle)\n",
			(long)getpid (), fm.backend.name);
		fm_service_ready (true);
		while (fm.running) {
			struct timespec delay = { .tv_sec = 0, .tv_nsec = 250000000 };
			(void)nanosleep (&delay, NULL);
		}
		goto cleanup;
	}
	if (!fm.backend.begin (&fm)) {
		ret = 1;
		fm.backend.end (&fm);
		goto cleanup;
	}
	if (configured) eprintf ("fsmod started: pid=%ld backend=%s roots=%zu\n",
		(long)getpid (), fm.backend.name, fm.root_count);
	if (!fm.running) { fm.backend.end (&fm); ret = 1; goto cleanup; }
	/* -a is armed after initialization; handlers are installed before forking. */
	if (fm.alarm) {
		struct sigaction alarm_handler = { .sa_handler = control_c };
		sigemptyset (&alarm_handler.sa_mask);
		if (sigaction (SIGALRM, &alarm_handler, NULL) < 0) {
			fm.backend.end (&fm); ret = 1; goto cleanup;
		}
		alarm ((unsigned int)fm.alarm);
	}
	fm_service_ready (true);
	if (fm.json && !fm.jsonStream) {
		printf ("[");
	}
	if (!fm.backend.loop (&fm, callback) && fm.running) ret = 1;
	if (fm.json && !fm.jsonStream) {
		printf ("]\n");
	}
	fflush (stdout);
	fm.backend.end (&fm);
cleanup:
	fm_service_ready (false);
	if (runtime_control) { fm_control_stop (runtime_control); runtime_control = NULL; }
	if (reporter) fm_reporter_stop (reporter);
	if (configured) {
		eprintf ("fsmod stopped: pid=%ld status=%d\n", (long)getpid (), ret);
		fm_service_release ();
	}
	fm_config_free (&config);
	return ret;
}
