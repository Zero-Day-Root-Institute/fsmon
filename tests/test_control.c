/* Deterministic control-state tests; HTTP transport is exercised separately. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include <assert.h>
#include "../control.c"

static bool probe_paused, probe_discard;
static unsigned probe_calls;

bool fm_reporter_pause(FmReporter *reporter, bool paused, bool discard_pending, unsigned timeout_seconds) {
	assert(reporter); assert(timeout_seconds == CONTROL_TIMEOUT);
	probe_calls++; probe_paused = paused; probe_discard = discard_pending; return true;
}

void fm_reporter_status(FmReporter *reporter, FmReporterStatus *status) {
	assert(reporter); *status = (FmReporterStatus){ .paused = probe_paused };
}

static void initial_control(FmControl *control, FileMonitor *monitor) {
	memset(control, 0, sizeof(*control));
	control->reporter = (FmReporter *)(uintptr_t)1;
	control->monitor = monitor;
	assert(!pthread_mutex_init(&control->mutex, NULL));
	assert(boot_identity(control->boot, sizeof(control->boot)));
}

static void test_gate_transitions(void) {
	FileMonitor monitor = { .running = true, .backend = { .name = "devfsev" } };
	FmControl control; initial_control(&control, &monitor);
	FileMonitorEvent event = { .file = "/watched/file", .tstamp = 110 };
	assert(fm_control_event_begin(&control, &event));
	assert(fm_control_should_report(&control, &event)); fm_control_event_end(&control);
	control.hold_count = 1; strcpy(control.holds[0].id, "com.example.a"); control.holds[0].all = true;
	assert(apply_state(&control, true)); assert(probe_paused && probe_discard);
	assert(!fm_control_event_begin(&control, &event)); assert(control.suppressed_monitor == 1);
	/* all -> report opens local logging but keeps HTTP blocked. Explicit
	 * cutoffs here isolate the clock-domain comparison from platform clocks. */
	control.holds[0].all = false; assert(apply_state(&control, false));
	assert(!control.monitor_paused && control.report_paused);
	control.monitor_cutoff = 100;
	assert(fm_control_event_begin(&control, &event));
	assert(!fm_control_should_report(&control, &event)); fm_control_event_end(&control);
	event.tstamp = 100; assert(!fm_control_event_begin(&control, &event));
	event.tstamp = 110; control.hold_count = 0; assert(apply_state(&control, false));
	assert(!control.monitor_paused && !control.report_paused && !probe_paused);
	control.report_cutoff = 120;
	assert(fm_control_event_begin(&control, &event));
	assert(!fm_control_should_report(&control, &event)); fm_control_event_end(&control);
	event.tstamp = 121; assert(fm_control_event_begin(&control, &event));
	assert(fm_control_should_report(&control, &event)); fm_control_event_end(&control);
	/* Untimestamped backends retain their normal event path. */
	event.tstamp = 0; assert(fm_control_event_begin(&control, &event));
	assert(fm_control_should_report(&control, &event)); fm_control_event_end(&control);
	assert(probe_calls >= 3); pthread_mutex_destroy(&control.mutex);
}

static void test_clock_scope_and_shutdown(void) {
	FileMonitor monitor = { .running = true, .backend = { .name = "fsevapi" } };
	FmControl control; initial_control(&control, &monitor);
	control.hold_count = 1; control.holds[0].all = true; control.holds[0].expires = monotonic_now() - 1;
	assert(apply_state(&control, false));
	control.hold_count = 0; assert(apply_state(&control, false));
	assert(!control.monitor_cutoff && !control.report_cutoff);
	control.hold_count = 1; assert(apply_state(&control, false));
	monitor.running = false; unsigned before = probe_calls;
	expire_holds(&control); assert(control.hold_count == 1);
	control.hold_count = 0; assert(apply_state(&control, false));
	assert(probe_calls == before && control.report_paused && probe_paused);
	pthread_mutex_destroy(&control.mutex);
}

static void write_fixture(const char *path, const char *content) {
	FILE *stream = fopen(path, "w"); assert(stream); assert(fputs(content, stream) >= 0); assert(!fclose(stream)); assert(!chmod(path, 0600));
}

static void test_state_and_paths(void) {
	char directory[] = "/tmp/fsmod-control-unit-XXXXXX"; assert(mkdtemp(directory));
	FileMonitor monitor = { .running = true }; FmControl control; initial_control(&control, &monitor);
	assert(snprintf(control.state_path, sizeof(control.state_path), "%s/leases", directory) > 0);
	char error[256]; control.hold_count = 2;
	strcpy(control.holds[0].id, "com.example.future"); control.holds[0].all = true; control.holds[0].expires = monotonic_now() + 30;
	strcpy(control.holds[1].id, "com.example.expired"); control.holds[1].expires = monotonic_now() - 1;
	assert(save_state(&control, error, sizeof(error)));
	struct stat st; assert(!lstat(control.state_path, &st)); assert((st.st_mode & 0777) == 0600);
	control.hold_count = 0; assert(load_state(&control, error, sizeof(error)));
	assert(control.hold_count == 1 && control.holds[0].all && !strcmp(control.holds[0].id, "com.example.future"));
	write_fixture(control.state_path, "FSMOD-CONTROL-1 different-boot\nmalformed content intentionally ignored\n");
	control.hold_count = 0; assert(load_state(&control, error, sizeof(error))); assert(!control.hold_count);
	write_fixture(control.state_path, "not a valid header\n"); assert(!load_state(&control, error, sizeof(error)));
	char invalid[512];
	assert(snprintf(invalid, sizeof(invalid), "FSMOD-CONTROL-1 %s\ncom.example.bad all nan\n", control.boot) > 0);
	write_fixture(control.state_path, invalid); assert(!load_state(&control, error, sizeof(error)));
	write_fixture(control.state_path, "FSMOD-CONTROL-1 other\n"); assert(!chmod(control.state_path, 0644));
	assert(!load_state(&control, error, sizeof(error))); assert(!save_state(&control, error, sizeof(error)));
	assert(!unlink(control.state_path));
	char target[PATH_MAX]; assert(snprintf(target, sizeof(target), "%s/target", directory) > 0); write_fixture(target, "untouched");
	assert(!symlink(target, control.state_path)); assert(!load_state(&control, error, sizeof(error))); assert(!save_state(&control, error, sizeof(error)));
	assert(!unlink(control.state_path)); assert(!unlink(target));
	/* Cleanup must never remove an endpoint replacement owned by someone else. */
	assert(snprintf(control.socket_path, sizeof(control.socket_path), "%s/endpoint", directory) > 0);
	write_fixture(control.socket_path, "replacement");
	assert(!lstat(control.socket_path, &st)); control.socket_device = st.st_dev; control.socket_inode = st.st_ino;
	unlink_own_socket(&control); assert(!lstat(control.socket_path, &st)); assert(!unlink(control.socket_path));
	char normal[PATH_MAX], shortened[PATH_MAX], long_pid[PATH_MAX];
	assert(fm_control_path("/var/run/fsmod.pid", normal, sizeof(normal))); assert(!strcmp(normal, "/var/run/fsmod.pid.ctl"));
	memset(long_pid, 'a', 400); long_pid[0] = '/'; long_pid[400] = 0;
	assert(fm_control_path(long_pid, shortened, sizeof(shortened))); assert(strlen(shortened) < sizeof(((struct sockaddr_un *)0)->sun_path));
	assert(!strncmp(shortened, "/tmp/fsmod-", 11)); assert(fm_control_path(long_pid, normal, sizeof(normal))); assert(!strcmp(normal, shortened));
	assert(valid_id("com.example_test-123") && !valid_id("bad/id") && !valid_id("bad id"));
	char id[129]; memset(id, 'a', sizeof(id)); id[128] = 0; assert(!valid_id(id));
	pthread_mutex_destroy(&control.mutex); assert(!rmdir(directory));
}

int main(void) {
	test_gate_transitions(); test_clock_scope_and_shutdown(); test_state_and_paths();
	puts("control unit tests passed"); return 0;
}
