#include "ps5vk_compiler.h"
#include "libpsbc/psbc_compile.h"
#include "include/pssl_types.h"
#include "gnm_shaderbinary.h"
#include <stdlib.h>
#include <string.h>

VkResult ps5vk_runtime_compile_compute(
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
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

    PsbcCompileOptions opts = {0};
    opts.target = PSBC_TARGET_PS5;
    opts.stage = PSBC_STAGE_COMPUTE;
    opts.entrypoint = entry_name;
    opts.optimise = true;
    opts.address32_hi = 2;

    /* The initial runtime ABI exposes one scalar storage descriptor table. */
    if (layout->set_count != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Build descriptor layout bindings from pipeline layout set 0 */
    if (layout->set_count > 0) {
        const struct ps5vk_set_signature *sig = &layout->sets[0];
        for (uint32_t b = 0; b < PS5VK_MAX_BINDINGS; b++) {
            if (sig->binding[b].count > 0 && (sig->binding[b].stages & VK_SHADER_STAGE_COMPUTE_BIT)) {
                if (sig->combined_image[b] || sig->binding[b].count != 1) {
                    /* Images and descriptor arrays are outside this compute profile. */
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                }
                uint32_t idx = opts.descriptor_binding_count++;
                opts.descriptor_bindings[idx].set = 0;
                opts.descriptor_bindings[idx].binding = b;
                opts.descriptor_bindings[idx].type = PSBC_DESCRIPTOR_STORAGE_BUFFER;
                opts.descriptor_bindings[idx].array_size = 1;
                opts.descriptor_bindings[idx].offset = b * 16;
                opts.descriptor_bindings[idx].stride = 16;

                struct ps5vk_program_descriptor *desc = &out_program->descriptors[out_program->descriptor_count++];
                desc->set = 0;
                desc->binding = b;
                desc->element = 0;
                desc->table_dword = b * 4;
            }
        }
    }

    if (!out_program->descriptor_count) {
        /* Supported compute profile requires at least one storage buffer descriptor */
        return VK_ERROR_UNKNOWN;
    }

    PsbcShaderOutput out = {0};
    PsbcResult res = psbc_compile_shader(spirv, spirv_words * sizeof(uint32_t), &opts, &out);
    if (res != PSBC_RESULT_OK || !out.machine_code || !out.machine_code_size || (out.machine_code_size % 4) != 0) {
        if (out.machine_code) psbc_free_output(&out);
        return VK_ERROR_UNKNOWN;
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

    /* Scratch and LDS require allocation/programming that the current native
     * dispatch ABI intentionally does not provide. Never silently clear the
     * compiler's requirements when reconstructing COMPUTE_PGM_RSRC2. */
    if ((rsrc2 & 1u) || ((rsrc2 >> 15) & 0x1ffu) || out.metadata.scratch_valid) {
        psbc_free_output(&out);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    uint32_t *code = malloc(out.machine_code_size);
    if (!code) {
        psbc_free_output(&out);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(code, out.machine_code, out.machine_code_size);

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

VkResult ps5vk_compiler_adapter_compile(
    void *context,
    const uint32_t *spirv,
    size_t spirv_words,
    const char *entry_name,
    VkPipelineLayout layout,
    struct ps5vk_compiled_program *out_program,
    uint32_t **out_code)
{
    (void)context;
    return ps5vk_runtime_compile_compute(spirv, spirv_words, entry_name, layout, out_program, out_code);
}
