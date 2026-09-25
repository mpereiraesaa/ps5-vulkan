/* Host harness: runs the same workload through the host DXVK build and a
 * host Vulkan driver. It validates the shaders and the oracle end to end; it
 * is not evidence about ps5vk or the PS5. */
#include "workload.h"

#include <SDL2/SDL.h>

#include <cstdio>

static void stage(const char *name, const char *state, const char *detail)
{
    std::printf("DXVK_NATIVE_STAGE stage=%s state=%s %s\n", name, state, detail);
}

static void oracle(const dxvk_oracle_result *r)
{
    std::printf("DXVK_ORACLE checked=%u mismatches=%u checksum=%08x expected_checksum=%08x\n",
                r->checked, r->mismatches, r->checksum, r->expected_checksum);
    for (uint32_t i = 0; i < r->reported; ++i)
        std::printf("DXVK_ORACLE_MISMATCH x=%u y=%u got=%08x expected=%08x\n",
                    r->first[i].x, r->first[i].y, r->first[i].got, r->first[i].expected);
}

int main()
{
    /* DXVK's host SDL2 WSI backend needs SDL video initialized. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("SDL video initialization failed: %s\n", SDL_GetError());
        return 3;
    }
    DxvkNativeHooks hooks = {stage, oracle};
    DxvkNativeSummary summary;
    int outcome = dxvk_native_run_workload(hooks, &summary);
    std::printf("DXVK_NATIVE_RESULT outcome=%d last_stage=%s hr=0x%08x\n",
                outcome, summary.last_stage, summary.create_hresult);
    SDL_Quit();
    return outcome;
}
