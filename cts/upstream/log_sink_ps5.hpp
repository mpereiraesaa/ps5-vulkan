#ifndef _CTS_UPSTREAM_LOG_SINK_PS5_HPP
#define _CTS_UPSTREAM_LOG_SINK_PS5_HPP

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void cts_qpa_sink_init(const char *run_id, const char *selection_hash, const char *eboot_sha256);
int cts_qpa_sink_wait_completion(void);

/* Internal linker-wrap entry point.  Declared here so its bounded stream
 * contract can be exercised by the host regression without changing CTS. */
FILE *__wrap_fopen(const char *path, const char *mode);
int __wrap_fprintf(FILE *stream, const char *format, ...);
int __wrap_fputs(const char *text, FILE *stream);
int __wrap_fputc(int character, FILE *stream);
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *stream);
int __wrap_fseek(FILE *stream, long offset, int origin);
int __wrap_fflush(FILE *stream);
int __wrap_fclose(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif // _CTS_UPSTREAM_LOG_SINK_PS5_HPP
