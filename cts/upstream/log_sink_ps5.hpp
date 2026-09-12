#ifndef _CTS_UPSTREAM_LOG_SINK_PS5_HPP
#define _CTS_UPSTREAM_LOG_SINK_PS5_HPP

#ifdef __cplusplus
extern "C" {
#endif

void cts_qpa_sink_init(const char *run_id, const char *selection_hash, const char *eboot_sha256);
void cts_qpa_sink_wait_completion(void);

#ifdef __cplusplus
}
#endif

#endif // _CTS_UPSTREAM_LOG_SINK_PS5_HPP
