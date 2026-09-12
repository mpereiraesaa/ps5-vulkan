/* Native compiler/link ABI experiment. No draw or Vulkan graphics claim. */
#include "vk_internal.h"
#include "graphics_embedded.h"
#include "ps5_platform.h"
#include "ps5log.h"
#include <time.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    struct timespec ts = {0}; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t boot = (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
    ps5log_config cfg; const char *loaded = NULL;
    const char *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&cfg);
    if (ps5log_load_config(paths, 1, &cfg, &loaded)) _exit(0);
    cfg.udp = 0;
    if (ps5log_init(&cfg, "PPSA99994", "ps5vk", boot)) _exit(0);
    ps5log_line(PS5LOG_MARK, "PS5VK_BOOT stage=graphics-link submit_enabled=0");
    int rc = sceSysmoduleLoadModuleInternal(0x80000094u);
    ps5log_printf(PS5LOG_INFO, "PS5VK_LINK_LOAD rc=%d", rc);
    if (!rc) {
        uint64_t state = 0;
        rc = sceAgcInit(&state, sizeof(state));
        ps5log_printf(PS5LOG_INFO, "PS5VK_LINK_INIT rc=%d", rc);
        if (!rc) {
            struct ps5vk_native_memory_budget budget = {131072, 0};
            struct ps5vk_memory_backend memory = ps5vk_native_graphics_memory_backend();
            memory.context = &budget;
            void *arena = NULL, *backing = NULL;
            rc = memory.allocate(memory.context, 65536, &arena, &backing);
            if (!rc) {
                ps5log_printf(PS5LOG_MARK, "PS5VK_GRAPHICS_ALIGNMENT alignment=131072 remainder=%llu charged=%llu",
                    (unsigned long long)((uintptr_t)arena % 131072), (unsigned long long)budget.used);
                memset(arena, 0, 65536);
                struct ps5vk_graphics_pair *pair = arena;
                ps5log_line(PS5LOG_MARK, "PS5VK_LINK_PREPARE_BEGIN");
                rc = ps5vk_graphics_pair_prepare(pair, (unsigned char *)arena + 4096,
                                                65536 - 4096, &graphics_input);
                ps5log_printf(PS5LOG_MARK, "PS5VK_LINK_RESULT rc=%d ready=%u", rc, pair->ready);
                memory.release(memory.context, backing);
            }
            ps5log_printf(PS5LOG_MARK, "PS5VK_LINK_CLEANUP rc=%d allocations_bytes=%llu", rc,
                          (unsigned long long)budget.used);
        }
        rc = sceSysmoduleUnloadModuleInternal(0x80000094u);
        ps5log_printf(PS5LOG_INFO, "PS5VK_LINK_UNLOAD rc=%d", rc);
    }
    ps5log_close("graphics-link-end");
    _exit(0);
}
