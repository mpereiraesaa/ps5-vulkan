/* ps5log/1 telemetry shared by the payload entry point, the DXVK logger sink
 * and the Vulkan call trace. Every function serializes on one recursive lock,
 * because DXVK logs and calls Vulkan from several threads and ps5log itself is
 * single-threaded. */
#ifndef DXVK_NATIVE_TELEMETRY_H
#define DXVK_NATIVE_TELEMETRY_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void dxvk_telemetry_lock(void);
void dxvk_telemetry_unlock(void);

/* One ps5log record, formatted; level is INFO/WARN/ERR/MARK. */
void dxvk_telemetry_emit(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* DXVK_NATIVE_STAGE stage=<stage> state=<begin|ok|fail> <detail> */
void dxvk_telemetry_stage(const char *stage, const char *state, const char *detail);
/* Name of the innermost stage that began and has not ended. */
const char *dxvk_telemetry_open_stage(void);

/* Remember the most recent Vulkan call and its parameters. */
void dxvk_telemetry_last_call(const char *call, const char *params);

/* Record a refusal candidate. The first one becomes the run's first refusal.
 * source is "vulkan", "dxvk_log" or "d3d11". */
void dxvk_telemetry_refusal(const char *source, const char *call, int result,
                            const char *detail);

/* Emit DXVK_FIRST_REFUSAL once at the end of the run. */
void dxvk_telemetry_first_refusal_summary(void);

/* Emit the Vulkan trace counters (calls, refusals, missing entry points). */
void dxvk_trace_summary(void);

#ifdef __cplusplus
}
#endif

#endif
