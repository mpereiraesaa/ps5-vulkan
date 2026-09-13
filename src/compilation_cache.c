#include "compilation_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal standard SHA-256 implementation */
static inline uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_process_block(uint32_t state[8], const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_hash(const void *data, size_t len, uint8_t digest[32])
{
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    const uint8_t *bytes = (const uint8_t *)data;
    size_t rem = len;
    while (rem >= 64) {
        sha256_process_block(state, bytes);
        bytes += 64;
        rem -= 64;
    }
    uint8_t pad[128];
    memcpy(pad, bytes, rem);
    pad[rem++] = 0x80;
    size_t pad_block_len = (rem <= 56) ? 64 : 128;
    memset(pad + rem, 0, pad_block_len - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        pad[pad_block_len - 1 - i] = (uint8_t)(bits >> (i * 8));
    sha256_process_block(state, pad);
    if (pad_block_len == 128)
        sha256_process_block(state, pad + 64);
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)state[i];
    }
}

static uint64_t hash_key(const struct ps5vk_cache_key *key)
{
    /* FNV-1a 64-bit */
    uint64_t h = UINT64_C(14695981039346656037);
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof(*key); i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

bool ps5vk_cache_build_stage_key(const uint32_t *spirv, size_t spirv_words,
    const char *entry_name, uint32_t stages, uint32_t flags,
    struct ps5vk_cache_key *out_key)
{
    if (!spirv || !spirv_words || spirv_words > SIZE_MAX / sizeof(uint32_t) ||
        !entry_name || !*entry_name || !stages || !out_key)
        return false;
    if (strlen(entry_name) >= sizeof(out_key->entry_name))
        return false;
    memset(out_key, 0, sizeof(*out_key));
    out_key->target_gfx = 1013;
    out_key->stage = stages;
    out_key->compiler_id = PS5VK_COMPILER_ID_PSBC_ACO;
    out_key->compiler_version = PS5VK_COMPILER_VERSION_1;
    out_key->abi_version = PS5VK_CACHE_ABI_VERSION_1;
    out_key->flags = flags;
    strncpy(out_key->entry_name, entry_name, sizeof(out_key->entry_name) - 1);

    out_key->spirv_words = spirv_words;
    sha256_hash(spirv, spirv_words * sizeof(uint32_t), out_key->spirv_sha256);
    return true;
}

bool ps5vk_cache_build_key(const uint32_t *spirv, size_t spirv_words,
    const char *entry_name, VkPipelineLayout layout,
    const VkSpecializationInfo *specialization, uint32_t feature_mask,
    struct ps5vk_cache_key *out_key)
{
    if (!layout || layout->set_count > PS5VK_MAX_SETS ||
        !ps5vk_cache_build_stage_key(spirv, spirv_words, entry_name,
                                    VK_SHADER_STAGE_COMPUTE_BIT, feature_mask, out_key))
        return false;

    out_key->set_count = layout->set_count;
    for (uint32_t s = 0; s < layout->set_count; s++) {
        const struct ps5vk_set_signature *sig = &layout->sets[s];
        out_key->sets[s].count = sig->count;
        for (uint32_t b = 0; b < PS5VK_MAX_BINDINGS; b++) {
            out_key->sets[s].bindings[b].count = sig->binding[b].count;
            out_key->sets[s].bindings[b].first = sig->binding[b].first;
            out_key->sets[s].bindings[b].stages = sig->binding[b].stages;
            out_key->sets[s].bindings[b].type = sig->type[b];
        }
    }
    out_key->push_constant_size = layout->push_constant_size;
    memcpy(out_key->push_constant_stages, layout->push_constant_stages,
           sizeof(out_key->push_constant_stages));

    if (specialization) {
        if (specialization->mapEntryCount > PS5VK_MAX_SPECIALIZATION_CONSTANTS ||
            (specialization->mapEntryCount && !specialization->pMapEntries) ||
            (specialization->dataSize && !specialization->pData)) return false;
        for (uint32_t i = 0; i < specialization->mapEntryCount; ++i) {
            const VkSpecializationMapEntry *source = &specialization->pMapEntries[i];
            if (!source->size || source->size > PS5VK_MAX_SPECIALIZATION_BYTES ||
                source->offset > specialization->dataSize ||
                source->size > specialization->dataSize - source->offset)
                return false;
            uint32_t at = out_key->specialization_count++;
            out_key->specializations[at].constant_id = source->constantID;
            out_key->specializations[at].size = (uint32_t)source->size;
            memcpy(out_key->specializations[at].data,
                   (const uint8_t *)specialization->pData + source->offset,
                   source->size);
        }
        /* Map-entry order is not semantic. Canonicalize it and reject duplicate
         * constant IDs so equivalent VkSpecializationInfo values share a key. */
        for (uint32_t i = 1; i < out_key->specialization_count; ++i) {
            struct ps5vk_cache_specialization value = out_key->specializations[i];
            uint32_t j = i;
            while (j && out_key->specializations[j - 1].constant_id > value.constant_id) {
                out_key->specializations[j] = out_key->specializations[j - 1]; --j;
            }
            out_key->specializations[j] = value;
        }
        for (uint32_t i = 1; i < out_key->specialization_count; ++i)
            if (out_key->specializations[i - 1].constant_id ==
                out_key->specializations[i].constant_id) return false;
    }

    return true;
}

struct ps5vk_compilation_cache *ps5vk_compilation_cache_create(size_t max_entries, size_t max_bytes)
{
    struct ps5vk_compilation_cache *cache = calloc(1, sizeof(*cache));
    if (!cache) return NULL;
    pthread_mutex_init(&cache->mutex, NULL);
    cache->bucket_count = 64;
    cache->buckets = calloc(cache->bucket_count, sizeof(struct ps5vk_cache_entry *));
    if (!cache->buckets) {
        pthread_mutex_destroy(&cache->mutex);
        free(cache);
        return NULL;
    }
    cache->max_entries = max_entries ? max_entries : 64;
    cache->max_bytes = max_bytes ? max_bytes : (4 * 1024 * 1024);
    return cache;
}

static void lru_remove(struct ps5vk_compilation_cache *cache, struct ps5vk_cache_entry *entry)
{
    if (entry->prev_lru) entry->prev_lru->next_lru = entry->next_lru;
    else cache->lru_head = entry->next_lru;
    if (entry->next_lru) entry->next_lru->prev_lru = entry->prev_lru;
    else cache->lru_tail = entry->prev_lru;
    entry->prev_lru = NULL;
    entry->next_lru = NULL;
}

static void lru_push_front(struct ps5vk_compilation_cache *cache, struct ps5vk_cache_entry *entry)
{
    entry->next_lru = cache->lru_head;
    entry->prev_lru = NULL;
    if (cache->lru_head) cache->lru_head->prev_lru = entry;
    cache->lru_head = entry;
    if (!cache->lru_tail) cache->lru_tail = entry;
}

static void free_entry_memory(struct ps5vk_cache_entry *entry)
{
    free(entry->spirv_copy);
    free(entry->code_copy);
    free(entry->payload_copy);
    free(entry);
}

struct ps5vk_cache_entry *ps5vk_compilation_cache_lookup(
    struct ps5vk_compilation_cache *cache,
    const struct ps5vk_cache_key *key,
    const uint32_t *spirv)
{
    if (!cache || !key || !spirv || key->spirv_words > SIZE_MAX / sizeof(uint32_t)) return NULL;
    pthread_mutex_lock(&cache->mutex);

    uint64_t h = hash_key(key);
    size_t idx = h % cache->bucket_count;
    struct ps5vk_cache_entry *entry = cache->buckets[idx];

    while (entry) {
        if (memcmp(&entry->key, key, sizeof(*key)) == 0 &&
            memcmp(entry->spirv_copy, spirv, key->spirv_words * sizeof(uint32_t)) == 0) {
            /* Cache hit */
            entry->refcount++;
            lru_remove(cache, entry);
            lru_push_front(cache, entry);
            cache->stats.hits++;
            pthread_mutex_unlock(&cache->mutex);
            return entry;
        }
        entry = entry->next_hash;
    }

    cache->stats.misses++;
    pthread_mutex_unlock(&cache->mutex);
    return NULL;
}

static void evict_unreferenced(struct ps5vk_compilation_cache *cache, size_t needed_bytes)
{
    while ((cache->stats.current_entries >= cache->max_entries ||
            needed_bytes > cache->max_bytes - cache->stats.current_bytes) &&
           cache->lru_tail) {
        /* Find oldest entry with refcount == 0 */
        struct ps5vk_cache_entry *cur = cache->lru_tail;
        while (cur && cur->refcount > 0) {
            cur = cur->prev_lru;
        }
        if (!cur) {
            /* All entries currently held by active pipelines; cannot evict */
            break;
        }

        /* Remove from hash bucket */
        uint64_t h = hash_key(&cur->key);
        size_t idx = h % cache->bucket_count;
        struct ps5vk_cache_entry **pp = &cache->buckets[idx];
        while (*pp && *pp != cur) pp = &(*pp)->next_hash;
        if (*pp) *pp = cur->next_hash;

        lru_remove(cache, cur);
        cur->in_cache = false;
        cache->stats.current_entries--;
        cache->stats.current_bytes -= cur->size_bytes;
        cache->stats.evictions++;

        free_entry_memory(cur);
    }
}

static struct ps5vk_cache_entry *insert_entry(
    struct ps5vk_compilation_cache *cache,
    const struct ps5vk_cache_key *key,
    const uint32_t *spirv,
    const struct ps5vk_compiled_program *program,
    const uint32_t *code,
    const void *payload,
    size_t payload_bytes)
{
    if (!cache || !key || !spirv || !key->spirv_words ||
        key->spirv_words > SIZE_MAX / sizeof(uint32_t)) return NULL;
    if (program ? (!code || payload || payload_bytes ||
                   program->code_words > SIZE_MAX / sizeof(uint32_t)) :
                  (!payload || !payload_bytes || code)) return NULL;
    size_t spirv_bytes = key->spirv_words * sizeof(uint32_t);
    size_t code_bytes = program ? program->code_words * sizeof(uint32_t) : 0;
    size_t total_bytes = sizeof(struct ps5vk_cache_entry);
    if (spirv_bytes > SIZE_MAX - total_bytes) return NULL;
    total_bytes += spirv_bytes;
    if (code_bytes > SIZE_MAX - total_bytes) return NULL;
    total_bytes += code_bytes;
    if (payload_bytes > SIZE_MAX - total_bytes) return NULL;
    total_bytes += payload_bytes;
    pthread_mutex_lock(&cache->mutex);

    /* Reject entry if it exceeds total budget or if max_entries is zero */
    if (total_bytes > cache->max_bytes || cache->max_entries == 0) {
        pthread_mutex_unlock(&cache->mutex);
        return NULL;
    }

    evict_unreferenced(cache, total_bytes);

    /* Enforce strict bounds: if unreferenced entries could not be evicted
     * because all entries are actively referenced, reject insertion. */
    if (cache->stats.current_entries >= cache->max_entries ||
        total_bytes > cache->max_bytes - cache->stats.current_bytes) {
        pthread_mutex_unlock(&cache->mutex);
        return NULL;
    }

    struct ps5vk_cache_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        pthread_mutex_unlock(&cache->mutex);
        return NULL;
    }

    entry->spirv_copy = malloc(spirv_bytes);
    if (program) entry->code_copy = malloc(code_bytes);
    else entry->payload_copy = malloc(payload_bytes);
    if (!entry->spirv_copy || (program ? !entry->code_copy : !entry->payload_copy)) {
        free_entry_memory(entry);
        pthread_mutex_unlock(&cache->mutex);
        return NULL;
    }

    memcpy(entry->spirv_copy, spirv, spirv_bytes);
    entry->key = *key;
    if (program) {
        memcpy(entry->code_copy, code, code_bytes);
        entry->program = *program;
        entry->program.code = entry->code_copy;
        entry->program.spirv = entry->spirv_copy;
        snprintf(entry->entry_name_copy, sizeof(entry->entry_name_copy), "%s", key->entry_name);
        entry->program.entry = entry->entry_name_copy;
    } else {
        memcpy(entry->payload_copy, payload, payload_bytes);
        entry->payload_bytes = payload_bytes;
    }

    entry->size_bytes = total_bytes;
    entry->refcount = 1;
    entry->in_cache = true;

    /* Insert into hash table */
    uint64_t h = hash_key(key);
    size_t idx = h % cache->bucket_count;
    entry->next_hash = cache->buckets[idx];
    cache->buckets[idx] = entry;

    lru_push_front(cache, entry);

    cache->stats.compiles++;
    cache->stats.current_entries++;
    if (cache->stats.current_entries > cache->stats.peak_entries)
        cache->stats.peak_entries = cache->stats.current_entries;
    cache->stats.current_bytes += total_bytes;
    if (cache->stats.current_bytes > cache->stats.peak_bytes)
        cache->stats.peak_bytes = cache->stats.current_bytes;

    pthread_mutex_unlock(&cache->mutex);
    return entry;
}

struct ps5vk_cache_entry *ps5vk_compilation_cache_insert(
    struct ps5vk_compilation_cache *cache, const struct ps5vk_cache_key *key,
    const uint32_t *spirv, const struct ps5vk_compiled_program *program,
    const uint32_t *code)
{
    if (!program) return NULL;
    return insert_entry(cache, key, spirv, program, code, NULL, 0);
}

struct ps5vk_cache_entry *ps5vk_compilation_cache_insert_payload(
    struct ps5vk_compilation_cache *cache, const struct ps5vk_cache_key *key,
    const uint32_t *input_words, const void *payload, size_t payload_bytes)
{
    return insert_entry(cache, key, input_words, NULL, NULL, payload, payload_bytes);
}

void ps5vk_cache_entry_acquire(struct ps5vk_cache_entry *entry)
{
    if (entry) {
        __atomic_fetch_add(&entry->refcount, 1, __ATOMIC_RELAXED);
    }
}

void ps5vk_cache_entry_release(
    struct ps5vk_compilation_cache *cache,
    struct ps5vk_cache_entry *entry)
{
    if (!entry) return;
    if (cache) pthread_mutex_lock(&cache->mutex);

    int rc = __atomic_sub_fetch(&entry->refcount, 1, __ATOMIC_RELEASE);
    if (rc == 0 && !entry->in_cache) {
        free_entry_memory(entry);
    }

    if (cache) pthread_mutex_unlock(&cache->mutex);
}

void ps5vk_compilation_cache_get_stats(
    struct ps5vk_compilation_cache *cache,
    struct ps5vk_cache_stats *out_stats)
{
    if (!cache || !out_stats) return;
    pthread_mutex_lock(&cache->mutex);
    *out_stats = cache->stats;
    pthread_mutex_unlock(&cache->mutex);
}

void ps5vk_compilation_cache_destroy(struct ps5vk_compilation_cache *cache)
{
    if (!cache) return;
    pthread_mutex_lock(&cache->mutex);

    for (size_t i = 0; i < cache->bucket_count; i++) {
        struct ps5vk_cache_entry *cur = cache->buckets[i];
        while (cur) {
            struct ps5vk_cache_entry *next = cur->next_hash;
            cur->in_cache = false;
            if (cur->refcount == 0) {
                free_entry_memory(cur);
            }
            cur = next;
        }
    }
    free(cache->buckets);
    cache->buckets = NULL;

    pthread_mutex_unlock(&cache->mutex);
    pthread_mutex_destroy(&cache->mutex);
    free(cache);
}
