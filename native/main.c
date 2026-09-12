/* bootstrap compute profile native experiment; build identity explicitly reports submit mode. */
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "ps5log.h"
#include "ps5_platform.h"
#include "ps5_agc.h"
#include "compute_check.h"
int ps5vk_run_compute(void);
#ifndef PS5VK_SUBMIT
#define PS5VK_SUBMIT 0
#endif

int main(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t boot = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
    ps5log_config cfg;
    const char *loaded = 0;
    const char *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&cfg);
    if (ps5log_load_config(paths, 1, &cfg, &loaded) != 0) _exit(0);
    cfg.udp = 0;
    if (ps5log_init(&cfg, "PPSA99994", "ps5vk", boot) != 0) _exit(0);
    ps5log_printf(PS5LOG_MARK, "PS5VK_BOOT stage=compute-bootstrap submit_enabled=%d", PS5VK_SUBMIT);
    ps5log_hex64(PS5LOG_INFO, "LOG_BOOT_MONOTONIC_NS", boot);
    int load = sceSysmoduleLoadModuleInternal(0x80000094u);
    ps5log_printf(PS5LOG_INFO, "agc_load=%d", load);
    if (load == 0) {
        uint64_t state = 0;
        int init = sceAgcInit(&state, sizeof(state));
        ps5log_printf(PS5LOG_INFO, "agc_init=%d", init);
        if (init == 0) {
            int compute = ps5vk_run_compute();
            ps5log_printf(PS5LOG_INFO, "compute_run=%d", compute);
        }
        int unload = sceSysmoduleUnloadModuleInternal(0x80000094u);
        ps5log_printf(PS5LOG_INFO, "agc_unload=%d", unload);
    }
    ps5log_line(PS5LOG_MARK, "PS5VK_NATIVE_END");
    ps5log_close("native-end");
    _exit(0);
}
