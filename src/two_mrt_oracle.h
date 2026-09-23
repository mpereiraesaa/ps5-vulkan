#ifndef PS5VK_TWO_MRT_ORACLE_H
#define PS5VK_TWO_MRT_ORACLE_H

/* DXVK262-T06 independentBlend oracle for the two-target witness. Pure: no
 * Vulkan, no logging, no allocation. The console witness and the host
 * regressions call the same predicate, so the verdict the artifact reports is
 * the one the tests exercise.
 *
 * The witness draws one fragment module that writes two colour attachments
 * (experiments/graphics/runtime_two_mrt.frag): Location 0 carries
 * (0.25,0.50,0.75,1.0) and Location 1 carries (0.80,0.40,0.20,0.50). Both
 * attachments are cleared to a colour neither export contains, and the two
 * reads must land in their own target: an implementation that dropped the
 * second export, or that wrote one value into both targets, cannot pass. */

/* RGBA8 attachment zero must report.  0.25/0.50/0.75 are not exactly
 * representable in UNORM8 (63.75/127.5/191.25), so they are judged inside
 * PS5VK_TWO_MRT_TOLERANCE and only when the alpha lands on 255. */
enum { PS5VK_TWO_MRT_TARGET0_R = 64, PS5VK_TWO_MRT_TARGET0_G = 128,
       PS5VK_TWO_MRT_TARGET0_B = 191, PS5VK_TWO_MRT_TARGET0_A = 255 };
/* RGBA8 attachment one must report.  The colour channels are exact in UNORM8
 * (0.80 -> 204, 0.40 -> 102, 0.20 -> 51); 0.50 is 127.5, so the alpha keeps the
 * same one-LSB allowance the control side uses. */
enum { PS5VK_TWO_MRT_TARGET1_R = 204, PS5VK_TWO_MRT_TARGET1_G = 102,
       PS5VK_TWO_MRT_TARGET1_B = 51, PS5VK_TWO_MRT_TARGET1_A = 128 };
enum { PS5VK_TWO_MRT_TOLERANCE = 1 };
/* The two reads must differ by more than twice the tolerance on a channel the
 * two exports disagree about, so a renderer that wrote one export into both
 * attachments cannot pass on the tolerance. */
enum { PS5VK_TWO_MRT_MIN_DELTA = 2 * PS5VK_TWO_MRT_TOLERANCE + 1 };

/* Returns non-zero only when attachment zero carries the first export,
 * attachment one carries the second (within the documented tolerance) and the
 * two differ by more than PS5VK_TWO_MRT_MIN_DELTA on a colour channel. */
int ps5vk_two_mrt_verdict(const unsigned char target0[4],
    const unsigned char target1[4]);

#endif
