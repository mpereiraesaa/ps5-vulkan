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

static int indices(VkDescriptorSet set, uint32_t binding, uint32_t element,
                   uint32_t count, uint32_t out[PS5VK_MAX_DESCRIPTORS])
{
    if (!set || !count || count > PS5VK_MAX_DESCRIPTORS || binding >= PS5VK_MAX_BINDINGS ||
        element >= set->signature.binding[binding].count) return 0;
    VkShaderStageFlags stages = set->signature.binding[binding].stages;
    VkBool32 image=set->signature.combined_image[binding];
    for (uint32_t j = 0; j < count; ++j) {
        while (binding < PS5VK_MAX_BINDINGS && element == set->signature.binding[binding].count) {
            ++binding; element = 0;
        }
        if (binding == PS5VK_MAX_BINDINGS || set->signature.binding[binding].stages != stages ||
            set->signature.combined_image[binding]!=image)
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
        VkBool32 image=w->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        uint32_t dst[PS5VK_MAX_DESCRIPTORS];
        if (w->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || w->pNext || !w->dstSet ||
            w->dstSet->pool->device != d || w->dstSet->pending || (image?!w->pImageInfo:!w->pBufferInfo) ||
            (!image && w->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ||
            w->dstBinding>=PS5VK_MAX_BINDINGS || w->dstSet->signature.combined_image[w->dstBinding]!=image ||
            !indices(w->dstSet, w->dstBinding, w->dstArrayElement, w->descriptorCount, dst)) {
            ++d->lifetime_errors; return;
        }
        /* Prevalidate each whole write. Earlier writes follow Vulkan's ordering;
         * invalid application input does not retroactively roll them back. */
        for (uint32_t k = 0; k < w->descriptorCount; ++k) {
            if(image) {
                const VkDescriptorImageInfo *v=&w->pImageInfo[k];
                void *address;VkDeviceSize bytes;
                if(!v->sampler || v->sampler->device!=d || !v->imageView || v->imageView->device!=d ||
                    !v->imageView->image || !(v->imageView->image->info.usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
                    (v->imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && v->imageLayout!=VK_IMAGE_LAYOUT_GENERAL) ||
                    ps5vk_image_span(d,v->imageView->image,&address,&bytes)!=VK_SUCCESS) {
                    ++d->lifetime_errors;return;
                }
                continue;
            }
            const VkDescriptorBufferInfo *b = &w->pBufferInfo[k];
            void *address; VkDeviceSize size;
            if (!ps5vk_buffer_usage(d,b->buffer,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
                !d->buffer_alignment || b->offset % d->buffer_alignment ||
                ps5vk_buffer_span(d, b->buffer, b->offset, b->range, &address, &size) != VK_SUCCESS) {
                ++d->lifetime_errors; return;
            }
        }
        if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, w->dstSet)) { ++d->lifetime_errors; return; }
        for (uint32_t k = 0; k < w->descriptorCount; ++k) {
            if(image) {
                w->dstSet->images[dst[k]]=w->pImageInfo[k];
                w->dstSet->image_resources[dst[k]]=w->pImageInfo[k].imageView->image;
            } else w->dstSet->buffers[dst[k]] = w->pBufferInfo[k];
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
            c->srcSet->signature.combined_image[c->srcBinding]!=c->dstSet->signature.combined_image[c->dstBinding] ||
            !indices(c->srcSet, c->srcBinding, c->srcArrayElement, c->descriptorCount, src) ||
            !indices(c->dstSet, c->dstBinding, c->dstArrayElement, c->descriptorCount, dst)) {
            ++d->lifetime_errors; return;
        }
        /* Copy descriptor references, not pointed-to data. Undefined descriptors
         * may legally be copied; resolving GPU addresses happens at consumption. */
        VkDescriptorBufferInfo values[PS5VK_MAX_DESCRIPTORS];
        VkDescriptorImageInfo image_values[PS5VK_MAX_DESCRIPTORS];
        VkImage resources[PS5VK_MAX_DESCRIPTORS];
        if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, c->dstSet)) { ++d->lifetime_errors; return; }
        VkBool32 defined[PS5VK_MAX_DESCRIPTORS];
        for (uint32_t k = 0; k < c->descriptorCount; ++k) {
            values[k] = c->srcSet->buffers[src[k]]; defined[k] = c->srcSet->defined[src[k]];
            image_values[k]=c->srcSet->images[src[k]];resources[k]=c->srcSet->image_resources[src[k]];
        }
        for (uint32_t k = 0; k < c->descriptorCount; ++k) {
            c->dstSet->buffers[dst[k]] = values[k]; c->dstSet->defined[dst[k]] = defined[k];
            c->dstSet->images[dst[k]]=image_values[k];c->dstSet->image_resources[dst[k]]=resources[k];
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
        VkBool32 image=b->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (b->descriptorCount && (image ? (!d->graphics_enabled || b->pImmutableSamplers ||
                b->stageFlags!=VK_SHADER_STAGE_FRAGMENT_BIT) :
                (b->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || (b->stageFlags & ~VK_SHADER_STAGE_COMPUTE_BIT))))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (b->descriptorCount > PS5VK_MAX_DESCRIPTORS - signature.count)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        signature.binding[b->binding].count = b->descriptorCount;
        signature.combined_image[b->binding]=b->descriptorCount?image:VK_FALSE;
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
    uint64_t capacity = 0,image_capacity=0;
    for (uint32_t j = 0; j < info->poolSizeCount; ++j) {
        VkBool32 image=info->pPoolSizes[j].type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (image ? !d->graphics_enabled : info->pPoolSizes[j].type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (!info->pPoolSizes[j].descriptorCount) return INVALID;
        if(image)image_capacity+=info->pPoolSizes[j].descriptorCount;
        else capacity += info->pPoolSizes[j].descriptorCount;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkDescriptorPool pool = alloc(d, a, sizeof(*pool), &saved, &custom);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->device = d; pool->allocator = saved; pool->custom_allocator = custom;
    pool->flags = info->flags; pool->max_sets = info->maxSets; pool->capacity = capacity;
    pool->image_capacity=image_capacity;
    ++d->descriptor_objects; *out = pool;
    return VK_SUCCESS;
}
static void free_set(VkDescriptorPool pool, VkDescriptorSet set)
{
    VkDescriptorSet *p = &pool->sets;
    while (*p && *p != set) p = &(*p)->next;
    if (!*p) return;
    *p = set->next; --pool->used_sets;
    for(unsigned j=0;j<PS5VK_MAX_BINDINGS;++j)
        if(set->signature.combined_image[j])pool->image_used-=set->signature.binding[j].count;
        else pool->used-=set->signature.binding[j].count;
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
    uint64_t needed = 0,image_needed=0;
    for (uint32_t j = 0; j < info->descriptorSetCount; ++j) {
        VkDescriptorSetLayout layout = info->pSetLayouts[j];
        if (!layout || layout->device != d) return INVALID;
        for(unsigned k=0;k<PS5VK_MAX_BINDINGS;++k)
            if(layout->signature.combined_image[k])image_needed+=layout->signature.binding[k].count;
            else needed+=layout->signature.binding[k].count;
    }
    if (needed > pool->capacity - pool->used || image_needed>pool->image_capacity-pool->image_used) return VK_ERROR_OUT_OF_POOL_MEMORY;
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
    pool->used += needed; pool->used_sets += info->descriptorSetCount;
    pool->image_used+=image_needed;
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
    if (info->pNext || info->flags || info->pushConstantRangeCount || info->setLayoutCount > PS5VK_MAX_SETS)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t j = 0; j < info->setLayoutCount; ++j)
        if (!info->pSetLayouts[j] || info->pSetLayouts[j]->device != d) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkPipelineLayout layout = alloc(d, a, sizeof(*layout), &saved, &custom);
    if (!layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    layout->device = d; layout->allocator = saved; layout->custom_allocator = custom;
    layout->set_count = info->setLayoutCount;
    for (uint32_t j = 0; j < info->setLayoutCount; ++j)
        layout->sets[j] = info->pSetLayouts[j]->signature;
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
