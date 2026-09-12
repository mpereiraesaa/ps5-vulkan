#ifndef CTS_ADAPTER_H
#define CTS_ADAPTER_H

#include <ps5vk/ps5vk.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cts_status {
    CTS_STATUS_PASS = 0,
    CTS_STATUS_FAIL = 1,
    CTS_STATUS_NOT_SUPPORTED = 2,
    CTS_STATUS_SKIP = 3
} cts_status_t;

typedef struct cts_result {
    const char *case_name;
    cts_status_t status;
    char details[512];
} cts_result_t;

/* Run a single CTS test case by exact case name. */
cts_result_t cts_run_case(const char *case_name);

/* Run all 26 focused CTS test cases in sequence. */
int cts_run_all(int format_json, int format_tap);

#ifdef __cplusplus
}
#endif

#endif /* CTS_ADAPTER_H */
