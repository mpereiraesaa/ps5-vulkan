#include "log_sink_ps5.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
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

enum
{
    QPA_RAW_CHUNK_BYTES = 384,
    QPA_BASE64_BYTES = 516,
    QPA_FORMAT_STACK_BYTES = 2048,
    QPA_FORMAT_MAX_BYTES = 1024 * 1024
};

typedef struct
{
    uint8_t pending[QPA_RAW_CHUNK_BYTES];
    size_t pending_bytes;
    size_t chunk_seq;
    size_t total_bytes;
    SHA256_CTX sha;
    int open_seen;
    int active;
    int completed;
    int failed;
} QpaSink;

static QpaSink s_sink;

static char s_run_id[64] = "unknown";
static char s_selection_hash[65] = "0";
static char s_eboot_sha256[65] = "0";

static int emit_chunk(const uint8_t *data, size_t size)
{
    char b64_buf[QPA_BASE64_BYTES];
    base64_encode(data, size, b64_buf);
    if (ps5log_printf("QPA", "CHUNK seq=%zu size=%zu data=%s",
                      s_sink.chunk_seq, size, b64_buf) != 0)
        return -1;

    sha256_update(&s_sink.sha, data, size);
    s_sink.chunk_seq++;
    s_sink.total_bytes += size;
    return 0;
}

/* The case the payload is running, logged as it starts.
 *
 * An unclean close leaves no result for the case in flight, and the QPA stream
 * alone cannot say which one that was: the run's own log did not name cases at
 * all, so "the payload died after 452 of 512" could not be turned into a
 * defect. deqp writes this marker as one line through the intercepted stream,
 * and it is the only per-case hook the payload has. */
static void log_case_begin(const char *data, size_t size)
{
    static const char marker[] = "#beginTestCaseResult ";
    const size_t marker_len = sizeof(marker) - 1;
    if (!data || size < marker_len) return;
    for (size_t i = 0; i + marker_len <= size; ++i)
    {
        if (memcmp(data + i, marker, marker_len) != 0) continue;
        const size_t start = i + marker_len;
        size_t end = start;
        while (end < size && data[end] != '\n' && data[end] != '\r') ++end;
        if (end == start) return;
        char name[256];
        size_t copy = end - start;
        if (copy > sizeof(name) - 1) copy = sizeof(name) - 1;
        memcpy(name, data + start, copy);
        name[copy] = '\0';
        ps5log_printf("MARK", "UPSTREAM_CTS_CASE name=%s", name);
        return;
    }
}

static ssize_t qpa_sink_write(const char *data, size_t size)
{
    QpaSink *sink = &s_sink;
    size_t consumed = 0;

    if ((!data && size != 0) || !sink->active || sink->completed || sink->failed)
    {
        errno = EIO;
        return -1;
    }

    log_case_begin(data, size);

    while (consumed < size)
    {
        size_t available = QPA_RAW_CHUNK_BYTES - sink->pending_bytes;
        size_t take = size - consumed;
        if (take > available)
            take = available;

        memcpy(sink->pending + sink->pending_bytes, data + consumed, take);
        sink->pending_bytes += take;
        consumed += take;

        if (sink->pending_bytes == QPA_RAW_CHUNK_BYTES)
        {
            if (emit_chunk(sink->pending, sink->pending_bytes) != 0)
            {
                sink->failed = 1;
                errno = EIO;
                return -1;
            }
            sink->pending_bytes = 0;
        }
    }

    return (ssize_t)size;
}

static int qpa_sink_close(void)
{
    QpaSink *sink = &s_sink;
    if (!sink->active || sink->completed)
    {
        errno = EIO;
        return -1;
    }

    if (!sink->failed && sink->pending_bytes > 0)
    {
        if (emit_chunk(sink->pending, sink->pending_bytes) != 0)
            sink->failed = 1;
        sink->pending_bytes = 0;
    }

    sink->active = 0;
    sink->completed = 1;
    if (sink->failed)
    {
        errno = EIO;
        return -1;
    }

    uint8_t hash[32];
    sha256_final(&sink->sha, hash);

    char hash_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + (i * 2), 3, "%02x", hash[i]);
    hash_hex[64] = '\0';

    if (ps5log_printf("MARK", "UPSTREAM_CTS_END chunks=%zu total_bytes=%zu sha256=%s",
                      sink->chunk_seq, sink->total_bytes, hash_hex) != 0)
    {
        sink->failed = 1;
        errno = EIO;
        return -1;
    }

    return 0;
}

static int is_qpa_stream(FILE *stream)
{
    return stream == (FILE *)&s_sink;
}

} // anonymous namespace

extern "C" {

void cts_qpa_sink_init(const char *run_id, const char *selection_hash, const char *eboot_sha256)
{
    memset(&s_sink, 0, sizeof(s_sink));
    sha256_init(&s_sink.sha);

    strcpy(s_run_id, "unknown");
    strcpy(s_selection_hash, "0");
    strcpy(s_eboot_sha256, "0");

    if (run_id)
    {
        strncpy(s_run_id, run_id, sizeof(s_run_id) - 1);
        s_run_id[sizeof(s_run_id) - 1] = '\0';
    }
    if (selection_hash)
    {
        strncpy(s_selection_hash, selection_hash, sizeof(s_selection_hash) - 1);
        s_selection_hash[sizeof(s_selection_hash) - 1] = '\0';
    }
    if (eboot_sha256)
    {
        strncpy(s_eboot_sha256, eboot_sha256, sizeof(s_eboot_sha256) - 1);
        s_eboot_sha256[sizeof(s_eboot_sha256) - 1] = '\0';
    }
}

int cts_qpa_sink_wait_completion(void)
{
    if (!s_sink.completed || s_sink.failed)
    {
        ps5log_printf("ERR", "QPA sink incomplete completed=%d failed=%d bytes=%zu chunks=%zu",
                      s_sink.completed, s_sink.failed,
                      s_sink.total_bytes, s_sink.chunk_seq);
        return -1;
    }
    return 0;
}

FILE *__real_fopen(const char *path, const char *mode);
int __real_fputs(const char *text, FILE *stream);
int __real_fputc(int character, FILE *stream);
size_t __real_fwrite(const void *data, size_t size, size_t count, FILE *stream);
int __real_fseek(FILE *stream, long offset, int origin);
int __real_fflush(FILE *stream);
int __real_fclose(FILE *stream);

FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (path && (strstr(path, ".qpa") != NULL || strcmp(path, "TestResults.qpa") == 0))
    {
        if (s_sink.open_seen || s_sink.active)
        {
            errno = EBUSY;
            ps5log_printf("ERR", "QPA sink rejects a second log stream");
            return NULL;
        }

        (void)mode;
        s_sink.open_seen = 1;
        s_sink.active = 1;

        if (ps5log_printf("MARK", "UPSTREAM_CTS_START run_id=%s selection_hash=%s eboot_sha256=%s",
                          s_run_id, s_selection_hash, s_eboot_sha256) != 0)
        {
            s_sink.failed = 1;
            s_sink.active = 0;
            s_sink.completed = 1;
            errno = EIO;
            return NULL;
        }

        return (FILE *)&s_sink;
    }

    return __real_fopen(path, mode);
}

int __wrap_fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    if (!is_qpa_stream(stream))
    {
        const int result = vfprintf(stream, format, args);
        va_end(args);
        return result;
    }

    char stack[QPA_FORMAT_STACK_BYTES];
    va_list measure;
    va_copy(measure, args);
    const int required = vsnprintf(stack, sizeof(stack), format, measure);
    va_end(measure);
    if (required < 0 || required > QPA_FORMAT_MAX_BYTES)
    {
        va_end(args);
        s_sink.failed = 1;
        errno = required < 0 ? EIO : EOVERFLOW;
        return -1;
    }

    char *text = stack;
    if ((size_t)required >= sizeof(stack))
    {
        text = (char *)malloc((size_t)required + 1);
        if (!text)
        {
            va_end(args);
            s_sink.failed = 1;
            errno = ENOMEM;
            return -1;
        }
        if (vsnprintf(text, (size_t)required + 1, format, args) != required)
        {
            free(text);
            va_end(args);
            s_sink.failed = 1;
            errno = EIO;
            return -1;
        }
    }
    va_end(args);

    const ssize_t written = qpa_sink_write(text, (size_t)required);
    if (text != stack)
        free(text);
    return written == required ? required : -1;
}

int __wrap_fputs(const char *text, FILE *stream)
{
    if (!is_qpa_stream(stream))
        return __real_fputs(text, stream);
    if (!text)
    {
        s_sink.failed = 1;
        errno = EINVAL;
        return EOF;
    }
    return qpa_sink_write(text, strlen(text)) < 0 ? EOF : 0;
}

int __wrap_fputc(int character, FILE *stream)
{
    if (!is_qpa_stream(stream))
        return __real_fputc(character, stream);
    const unsigned char byte = (unsigned char)character;
    return qpa_sink_write((const char *)&byte, 1) == 1 ? byte : EOF;
}

size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    if (!is_qpa_stream(stream))
        return __real_fwrite(data, size, count, stream);
    if (size != 0 && count > SIZE_MAX / size)
    {
        s_sink.failed = 1;
        errno = EOVERFLOW;
        return 0;
    }
    const size_t bytes = size * count;
    if (bytes > QPA_FORMAT_MAX_BYTES)
    {
        s_sink.failed = 1;
        errno = EOVERFLOW;
        return 0;
    }
    return qpa_sink_write((const char *)data, bytes) == (ssize_t)bytes ? count : 0;
}

int __wrap_fseek(FILE *stream, long offset, int origin)
{
    if (!is_qpa_stream(stream))
        return __real_fseek(stream, offset, origin);

    // qpTestLogWriteRaw() seeks to the end solely to preserve append order.
    // The synchronous sink is intrinsically append-only, so this exact form
    // is a no-op; every other seek is rejected rather than invented.
    if (offset != 0 || origin != SEEK_END || !s_sink.active || s_sink.failed)
    {
        s_sink.failed = 1;
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int __wrap_fflush(FILE *stream)
{
    if (!is_qpa_stream(stream))
        return __real_fflush(stream);
    return s_sink.active && !s_sink.failed ? 0 : EOF;
}

int __wrap_fclose(FILE *stream)
{
    if (!is_qpa_stream(stream))
        return __real_fclose(stream);
    return qpa_sink_close();
}

} // extern "C"
