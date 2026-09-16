#include "vk_descriptor.h"
#include "vk_image.h"
#include "vk_sampler.h"
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN
static void *alloc(VkDevice d, const VkAllocationCallbacks *a, size_t size,
                    VkAllocationCallbacks *saved, VkBool32 *custom)
{
    return ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, a, size,
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, saved, custom);
}

static VkBool32 valid_descriptor_stages(VkShaderStageFlags stages)
{
    const VkShaderStageFlags core = VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT;
    /* VK_SHADER_STAGE_ALL is a reserved convenience value rather than the OR
     * of today's bits.  Layout visibility does not create a shader stage and
     * may name stages that no pipeline ultimately consumes. */
    return stages == VK_SHADER_STAGE_ALL || (stages && !(stages & ~core));
}

static int indices(VkDescriptorSet set, uint32_t binding, uint32_t element,
                   uint32_t count, uint32_t out[PS5VK_MAX_DESCRIPTORS])
{
    if (!set || !count || count > PS5VK_MAX_DESCRIPTORS || binding >= PS5VK_MAX_BINDINGS ||
        element >= set->signature.binding[binding].count) return 0;
    VkShaderStageFlags stages = set->signature.binding[binding].stages;
    VkDescriptorType type = set->signature.type[binding];
    for (uint32_t j = 0; j < count; ++j) {
        while (binding < PS5VK_MAX_BINDINGS && element == set->signature.binding[binding].count) {
            ++binding; element = 0;
        }
        if (binding == PS5VK_MAX_BINDINGS || set->signature.binding[binding].stages != stages ||
            set->signature.type[binding] != type)
            return 0;
        out[j] = set->signature.binding[binding].first + element++;
    }
    return 1;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d, uint32_t write_count,
    const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *copies)
{
    if (!d) return;
    if ((write_count && !writes) || (copy_count && !copies)) { ++d->lifetime_errors; return; }
    for (uint32_t j = 0; j < write_count; ++j) {
        const VkWriteDescriptorSet *w = &writes[j];
        VkBool32 input=w->descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        VkBool32 image=w->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || input;
        VkDescriptorType base_type=ps5vk_base_buffer_descriptor_type(w->descriptorType);
        VkBool32 buffer=base_type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            base_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkBool32 texel=w->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        uint32_t dst[PS5VK_MAX_DESCRIPTORS];
        if (w->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || w->pNext || !w->dstSet ||
            w->dstSet->pool->device != d || w->dstSet->pending ||
            (image ? !w->pImageInfo : (buffer ? !w->pBufferInfo : (texel ? !w->pTexelBufferView : 1))) ||
            w->dstBinding>=PS5VK_MAX_BINDINGS ||
            w->dstSet->signature.type[w->dstBinding] != w->descriptorType ||
            !indices(w->dstSet, w->dstBinding, w->dstArrayElement, w->descriptorCount, dst)) {
            ++d->lifetime_errors; return;
        }
        /* Prevalidate each whole write. Earlier writes follow Vulkan's ordering;
         * invalid application input does not retroactively roll them back. */
        for (uint32_t k = 0; k < w->descriptorCount; ++k) {
            if(image) {
                const VkDescriptorImageInfo *v=&w->pImageInfo[k];
                void *address;VkDeviceSize bytes;
                if(input) {
                   /* Image-view-only: a 2D view of a live image on this device,
                    * whose image was created for input attachment use
                    * (VUID-VkWriteDescriptorSet-descriptorType-00338), read
                    * through a layout a subpass may use. VkDescriptorImageInfo's
                    * sampler member is IGNORED for this descriptor type, so it
                    * is deliberately neither read nor rejected. The bound
                    * allocation stays a consumption-time obligation this slice
                    * does not claim. */
                   if(!v->imageView || v->imageView->device!=d ||
                      !v->imageView->image || v->imageView->image->device!=d ||
                      !(v->imageView->image->info.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ||
                      (v->imageView->view_type!=VK_IMAGE_VIEW_TYPE_2D &&
                       v->imageView->view_type!=VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
                      (v->imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                       v->imageLayout!=VK_IMAGE_LAYOUT_GENERAL)) {
                        ++d->lifetime_errors;return;
                    }
                    continue;
                }
                if(!v->sampler || v->sampler->device!=d || !v->imageView || v->imageView->device!=d ||
                    !v->imageView->image || !(v->imageView->image->info.usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
                    (v->imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && v->imageLayout!=VK_IMAGE_LAYOUT_GENERAL) ||
                    ps5vk_image_span(d,v->imageView->image,&address,&bytes)!=VK_SUCCESS) {
                    ++d->lifetime_errors;return;
                }
                continue;
            }
            if(texel) {
                VkBufferView view=w->pTexelBufferView[k];
                if(!view || view->device!=d || !view->buffer) {++d->lifetime_errors;return;}
                continue;
            }
            const VkDescriptorBufferInfo *b = &w->pBufferInfo[k];
            void *address; VkDeviceSize size;
            const VkBufferUsageFlags usage = base_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ?
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            const VkDeviceSize alignment = base_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ?
                d->uniform_buffer_alignment : d->buffer_alignment;
            if (!ps5vk_buffer_usage(d,b->buffer,usage) || !alignment || b->offset % alignment ||
                ps5vk_buffer_span(d, b->buffer, b->offset, b->range, &address, &size) != VK_SUCCESS) {
                ++d->lifetime_errors; return;
            }
        }
        if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, w->dstSet)) { ++d->lifetime_errors; return; }
        for (uint32_t k = 0; k < w->descriptorCount; ++k) {
            if(image) {
                VkDescriptorImageInfo stored = w->pImageInfo[k];
                /* The sampler member is ignored for an input attachment, so it
                 * is canonicalized rather than copied: a future consumer must
                 * not be able to observe application data this descriptor type
                 * never uses, and a copied descriptor carries the canonical
                 * value too. */
                if(input) stored.sampler = VK_NULL_HANDLE;
                w->dstSet->images[dst[k]]=stored;
                w->dstSet->image_resources[dst[k]]=stored.imageView->image;
            } else if(texel) w->dstSet->texel_views[dst[k]]=w->pTexelBufferView[k];
            else w->dstSet->buffers[dst[k]] = w->pBufferInfo[k];
            w->dstSet->defined[dst[k]] = VK_TRUE;
        }
        ++w->dstSet->generation;
    }
    for (uint32_t j = 0; j < copy_count; ++j) {
        const VkCopyDescriptorSet *c = &copies[j];
        uint32_t src[PS5VK_MAX_DESCRIPTORS], dst[PS5VK_MAX_DESCRIPTORS];
        if (c->sType != VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET || c->pNext ||
            !c->srcSet || !c->dstSet || c->srcSet->pool->device != d ||
            c->dstSet->pool->device != d || c->dstSet->pending ||
            c->srcBinding>=PS5VK_MAX_BINDINGS || c->dstBinding>=PS5VK_MAX_BINDINGS ||
            c->srcSet->signature.type[c->srcBinding]!=c->dstSet->signature.type[c->dstBinding] ||
            !indices(c->srcSet, c->srcBinding, c->srcArrayElement, c->descriptorCount, src) ||
            !indices(c->dstSet, c->dstBinding, c->dstArrayElement, c->descriptorCount, dst)) {
            ++d->lifetime_errors; return;
        }
        /* Copy descriptor references, not pointed-to data. Undefined descriptors
         * may legally be copied; resolving GPU addresses happens at consumption. */
        VkDescriptorBufferInfo values[PS5VK_MAX_DESCRIPTORS];
        VkDescriptorImageInfo image_values[PS5VK_MAX_DESCRIPTORS];
        VkBufferView texel_values[PS5VK_MAX_DESCRIPTORS];
        VkImage resources[PS5VK_MAX_DESCRIPTORS];
        if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, c->dstSet)) { ++d->lifetime_errors; return; }
        VkBool32 defined[PS5VK_MAX_DESCRIPTORS];
        for (uint32_t k = 0; k < c->descriptorCount; ++k) {
            values[k] = c->srcSet->buffers[src[k]]; defined[k] = c->srcSet->defined[src[k]];
            image_values[k]=c->srcSet->images[src[k]];resources[k]=c->srcSet->image_resources[src[k]];
            texel_values[k]=c->srcSet->texel_views[src[k]];
        }
        for (uint32_t k = 0; k < c->descriptorCount; ++k) {
            c->dstSet->buffers[dst[k]] = values[k]; c->dstSet->defined[dst[k]] = defined[k];
            c->dstSet->images[dst[k]]=image_values[k];c->dstSet->image_resources[dst[k]]=resources[k];
            c->dstSet->texel_views[dst[k]]=texel_values[k];
        }
        ++c->dstSet->generation;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d,
    const VkDescriptorSetLayoutCreateInfo *info, const VkAllocationCallbacks *a,
    VkDescriptorSetLayout *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO ||
        (info->bindingCount && !info->pBindings)) return INVALID;
    if (info->pNext || info->flags || info->bindingCount > PS5VK_MAX_BINDINGS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_set_signature signature = {0};
    VkBool32 seen[PS5VK_MAX_BINDINGS] = {0};
    for (uint32_t j = 0; j < info->bindingCount; ++j) {
        const VkDescriptorSetLayoutBinding *b = &info->pBindings[j];
        if (b->binding >= PS5VK_MAX_BINDINGS) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (seen[b->binding]) return INVALID;
        seen[b->binding] = VK_TRUE;
        VkBool32 input=b->descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        VkBool32 image=b->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || input;
        VkDescriptorType base_type=ps5vk_base_buffer_descriptor_type(b->descriptorType);
        VkBool32 buffer=base_type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            base_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkBool32 texel=b->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        /* VUID-VkDescriptorSetLayoutBinding-descriptorType-01510: an input
         * attachment is read by a fragment shader, so its visibility is EITHER
         * nothing or exactly the fragment stage. The empty mask is therefore
         * legal for this one descriptor type and is accepted here; every other
         * role keeps the profile's rule that a used binding needs a real
         * mask. */
        if (b->descriptorCount && !(input && !b->stageFlags) &&
            !valid_descriptor_stages(b->stageFlags))
            return INVALID;
        if (b->descriptorCount && input && b->stageFlags &&
            b->stageFlags != VK_SHADER_STAGE_FRAGMENT_BIT)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (b->descriptorCount && base_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
            !d->uniform_buffer_alignment)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        /* Layout visibility is not shader-stage execution. The common mask
         * validator above applies to images too; backend support is checked
         * when a pipeline consumes the signature. */
        /* pImmutableSamplers is meaningful only for SAMPLER and
         * COMBINED_IMAGE_SAMPLER; for an input attachment it is IGNORED, so it
         * is neither read nor rejected there. */
        if (b->descriptorCount && (image ? (!d->graphics_enabled ||
                (b->pImmutableSamplers && !input)) :
                (!(buffer||texel) || b->pImmutableSamplers)))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (b->descriptorCount > PS5VK_MAX_DESCRIPTORS - signature.count)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        signature.binding[b->binding].count = b->descriptorCount;
        signature.type[b->binding]=b->descriptorCount?b->descriptorType:0;
        signature.binding[b->binding].stages = b->descriptorCount ? b->stageFlags : 0;
        signature.count += b->descriptorCount;
    }
    uint32_t offset = 0;
    for (unsigned j = 0; j < PS5VK_MAX_BINDINGS; ++j) {
        signature.binding[j].first = offset;
        offset += signature.binding[j].count;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkDescriptorSetLayout layout = alloc(d, a, sizeof(*layout), &saved, &custom);
    if (!layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    layout->device = d; layout->allocator = saved; layout->custom_allocator = custom;
    layout->signature = signature; ++d->descriptor_objects; *out = layout;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout layout,
    const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !layout || layout->device != d) return;
    /* Sets and pipeline layouts store a value copy, not this object's address. */
    VkAllocationCallbacks saved = layout->allocator; VkBool32 custom = layout->custom_allocator;
    --d->descriptor_objects; ps5vk_object_free(layout, &saved, custom);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d,
    const VkDescriptorPoolCreateInfo *info, const VkAllocationCallbacks *a, VkDescriptorPool *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO ||
        !info->maxSets || (info->poolSizeCount && !info->pPoolSizes)) return INVALID;
    if (info->pNext || (info->flags & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t storage_capacity=0,uniform_capacity=0,dynamic_storage_capacity=0,
        dynamic_uniform_capacity=0,texel_capacity=0,image_capacity=0,input_capacity=0;
    for (uint32_t j = 0; j < info->poolSizeCount; ++j) {
        const VkDescriptorPoolSize *size=&info->pPoolSizes[j];
        uint64_t *capacity=size->type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT?&input_capacity:
            size->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER?&storage_capacity:
            size->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?&uniform_capacity:
            size->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC?&dynamic_storage_capacity:
            size->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC?&dynamic_uniform_capacity:
            size->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER?&texel_capacity:
            size->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?&image_capacity:NULL;
        if (!capacity ||
            ((size->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
              size->type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) && !d->graphics_enabled))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (!size->descriptorCount) return INVALID;
        if (*capacity > UINT64_MAX-size->descriptorCount)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        *capacity += size->descriptorCount;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkDescriptorPool pool = alloc(d, a, sizeof(*pool), &saved, &custom);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->device = d; pool->allocator = saved; pool->custom_allocator = custom;
    pool->flags = info->flags; pool->max_sets = info->maxSets;
    pool->storage_capacity=storage_capacity;pool->uniform_capacity=uniform_capacity;
    pool->dynamic_storage_capacity=dynamic_storage_capacity;
    pool->dynamic_uniform_capacity=dynamic_uniform_capacity;
    pool->texel_capacity=texel_capacity;pool->image_capacity=image_capacity;
    pool->input_capacity=input_capacity;
    ++d->descriptor_objects; *out = pool;
    return VK_SUCCESS;
}
static void free_set(VkDescriptorPool pool, VkDescriptorSet set)
{
    VkDescriptorSet *p = &pool->sets;
    while (*p && *p != set) p = &(*p)->next;
    if (!*p) return;
    *p = set->next; --pool->used_sets;
    for(unsigned j=0;j<PS5VK_MAX_BINDINGS;++j) {
        uint32_t count=set->signature.binding[j].count;
        switch(set->signature.type[j]) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: pool->storage_used-=count;break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: pool->uniform_used-=count;break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: pool->dynamic_storage_used-=count;break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: pool->dynamic_uniform_used-=count;break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: pool->texel_used-=count;break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: pool->image_used-=count;break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: pool->input_used-=count;break;
        default: break;
        }
    }
    ps5vk_object_free(set, &pool->allocator, pool->custom_allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice d, VkDescriptorPool pool,
                                                    VkDescriptorPoolResetFlags flags)
{
    if (!d || !pool || pool->device != d || flags) return INVALID;
    for (VkDescriptorSet s = pool->sets; s; s = s->next)
        if (s->pending) return INVALID;
    for (VkDescriptorSet s = pool->sets; s; s = s->next)
        if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, s)) return INVALID;
    while (pool->sets) free_set(pool, pool->sets);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice d, VkDescriptorPool pool,
                                                  const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !pool || pool->device != d) return;
    if (vkResetDescriptorPool(d, pool, 0) != VK_SUCCESS) { ++d->lifetime_errors; return; }
    VkAllocationCallbacks saved = pool->allocator; VkBool32 custom = pool->custom_allocator;
    --d->descriptor_objects; ps5vk_object_free(pool, &saved, custom);
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,
    const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *out)
{
    if (!out || !info) return INVALID;
    for (uint32_t j = 0; j < info->descriptorSetCount; ++j) out[j] = VK_NULL_HANDLE;
    if (!d || info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO ||
        !info->descriptorSetCount || !info->pSetLayouts || !info->descriptorPool ||
        info->descriptorPool->device != d) return INVALID;
    if (info->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkDescriptorPool pool = info->descriptorPool;
    if (info->descriptorSetCount > pool->max_sets - pool->used_sets)
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    uint64_t storage_needed=0,uniform_needed=0,dynamic_storage_needed=0,
        dynamic_uniform_needed=0,texel_needed=0,image_needed=0,input_needed=0;
    for (uint32_t j = 0; j < info->descriptorSetCount; ++j) {
        VkDescriptorSetLayout layout = info->pSetLayouts[j];
        if (!layout || layout->device != d) return INVALID;
        for(unsigned k=0;k<PS5VK_MAX_BINDINGS;++k) {
            uint32_t count=layout->signature.binding[k].count;
            switch(layout->signature.type[k]) {
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: storage_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: uniform_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: dynamic_storage_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: dynamic_uniform_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: texel_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: image_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: input_needed+=count;break;
            default: if(count)return INVALID;
            }
        }
    }
    if (storage_needed>pool->storage_capacity-pool->storage_used ||
        uniform_needed>pool->uniform_capacity-pool->uniform_used ||
        dynamic_storage_needed>pool->dynamic_storage_capacity-pool->dynamic_storage_used ||
        dynamic_uniform_needed>pool->dynamic_uniform_capacity-pool->dynamic_uniform_used ||
        texel_needed>pool->texel_capacity-pool->texel_used ||
        image_needed>pool->image_capacity-pool->image_used ||
        input_needed>pool->input_capacity-pool->input_used)
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    VkDescriptorSet pending = NULL;
    for (uint32_t j = 0; j < info->descriptorSetCount; ++j) {
        VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
        VkDescriptorSet set = ps5vk_object_alloc(NULL,
            pool->custom_allocator ? &pool->allocator : NULL, sizeof(*set),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
        if (!set) {
            while (pending) {
                VkDescriptorSet next = pending->next;
                ps5vk_object_free(pending, &pool->allocator, pool->custom_allocator);
                pending = next;
            }
            for (uint32_t k = 0; k < info->descriptorSetCount; ++k) out[k] = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        set->pool = pool; set->signature = info->pSetLayouts[j]->signature;
        set->generation = 1; set->next = pending; pending = set; out[j] = set;
    }
    while (pending) {
        VkDescriptorSet next = pending->next;
        pending->next = pool->sets; pool->sets = pending; pending = next;
    }
    pool->storage_used+=storage_needed;pool->uniform_used+=uniform_needed;
    pool->dynamic_storage_used+=dynamic_storage_needed;
    pool->dynamic_uniform_used+=dynamic_uniform_needed;
    pool->texel_used+=texel_needed;pool->image_used+=image_needed;
    pool->input_used+=input_needed;
    pool->used_sets += info->descriptorSetCount;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice d, VkDescriptorPool pool,
    uint32_t count, const VkDescriptorSet *sets)
{
    if (!d || !pool || pool->device != d || (count && !sets) ||
        !(pool->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)) return INVALID;
    for (uint32_t j = 0; j < count; ++j) {
        if (!sets[j]) continue;
        if (sets[j]->pool != pool || sets[j]->pending) return INVALID;
        for (uint32_t k = 0; k < j; ++k) if (sets[k] == sets[j]) return INVALID;
    }
    for (uint32_t j = 0; j < count; ++j)
        if (sets[j] && d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, sets[j])) return INVALID;
    for (uint32_t j = 0; j < count; ++j) if (sets[j]) free_set(pool, sets[j]);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice d,
    const VkPipelineLayoutCreateInfo *info, const VkAllocationCallbacks *a, VkPipelineLayout *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        (info->setLayoutCount && !info->pSetLayouts)) return INVALID;
    if (info->pNext || info->flags || info->setLayoutCount > PS5VK_MAX_SETS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pushConstantRangeCount && !info->pPushConstantRanges) return INVALID;
    for (uint32_t j = 0; j < info->setLayoutCount; ++j)
        if (!info->pSetLayouts[j] || info->pSetLayouts[j]->device != d) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkPipelineLayout layout = alloc(d, a, sizeof(*layout), &saved, &custom);
    if (!layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(layout, 0, sizeof(*layout));
    layout->device = d; layout->allocator = saved; layout->custom_allocator = custom;
    layout->set_count = info->setLayoutCount;
    for (uint32_t j = 0; j < info->setLayoutCount; ++j)
        layout->sets[j] = info->pSetLayouts[j]->signature;
    const VkShaderStageFlags supported = VK_SHADER_STAGE_COMPUTE_BIT |
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t j = 0; j < info->pushConstantRangeCount; ++j) {
        const VkPushConstantRange *range = &info->pPushConstantRanges[j];
        if (!range->stageFlags || (range->stageFlags & ~supported) || !range->size ||
            (range->offset & 3u) || (range->size & 3u) ||
            range->offset >= PS5VK_MAX_PUSH_CONSTANT_BYTES ||
            range->size > PS5VK_MAX_PUSH_CONSTANT_BYTES - range->offset) {
            ps5vk_object_free(layout, &saved, custom); return INVALID;
        }
        uint32_t first = range->offset / 4u, end = (range->offset + range->size) / 4u;
        for (uint32_t k = first; k < end; ++k) {
            if (layout->push_constant_stages[k] & range->stageFlags) {
                ps5vk_object_free(layout, &saved, custom); return INVALID;
            }
            layout->push_constant_stages[k] |= range->stageFlags;
        }
        if (range->offset + range->size > layout->push_constant_size)
            layout->push_constant_size = range->offset + range->size;
    }
    ++d->descriptor_objects; *out = layout;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout layout,
                                                  const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !layout || layout->device != d) return;
    VkAllocationCallbacks saved = layout->allocator; VkBool32 custom = layout->custom_allocator;
    --d->descriptor_objects; ps5vk_object_free(layout, &saved, custom);
}
