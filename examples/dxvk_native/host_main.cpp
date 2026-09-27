/* Host harness: runs the same workload through the host DXVK build and a
 * host Vulkan driver. It validates the shaders and the oracle end to end; it
 * is not evidence about ps5vk or the PS5. */
#include "workload.h"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>

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

static void frame(uint32_t index, const dxvk_oracle_result *r, uint32_t hr)
{
    std::printf("DXVK_PRESENT_FRAME index=%u checked=%u mismatches=%u checksum=%08x expected_checksum=%08x present_hr=0x%08x\n",
                index, r->checked, r->mismatches, r->checksum, r->expected_checksum, hr);
}

int main(int argc, char **argv)
{
    /* DXVK's host SDL2 WSI backend needs SDL video initialized. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("SDL video initialization failed: %s\n", SDL_GetError());
        return 3;
    }
    const bool present = argc == 2 && !std::strcmp(argv[1], "--present");
    SDL_Window *window = nullptr;
    if (present) {
        window = SDL_CreateWindow("DXVK presentation witness", SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, DXVK_PRESENT_WIDTH, DXVK_PRESENT_HEIGHT,
            SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
        if (!window) { std::fprintf(stderr, "%s\n", SDL_GetError()); SDL_Quit(); return 3; }
        SDL_PumpEvents();
    }
    DxvkNativeHooks hooks = {stage, oracle, frame};
    DxvkNativeSummary summary;
    int outcome = dxvk_native_run_workload(hooks, &summary, window);
    std::printf("DXVK_NATIVE_RESULT outcome=%d last_stage=%s hr=0x%08x\n",
                outcome, summary.last_stage, summary.create_hresult);
    std::printf("DXVK_PRESENT_RESULT frames=%u swapchain_refs=%u device_refs=%u context_refs=%u\n",
                summary.presented_frames, summary.swapchain_refs_at_release,
                summary.device_refs_at_release, summary.context_refs_at_release);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return outcome;
}
