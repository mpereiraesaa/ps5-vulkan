#ifndef PS5VK_INPUT_ATTACHMENT_ORACLE_H
#define PS5VK_INPUT_ATTACHMENT_ORACLE_H
#include <stddef.h>
#include <stdint.h>

/* The subpass-input readback oracle, as arithmetic rather than as a picture.
 *
 * Subpass 0 writes a deterministic full-screen pattern, subpass 1 reads that
 * attachment through subpassLoad and writes a deterministic transform of it.
 * Both are computed here, independently of the GPU, so the strict run compares
 * every pixel of the real readback against numbers this file produced - and so
 * a failure is classified rather than merely reported: a readback that still
 * holds the pattern means subpass 1 did not read it (pass-through), one that
 * holds the pre-pass background means subpass 1 never ran, one that holds a
 * single value everywhere means the transform collapsed to a constant, and
 * anything else is a mismatch against the expected transform. A readback is
 * only accepted when every pixel is the expected transform.
 *
 * Colour words are the transfer role's own BGRA layout: one little-endian
 * 32-bit word per pixel, 0xAARRGGBB-style packing of a byte order that reads
 * back as B,G,R,A in memory - the same convention the existing colour-scene
 * witnesses and the readback verifier already use. */
enum ps5vk_input_attachment_oracle_verdict {
    PS5VK_INPUT_ATTACHMENT_ORACLE_OK = 0,
    /* Subpass 1 produced nothing: every pixel is still the background. */
    PS5VK_INPUT_ATTACHMENT_ORACLE_SKIPPED,
    /* Subpass 1 ran but did not read the attachment: the pattern survived. */
    PS5VK_INPUT_ATTACHMENT_ORACLE_PASS_THROUGH,
    /* The transform collapsed: one value everywhere, whatever it is. */
    PS5VK_INPUT_ATTACHMENT_ORACLE_CONSTANT,
    /* Some pixel is neither the expected transform nor one of the shapes
     * above, or the expected transform itself holds pixels that would let one
     * of those shapes pass unnoticed. */
    PS5VK_INPUT_ATTACHMENT_ORACLE_MISMATCH
};

struct ps5vk_input_attachment_oracle_result {
    enum ps5vk_input_attachment_oracle_verdict verdict;
    /* How many pixels matched the expected transform, and how many were
     * examined. A verdict of OK means matched == total and total != 0. */
    unsigned long matched, total;
    /* The first pixel that disagreed, for a diagnostic that names a place. */
    unsigned first_x, first_y;
};

/* Subpass 0's deterministic pattern for the pixel at (x, y). */
uint32_t ps5vk_input_attachment_oracle_pattern(uint32_t x, uint32_t y);

/* Subpass 1's deterministic transform of one pattern pixel. It is chosen so
 * that it can never equal its input, so pass-through cannot look like a
 * successful read, and so that it can never equal the background word the
 * scene is cleared to, so a skipped subpass cannot look like a transform. */
uint32_t ps5vk_input_attachment_oracle_transform(uint32_t pattern);

/* The exact word subpass 1 must have written at (x, y). */
uint32_t ps5vk_input_attachment_oracle_expected(uint32_t x, uint32_t y);

/* The word the colour target holds before subpass 0 runs. */
#define PS5VK_INPUT_ATTACHMENT_ORACLE_BACKGROUND UINT32_C(0xff101010)

/* Classify a whole readback. `pixels` is width*height words in the BGRA
 * convention above; anything else about the run - identity, shape, boundary,
 * dependency - is the telemetry verifier's business, not this function's. */
struct ps5vk_input_attachment_oracle_result ps5vk_input_attachment_oracle_verify(
    const uint32_t *pixels, uint32_t width, uint32_t height);

#endif
