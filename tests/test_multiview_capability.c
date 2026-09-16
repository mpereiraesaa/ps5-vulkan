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

/* The capability line is placed correctly only when the guard that encloses it
 * is the LAST copy of that exact guard before it. platform_ps5.c contains
 * several identical graphics guards - the queue_flags block is one - so the
 * first match says nothing about the feature assignment; only the nearest
 * preceding guard does, and the bit has to sit before that guard's own #endif
 * with no #else in between. Pure string work, so the negative fixtures below
 * run it too. */
static const char *last_before(const char *hay, const char *needle, const char *before)
{
    const char *found = NULL;
    for (const char *at = strstr(hay, needle); at && at < before;
         at = strstr(at + 1, needle))
        found = at;
    return found;
}

static int capability_placement_ok(const char *source)
{
    const char *guard = "#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW";
    const char *bit = strstr(source, "PS5VK_FEATURE_MULTIVIEW");
    if (!bit || strstr(bit + 1, "PS5VK_FEATURE_MULTIVIEW")) return 0;
    const char *guard_at = last_before(source, guard, bit);
    if (!guard_at || last_before(source, guard, bit) != guard_at) return 0;
    const char *endif_at = strstr(guard_at, "\n#endif");
    if (!endif_at || !(guard_at < bit && bit < endif_at)) return 0;
    /* The guard directly encloses the bit: nothing re-opens the branch between
     * them and no #else splits it before the bit. */
    if (last_before(source, guard, bit) != guard_at) return 0;
    if (strstr(guard_at, "\n#else") && strstr(guard_at, "\n#else") < endif_at) return 0;
    /* The compiler-only configuration, which starts at the runtime-compiler
     * block's #else, must not carry the bit at all. */
    const char *compiler_else = strstr(endif_at, "\n#else");
    if (compiler_else && strstr(compiler_else, "PS5VK_FEATURE_MULTIVIEW")) return 0;
    return 1;
}

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
    /* ...and it is guarded by the LAST matching graphics-api+draw guard before
     * it, so a runtime-compiler-only build cannot report itself
     * multiview-capable. The check is a pure function of the text, so the two
     * fixtures below prove it rejects the placements a first-match scan would
     * have accepted. */
    assert(capability_placement_ok(source));
    static const char unguarded[] =
        "#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW\n"
        "    platform->queue_flags = VK_QUEUE_GRAPHICS_BIT;\n"
        "#endif\n"
        "    platform->supported_features |= PS5VK_FEATURE_MULTIVIEW;\n"
        "#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW\n"
        "    platform->supported_features |= PS5VK_FEATURE_SHADER_DRAW_PARAMETERS;\n"
        "#endif\n";
    /* The old placement: an earlier identical guard exists, but the bit is NOT
     * inside the nearest one. A first-match scan accepted this; it must not. */
    assert(!capability_placement_ok(unguarded));
    static const char compiler_only[] =
        "#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER\n"
        "    platform->supported_features |= PS5VK_FEATURE_MULTIVIEW;\n"
        "#else\n"
        "#endif\n";
    assert(!capability_placement_ok(compiler_only));
    static const char inside_else[] =
        "#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW\n"
        "    platform->supported_features |= PS5VK_FEATURE_SHADER_DRAW_PARAMETERS;\n"
        "#else\n"
        "    platform->supported_features |= PS5VK_FEATURE_MULTIVIEW;\n"
        "#endif\n";
    assert(!capability_placement_ok(inside_else));
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
