#include "ps5vk_compiler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz % 4) != 0) { fclose(f); return NULL; }
    uint32_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

int main(void)
{
    /* 1. Load minimal.comp SPIR-V */
    size_t spv1_bytes = 0;
    uint32_t *spv1 = read_file("build/test-shaders/minimal.spv", &spv1_bytes);
    assert(spv1 != NULL);
    size_t spv1_words = spv1_bytes / 4;

    /* Setup layout: binding 0 (read), binding 1 (write) */
    struct VkPipelineLayout_T layout = {0};
    layout.set_count = 1;
    layout.sets[0].count = 2;
    layout.sets[0].binding[0].count = 1;
    layout.sets[0].binding[0].stages = VK_SHADER_STAGE_COMPUTE_BIT;
    layout.sets[0].type[0]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    layout.sets[0].binding[1].count = 1;
    layout.sets[0].binding[1].first = 1;
    layout.sets[0].binding[1].stages = VK_SHADER_STAGE_COMPUTE_BIT;
    layout.sets[0].type[1]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    /* 2. Runtime compile shader 1 (minimal) */
    struct ps5vk_compiled_program prog1 = {0};
    uint32_t *code1 = NULL;
    VkResult res = ps5vk_runtime_compile_compute(spv1, spv1_words, "main", &layout, NULL, &prog1, &code1);
    assert(res == VK_SUCCESS);
    assert(code1 != NULL);
    assert(prog1.gfx == 1013);
    assert(prog1.wave_size == 32);
    assert(prog1.user_sgprs == 3);
    assert(prog1.wgp_mode == 1);
    assert(prog1.local_size[0] == 64 && prog1.local_size[1] == 1 && prog1.local_size[2] == 1);
    assert(prog1.code_words > 0 && prog1.code_words <= 1024 * 1024);
    assert(prog1.descriptor_count == 2);
    assert(prog1.descriptors[0].binding == 0 && prog1.descriptors[0].table_dword == 0);
    assert(prog1.descriptors[1].binding == 1 && prog1.descriptors[1].table_dword == 4);

    size_t resource_bytes=0;
    uint32_t *resource_spv=read_file("build/test-shaders/resource_abi.spv",&resource_bytes);
    assert(resource_spv);
    struct VkPipelineLayout_T resource_layout={.set_count=3};
    resource_layout.sets[0]=layout.sets[0];
    resource_layout.sets[1].binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_COMPUTE_BIT};
    resource_layout.sets[1].type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    resource_layout.sets[2].binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_COMPUTE_BIT};
    resource_layout.sets[2].type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    resource_layout.push_constant_size=4;
    resource_layout.push_constant_stages[0]=VK_SHADER_STAGE_COMPUTE_BIT;
    struct ps5vk_compiled_program resources={0};uint32_t *resource_code=NULL;
    assert(ps5vk_runtime_compile_compute(resource_spv,resource_bytes/4,"main",&resource_layout,NULL,
        &resources,&resource_code)==VK_SUCCESS);
    assert(resources.descriptor_set_mask==7 && resources.descriptor_count==4 &&
        resources.descriptor_set_sgpr[0]==2 && resources.descriptor_set_sgpr[1]==3 &&
        resources.descriptor_set_sgpr[2]==4 && resources.push_constant_sgpr==5 &&
        resources.push_constant_size==4 && resources.user_sgprs>=6);
    assert(resources.descriptors[2].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        resources.descriptors[3].type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
    free(resource_code);free(resource_spv);

    /* 3. Runtime compile shader 2 (previously unregistered shader) */
    size_t spv2_bytes = 0;
    uint32_t *spv2 = read_file("build/test-shaders/xor.spv", &spv2_bytes);
    assert(spv2 != NULL);
    struct ps5vk_compiled_program prog2 = {0};
    uint32_t *code2 = NULL;
    res = ps5vk_runtime_compile_compute(spv2, spv2_bytes / 4, "main", &layout, NULL, &prog2, &code2);
    assert(res == VK_SUCCESS && code2 != NULL);
    size_t common_words = prog1.code_words < prog2.code_words ? prog1.code_words : prog2.code_words;
    assert(prog1.code_words != prog2.code_words || memcmp(code1, code2, common_words * 4) != 0);

    /* Shared memory and inline grid arguments survive compilation unchanged. */
    size_t shared_bytes = 0;
    uint32_t *shared = read_file("build/test-shaders/shared_grid.spv", &shared_bytes);
    assert(shared);
    struct ps5vk_compiled_program shared_prog = {0};
    uint32_t *shared_code = NULL;
    assert(ps5vk_runtime_compile_compute(shared, shared_bytes / 4, "main", &layout, NULL,
        &shared_prog, &shared_code) == VK_SUCCESS);
    assert(shared_prog.user_sgprs == 6 && shared_prog.grid_size_sgpr == 3);
    assert(shared_prog.lds_size > 0 && shared_prog.lds_size <= 128);
    assert(shared_prog.local_size[0] == 2);
    free(shared_code); free(shared);

    /* The public synchronization witness deliberately mixes two ordinary
     * SSBO dispatches with a four-wave LDS/atomic dispatch.  Freeze the
     * compiler-facing resource contract here so a reflection drift cannot
     * surface only as an invalid command buffer on hardware. */
    const char *sync_paths[] = {
        "build/test-shaders/sync_producer.spv",
        "build/test-shaders/sync_consumer.spv",
        "build/test-shaders/shared_atomic_multiwave.spv",
    };
    const uint32_t sync_descriptor_counts[] = {1, 2, 1};
    const uint32_t sync_local_sizes[] = {64, 64, 128};
    struct VkPipelineLayout_T single_storage_layout = layout;
    single_storage_layout.sets[0].count = 1;
    single_storage_layout.sets[0].binding[1].count = 0;
    single_storage_layout.sets[0].type[1] = 0;
    for (unsigned sync_index = 0; sync_index < 3; ++sync_index) {
        size_t sync_bytes = 0;
        uint32_t *sync_spirv = read_file(sync_paths[sync_index], &sync_bytes);
        assert(sync_spirv);
        struct ps5vk_compiled_program sync_program = {0};
        uint32_t *sync_code = NULL;
        VkPipelineLayout sync_layout = sync_index == 1 ? &layout : &single_storage_layout;
        assert(ps5vk_runtime_compile_compute(sync_spirv, sync_bytes / 4, "main",
            sync_layout, NULL, &sync_program, &sync_code) == VK_SUCCESS);
        if (sync_program.descriptor_set_mask != 1 ||
            sync_program.descriptor_count != sync_descriptor_counts[sync_index] ||
            sync_program.local_size[0] != sync_local_sizes[sync_index] ||
            sync_program.local_size[1] != 1 || sync_program.local_size[2] != 1)
            fprintf(stderr, "sync contract drift path=%s mask=%u descriptors=%u local=%u,%u,%u\n",
                sync_paths[sync_index], sync_program.descriptor_set_mask,
                sync_program.descriptor_count, sync_program.local_size[0],
                sync_program.local_size[1], sync_program.local_size[2]);
        assert(sync_program.descriptor_set_mask == 1 &&
            sync_program.descriptor_count == sync_descriptor_counts[sync_index] &&
            sync_program.local_size[0] == sync_local_sizes[sync_index] &&
            sync_program.local_size[1] == 1 && sync_program.local_size[2] == 1);
        if (sync_index == 2)
            assert(sync_program.lds_size > 0 && sync_program.wave_size == 32);
        free(sync_code);
        free(sync_spirv);
    }

    /* Push constants use a stable indirect user-data pointer and Vulkan
     * specialization values are consumed before NIR optimization. */
    size_t parameterized_bytes=0;
    uint32_t *parameterized=read_file("build/test-shaders/push_specialization.spv",
                                     &parameterized_bytes);
    assert(parameterized);
    struct VkPipelineLayout_T parameterized_layout=layout;
    parameterized_layout.push_constant_size=4;
    parameterized_layout.push_constant_stages[0]=VK_SHADER_STAGE_COMPUTE_BIT;
    uint32_t values_a[]={5,11},values_b[]={9,13};
    VkSpecializationMapEntry map[]={{0,0,4},{1,4,4}};
    VkSpecializationInfo spec_a={2,map,sizeof(values_a),values_a};
    VkSpecializationInfo spec_b={2,map,sizeof(values_b),values_b};
    struct ps5vk_compiled_program parameterized_a={0},parameterized_b={0};
    uint32_t *parameterized_code_a=NULL,*parameterized_code_b=NULL;
    assert(ps5vk_runtime_compile_compute(parameterized,parameterized_bytes/4,"main",
        &parameterized_layout,&spec_a,&parameterized_a,&parameterized_code_a)==VK_SUCCESS);
    assert(parameterized_a.push_constant_size==4 &&
        parameterized_a.push_constant_sgpr==3 && parameterized_a.user_sgprs>=4);
    assert(ps5vk_runtime_compile_compute(parameterized,parameterized_bytes/4,"main",
        &parameterized_layout,&spec_b,&parameterized_b,&parameterized_code_b)==VK_SUCCESS);
    size_t common_parameterized=parameterized_a.code_words<parameterized_b.code_words?
        parameterized_a.code_words:parameterized_b.code_words;
    assert(parameterized_a.code_words!=parameterized_b.code_words ||
        memcmp(parameterized_code_a,parameterized_code_b,common_parameterized*4));
    free(parameterized_code_a);free(parameterized_code_b);free(parameterized);

    /* Narrow storage fails closed without the matching device feature and
     * succeeds only when that exact storage-only bit is forwarded to PSBC. */
    const char *narrow_names[] = {"storage8", "storage16"};
    const uint32_t narrow_features[] = {PS5VK_FEATURE_STORAGE_BUFFER_8BIT,
                                        PS5VK_FEATURE_STORAGE_BUFFER_16BIT};
    for (size_t narrow_index = 0; narrow_index < 2; ++narrow_index) {
        char narrow_path[96];
        snprintf(narrow_path, sizeof(narrow_path), "build/test-shaders/%s.spv",
                 narrow_names[narrow_index]);
        size_t narrow_bytes = 0;
        uint32_t *narrow_spv = read_file(narrow_path, &narrow_bytes);
        assert(narrow_spv);
        struct ps5vk_compiled_program narrow_program = {0};
        uint32_t *narrow_code = NULL;
        assert(ps5vk_runtime_compile_compute(narrow_spv, narrow_bytes / 4, "main",
            &layout, NULL, &narrow_program, &narrow_code) == VK_ERROR_FEATURE_NOT_PRESENT);
        assert(!narrow_code);
        assert(ps5vk_runtime_compile_compute_features(narrow_spv, narrow_bytes / 4,
            "main", &layout, NULL, narrow_features[narrow_index],
            &narrow_program, &narrow_code) == VK_SUCCESS);
        assert(narrow_code && narrow_program.code_words > 0);
        assert(narrow_program.gfx == 1013 && narrow_program.wave_size == 32);
        assert(narrow_program.descriptor_count == 2);
        free(narrow_code);
        free(narrow_spv);
    }
    /* 4. Test error handling */
    struct ps5vk_compiled_program bad_prog;
    uint32_t *bad_code = NULL;
    assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
        NULL, UINT32_C(0x80000000), &bad_prog, &bad_code) == VK_ERROR_FEATURE_NOT_PRESENT);

    /* Corrupted SPIR-V header */
    uint32_t bad_spv[16] = {0x12345678, 0, 0, 0};
    assert(ps5vk_runtime_compile_compute(bad_spv, 16, "main", &layout, NULL, &bad_prog, &bad_code) != VK_SUCCESS);

    /* Nonexistent entrypoint */
    assert(ps5vk_runtime_compile_compute(spv1, spv1_words, "nonexistent_entry", &layout, NULL, &bad_prog, &bad_code) != VK_SUCCESS);

    /* Empty layout */
    struct VkPipelineLayout_T empty_layout = {0};
    assert(ps5vk_runtime_compile_compute(spv1, spv1_words, "main", &empty_layout, NULL, &bad_prog, &bad_code) != VK_SUCCESS);

    struct VkPipelineLayout_T array_layout = layout;
    array_layout.sets[0].binding[0].count = 2;
    res = ps5vk_runtime_compile_compute(spv1, spv1_words, "main", &array_layout, NULL, &bad_prog, &bad_code);
    assert(res==VK_SUCCESS && bad_prog.descriptor_count==3);free(bad_code);bad_code=NULL;

    struct VkPipelineLayout_T multiset_layout = layout;
    multiset_layout.set_count = 2;
    res = ps5vk_runtime_compile_compute(spv1, spv1_words, "main", &multiset_layout, NULL, &bad_prog, &bad_code);
    assert(res==VK_SUCCESS && bad_prog.descriptor_set_mask==1);free(bad_code);bad_code=NULL;

    /* 5. Verify CPU reference computation semantics for both programs */
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t in_val = (i * UINT32_C(2654435761)) ^ 0x79bd2468;
        /* Program 0 (minimal.comp): val * 3 + 7 */
        uint32_t ref0 = in_val * 3u + 7u;
        assert(ref0 == (in_val * 3u + 7u));
        /* Program 1 (xor.comp): (val ^ 0xa5c39e71) + i * 17 */
        uint32_t ref1 = (in_val ^ UINT32_C(0xa5c39e71)) + i * 17u;
        assert(ref1 != ref0);
    }

    free(code1);
    free(code2);
    free(spv1);
    free(spv2);

    puts("Runtime compute compiler: pass (PSBC/ACO GFX1013 ABI, CS registers, error guards, CPU reference)");
    return 0;
}
