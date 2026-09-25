/* Small SHA-256 (FIPS 180-4) used by the payload to report the digest of
 * the eboot it is running from. Pure C, host-testable. */
#ifndef DXVK_NATIVE_SHA256_H
#define DXVK_NATIVE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dxvk_sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
} dxvk_sha256;

static inline uint32_t dxvk_sha256_ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static inline void dxvk_sha256_compress(dxvk_sha256 *s, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], v[8];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = dxvk_sha256_ror(w[i - 15], 7) ^ dxvk_sha256_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = dxvk_sha256_ror(w[i - 2], 17) ^ dxvk_sha256_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (unsigned i = 0; i < 8; ++i) v[i] = s->state[i];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = dxvk_sha256_ror(v[4], 6) ^ dxvk_sha256_ror(v[4], 11) ^ dxvk_sha256_ror(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
        uint32_t s0 = dxvk_sha256_ror(v[0], 2) ^ dxvk_sha256_ror(v[0], 13) ^ dxvk_sha256_ror(v[0], 22);
        uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (unsigned i = 0; i < 8; ++i) s->state[i] += v[i];
}

static inline void dxvk_sha256_init(dxvk_sha256 *s)
{
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (unsigned i = 0; i < 8; ++i) s->state[i] = initial[i];
    s->length = 0;
    s->used = 0;
}

static inline void dxvk_sha256_update(dxvk_sha256 *s, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    s->length += size;
    while (size) {
        size_t take = 64 - s->used < size ? 64 - s->used : size;
        for (size_t i = 0; i < take; ++i) s->block[s->used + i] = p[i];
        s->used += take; p += take; size -= take;
        if (s->used == 64) {
            dxvk_sha256_compress(s, s->block);
            s->used = 0;
        }
    }
}

/* Writes 64 lowercase hex digits and a NUL into hex[65]. */
static inline void dxvk_sha256_hex(dxvk_sha256 *s, char hex[65])
{
    uint64_t bits = s->length * 8u;
    uint8_t pad = 0x80, zero = 0, length[8];
    dxvk_sha256_update(s, &pad, 1);
    while (s->used != 56) dxvk_sha256_update(s, &zero, 1);
    for (unsigned i = 0; i < 8; ++i) length[i] = (uint8_t)(bits >> (56 - 8 * i));
    dxvk_sha256_update(s, length, 8);
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        uint8_t byte = (uint8_t)(s->state[i / 4] >> (24 - 8 * (i % 4)));
        hex[2 * i] = digits[byte >> 4];
        hex[2 * i + 1] = digits[byte & 15];
    }
    hex[64] = 0;
}

#ifdef __cplusplus
}
#endif

#endif
