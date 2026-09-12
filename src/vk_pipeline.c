#include "vk_pipeline.h"
#include "compilation_cache.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN
static int module_valid(const uint32_t *words, size_t count)
{
    if (!words || count < 5 || words[0] != 0x07230203 || !words[3] || words[4]) return 0;
    for (size_t i = 5; i < count;) {
        size_t n = words[i] >> 16;
        if (!n || n > count - i) return 0;
        i += n;
    }
    return 1;
}
VkBool32 ps5vk_shader_entry(VkShaderModule module, VkShaderStageFlagBits stage,
                            const char *name, uint32_t *out)
{
    if (!out) return VK_FALSE;
    *out = 0;
    if (!module || !name || !module_valid(module->words, module->word_count)) return VK_FALSE;
    uint32_t model;
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: model = 0; break;
    case VK_SHADER_STAGE_FRAGMENT_BIT: model = 4; break;
    case VK_SHADER_STAGE_COMPUTE_BIT: model = 5; break;
    default: return VK_FALSE;
    }
    uint32_t id = 0; unsigned found = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        size_t n = w[0] >> 16;
        if ((w[0] & 0xffff) != 15) continue;
        if (n < 4 || !w[2] || w[2] >= module->words[3]) return VK_FALSE;
        const char *entry = (const char *)(w + 3);
        const char *end = memchr(entry, 0, (n - 3) * 4);
        if (!end) return VK_FALSE;
        if (w[1] == model && (size_t)(end - entry) == strlen(name) && !memcmp(entry, name, end - entry)) {
            id = w[2]; ++found;
        }
    }
    if (found != 1) return VK_FALSE;
    *out = id; return VK_TRUE;
}
static int local_size(VkShaderModule module, const char *name, uint32_t dims[3])
{
    uint32_t id;
    if (!ps5vk_shader_entry(module, VK_SHADER_STAGE_COMPUTE_BIT, name, &id)) return 0;
    unsigned found = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        if ((w[0] & 0xffff) == 16 && w[0] >> 16 == 6 && w[1] == id && w[2] == 17) {
            memcpy(dims, w + 3, 3 * sizeof(*dims)); ++found;
        }
    }
    return found == 1;
}

VkResult ps5vk_program_resolve(void *context, const uint32_t *words, size_t count,
                             const char *entry, const struct ps5vk_compiled_program **out)
{
    if (!out) return INVALID;
    *out = NULL;
    const struct ps5vk_program_library *library = context;
    if (!library || !entry || !words || count > SIZE_MAX / 4) return INVALID;
    for (size_t j = 0; j < library->count; ++j) {
        const struct ps5vk_compiled_program *p = &library->programs[j];
        if (p->spirv && p->entry && p->spirv_words == count && !strcmp(p->entry, entry) &&
            !memcmp(words, p->spirv, count * sizeof(*words))) { *out = p; return VK_SUCCESS; }
    }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *info,
    const VkAllocationCallbacks *a, VkShaderModule *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        !info->pCode || info->codeSize % 4) return INVALID;
    if (info->pNext || info->flags) return VK_ERROR_UNKNOWN;
    if (info->codeSize > 16 * 1024 * 1024 || info->codeSize > SIZE_MAX - sizeof(struct VkShaderModule_T))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!module_valid(info->pCode, info->codeSize / 4)) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkShaderModule m = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, a,
        sizeof(*m) + info->codeSize, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->device = d; m->allocator = saved; m->custom_allocator = custom; m->word_count = info->codeSize / 4;
    memcpy(m->words, info->pCode, info->codeSize); ++d->pipeline_objects; *out = m;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !m || m->device != d) return;
    VkAllocationCallbacks saved = m->allocator; VkBool32 custom = m->custom_allocator;
    --d->pipeline_objects; ps5vk_object_free(m, &saved, custom);
}

static int program_valid(const struct ps5vk_compiled_program *p, VkShaderModule m,
                         VkPipelineLayout layout, const char *entry, const uint32_t dims[3])
{
    if (!p || !p->spirv || !p->code || !p->entry || p->gfx != 1013 || p->wave_size != 32 ||
        !p->code_words || p->code_words > 1024 * 1024 || p->spirv_words != m->word_count ||
        strcmp(p->entry, entry) || memcmp(p->spirv, m->words, m->word_count * 4) ||
        memcmp(p->local_size, dims, 3 * sizeof(*dims)) || !p->vgprs || p->vgprs > 256 ||
        !p->sgprs || p->sgprs > 106 || p->float_mode > 255 || p->ieee_mode > 1 ||
        p->mem_ordered > 1 || p->user_sgprs < 2 || p->user_sgprs > 9 || p->lds_size > 128 ||
        p->tg_size > 1 || p->tidig_components > 2 ||
        !p->descriptor_count || p->descriptor_count > PS5VK_MAX_DESCRIPTORS ||
        !p->descriptor_set_mask || (p->descriptor_set_mask & ~((1u << PS5VK_MAX_SETS) - 1))) return 0;
    uint32_t expected_user_sgprs = 2;
    for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set) {
        VkBool32 used = (p->descriptor_set_mask & (1u << set)) != 0;
        if (used) {
            if (set >= layout->set_count || p->descriptor_set_sgpr[set] != expected_user_sgprs++) return 0;
        } else if (p->descriptor_set_sgpr[set]) return 0;
    }
    if (p->grid_size_sgpr) {
        if (p->grid_size_sgpr != expected_user_sgprs) return 0;
        expected_user_sgprs += 3;
    }
    if (p->user_sgprs != expected_user_sgprs) return 0;
    uint64_t invocations = 1;
    for (unsigned j = 0; j < 3; ++j) {
        if (!dims[j] || dims[j] > 1024 || p->tgid[j] > 1) return 0;
        invocations *= dims[j];
    }
    if (invocations > 1024) return 0;
    for (uint32_t j = 0; j < p->descriptor_count; ++j) {
        const struct ps5vk_program_descriptor *b = &p->descriptors[j];
        if (b->set >= layout->set_count || !(p->descriptor_set_mask & (1u << b->set)) ||
            b->binding >= PS5VK_MAX_BINDINGS || b->table_dword >= 128 || b->table_dword % 4) return 0;
        const struct ps5vk_binding *binding = &layout->sets[b->set].binding[b->binding];
        if (layout->sets[b->set].type[b->binding] != b->type ||
            (b->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
             b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) ||
            binding->count <= b->element || !(binding->stages & VK_SHADER_STAGE_COMPUTE_BIT)) return 0;
        for (uint32_t k = 0; k < j; ++k)
            if (p->descriptors[k].set == b->set &&
                (p->descriptors[k].table_dword == b->table_dword ||
                 (p->descriptors[k].binding == b->binding && p->descriptors[k].element == b->element))) return 0;
    }
    return 1;
}

static VkResult create_pipeline(VkDevice d, const VkComputePipelineCreateInfo *info,
                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    if (info->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || !info->layout ||
        info->layout->device != d || info->stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
        !info->stage.module || info->stage.module->device != d || !info->stage.pName) return INVALID;
    if (info->pNext || info->flags || info->stage.pNext || info->stage.flags ||
        info->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT || info->stage.pSpecializationInfo)
        return VK_ERROR_UNKNOWN;
    if (!d->compiler.resolve && (!d->runtime_compiler_enabled || !d->compiler.compile)) return VK_ERROR_UNKNOWN;
    uint32_t dims[3];
    if (!local_size(info->stage.module, info->stage.pName, dims)) return VK_ERROR_UNKNOWN;

    const struct ps5vk_compiled_program *program = NULL;
    struct ps5vk_cache_entry *entry = NULL;
    struct ps5vk_compiled_program compiled_storage = {0};
    uint32_t *compiled_code = NULL;

    if (d->pipeline_cache) {
        struct ps5vk_cache_key key;
        if (ps5vk_cache_build_key(info->stage.module->words, info->stage.module->word_count,
                                  info->stage.pName, info->layout, &key)) {
            entry = ps5vk_compilation_cache_lookup(d->pipeline_cache, &key, info->stage.module->words);
            if (entry) {
                program = &entry->program;
            } else if (d->runtime_compiler_enabled && d->compiler.compile) {
                VkResult cr = d->compiler.compile(d->compiler.context,
                    info->stage.module->words, info->stage.module->word_count,
                    info->stage.pName, info->layout, &compiled_storage, &compiled_code);
                if (cr == VK_SUCCESS) {
                    compiled_storage.code = compiled_code;
                    entry = ps5vk_compilation_cache_insert(d->pipeline_cache, &key,
                        info->stage.module->words, &compiled_storage, compiled_code);
                    if (entry) {
                        free(compiled_code);
                        compiled_code = NULL;
                        program = &entry->program;
                    } else {
                        program = &compiled_storage;
                    }
                } else if (cr != VK_ERROR_FEATURE_NOT_PRESENT) {
                    return cr;
                }
            }
        }
    }

    if (!program && d->compiler.resolve) {
        VkResult result = d->compiler.resolve(d->compiler.context, info->stage.module->words,
            info->stage.module->word_count, info->stage.pName, &program);
        if (result != VK_SUCCESS) {
            if (compiled_code) free(compiled_code);
            return result == VK_ERROR_FEATURE_NOT_PRESENT ? VK_ERROR_UNKNOWN : result;
        }
    }

    if (!program) {
        if (compiled_code) free(compiled_code);
        return VK_ERROR_UNKNOWN;
    }
    if (!program_valid(program, info->stage.module, info->layout, info->stage.pName, dims)) {
        if (compiled_code) free(compiled_code);
        if (entry) ps5vk_cache_entry_release(d->pipeline_cache, entry);
        return INVALID;
    }

    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkPipeline p = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, a,
        sizeof(*p) + program->code_words * 4, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!p) {
        if (compiled_code) free(compiled_code);
        if (entry) ps5vk_cache_entry_release(d->pipeline_cache, entry);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    p->device = d; p->allocator = saved; p->custom_allocator = custom;
    p->set_count = info->layout->set_count;
    memcpy(p->sets, info->layout->sets, sizeof(p->sets));
    p->program = *program;
    p->cache_entry = entry;
    memcpy(p->code, program->code, program->code_words * 4); p->program.code = p->code;
    /* Pipeline owns all execution metadata and code. Module/compiler source
     * pointers are deliberately removed; neither is needed at dispatch. */
    p->program.spirv = NULL; p->program.spirv_words = 0; p->program.entry = NULL;
    if (compiled_code) free(compiled_code);
    ++d->pipeline_objects; *out = p;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d, VkPipelineCache cache,
    uint32_t count, const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a, VkPipeline *out)
{
    if (!out) return INVALID;
    for (uint32_t j = 0; j < count; ++j) out[j] = VK_NULL_HANDLE;
    if (!d || !count || !infos) return INVALID;
    if (cache) return VK_ERROR_UNKNOWN;
    VkResult result = VK_SUCCESS;
    for (uint32_t j = 0; j < count; ++j) {
        VkResult r = create_pipeline(d, &infos[j], a, &out[j]);
        if (r != VK_SUCCESS && result == VK_SUCCESS) result = r;
    }
    /* As Vulkan permits, successful siblings remain caller-owned on failure. */
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !p || p->device != d) return;
    if (p->pending) { ++d->lifetime_errors; return; }
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_PIPELINE, p)) { ++d->lifetime_errors; return; }
    if (p->cache_entry) {
        ps5vk_cache_entry_release(d->pipeline_cache, p->cache_entry);
        p->cache_entry = NULL;
    }
    VkAllocationCallbacks saved = p->allocator; VkBool32 custom = p->custom_allocator;
    if (p->graphics && p->graphics_state) p->graphics_release(d, p->graphics_state);
    --d->pipeline_objects; ps5vk_object_free(p, &saved, custom);
}
