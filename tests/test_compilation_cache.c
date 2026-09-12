#include "compilation_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* 1. Test cache creation */
    struct ps5vk_compilation_cache *cache = ps5vk_compilation_cache_create(4, 1024 * 1024);
    assert(cache != NULL);

    /* Synthetic SPIR-V modules */
    uint32_t spv_a[32] = {0x07230203, 1, 2, 3, 4, 5, 6, 7};
    uint32_t spv_b[32] = {0x07230203, 10, 20, 30, 40, 50, 60, 70};

    struct VkPipelineLayout_T layout = {0};
    layout.set_count = 1;
    layout.sets[0].count = 2;
    layout.sets[0].binding[0].count = 1;
    layout.sets[0].binding[0].stages = VK_SHADER_STAGE_COMPUTE_BIT;
    layout.sets[0].binding[1].count = 1;
    layout.sets[0].binding[1].stages = VK_SHADER_STAGE_COMPUTE_BIT;

    /* 2. Build keys */
    struct ps5vk_cache_key key_a, key_b, key_mut;
    assert(ps5vk_cache_build_key(spv_a, 32, "main", &layout, &key_a));
    assert(ps5vk_cache_build_key(spv_b, 32, "main", &layout, &key_b));
    assert(memcmp(&key_a, &key_b, sizeof(key_a)) != 0);

    /* Key rejection on nulls / invalid */
    assert(!ps5vk_cache_build_key(NULL, 32, "main", &layout, &key_mut));
    assert(!ps5vk_cache_build_key(spv_a, 0, "main", &layout, &key_mut));
    assert(!ps5vk_cache_build_key(spv_a, 32, NULL, &layout, &key_mut));
    assert(!ps5vk_cache_build_key(spv_a, 32, "main", NULL, &key_mut));

    /* 3. Lookup on empty cache: miss */
    assert(ps5vk_compilation_cache_lookup(cache, &key_a, spv_a) == NULL);
    struct ps5vk_cache_stats stats;
    ps5vk_compilation_cache_get_stats(cache, &stats);
    assert(stats.misses == 1);
    assert(stats.hits == 0);
    assert(stats.compiles == 0);

    /* 4. Insert program A */
    uint32_t code_a[16] = {0x11111111, 0x22222222};
    struct ps5vk_compiled_program prog_a = {
        .gfx = 1013, .wave_size = 32, .code_words = 16, .spirv_words = 32,
        .user_sgprs = 3, .wgp_mode = 1, .vgprs = 16, .sgprs = 32,
        .descriptor_count = 2, .descriptors = {{0, 0, 0, 0}, {0, 1, 0, 4}}
    };
    struct ps5vk_cache_entry *entry_a = ps5vk_compilation_cache_insert(cache, &key_a, spv_a, &prog_a, code_a);
    assert(entry_a != NULL);
    assert(entry_a->refcount == 1);
    assert(entry_a->program.gfx == 1013);
    assert(entry_a->program.user_sgprs == 3);

    ps5vk_compilation_cache_get_stats(cache, &stats);
    assert(stats.compiles == 1);
    assert(stats.current_entries == 1);

    /* 5. Lookup program A: hit */
    struct ps5vk_cache_entry *hit_a = ps5vk_compilation_cache_lookup(cache, &key_a, spv_a);
    assert(hit_a == entry_a);
    assert(entry_a->refcount == 2); /* Increment on hit */

    ps5vk_compilation_cache_get_stats(cache, &stats);
    assert(stats.hits == 1);

    /* 6. Collision resistance: query with modified SPIR-V but same key */
    uint32_t spv_collision[32];
    memcpy(spv_collision, spv_a, sizeof(spv_a));
    spv_collision[5] = 0xdeadbeef;
    assert(ps5vk_compilation_cache_lookup(cache, &key_a, spv_collision) == NULL);

    /* 7. Key mutations must not hit */
    key_mut = key_a;
    key_mut.target_gfx = 1010; /* Different target */
    assert(ps5vk_compilation_cache_lookup(cache, &key_mut, spv_a) == NULL);

    key_mut = key_a;
    strcpy(key_mut.entry_name, "other_entry");
    assert(ps5vk_compilation_cache_lookup(cache, &key_mut, spv_a) == NULL);

    key_mut = key_a;
    key_mut.compiler_version = 99;
    assert(ps5vk_compilation_cache_lookup(cache, &key_mut, spv_a) == NULL);

    /* 8. Insert up to capacity and verify bounded eviction */
    /* Release references on A */
    ps5vk_cache_entry_release(cache, entry_a); /* refcount = 1 */
    ps5vk_cache_entry_release(cache, hit_a);   /* refcount = 0, still in cache */

    uint32_t code_b[16] = {0x33333333};
    struct ps5vk_cache_entry *e_b = ps5vk_compilation_cache_insert(cache, &key_b, spv_b, &prog_a, code_b);
    assert(e_b != NULL);
    ps5vk_cache_entry_release(cache, e_b); /* refcount = 0 */

    /* Insert 3 more distinct programs to exceed max_entries (4) */
    for (int i = 0; i < 3; i++) {
        uint32_t spv_temp[32] = {0x07230203, 100 + i};
        struct ps5vk_cache_key k_temp;
        char name[16];
        snprintf(name, sizeof(name), "entry_%d", i);
        assert(ps5vk_cache_build_key(spv_temp, 32, name, &layout, &k_temp));
        struct ps5vk_cache_entry *e = ps5vk_compilation_cache_insert(cache, &k_temp, spv_temp, &prog_a, code_a);
        assert(e != NULL);
        ps5vk_cache_entry_release(cache, e);
    }

    ps5vk_compilation_cache_get_stats(cache, &stats);
    assert(stats.current_entries <= 4);
    assert(stats.evictions >= 1);

    /* 9. Referenced entries cannot be evicted even under capacity pressure */
    struct ps5vk_cache_entry *held = ps5vk_compilation_cache_lookup(cache, &key_b, spv_b);
    if (!held) {
        /* B may have been evicted, re-insert and keep refcount > 0 */
        held = ps5vk_compilation_cache_insert(cache, &key_b, spv_b, &prog_a, code_b);
    }
    assert(held != NULL && held->refcount >= 1);

    /* Blast with 10 more inserts */
    for (int i = 0; i < 10; i++) {
        uint32_t spv_temp[32] = {0x07230203, 500 + i};
        struct ps5vk_cache_key k_temp;
        char name[16];
        snprintf(name, sizeof(name), "spam_%d", i);
        assert(ps5vk_cache_build_key(spv_temp, 32, name, &layout, &k_temp));
        struct ps5vk_cache_entry *e = ps5vk_compilation_cache_insert(cache, &k_temp, spv_temp, &prog_a, code_a);
        if (e) ps5vk_cache_entry_release(cache, e);
    }

    /* The held entry's memory and contents must still be valid */
    assert(held->program.gfx == 1013);
    assert(held->program.code[0] == code_b[0]);

    /* Release held entry */
    ps5vk_cache_entry_release(cache, held);

    /* 10. Clean cache destruction */
    ps5vk_compilation_cache_destroy(cache);

    puts("Compilation cache contracts: pass (bounded memory, collision rejection, refcounting)");
    return 0;
}
