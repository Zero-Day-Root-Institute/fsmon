#ifndef INCLUDE_FM_REPORT_H
#define INCLUDE_FM_REPORT_H

#include <stddef.h>
#include "config.h"
#include "fsmod.h"

typedef struct FmReporter FmReporter;

typedef struct {
	size_t queued;
	bool in_flight;
	bool paused;
	uint64_t delivered, failed, dropped;
} FmReporterStatus;

/* Returns {source:"fsmod",data:{...}} for HTTP and configured event logs.
 * Caller owns returned JSON. The event is never modified. */
char *fm_event_json(const FileMonitor *fm, const FileMonitorEvent *ev);
/* Config stays immutable and alive until stop returns. In-process HTTP uses
 * system CFNetwork on iOS and libcurl on supported desktop targets. */
FmReporter *fm_reporter_start(const FmConfig *config, char *error, size_t error_size);
/* Copies the event now; never waits for network or queue space. */
bool fm_reporter_submit(FmReporter *reporter, const FileMonitor *fm, const FileMonitorEvent *ev);
/* Pause rejects new submissions and cancels an active transfer. When keeping
 * pending work, the interrupted active payload is put back at the queue head
 * if capacity permits. A timeout leaves the reporter paused. */
bool fm_reporter_pause(FmReporter *reporter, bool paused, bool discard_pending,
	unsigned timeout_seconds);
void fm_reporter_status(FmReporter *reporter, FmReporterStatus *status);
/* Suppress events attributed to the monitor process and its children. */
bool fm_reporter_is_internal(FmReporter *reporter, const FileMonitorEvent *ev);
/* Stop producers first. Drain until deadline, cancel transfers and discard leftovers. */
void fm_reporter_stop(FmReporter *reporter);

#endif
