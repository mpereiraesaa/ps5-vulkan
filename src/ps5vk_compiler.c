#include "ps5vk_compiler.h"
#include "compile_stack.h"
#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define COMPUTE_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define COMPUTE_MARK(...) ((void)0)
#endif
#include "descriptor_table_layout.h"
#include "spirv_descriptor_types.h"
#include "libpsbc/psbc_compile.h"
#include "include/pssl_types.h"
#include "gnm_shaderbinary.h"
#include <stdlib.h>
#include <string.h>

/* PSBC interprets Device-scope barriers according to the enabled SPIR-V
 * memory model. A logical device may enable VK_KHR_vulkan_memory_model while
 * still compiling ordinary GLSL450 modules. Passing the KHR compiler option
 * to such a module incorrectly asks SPIRV-to-NIR to apply VulkanKHR's
 * DeviceScope capability rule to its legacy barriers. Read the module's
 * OpMemoryModel (opcode 14) before choosing the compiler options. */
static int module_uses_vulkan_memory_model(const uint32_t *spirv, size_t words)
{
    int model = -1;
    for (size_t at = 5; at < words;) {
        uint32_t length = spirv[at] >> 16;
        uint32_t opcode = spirv[at] & 0xffffu;
        if (!length || length > words - at) return -1;
        if (opcode == 14u) {
            if (length != 3 || model != -1) return -1;
            model = (int)spirv[at + 2];
        }
        at += length;
    }
    /* Vulkan permits GLSL450 (1) and VulkanKHR (3) for this compute path. */
    if (model != 1 && model != 3) return -1;
    return model == 3;
}

static VkResult runtime_compile_compute_features(
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    const VkSpecializationInfo *specialization,
    uint32_t feature_mask,
    struct ps5vk_compiled_program *out_program,
    uint32_t **out_code)
{
    if (!spirv || !spirv_words || !entry_name || !layout || !out_program || !out_code)
        return VK_ERROR_UNKNOWN;
    *out_code = NULL;
    memset(out_program, 0, sizeof(*out_program));

    if (spirv_words < 5 || spirv[0] != 0x07230203)
        return VK_ERROR_UNKNOWN;

    int entry_found = 0;
    size_t offset = 5;
    while (offset < spirv_words) {
        uint32_t word = spirv[offset];
        uint16_t len = word >> 16;
        uint16_t opcode = word & 0xffff;
        if (len == 0 || offset + len > spirv_words) break;
        if (opcode == 15 && len >= 4) {
            uint32_t model = spirv[offset + 1];
            if (model == 5 /* GLCompute */) {
                const char *name = (const char *)&spirv[offset + 3];
                size_t max_bytes = (len - 3) * 4;
                const char *nul = memchr(name, '\0', max_bytes);
                if (nul && strcmp(name, entry_name) == 0) {
                    entry_found = 1;
                    break;
                }
            }
        }
        offset += len;
    }
    if (!entry_found)
        return VK_ERROR_UNKNOWN;
    const int vulkan_memory_model = module_uses_vulkan_memory_model(spirv, spirv_words);
    if (vulkan_memory_model < 0)
        return VK_ERROR_UNKNOWN;

    PsbcCompileOptions opts = {0};
    opts.target = PSBC_TARGET_PS5;
    opts.stage = PSBC_STAGE_COMPUTE;
    opts.entrypoint = entry_name;
    opts.optimise = true;
    opts.address32_hi = 2;
    /* VK_EXT_robustness2 robustBufferAccess2 load handling on every compile:
     * SSBO/UBO loads whose combined offset could wrap stay separate, so each
     * is bounds-checked on its own. It is valid for robustBufferAccess (1.0)
     * too, and the pinned DXVK always enables robustBufferAccess2. */
    opts.robust_buffer_access2 = true;
    opts.force_indirect_push_constants = layout->push_constant_size != 0;
    /* EXPERIMENT: ask the compiler for honest static descriptor use. */
    opts.static_descriptor_use = true;
    if (feature_mask & ~(PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                         PS5VK_FEATURE_STORAGE_BUFFER_16BIT |
                         PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
                         /* Shader draw parameters are a graphics-stage
                         * contract. The compute adapter neither consumes nor
                         * rejects the bit; without it here, any device that
                         * enabled the extension failed every compute
                         * pipeline, which the first hardware run of the
                         * promotion showed as VK_ERROR_UNKNOWN from
                         * vkCreateComputePipelines. */
                         PS5VK_FEATURE_SHADER_DRAW_PARAMETERS |
                         /* Multiview is a graphics-stage capability too: the
                          * view index and the per-view render pass are the draw
                          * path's business, so the compute adapter has no PSBC
                          * option to map this bit onto and must not invent one.
                          * It is listed here for the same reason as the bit
                          * above: a device that declares it - the console
                          * platform does, and the pinned CTS enables it on
                          * every device it creates - would otherwise fail
                          * every compute pipeline on hardware and report it as
                          * VK_ERROR_UNKNOWN from vkCreateComputePipelines.
                          * Every bit this adapter does not know still fails
                          * closed below. */
                         PS5VK_FEATURE_MULTIVIEW |
                         /* The three DXVK262-T03 draw features are likewise
                          * graphics-only: indirect firstInstance, multi-draw
                          * DrawIndex and the 32-bit index range live in the
                          * indirect frontend and the graphics queue, and a
                          * compute shader has no PSBC option for any of them.
                          * The pinned CTS enables every reported core feature
                          * on the device it creates, so leaving them out here
                          * would once more fail every compute pipeline with
                          * VK_ERROR_UNKNOWN - the first candidate run of this
                          * promotion showed exactly that. */
                         PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE |
                         PS5VK_FEATURE_MULTI_DRAW_INDIRECT |
                         PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32 |
                         /* User-defined distances are a pre-raster export and a
                          * pixel input: the compute adapter has no PSBC option
                          * for either bit, and a compute-only device that
                          * enabled them (the pinned CTS enables every reported
                          * core feature on the device it creates) would
                          * otherwise fail every vkCreateComputePipelines call
                          * with VK_ERROR_FEATURE_NOT_PRESENT - the baseline run
                          * of the clip/cull promotion showed exactly that. */
                          PS5VK_FEATURE_SHADER_CLIP_DISTANCE |
                          PS5VK_FEATURE_SHADER_CULL_DISTANCE |
                          /* The optional geometry stage is the same kind of
                           * device-wide bit: the compute adapter has no PSBC
                           * option for it either, and the pinned CTS enables every
                           * reported core feature on the device it creates, so a
                           * compute pipeline created on a device that reports it
                           * must not fail for a bit this path cannot act on. */
                          PS5VK_FEATURE_GEOMETRY_SHADER |
                          /* The tessellation pair is device-wide the same way:
                           * a compute pipeline on a device whose enabled mask
                           * carries the bit is refused by nothing this adapter
                           * can act on. Measured by the tessellation slice with
                           * the guard the geometry promotion added. */
                          PS5VK_FEATURE_TESSELLATION_SHADER |
                         /* The four DXVK262-T05 rasterization/viewport
                          * features (depthBiasClamp, depthClamp,
                          * fillModeNonSolid, multiViewport) are fixed-function
                          * draw state programmed by native/draw_state_ps5.c;
                          * a compute shader has no PSBC option for any of
                          * them, and they are listed for the same reason as
                          * every graphics bit above. */
                          PS5VK_FEATURE_DEPTH_BIAS_CLAMP |
                          PS5VK_FEATURE_DEPTH_CLAMP |
                          PS5VK_FEATURE_FILL_MODE_NON_SOLID |
                          PS5VK_FEATURE_MULTI_VIEWPORT |
                          /* Graphics-only device features are accepted on a
                           * compute pipeline without changing the PSBC
                           * compile options. The adapter still rejects any
                           * unknown feature bit outside this allow-list. */
                          PS5VK_FEATURE_INDEPENDENT_BLEND |
                          PS5VK_FEATURE_DUAL_SRC_BLEND |
                          PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS |
                          PS5VK_FEATURE_SAMPLE_RATE_SHADING |
                          PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS |
                          PS5VK_FEATURE_VULKAN_MEMORY_MODEL |
                          PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE |
                           PS5VK_FEATURE_IMAGE_CUBE_ARRAY |
                          PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED |
                          PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE |
                          PS5VK_FEATURE_TEXTURE_COMPRESSION_BC |
                          /* The UBO layout gate is checked at module creation;
                           * Int16 is forwarded to the PSBC compute adapter. */
                          PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT |
                           PS5VK_FEATURE_SHADER_INT16 |
                           PS5VK_FEATURE_SHADER_INT8_COMPUTE))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((feature_mask & PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE) &&
        !(feature_mask & PS5VK_FEATURE_VULKAN_MEMORY_MODEL))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    opts.enable_storage_buffer_8bit_access =
        !!(feature_mask & PS5VK_FEATURE_STORAGE_BUFFER_8BIT);
    opts.enable_int8 = !!(feature_mask & PS5VK_FEATURE_SHADER_INT8_COMPUTE);
    opts.enable_storage_buffer_16bit_access =
        !!(feature_mask & PS5VK_FEATURE_STORAGE_BUFFER_16BIT);
    opts.enable_int16 = !!(feature_mask & PS5VK_FEATURE_SHADER_INT16);
    opts.enable_physical_storage_buffer_addresses =
        !!(feature_mask & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS);
    opts.enable_vulkan_memory_model =
        vulkan_memory_model && !!(feature_mask & PS5VK_FEATURE_VULKAN_MEMORY_MODEL);
    opts.enable_vulkan_memory_model_device_scope =
        vulkan_memory_model && !!(feature_mask & PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE);

    if (specialization) {
        if (specialization->mapEntryCount > PSBC_MAX_SPECIALIZATION_CONSTANTS ||
            (specialization->mapEntryCount && !specialization->pMapEntries) ||
            (specialization->dataSize && !specialization->pData))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        for (uint32_t i = 0; i < specialization->mapEntryCount; ++i) {
            const VkSpecializationMapEntry *source = &specialization->pMapEntries[i];
            if (!source->size || source->size > PSBC_MAX_SPECIALIZATION_BYTES ||
                source->offset > specialization->dataSize ||
                source->size > specialization->dataSize - source->offset)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            PsbcSpecializationConstant *target =
                &opts.specialization_constants[opts.specialization_constant_count++];
            target->constant_id = source->constantID;
            target->size = (uint32_t)source->size;
            memcpy(target->data, (const uint8_t *)specialization->pData + source->offset,
                   source->size);
        }
    }

    if (layout->set_count > PS5VK_MAX_SETS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Opaque resources must be the type their binding holds, or the compiled
     * code would read another record's width at that offset. */
    if (!ps5vk_spirv_descriptor_types_match(spirv, spirv_words, layout->set_count, layout->sets))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Pipeline layouts built by Vulkan already have canonical prefixes. Build
     * them here as well for callers of this adapter that supply only counts. */
    struct ps5vk_set_signature canonical[PS5VK_MAX_SETS];
    memset(canonical, 0, sizeof(canonical));
    for (uint32_t set = 0; set < layout->set_count; ++set) {
        canonical[set] = layout->sets[set];
        uint32_t prefix = 0;
        for (uint32_t binding = 0; binding < PS5VK_MAX_BINDINGS; ++binding) {
            canonical[set].binding[binding].first = prefix;
            if (!canonical[set].binding[binding].count) {
                canonical[set].binding[binding].stages = 0;
                canonical[set].type[binding] = 0;
            }
            prefix += canonical[set].binding[binding].count;
        }
        canonical[set].count = prefix;
    }
    struct ps5vk_descriptor_table_layout table_layout;
    VkResult table_result = ps5vk_descriptor_table_layout_build(
        layout->set_count, canonical, &table_layout);
    if (table_result != VK_SUCCESS) return table_result;
    /* Every Vulkan set remains a distinct RADV table. Offsets are local to a
     * set, while the compiler metadata identifies its direct user-SGPR slot.
     * The layout only declares the canonical offsets here; which of these
     * bindings become a requirement is decided from the compiled metadata. */
    uint32_t declared_descriptor_count = 0;
    for (uint32_t set = 0; set < layout->set_count; ++set) {
        const struct ps5vk_set_signature *sig = &layout->sets[set];
        for (uint32_t b = 0; b < PS5VK_MAX_BINDINGS; b++) {
            if (sig->binding[b].count > 0 && (sig->binding[b].stages & VK_SHADER_STAGE_COMPUTE_BIT)) {
                PsbcDescriptorType type;
                switch (sig->type[b]) {
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: type=PSBC_DESCRIPTOR_STORAGE_BUFFER;break;
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: type=PSBC_DESCRIPTOR_STORAGE_BUFFER;break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: type=PSBC_DESCRIPTOR_UNIFORM_BUFFER;break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: type=PSBC_DESCRIPTOR_UNIFORM_BUFFER;break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: type=PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER;break;
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: type=PSBC_DESCRIPTOR_STORAGE_IMAGE;break;
                /* DXVK's DXBC forms: a separate S# and T# the shader combines
                 * with OpSampledImage, and a typed UAV buffer. */
                case VK_DESCRIPTOR_TYPE_SAMPLER: type=PSBC_DESCRIPTOR_SAMPLER;break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: type=PSBC_DESCRIPTOR_SAMPLED_IMAGE;break;
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: type=PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER;break;
                default:
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                }
                if (opts.descriptor_binding_count == PSBC_MAX_DESCRIPTOR_BINDINGS ||
                    sig->binding[b].count > PS5VK_MAX_DESCRIPTORS-declared_descriptor_count)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                uint32_t idx = opts.descriptor_binding_count++;
                opts.descriptor_bindings[idx].set = set;
                opts.descriptor_bindings[idx].binding = b;
                opts.descriptor_bindings[idx].type = type;
                opts.descriptor_bindings[idx].array_size = sig->binding[b].count;
                opts.descriptor_bindings[idx].offset = table_layout.binding[set][b].byte_offset;
                opts.descriptor_bindings[idx].stride = table_layout.binding[set][b].byte_stride;
                declared_descriptor_count += sig->binding[b].count;
            }
        }
    }

    PsbcShaderOutput out = {0};
    COMPUTE_MARK("PS5VK_COMPUTE_COMPILE phase=psbc bindings=%u", opts.descriptor_binding_count);
    PsbcResult res = psbc_compile_shader(spirv, spirv_words * sizeof(uint32_t), &opts, &out);
    COMPUTE_MARK("PS5VK_COMPUTE_COMPILE phase=psbc_done rc=%d bytes=%zu", (int)res, out.machine_code_size);
    if (res != PSBC_RESULT_OK || !out.machine_code || !out.machine_code_size || (out.machine_code_size % 4) != 0) {
        if (out.machine_code) psbc_free_output(&out);
        return res == PSBC_RESULT_UNSUPPORTED_CAPABILITY ?
            VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_UNKNOWN;
    }

    /* Inspect PSSL/GNM header to extract hardware compute registers */
    size_t header_min_size = sizeof(PsslBinaryHeader) + sizeof(GnmShaderFileHeader) + sizeof(GnmCsShader);
    if (out.size < header_min_size) {
        psbc_free_output(&out);
        return VK_ERROR_UNKNOWN;
    }

    const uint8_t *header_ptr = (const uint8_t *)out.data;
    const GnmCsShader *csh = (const GnmCsShader *)(header_ptr + sizeof(PsslBinaryHeader) + sizeof(GnmShaderFileHeader));

    uint32_t rsrc1 = csh->registers.computepgmrsrc1;
    uint32_t rsrc2 = csh->registers.computepgmrsrc2;

    /* LDS is allocated by hardware from LDS_SIZE; scratch still requires a
     * backing allocation and remains unsupported. Preserve compiler sizing. */
    uint32_t lds_size = (rsrc2 >> 15) & 0x1ffu;
    uint32_t user_sgprs = (rsrc2 >> 1) & 0x1fu;
    uint32_t expected_set_mask=0;
    for(uint32_t i=0;i<opts.descriptor_binding_count;++i)
        expected_set_mask|=1u<<opts.descriptor_bindings[i].set;
    /* A layout is a declaration, not evidence of use: only the sets and
     * bindings the optimized NIR dereferences become requirements. */
    uint32_t used_set_mask=0;
    for(uint32_t set=0;set<PS5VK_MAX_SETS;++set)
        if(out.metadata.descriptor_set_valid[set])used_set_mask|=1u<<set;
    if(used_set_mask&~expected_set_mask) {
        psbc_free_output(&out);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    uint32_t direct_set_count=0;
    for(uint32_t set=0;set<PS5VK_MAX_SETS;++set)
        direct_set_count+=(used_set_mask>>set)&1u;
    uint32_t base_user_sgprs=2+direct_set_count;
    VkBool32 expects_push = layout->push_constant_size != 0;
    if (expects_push) ++base_user_sgprs;
    /* Pinned RADV compute arguments: ring offsets, one direct 32-bit pointer
     * per used set, then optional inline grid dimensions. */
    if ((rsrc2 & 1u) || out.metadata.scratch_valid || lds_size > 128 ||
        (user_sgprs != base_user_sgprs && user_sgprs != base_user_sgprs+3)) {
        psbc_free_output(&out);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    for(uint32_t set=0;set<PS5VK_MAX_SETS;++set) {
        VkBool32 expected=(used_set_mask&(1u<<set))!=0;
        if(out.metadata.descriptor_set_valid[set]!=expected ||
           (expected && out.metadata.descriptor_set_user_data_dword[set]>=user_sgprs)) {
            psbc_free_output(&out);return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (out.metadata.push_constants_valid != expects_push ||
        (expects_push && (out.metadata.push_constants_user_data_dword >= user_sgprs ||
         !out.metadata.push_constant_size ||
         out.metadata.push_constant_size > layout->push_constant_size))) {
        psbc_free_output(&out); return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    uint32_t *code = malloc(out.machine_code_size);
    if (!code) {
        psbc_free_output(&out);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(code, out.machine_code, out.machine_code_size);

    /* Keep only the descriptors the compiled stage really dereferences. A used
     * set whose binding the compiler could not name keeps its whole
     * declaration, so an unknown entry can never be treated as unused. */
    out_program->descriptor_count = 0;
    for (uint32_t set = 0; set < layout->set_count; ++set) {
        if (!(used_set_mask & (1u << set))) continue;
        const uint64_t named = out.metadata.descriptor_used_binding_mask[set];
        const struct ps5vk_set_signature *sig = &layout->sets[set];
        for (uint32_t b = 0; b < PS5VK_MAX_BINDINGS; ++b) {
            const struct ps5vk_binding *binding = &sig->binding[b];
            if (!binding->count || !(binding->stages & VK_SHADER_STAGE_COMPUTE_BIT) ||
                (named && !(named & (UINT64_C(1) << b))))
                continue;
            for (uint32_t element = 0; element < binding->count; ++element) {
                struct ps5vk_program_descriptor *desc =
                    &out_program->descriptors[out_program->descriptor_count++];
                desc->set = set;
                desc->binding = b;
                desc->element = element;
                desc->table_dword = (table_layout.binding[set][b].byte_offset +
                    element * table_layout.binding[set][b].byte_stride) / 4;
                desc->type = sig->type[b];
            }
        }
    }

    out_program->spirv = spirv;
    out_program->spirv_words = spirv_words;
    out_program->code = code;
    out_program->code_words = out.machine_code_size / 4;
    out_program->entry = entry_name;
    out_program->gfx = 1013;
    out_program->wave_size = 32;

    uint32_t vgpr_granule = rsrc1 & 0x3fu;
    out_program->vgprs = (vgpr_granule + 1u) * 8u;
    out_program->sgprs = 32u; /* Conservative allocation bound within [1, 106] */
    out_program->float_mode = (rsrc1 >> 12) & 0xffu;
    out_program->ieee_mode = (rsrc1 >> 23) & 1u;
    out_program->wgp_mode = (rsrc1 >> 29) & 1u;
    out_program->mem_ordered = (rsrc1 >> 30) & 1u;

    out_program->user_sgprs = (rsrc2 >> 1) & 0x1fu;
    out_program->grid_size_sgpr = user_sgprs == base_user_sgprs+3 ? base_user_sgprs : 0;
    out_program->push_constant_size = out.metadata.push_constant_size;
    out_program->push_constant_sgpr = out.metadata.push_constants_user_data_dword;
    out_program->descriptor_set_mask=used_set_mask;
    for(uint32_t set=0;set<PS5VK_MAX_SETS;++set)
        out_program->descriptor_set_sgpr[set]=out.metadata.descriptor_set_user_data_dword[set];
    out_program->lds_size = lds_size;
    out_program->tgid[0] = (rsrc2 >> 7) & 1u;
    out_program->tgid[1] = (rsrc2 >> 8) & 1u;
    out_program->tgid[2] = (rsrc2 >> 9) & 1u;
    out_program->tg_size = (rsrc2 >> 10) & 1u;
    out_program->tidig_components = (rsrc2 >> 11) & 3u;

    out_program->local_size[0] = csh->registers.computenumthreadx;
    out_program->local_size[1] = csh->registers.computenumthready;
    out_program->local_size[2] = csh->registers.computenumthreadz;

    psbc_free_output(&out);
    *out_code = code;
    return VK_SUCCESS;
}

struct compute_compile_call {
    const uint32_t *spirv; size_t spirv_words; const char *entry_name;
    VkPipelineLayout layout; const VkSpecializationInfo *specialization;
    uint32_t feature_mask; struct ps5vk_compiled_program *out_program;
    uint32_t **out_code; VkResult result;
};

static void compute_compile_call(void *opaque)
{
    struct compute_compile_call *c = opaque;
    c->result = runtime_compile_compute_features(c->spirv, c->spirv_words, c->entry_name,
        c->layout, c->specialization, c->feature_mask, c->out_program, c->out_code);
}

/* The compile runs on a driver-sized stack (compile_stack.h): the calling
 * thread may be an application worker with a small default stack. */
VkResult ps5vk_runtime_compile_compute_features(
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    const VkSpecializationInfo *specialization,
    uint32_t feature_mask,
    struct ps5vk_compiled_program *out_program,
    uint32_t **out_code)
{
    struct compute_compile_call call = {spirv, spirv_words, entry_name, layout,
        specialization, feature_mask, out_program, out_code, VK_ERROR_UNKNOWN};
    COMPUTE_MARK("PS5VK_COMPUTE_COMPILE phase=spawn words=%zu", spirv_words);
    if (ps5vk_compile_on_sized_stack(compute_compile_call, &call))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    COMPUTE_MARK("PS5VK_COMPUTE_COMPILE phase=joined rc=%d descriptors=%u",
                 (int)call.result, out_program->descriptor_count);
    return call.result;
}

VkResult ps5vk_runtime_compile_compute(
    const uint32_t *spirv, size_t spirv_words, const char *entry_name,
    VkPipelineLayout layout, const VkSpecializationInfo *specialization,
    struct ps5vk_compiled_program *out_program, uint32_t **out_code)
{
    return ps5vk_runtime_compile_compute_features(spirv, spirv_words, entry_name,
        layout, specialization, 0, out_program, out_code);
}

VkResult ps5vk_compiler_adapter_compile(
    void *context,
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    const VkSpecializationInfo *specialization,
    uint32_t feature_mask,
    struct ps5vk_compiled_program *out_program,
    uint32_t **out_code)
{
    (void)context;
    return ps5vk_runtime_compile_compute_features(spirv, spirv_words, entry_name,
        layout, specialization, feature_mask, out_program, out_code);
}
