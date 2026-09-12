#ifndef PS5VK_COMPILATION_CACHE_H
#define PS5VK_COMPILATION_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "vk_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS5VK_COMPILER_ID_PSBC_ACO  UINT32_C(0x50534243) /* "PSBC" */
#define PS5VK_COMPILER_VERSION_1    UINT32_C(1)
#define PS5VK_CACHE_ABI_VERSION_1   UINT32_C(1)

/* Immutable compiler cache key capturing every compiler-relevant input. */
struct ps5vk_cache_key {
    uint32_t target_gfx;        /* GFX target (1013) */
    uint32_t stage;             /* shader stage or linked-stage mask */
    uint32_t compiler_id;       /* PS5VK_COMPILER_ID_PSBC_ACO */
    uint32_t compiler_version;  /* Compiler implementation version */
    uint32_t abi_version;       /* ABI version */
    uint32_t flags;             /* Optimization / compilation flags */
    char entry_name[64];        /* Entry point string */
    uint32_t set_count;
    struct {
        uint32_t count;
        struct {
            uint32_t count;
            uint32_t first;
            uint32_t stages;
            uint32_t type;
        } bindings[PS5VK_MAX_BINDINGS];
    } sets[PS5VK_MAX_SETS];
    size_t spirv_words;
    uint8_t spirv_sha256[32];
};

struct ps5vk_cache_stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t compiles;
    uint64_t evictions;
    uint32_t current_entries;
    uint32_t peak_entries;
    size_t current_bytes;
    size_t peak_bytes;
};

struct ps5vk_cache_entry {
    struct ps5vk_cache_key key;
    uint32_t *spirv_copy;
    struct ps5vk_compiled_program program;
    uint32_t *code_copy;
    /* Pointer-free serialized compiler output for non-compute stages. */
    void *payload_copy;
    size_t payload_bytes;
    char entry_name_copy[64];
    size_t size_bytes;
    int refcount;
    bool in_cache;
    struct ps5vk_cache_entry *next_hash;
    struct ps5vk_cache_entry *prev_lru;
    struct ps5vk_cache_entry *next_lru;
};

struct ps5vk_compilation_cache {
    pthread_mutex_t mutex;
    struct ps5vk_cache_entry **buckets;
    size_t bucket_count;
    struct ps5vk_cache_entry *lru_head;
    struct ps5vk_cache_entry *lru_tail;
    size_t max_entries;
    size_t max_bytes;
    struct ps5vk_cache_stats stats;
};

/* Initialize compilation cache. Returns NULL on allocation failure. */
struct ps5vk_compilation_cache *ps5vk_compilation_cache_create(size_t max_entries, size_t max_bytes);

/* Destroy cache. Unreferenced entries are freed immediately; referenced entries
 * will be freed when their holders call ps5vk_cache_entry_release. */
void ps5vk_compilation_cache_destroy(struct ps5vk_compilation_cache *cache);

/* Build a cache key from pipeline creation parameters. Returns false on invalid inputs. */
bool ps5vk_cache_build_key(
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    struct ps5vk_cache_key *out_key
);

/* Canonical stage-domain key for compiler adapters with a word-serialized
 * input. Adapters encode stage-specific options, entrypoints and resources in
 * those words; this routine adds compiler/ABI/target identity and SHA-256. */
bool ps5vk_cache_build_stage_key(const uint32_t *words, size_t word_count,
    const char *entry_name, uint32_t stages, uint32_t flags,
    struct ps5vk_cache_key *out_key);

/* Look up an entry in the cache. On hit, increments entry refcount and returns entry.
 * On miss, returns NULL. */
struct ps5vk_cache_entry *ps5vk_compilation_cache_lookup(
    struct ps5vk_compilation_cache *cache,
    const struct ps5vk_cache_key *key,
    const uint32_t *spirv
);

/* Insert a newly compiled program into the cache. The cache copies/owns the code and spirv.
 * Increments refcount to 1 for the caller and returns the entry. */
struct ps5vk_cache_entry *ps5vk_compilation_cache_insert(
    struct ps5vk_compilation_cache *cache,
    const struct ps5vk_cache_key *key,
    const uint32_t *spirv,
    const struct ps5vk_compiled_program *program,
    const uint32_t *code
);

/* Copy a pointer-free compiled payload using the same bounded LRU/lease rules.
 * key + input words must encode ALL compiler inputs, including both entrypoints
 * for a linked pair. A distinct stage/ABI separates this from compute entries.
 * The caller retains its input and payload allocations on success or failure.
 * Deserialization must validate sizes and must not persist process pointers. */
struct ps5vk_cache_entry *ps5vk_compilation_cache_insert_payload(
    struct ps5vk_compilation_cache *cache,
    const struct ps5vk_cache_key *key,
    const uint32_t *input_words,
    const void *payload,
    size_t payload_bytes);

/* Acquire an additional reference on a cache entry. */
void ps5vk_cache_entry_acquire(struct ps5vk_cache_entry *entry);

/* Release a reference on a cache entry. Frees entry if refcount reaches 0 and not in cache. */
void ps5vk_cache_entry_release(
    struct ps5vk_compilation_cache *cache,
    struct ps5vk_cache_entry *entry
);

/* Query current statistics. */
void ps5vk_compilation_cache_get_stats(
    struct ps5vk_compilation_cache *cache,
    struct ps5vk_cache_stats *out_stats
);

#ifdef __cplusplus
}
#endif

#endif /* PS5VK_COMPILATION_CACHE_H */
