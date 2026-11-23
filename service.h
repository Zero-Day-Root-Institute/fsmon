#ifndef FSMOD_SERVICE_H
#define FSMOD_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/* Detach before acquiring the PID lock or creating backend handles/threads.
 * Inherited descriptors except standard I/O are closed in the final child.
 * The original parent exits only after fm_service_ready() reports startup
 * status (or the startup pipe closes before a successful report). */
bool fm_service_detach(const char *log_file, char *error, size_t error_size);
bool fm_service_lock(const char *pid_file, char *error, size_t error_size);
/* Validate a currently locked PID file and request graceful shutdown with
 * SIGTERM. Success means the signal was sent, not that shutdown completed. */
bool fm_service_stop(const char *pid_file, char *error, size_t error_size);
void fm_service_ready(bool success);
void fm_service_release(void);

#endif
