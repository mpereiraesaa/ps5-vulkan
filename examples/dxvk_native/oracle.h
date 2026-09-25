/* Pixel oracle for the DXVK native D3D11 workload.
 *
 * The workload clears a 64x64 R8G8B8A8_UNORM render target to CLEAR_RGBA,
 * then draws a fullscreen triangle through a 48x64 viewport. The pixel shader
 * (pattern.ps.hlsl) writes, for pixel (x, y):
 *   r = 4x, g = 4y, b = (7x + 13y) & 255, a = 255
 * so columns 0..47 check rasterized geometry and position, and columns
 * 48..63 check that the clear survived outside the viewport.
 *
 * Pure C, no platform headers: the PS5 payload, the host harness and the host
 * unit test all use this exact code. */
#ifndef DXVK_NATIVE_ORACLE_H
#define DXVK_NATIVE_ORACLE_H

#include <stddef.h>
#include <stdint.h>

#define DXVK_ORACLE_WIDTH 64u
#define DXVK_ORACLE_HEIGHT 64u
#define DXVK_ORACLE_VIEWPORT_WIDTH 48u
#define DXVK_ORACLE_CLEAR_R 64u
#define DXVK_ORACLE_CLEAR_G 128u
#define DXVK_ORACLE_CLEAR_B 192u
#define DXVK_ORACLE_CLEAR_A 255u
#define DXVK_ORACLE_MAX_REPORTED 8u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dxvk_oracle_mismatch {
    uint32_t x, y;
    uint32_t got, expected; /* packed little-endian RGBA: r in bits 0..7 */
} dxvk_oracle_mismatch;

typedef struct dxvk_oracle_result {
    uint32_t checked;
    uint32_t mismatches;
    uint32_t reported;
    uint32_t checksum;          /* FNV-1a over the observed tightly packed rows */
    uint32_t expected_checksum; /* FNV-1a over the expected image */
    dxvk_oracle_mismatch first[DXVK_ORACLE_MAX_REPORTED];
} dxvk_oracle_result;

static inline uint32_t dxvk_oracle_pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (r & 255u) | ((g & 255u) << 8) | ((b & 255u) << 16) | ((a & 255u) << 24);
}

/* Expected packed RGBA of pixel (x, y). */
static inline uint32_t dxvk_oracle_expected(uint32_t x, uint32_t y)
{
    if (x >= DXVK_ORACLE_VIEWPORT_WIDTH)
        return dxvk_oracle_pack(DXVK_ORACLE_CLEAR_R, DXVK_ORACLE_CLEAR_G,
                                DXVK_ORACLE_CLEAR_B, DXVK_ORACLE_CLEAR_A);
    return dxvk_oracle_pack(x * 4u, y * 4u, (x * 7u + y * 13u) & 255u, 255u);
}

static inline uint32_t dxvk_oracle_fnv1a(uint32_t hash, uint32_t packed)
{
    for (unsigned i = 0; i < 4; ++i) {
        hash ^= (packed >> (8u * i)) & 255u;
        hash *= 16777619u;
    }
    return hash;
}

static inline uint32_t dxvk_oracle_expected_checksum(void)
{
    uint32_t hash = 2166136261u;
    for (uint32_t y = 0; y < DXVK_ORACLE_HEIGHT; ++y)
        for (uint32_t x = 0; x < DXVK_ORACLE_WIDTH; ++x)
            hash = dxvk_oracle_fnv1a(hash, dxvk_oracle_expected(x, y));
    return hash;
}

/* Compare a mapped image (row_pitch bytes per row, >= 4 * width) with the
 * expected one. Every pixel is checked; padding between rows is ignored.
 * Returns 0 when every pixel matches, 1 otherwise, -1 on bad arguments. */
static inline int dxvk_oracle_check(const uint8_t *pixels, size_t row_pitch,
                                    dxvk_oracle_result *result)
{
    if (!result) return -1;
    dxvk_oracle_result r;
    r.checked = r.mismatches = r.reported = 0;
    r.checksum = 2166136261u;
    r.expected_checksum = dxvk_oracle_expected_checksum();
    for (unsigned i = 0; i < DXVK_ORACLE_MAX_REPORTED; ++i)
        r.first[i].x = r.first[i].y = r.first[i].got = r.first[i].expected = 0;
    if (!pixels || row_pitch < 4u * DXVK_ORACLE_WIDTH) {
        *result = r;
        return -1;
    }
    for (uint32_t y = 0; y < DXVK_ORACLE_HEIGHT; ++y) {
        const uint8_t *row = pixels + (size_t)y * row_pitch;
        for (uint32_t x = 0; x < DXVK_ORACLE_WIDTH; ++x) {
            const uint8_t *p = row + 4u * x;
            uint32_t got = dxvk_oracle_pack(p[0], p[1], p[2], p[3]);
            uint32_t expected = dxvk_oracle_expected(x, y);
            r.checksum = dxvk_oracle_fnv1a(r.checksum, got);
            ++r.checked;
            if (got != expected) {
                if (r.reported < DXVK_ORACLE_MAX_REPORTED) {
                    dxvk_oracle_mismatch *m = &r.first[r.reported++];
                    m->x = x; m->y = y; m->got = got; m->expected = expected;
                }
                ++r.mismatches;
            }
        }
    }
    *result = r;
    return r.mismatches ? 1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif
