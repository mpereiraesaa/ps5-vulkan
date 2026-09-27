/* Offscreen and bounded presentation workloads through pinned DXVK D3D11/DXGI.
 * Platform independent: the PS5 payload and the host harness supply the
 * telemetry hooks. */
#ifndef DXVK_NATIVE_WORKLOAD_H
#define DXVK_NATIVE_WORKLOAD_H

#include "oracle.h"

#include <stdint.h>

#define DXVK_PRESENT_WIDTH 1920u
#define DXVK_PRESENT_HEIGHT 1080u
#define DXVK_PRESENT_FRAMES 3u

struct DxvkNativeHooks {
    /* state is "begin", "ok" or "fail"; detail may be empty, never null. */
    void (*stage)(const char *stage, const char *state, const char *detail);
    void (*oracle)(const dxvk_oracle_result *result);
    void (*frame)(uint32_t index, const dxvk_oracle_result *result, uint32_t present_hr) = nullptr;
};

enum DxvkNativeOutcome {
    DXVK_NATIVE_RENDERED = 0,        /* every pixel matched */
    DXVK_NATIVE_REFUSED = 1,         /* a D3D11 call failed before readback */
    DXVK_NATIVE_ORACLE_MISMATCH = 2, /* readback completed with wrong pixels */
};

struct DxvkNativeSummary {
    int outcome;
    uint32_t create_hresult;
    uint32_t feature_level;
    uint32_t device_refs_at_release;  /* Release() result of the device */
    uint32_t context_refs_at_release;
    uint32_t presented_frames;
    uint32_t swapchain_refs_at_release;
    char last_stage[48];
};

/* A null window selects the historical offscreen oracle; a non-null native
 * WSI handle selects three swapchain frames. No Vulkan capability override. */
int dxvk_native_run_workload(const DxvkNativeHooks &hooks, DxvkNativeSummary *summary,
                             void *presentation_window = nullptr);

#endif
