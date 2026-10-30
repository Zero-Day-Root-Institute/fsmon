#ifndef INCLUDE_FM_CONTROL_H
#define INCLUDE_FM_CONTROL_H

#include <stddef.h>
#include "report.h"

typedef struct FmControl FmControl;

/* One private local control endpoint per configured PID file. */
bool fm_control_path(const char *pid_file, char *path, size_t capacity);
FmControl *fm_control_start(const char *pid_file, FmReporter *reporter, const FileMonitor *monitor,
	char *error, size_t error_size);
void fm_control_stop(FmControl *control);
/* A successful begin holds the gate until end. This makes pause ACK a
 * barrier for both JSON logging and submission, not merely a flag change. */
bool fm_control_event_begin(FmControl *control, const FileMonitorEvent *event);
bool fm_control_should_report(FmControl *control, const FileMonitorEvent *event);
void fm_control_event_end(FmControl *control);
bool fm_control_is_internal(FmControl *control, const char *path);
/* argv starts at status/pause/resume/run; --socket bypasses configuration lookup. */
int fm_control_cli(int argc, char **argv, const char *default_socket);

#endif
