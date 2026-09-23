#ifndef PS5VK_DUAL_SOURCE_ORACLE_H
#define PS5VK_DUAL_SOURCE_ORACLE_H

/* DXVK262-T06 dual-source blend oracle.  Pure: no Vulkan, no logging, no
 * allocation.  The console witness and the host regressions call the same
 * predicate, so the verdict the artifact reports is the one the tests exercise.
 *
 * The witness draws one fragment module that exports both sources of
 * attachment zero (experiments/graphics/runtime_dual_source.frag): primary
 * (0.25,0.50,0.75,1.0) at Location 0 Index 0 and secondary
 * (0.80,0.40,0.20,0.50) at Location 0 Index 1.  With blending disabled the
 * target receives the primary.  With color = SRC1_COLOR x src + ZERO x dst and
 * alpha = ONE x src + ZERO x dst it receives primary.rgb * secondary.rgb with
 * the primary alpha, and neither read depends on the clear colour. */

/* RGBA8 the control must report.  These channels are not exactly representable
 * in UNORM8 (0.25/0.50/0.75 -> 63.75/127.5/191.25), so they are judged inside
 * PS5VK_DUAL_SOURCE_TOLERANCE and only when the alpha lands on 255. */
enum { PS5VK_DUAL_SOURCE_CONTROL_R = 64, PS5VK_DUAL_SOURCE_CONTROL_G = 128,
       PS5VK_DUAL_SOURCE_CONTROL_B = 191, PS5VK_DUAL_SOURCE_CONTROL_A = 255 };
/* RGBA8 the blend must report.  The products are exact in UNORM8
 * (0.20 -> 51, 0.15 -> 38), so this side is judged on its bytes. */
enum { PS5VK_DUAL_SOURCE_BLEND_R = 51, PS5VK_DUAL_SOURCE_BLEND_G = 51,
       PS5VK_DUAL_SOURCE_BLEND_B = 38, PS5VK_DUAL_SOURCE_BLEND_A = 255 };
enum { PS5VK_DUAL_SOURCE_TOLERANCE = 1 };
/* The two reads must differ by more than twice the tolerance, so a difference
 * the tolerance could explain never counts as evidence that the blender
 * consumed the secondary export. */
enum { PS5VK_DUAL_SOURCE_MIN_DELTA = 2 * PS5VK_DUAL_SOURCE_TOLERANCE + 1 };

/* Returns non-zero only when the control read matches the primary export, the
 * candidate read matches the blended value exactly, and the two differ by more
 * than PS5VK_DUAL_SOURCE_MIN_DELTA on a channel the blend changes. */
int ps5vk_dual_source_verdict(const unsigned char control[4],
    const unsigned char candidate[4]);

#endif
