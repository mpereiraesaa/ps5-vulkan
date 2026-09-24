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
    /* Core Int16 arithmetic is separate from 16-bit storage. The fixture
     * declares Int16 alone and uses only 32-bit storage buffers. */
    size_t int16_bytes = 0;
    uint32_t *int16_spv = read_file("build/test-shaders/shader_int16.spv", &int16_bytes);
    assert(int16_spv);
    struct ps5vk_compiled_program int16_program = {0};
    uint32_t *int16_code = NULL;
    assert(ps5vk_runtime_compile_compute_features(int16_spv, int16_bytes / 4,
        "main", &layout, NULL, 0, &int16_program, &int16_code) ==
        VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!int16_code);
    assert(ps5vk_runtime_compile_compute_features(int16_spv, int16_bytes / 4,
        "main", &layout, NULL, PS5VK_FEATURE_SHADER_INT16,
        &int16_program, &int16_code) == VK_SUCCESS);
    assert(int16_code && int16_program.code_words &&
           int16_program.descriptor_count == 2);
    free(int16_code);
    free(int16_spv);
    /* The pinned compiler fixtures declare the actual SPIR-V capabilities.
     * The adapter must keep each option off until its logical-device bit is
     * enabled; DeviceScope also needs the base Vulkan memory model. */
    const struct {
        const char *shader;
        uint32_t feature;
        VkBool32 uses_push;
    } t08_cases[] = {
        {"t08_address", PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS, VK_TRUE},
        {"t08_memory_model_queue", PS5VK_FEATURE_VULKAN_MEMORY_MODEL, VK_FALSE},
        {"t08_memory_model", PS5VK_FEATURE_VULKAN_MEMORY_MODEL |
            PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE, VK_FALSE},
    };
    for (size_t i = 0; i < sizeof(t08_cases) / sizeof(t08_cases[0]); ++i) {
        char path[128];
        snprintf(path, sizeof(path), "build/test-shaders/%s.spv",
                 t08_cases[i].shader);
        size_t bytes = 0;
        uint32_t *spirv = read_file(path, &bytes);
        assert(spirv);
        struct VkPipelineLayout_T t08_layout = {0};
        if (t08_cases[i].uses_push) {
            t08_layout.push_constant_size = 8;
            t08_layout.push_constant_stages[0] = VK_SHADER_STAGE_COMPUTE_BIT;
        } else {
            t08_layout.set_count = 1;
            t08_layout.sets[0].count = 1;
            t08_layout.sets[0].binding[0].count = 1;
            t08_layout.sets[0].binding[0].stages = VK_SHADER_STAGE_COMPUTE_BIT;
            t08_layout.sets[0].type[0] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        struct ps5vk_compiled_program t08_program = {0};
        uint32_t *t08_code = NULL;
        assert(ps5vk_runtime_compile_compute_features(spirv, bytes / 4, "main",
            &t08_layout, NULL, 0, &t08_program, &t08_code) == VK_ERROR_FEATURE_NOT_PRESENT);
        assert(!t08_code);
        if (i == 2) {
            assert(ps5vk_runtime_compile_compute_features(spirv, bytes / 4, "main",
                &t08_layout, NULL, PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE,
                &t08_program, &t08_code) == VK_ERROR_FEATURE_NOT_PRESENT);
            assert(!t08_code);
            assert(ps5vk_runtime_compile_compute_features(spirv, bytes / 4, "main",
                &t08_layout, NULL, PS5VK_FEATURE_VULKAN_MEMORY_MODEL,
                &t08_program, &t08_code) == VK_ERROR_FEATURE_NOT_PRESENT);
            assert(!t08_code);
        }
        assert(ps5vk_runtime_compile_compute_features(spirv, bytes / 4, "main",
            &t08_layout, NULL, t08_cases[i].feature,
            &t08_program, &t08_code) == VK_SUCCESS);
        assert(t08_code && t08_program.code_words && t08_program.gfx == 1013);
        free(t08_code);
        free(spirv);
    }
    /* The original ssbo_local_barrier_multiple_groups shader uses GLSL450
     * OpMemoryModel and Device-scope barriers. Advertising the separate KHR
     * base feature on the logical device must not reinterpret that legacy
     * module as VulkanKHR and demand DeviceScope for it. */
    size_t legacy_bytes = 0;
    uint32_t *legacy = read_file("build/test-shaders/cts_ssbo_local_barrier.spv",
                                 &legacy_bytes);
    assert(legacy);
    VkBool32 glsl450 = VK_FALSE;
    for (size_t at = 5; at < legacy_bytes / 4;) {
        const uint32_t words = legacy[at] >> 16;
        assert(words && at + words <= legacy_bytes / 4);
        if ((legacy[at] & 0xffffu) == 14u) {
            assert(words == 3 && legacy[at + 2] == 1u);
            glsl450 = VK_TRUE;
        }
        at += words;
    }
    assert(glsl450);
    struct ps5vk_compiled_program legacy_program = {0};
    uint32_t *legacy_code = NULL;
    assert(ps5vk_runtime_compile_compute_features(legacy, legacy_bytes / 4,
        "main", &single_storage_layout, NULL, PS5VK_FEATURE_VULKAN_MEMORY_MODEL,
        &legacy_program, &legacy_code) == VK_SUCCESS);
    assert(legacy_code && legacy_program.local_size[0] == 3 &&
           legacy_program.local_size[1] == 4 && legacy_program.local_size[2] == 1);
    free(legacy_code);
    free(legacy);
    /* 4. Test error handling */
    struct ps5vk_compiled_program bad_prog;
    uint32_t *bad_code = NULL;
    assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
        NULL, UINT32_C(0x80000000), &bad_prog, &bad_code) == VK_ERROR_FEATURE_NOT_PRESENT);
    /* Every graphics-only device feature the console platform reports and the
     * pinned CTS enables on the device it creates must be transparent to the
     * compute adapter: the mask of a real T03 device compiles a compute shader
     * exactly like the bare mask does. A promotion candidate that forgot one
     * of these bits failed every compute pipeline on hardware. */
    {
        struct ps5vk_compiled_program graphics_device_program;
        uint32_t *graphics_device_code = NULL;
        const uint32_t graphics_device_mask = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
            PS5VK_FEATURE_STORAGE_BUFFER_8BIT | PS5VK_FEATURE_STORAGE_BUFFER_16BIT |
            PS5VK_FEATURE_SHADER_DRAW_PARAMETERS | PS5VK_FEATURE_MULTIVIEW |
            PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE | PS5VK_FEATURE_MULTI_DRAW_INDIRECT |
            PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32;
        assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
            NULL, graphics_device_mask, &graphics_device_program,
            &graphics_device_code) == VK_SUCCESS);
        assert(graphics_device_code && graphics_device_program.code_words > 0);
        free(graphics_device_code);
        /* The default-off core shaderInt16 bit must pass the adapter mask.
         * Module and pipeline validation decide whether Int16 is enabled. */
        graphics_device_code = NULL;
        assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
            NULL, graphics_device_mask | PS5VK_FEATURE_SHADER_INT16,
            &graphics_device_program, &graphics_device_code) == VK_SUCCESS);
        assert(graphics_device_code && graphics_device_program.code_words > 0);
        free(graphics_device_code);
        /* Standard UBO layout changes SPIR-V layout validation, not PSBC's
         * compute compilation options. A device that enables it must still
         * compile unrelated compute shaders on the same logical device. */
        graphics_device_code = NULL;
        assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
            NULL, graphics_device_mask | PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT,
            &graphics_device_program, &graphics_device_code) == VK_SUCCESS);
        assert(graphics_device_code && graphics_device_program.code_words > 0);
        free(graphics_device_code);
        /* The four DXVK262-T05 rasterization/viewport bits are the same kind
         * of graphics-only device state. The FULL combined mask a T05 device
         * would carry is tested at once, not one bit at a time: the T04
         * promotion showed that fixing one new bit and repeating the failure
         * for the next is exactly the regression to avoid. */
        const uint32_t t05_device_mask = graphics_device_mask |
            PS5VK_FEATURE_DEPTH_BIAS_CLAMP | PS5VK_FEATURE_DEPTH_CLAMP |
            PS5VK_FEATURE_FILL_MODE_NON_SOLID | PS5VK_FEATURE_MULTI_VIEWPORT;
        graphics_device_code = NULL;
        assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
            NULL, t05_device_mask, &graphics_device_program,
            &graphics_device_code) == VK_SUCCESS);
        assert(graphics_device_code && graphics_device_program.code_words > 0);
        free(graphics_device_code);
        /* T06 adds four more graphics-only bits. They must not poison a
         * compute pipeline on a device which enabled every reported core
         * feature, while an unknown bit after the declared range is refused. */
        const uint32_t t06_device_mask = t05_device_mask |
            PS5VK_FEATURE_INDEPENDENT_BLEND | PS5VK_FEATURE_DUAL_SRC_BLEND |
            PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS |
            PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        graphics_device_code = NULL;
        assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
            NULL, t06_device_mask, &graphics_device_program,
            &graphics_device_code) == VK_SUCCESS);
        assert(graphics_device_code && graphics_device_program.code_words > 0);
        free(graphics_device_code);
        /* A bit above every known feature still fails closed. */
        graphics_device_code = NULL;
        assert(ps5vk_runtime_compile_compute_features(spv1, spv1_words, "main", &layout,
            NULL, t06_device_mask | (1u << 31), &graphics_device_program,
            &graphics_device_code) == VK_ERROR_FEATURE_NOT_PRESENT);
    }

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

    /* A declared set the shader never dereferences is a declaration, not a
     * requirement: no second table pointer, no extra user SGPR, and no extra
     * program descriptor. The previous contract reserved all of them. */
    struct VkPipelineLayout_T sparse_layout = multiset_layout;
    sparse_layout.sets[1].count = 1;
    sparse_layout.sets[1].binding[0].count = 1;
    sparse_layout.sets[1].binding[0].stages = VK_SHADER_STAGE_COMPUTE_BIT;
    sparse_layout.sets[1].type[0] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    res = ps5vk_runtime_compile_compute(spv1, spv1_words, "main", &sparse_layout, NULL, &bad_prog, &bad_code);
    assert(res==VK_SUCCESS && bad_prog.descriptor_set_mask==1 &&
        bad_prog.descriptor_count==2 && bad_prog.user_sgprs==3);
    free(bad_code);bad_code=NULL;
    /* A used set the layout does not declare still fails closed. */
    assert(ps5vk_runtime_compile_compute(spv1, spv1_words, "main", &empty_layout,
        NULL, &bad_prog, &bad_code) != VK_SUCCESS);

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
