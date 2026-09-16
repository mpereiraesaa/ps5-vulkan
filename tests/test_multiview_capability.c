/* T02-E1a, the foundation half: the internal multiview capability and the
 * measured floors, and nothing public. This test proves the facts the next
 * slice will advertise from - and, just as importantly, that a platform whose
 * mask lacks the bit reports the internal support as FALSE rather than
 * inheriting an assumption from the profile. */
#include "vk_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

VkResult ps5vk_platform_query(struct ps5vk_platform *);

int main(void)
{
    /* The floors are exactly what the two private witnesses measured: six views
     * rendered into six ordered array layers, and one instance at 2^27-1. */
    assert(PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR == 6);
    assert(PS5VK_MULTIVIEW_INSTANCE_INDEX_FLOOR == 134217727);
    assert(PS5VK_MULTIVIEW_INSTANCE_INDEX_FLOOR == (1u << 27) - 1u);

    /* The capability gate answers from the mask alone, so every "without"
     * case is testable without a device. */
    assert(!ps5vk_platform_multiview_supported(0));
    assert(!ps5vk_platform_multiview_supported(
        PS5VK_FEATURE_ROBUST_BUFFER_ACCESS | PS5VK_FEATURE_SHADER_DRAW_PARAMETERS));
    assert(ps5vk_platform_multiview_supported(PS5VK_FEATURE_MULTIVIEW));
    assert(PS5VK_FEATURE_MULTIVIEW != PS5VK_FEATURE_STORAGE_BUFFER_8BIT &&
           PS5VK_FEATURE_MULTIVIEW != PS5VK_FEATURE_STORAGE_BUFFER_16BIT &&
           PS5VK_FEATURE_MULTIVIEW != PS5VK_FEATURE_ROBUST_BUFFER_ACCESS &&
           PS5VK_FEATURE_MULTIVIEW != PS5VK_FEATURE_SHADER_DRAW_PARAMETERS);

    /* The host platform really carries it. */
    struct ps5vk_platform host;
    assert(ps5vk_platform_query(&host) == VK_SUCCESS);
    assert(ps5vk_platform_multiview_supported(host.supported_features));

    /* The console platform is native-only, so the fact is pinned at its source:
     * it must declare the same bit, next to the capability block it belongs
     * with, and E1a must not have taught anything else to advertise. */
    FILE *ps5 = fopen("native/platform_ps5.c", "rb");
    assert(ps5);
    char source[1 << 16];
    const size_t bytes = fread(source, 1, sizeof(source) - 1, ps5);
    fclose(ps5);
    source[bytes] = '\0';
    assert(strstr(source, "PS5VK_FEATURE_MULTIVIEW"));
    /* The slice is foundation-only: no public surface may mention the
     * extension, the capability or the floors yet. */
    static const char *forbidden[] = {
        "VK_KHR_MULTIVIEW_EXTENSION_NAME", "ps5vk_platform_multiview_supported",
    };
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i)
        assert(!strstr(source, forbidden[i]));
    struct {
        const char *path;
        const char *forbidden;
    } public_side[] = {
        {"src/vk_device.c", "VK_KHR_MULTIVIEW_EXTENSION_NAME"},
        {"src/vk_device.c", "VK_PHYSICAL_DEVICE_MULTIVIEW_FEATURES"},
        {"src/vk_render_pass.c", "PS5VK_FEATURE_MULTIVIEW"},
    };
    for (unsigned i = 0; i < sizeof(public_side) / sizeof(public_side[0]); ++i) {
        FILE *f = fopen(public_side[i].path, "rb");
        assert(f);
        const size_t n = fread(source, 1, sizeof(source) - 1, f);
        fclose(f);
        source[n] = '\0';
        assert(!strstr(source, public_side[i].forbidden));
    }
    puts("Multiview capability: internal bit and measured floors, nothing advertised");
    return 0;
}
