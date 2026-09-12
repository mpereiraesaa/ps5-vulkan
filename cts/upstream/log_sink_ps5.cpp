#include "log_sink_ps5.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include "ps5log.h"

namespace
{

/* SHA-256 implementation */
typedef struct
{
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} SHA256_CTX;

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define Ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define Sigma0(x) (ROR32((x), 2) ^ ROR32((x), 13) ^ ROR32((x), 22))
#define Sigma1(x) (ROR32((x), 6) ^ ROR32((x), 11) ^ ROR32((x), 25))
#define sigma0(x) (ROR32((x), 7) ^ ROR32((x), 18) ^ ((x) >> 3))
#define sigma1(x) (ROR32((x), 17) ^ ROR32((x), 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
    for (int i = 16; i < 64; i++)
        w[i] = sigma1(w[i - 2]) + w[i - 7] + sigma0(w[i - 15]) + w[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + K256[i] + w[i];
        uint32_t t2 = Sigma0(a) + Maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len)
{
    size_t buffer_bytes = (size_t)(ctx->count & 63);
    ctx->count += len;
    size_t offset = 0;

    if (buffer_bytes > 0)
    {
        size_t needed = 64 - buffer_bytes;
        if (len < needed)
        {
            memcpy(ctx->buffer + buffer_bytes, data, len);
            return;
        }
        memcpy(ctx->buffer + buffer_bytes, data, needed);
        sha256_transform(ctx, ctx->buffer);
        offset = needed;
    }

    while (offset + 64 <= len)
    {
        sha256_transform(ctx, data + offset);
        offset += 64;
    }

    if (offset < len)
        memcpy(ctx->buffer, data + offset, len - offset);
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[32])
{
    uint64_t total_bits = ctx->count * 8;
    size_t buffer_bytes = (size_t)(ctx->count & 63);
    ctx->buffer[buffer_bytes++] = 0x80;

    if (buffer_bytes > 56)
    {
        memset(ctx->buffer + buffer_bytes, 0, 64 - buffer_bytes);
        sha256_transform(ctx, ctx->buffer);
        buffer_bytes = 0;
    }
    memset(ctx->buffer + buffer_bytes, 0, 56 - buffer_bytes);
    for (int i = 7; i >= 0; i--)
        ctx->buffer[56 + (7 - i)] = (uint8_t)(total_bits >> (i * 8));
    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++)
    {
        hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* Base64 encoding */
static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t in_len, char *out)
{
    size_t i = 0, j = 0;
    for (; i + 2 < in_len; i += 3)
    {
        uint32_t octet_a = in[i];
        uint32_t octet_b = in[i + 1];
        uint32_t octet_c = in[i + 2];
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64_chars[(triple >> 18) & 0x3F];
        out[j++] = b64_chars[(triple >> 12) & 0x3F];
        out[j++] = b64_chars[(triple >> 6) & 0x3F];
        out[j++] = b64_chars[triple & 0x3F];
    }
    if (i < in_len)
    {
        uint32_t octet_a = in[i++];
        uint32_t octet_b = (i < in_len) ? in[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8);
        out[j++] = b64_chars[(triple >> 18) & 0x3F];
        out[j++] = b64_chars[(triple >> 12) & 0x3F];
        out[j++] = (in_len % 3 == 2) ? b64_chars[(triple >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
}

/* Pipe sink state */
static int s_pipe_fds[2] = { -1, -1 };
static pthread_t s_pump_thread;
static int s_pump_active = 0;

static char s_run_id[64] = "unknown";
static char s_selection_hash[65] = "0";
static char s_eboot_sha256[65] = "0";

void *qpa_pump_worker(void *arg)
{
    (void)arg;
    uint8_t raw_buf[384];
    char b64_buf[516];
    size_t chunk_seq = 0;
    size_t total_bytes = 0;

    SHA256_CTX sha_ctx;
    sha256_init(&sha_ctx);

    ps5log_printf("MARK", "UPSTREAM_CTS_START run_id=%s selection_hash=%s eboot_sha256=%s",
                  s_run_id, s_selection_hash, s_eboot_sha256);

    ssize_t n;
    while ((n = read(s_pipe_fds[0], raw_buf, sizeof(raw_buf))) > 0)
    {
        sha256_update(&sha_ctx, raw_buf, (size_t)n);
        base64_encode(raw_buf, (size_t)n, b64_buf);
        ps5log_printf("QPA", "CHUNK seq=%zu size=%zu data=%s", chunk_seq++, (size_t)n, b64_buf);
        total_bytes += (size_t)n;
    }

    close(s_pipe_fds[0]);
    s_pipe_fds[0] = -1;

    uint8_t hash[32];
    sha256_final(&sha_ctx, hash);

    char hash_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + (i * 2), 3, "%02x", hash[i]);
    hash_hex[64] = '\0';

    ps5log_printf("MARK", "UPSTREAM_CTS_END chunks=%zu total_bytes=%zu sha256=%s",
                  chunk_seq, total_bytes, hash_hex);

    return NULL;
}

} // anonymous namespace

extern "C" {

void cts_qpa_sink_init(const char *run_id, const char *selection_hash, const char *eboot_sha256)
{
    if (run_id) strncpy(s_run_id, run_id, sizeof(s_run_id) - 1);
    if (selection_hash) strncpy(s_selection_hash, selection_hash, sizeof(s_selection_hash) - 1);
    if (eboot_sha256) strncpy(s_eboot_sha256, eboot_sha256, sizeof(s_eboot_sha256) - 1);
}

void cts_qpa_sink_wait_completion(void)
{
    if (s_pump_active)
    {
        pthread_join(s_pump_thread, NULL);
        s_pump_active = 0;
    }
}

FILE *__real_fopen(const char *path, const char *mode);

FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (path && (strstr(path, ".qpa") != NULL || strcmp(path, "TestResults.qpa") == 0))
    {
        if (pipe(s_pipe_fds) != 0)
        {
            ps5log_printf("ERR", "Failed to create QPA pipe");
            return NULL;
        }

        s_pump_active = 1;
        if (pthread_create(&s_pump_thread, NULL, qpa_pump_worker, NULL) != 0)
        {
            ps5log_printf("ERR", "Failed to create QPA pump thread");
            close(s_pipe_fds[0]);
            close(s_pipe_fds[1]);
            s_pipe_fds[0] = s_pipe_fds[1] = -1;
            s_pump_active = 0;
            return NULL;
        }

        FILE *f = fdopen(s_pipe_fds[1], mode);
        return f;
    }

    return __real_fopen(path, mode);
}

} // extern "C"
