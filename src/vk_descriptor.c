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

/* Slot indices of a (binding, element, count) range, with Vulkan's rollover
 * into consecutive bindings of the same type and stages. */
static int signature_indices(const struct ps5vk_set_signature *signature, uint32_t binding,
    uint32_t element, uint32_t count, uint32_t out[PS5VK_MAX_DESCRIPTORS])
{
    if (!count || count > PS5VK_MAX_DESCRIPTORS || binding >= PS5VK_MAX_BINDINGS ||
        element >= signature->binding[binding].count) return 0;
    VkShaderStageFlags stages = signature->binding[binding].stages;
    VkDescriptorType type = signature->type[binding];
    for (uint32_t j = 0; j < count; ++j) {
        while (binding < PS5VK_MAX_BINDINGS && element == signature->binding[binding].count) {
            ++binding; element = 0;
        }
        if (binding == PS5VK_MAX_BINDINGS || signature->binding[binding].stages != stages ||
            signature->type[binding] != type)
            return 0;
        out[j] = signature->binding[binding].first + element++;
    }
    return 1;
}
static int indices(VkDescriptorSet set, uint32_t binding, uint32_t element,
                   uint32_t count, uint32_t out[PS5VK_MAX_DESCRIPTORS])
{
    return set && signature_indices(&set->signature, binding, element, count, out);
}

/* Byte range [offset, offset + bytes) inside one inline block. Offsets and
 * sizes are multiples of four (VUID-VkWriteDescriptorSet-descriptorType-02219
 * and -02220, VkCopyDescriptorSet -02223/-02224/-02225). A range that would
 * continue into the next binding is refused: that consecutive-binding
 * rollover is not implemented for inline blocks. */
static VkBool32 inline_range(VkDescriptorSet set, uint32_t binding, uint32_t offset,
                             uint32_t bytes)
{
    if (binding >= PS5VK_MAX_BINDINGS ||
        set->signature.type[binding] != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ||
        !bytes || (offset | bytes) % 4u) return VK_FALSE;
    uint32_t size = set->inline_uniform.bytes[binding];
    return offset < size && bytes <= size - offset;
}

static VkBool32 inline_write(VkDevice d, const VkWriteDescriptorSet *w)
{
    const VkWriteDescriptorSetInlineUniformBlock *data = w->pNext;
    if (!data || data->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK ||
        data->pNext || !data->pData || data->dataSize != w->descriptorCount ||
        !inline_range(w->dstSet, w->dstBinding, w->dstArrayElement, w->descriptorCount))
        return VK_FALSE;
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, w->dstSet))
        return VK_FALSE;
    VkDescriptorSet set = w->dstSet;
    memcpy(set->inline_data + set->inline_uniform.offset[w->dstBinding] + w->dstArrayElement,
           data->pData, w->descriptorCount);
    set->defined[set->signature.binding[w->dstBinding].first] = VK_TRUE;
    ++set->generation;
    return VK_TRUE;
}

static VkBool32 inline_copy(VkDevice d, const VkCopyDescriptorSet *c)
{
    if (!inline_range(c->srcSet, c->srcBinding, c->srcArrayElement, c->descriptorCount) ||
        !inline_range(c->dstSet, c->dstBinding, c->dstArrayElement, c->descriptorCount))
        return VK_FALSE;
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, c->dstSet))
        return VK_FALSE;
    memmove(c->dstSet->inline_data + c->dstSet->inline_uniform.offset[c->dstBinding] +
                c->dstArrayElement,
            c->srcSet->inline_data + c->srcSet->inline_uniform.offset[c->srcBinding] +
                c->srcArrayElement, c->descriptorCount);
    if (c->srcSet->defined[c->srcSet->signature.binding[c->srcBinding].first])
        c->dstSet->defined[c->dstSet->signature.binding[c->dstBinding].first] = VK_TRUE;
    ++c->dstSet->generation;
    return VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d, uint32_t write_count,
    const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *copies)
{
    if (!d) return;
    if ((write_count && !writes) || (copy_count && !copies)) { ++d->lifetime_errors; return; }
    for (uint32_t j = 0; j < write_count; ++j) {
        const VkWriteDescriptorSet *w = &writes[j];
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            if (w->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || !w->dstSet ||
                w->dstSet->pool->device != d || w->dstSet->pending || !inline_write(d, w)) {
                ++d->lifetime_errors; return;
            }
            continue;
        }
        VkBool32 input=w->descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        VkBool32 storage_image=w->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        VkBool32 sampler=w->descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER;
        VkBool32 sampled_image=w->descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        VkBool32 image=w->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || input ||
            storage_image || sampler || sampled_image;
        VkDescriptorType base_type=ps5vk_base_buffer_descriptor_type(w->descriptorType);
        VkBool32 buffer=base_type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            base_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkBool32 storage_texel=w->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        VkBool32 texel=w->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || storage_texel;
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
                if(sampler) {
                    /* A separate sampler: imageView and imageLayout are
                     * ignored for this type. */
                    if(!v->sampler || v->sampler->device!=d) {++d->lifetime_errors;return;}
                    continue;
                }
                /* nullDescriptor (VK_EXT_robustness2): a null view is a legal
                 * storage image or combined image (whose sampler stays
                 * required), never an input attachment. Separate sampled
                 * images keep refusing it until their encoder handles it. */
                if(!v->imageView && (d->enabled_features_t09 & PS5VK_T09_FEATURE_NULL_DESCRIPTOR) &&
                   !input && !sampled_image &&
                   (storage_image || (v->sampler && v->sampler->device==d)))
                    continue;
                if(input || storage_image) {
                   /* Both resource-only image descriptors ignore the sampler.
                    * The input attachment permits a subpass read layout; a
                    * storage image must be GENERAL. Bound allocation and
                    * actual layout remain consumption-time obligations. */
                   if(!v->imageView || v->imageView->device!=d ||
                      !v->imageView->image || v->imageView->image->device!=d ||
                      !(ps5vk_image_view_usage(v->imageView) &
                        (storage_image ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) ||
                      (v->imageView->view_type!=VK_IMAGE_VIEW_TYPE_2D &&
                       v->imageView->view_type!=VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
                      (storage_image ? v->imageLayout!=VK_IMAGE_LAYOUT_GENERAL :
                       (v->imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                        v->imageLayout!=VK_IMAGE_LAYOUT_GENERAL))) {
                        ++d->lifetime_errors;return;
                    }
                    continue;
                }
                /* A sampled image is the combined record's image half: the
                 * same view, usage, layout and backing rules, and its sampler
                 * member is ignored. */
                if((!sampled_image && (!v->sampler || v->sampler->device!=d)) ||
                    !v->imageView || v->imageView->device!=d ||
                    !v->imageView->image || !(ps5vk_image_view_usage(v->imageView)&VK_IMAGE_USAGE_SAMPLED_BIT) ||
                    (v->imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && v->imageLayout!=VK_IMAGE_LAYOUT_GENERAL) ||
                    ps5vk_image_span(d,v->imageView->image,&address,&bytes)!=VK_SUCCESS) {
                    ++d->lifetime_errors;return;
                }
                continue;
            }
            if(texel) {
                VkBufferView view=w->pTexelBufferView[k];
                /* nullDescriptor: a null uniform texel view. Storage texel
                 * views keep refusing it until their encoder handles it. */
                if(!view && !storage_texel &&
                   (d->enabled_features_t09 & PS5VK_T09_FEATURE_NULL_DESCRIPTOR))continue;
                if(!view || view->device!=d || !view->buffer ||
                   (storage_texel && !ps5vk_buffer_usage(d,view->buffer,
                        VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))) {++d->lifetime_errors;return;}
                continue;
            }
            const VkDescriptorBufferInfo *b = &w->pBufferInfo[k];
            void *address; VkDeviceSize size;
            /* VUID-VkDescriptorBufferInfo-buffer-02999: a null buffer has
             * offset zero and range VK_WHOLE_SIZE. */
            if(!b->buffer && (d->enabled_features_t09 & PS5VK_T09_FEATURE_NULL_DESCRIPTOR)) {
                if(b->offset || b->range!=VK_WHOLE_SIZE) {++d->lifetime_errors;return;}
                continue;
            }
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
                /* The sampler member is ignored for resource-only images, so it
                 * is canonicalized rather than copied: a future consumer must
                 * not be able to observe application data this descriptor type
                 * never uses, and a copied descriptor carries the canonical
                 * value too. */
                if(input || storage_image || sampled_image) stored.sampler = VK_NULL_HANDLE;
                if(sampler) {
                    stored.imageView = VK_NULL_HANDLE;
                    stored.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                }
                w->dstSet->images[dst[k]]=stored;
                w->dstSet->image_resources[dst[k]]=
                    stored.imageView ? stored.imageView->image : VK_NULL_HANDLE;
            } else if(texel) w->dstSet->texel_views[dst[k]]=w->pTexelBufferView[k];
            else w->dstSet->buffers[dst[k]] = w->pBufferInfo[k];
            w->dstSet->defined[dst[k]] = VK_TRUE;
        }
        ++w->dstSet->generation;
    }
    for (uint32_t j = 0; j < copy_count; ++j) {
        const VkCopyDescriptorSet *c = &copies[j];
        uint32_t src[PS5VK_MAX_DESCRIPTORS], dst[PS5VK_MAX_DESCRIPTORS];
        if (c->sType == VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET && !c->pNext &&
            c->srcSet && c->dstSet && c->srcBinding < PS5VK_MAX_BINDINGS &&
            c->srcSet->signature.type[c->srcBinding] == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            if (c->srcSet->pool->device != d || c->dstSet->pool->device != d ||
                c->dstSet->pending || !inline_copy(d, c)) {
                ++d->lifetime_errors; return;
            }
            continue;
        }
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
         * may legally be copied; resolving GPU addresses happens at consumption.
         * A range's slots are contiguous (canonical prefixes, rollover into the
         * next binding), so each array moves as one overlapping-safe block. */
        if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_DESCRIPTOR_SET, c->dstSet)) { ++d->lifetime_errors; return; }
        const size_t n = c->descriptorCount;
        memmove(c->dstSet->buffers + dst[0], c->srcSet->buffers + src[0], n * sizeof(VkDescriptorBufferInfo));
        memmove(c->dstSet->images + dst[0], c->srcSet->images + src[0], n * sizeof(VkDescriptorImageInfo));
        memmove(c->dstSet->texel_views + dst[0], c->srcSet->texel_views + src[0], n * sizeof(VkBufferView));
        memmove(c->dstSet->image_resources + dst[0], c->srcSet->image_resources + src[0], n * sizeof(VkImage));
        memmove(c->dstSet->defined + dst[0], c->srcSet->defined + src[0], n * sizeof(VkBool32));
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
    struct ps5vk_inline_uniform_layout inline_uniform = {0};
    VkBool32 seen[PS5VK_MAX_BINDINGS] = {0};
    uint32_t dynamic_uniform = 0, dynamic_storage = 0;
    for (uint32_t j = 0; j < info->bindingCount; ++j) {
        const VkDescriptorSetLayoutBinding *b = &info->pBindings[j];
        if (b->binding >= PS5VK_MAX_BINDINGS) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (seen[b->binding]) return INVALID;
        seen[b->binding] = VK_TRUE;
        if (b->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            /* descriptorCount is the block's byte size (VUID-02209: a
             * multiple of four). pImmutableSamplers is ignored for this type.
             * The block takes one signature slot and bytes of set storage. */
            if (!d->inline_uniform_block_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (!b->descriptorCount) continue;
            if (b->descriptorCount % 4u || !valid_descriptor_stages(b->stageFlags))
                return INVALID;
            if (b->descriptorCount > PS5VK_MAX_INLINE_UNIFORM_BLOCK_BYTES ||
                inline_uniform.blocks == PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_SET ||
                signature.count == PS5VK_MAX_DESCRIPTORS)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            inline_uniform.bytes[b->binding] = b->descriptorCount;
            ++inline_uniform.blocks;
            signature.binding[b->binding].count = 1;
            signature.type[b->binding] = b->descriptorType;
            signature.binding[b->binding].stages = b->stageFlags;
            ++signature.count;
            continue;
        }
        VkBool32 input=b->descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        VkBool32 storage_image=b->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        VkBool32 sampled_image=b->descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        VkBool32 sampler=b->descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER;
        VkBool32 image=b->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || input ||
            sampler;
        VkDescriptorType base_type=ps5vk_base_buffer_descriptor_type(b->descriptorType);
        VkBool32 buffer=base_type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            base_type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkBool32 texel=b->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            b->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
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
        /* Resource-only images (input attachment, storage and sampled image)
         * ignore pImmutableSamplers. Immutable samplers are not implemented,
         * so SAMPLER and COMBINED_IMAGE_SAMPLER refuse them. */
        const VkBool32 resource_image = input || storage_image || sampled_image;
        if (b->descriptorCount && ((image || resource_image) ? (!d->graphics_enabled ||
                (b->pImmutableSamplers && !resource_image)) :
                (!(buffer||texel) || b->pImmutableSamplers)))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (b->descriptorCount > PS5VK_MAX_DESCRIPTORS - signature.count)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        /* Bind-time offsets are stored per set in PS5VK_MAX_DYNAMIC_*
         * compact slots, the reported per-layout dynamic limits. */
        if (b->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            if (b->descriptorCount > PS5VK_MAX_DYNAMIC_UNIFORM - dynamic_uniform)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            dynamic_uniform += b->descriptorCount;
        } else if (b->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            if (b->descriptorCount > PS5VK_MAX_DYNAMIC_STORAGE - dynamic_storage)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            dynamic_storage += b->descriptorCount;
        }
        signature.binding[b->binding].count = b->descriptorCount;
        signature.type[b->binding]=b->descriptorCount?b->descriptorType:0;
        signature.binding[b->binding].stages = b->descriptorCount ? b->stageFlags : 0;
        signature.count += b->descriptorCount;
    }
    uint32_t offset = 0;
    for (unsigned j = 0; j < PS5VK_MAX_BINDINGS; ++j) {
        signature.binding[j].first = offset;
        offset += signature.binding[j].count;
        inline_uniform.offset[j] = inline_uniform.total_bytes;
        inline_uniform.total_bytes += inline_uniform.bytes[j];
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkDescriptorSetLayout layout = alloc(d, a, sizeof(*layout), &saved, &custom);
    if (!layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    layout->device = d; layout->allocator = saved; layout->custom_allocator = custom;
    layout->signature = signature; layout->inline_uniform = inline_uniform;
    ++d->descriptor_objects; *out = layout;
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
    if (info->flags & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t inline_bindings_capacity=0;
    if (info->pNext) {
        /* The only accepted extension sizes the pool's inline-block bindings. */
        const VkDescriptorPoolInlineUniformBlockCreateInfo *inline_info = info->pNext;
        if (!d->inline_uniform_block_enabled ||
            inline_info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO ||
            inline_info->pNext)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        inline_bindings_capacity = inline_info->maxInlineUniformBlockBindings;
    }
    uint64_t storage_capacity=0,uniform_capacity=0,dynamic_storage_capacity=0,
        dynamic_uniform_capacity=0,texel_capacity=0,image_capacity=0,input_capacity=0,
        storage_image_capacity=0,sampler_capacity=0,sampled_image_capacity=0,
        storage_texel_capacity=0,inline_bytes_capacity=0;
    for (uint32_t j = 0; j < info->poolSizeCount; ++j) {
        const VkDescriptorPoolSize *size=&info->pPoolSizes[j];
        if (size->type==VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            /* descriptorCount is a byte count (VUID-02218: a multiple of 4). */
            if (!d->inline_uniform_block_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
            if (!size->descriptorCount || size->descriptorCount % 4u) return INVALID;
            inline_bytes_capacity += size->descriptorCount;
            continue;
        }
        uint64_t *capacity=size->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?&storage_image_capacity:
            size->type==VK_DESCRIPTOR_TYPE_SAMPLER?&sampler_capacity:
            size->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE?&sampled_image_capacity:
            size->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?&storage_texel_capacity:
            size->type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT?&input_capacity:
            size->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER?&storage_capacity:
            size->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?&uniform_capacity:
            size->type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC?&dynamic_storage_capacity:
            size->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC?&dynamic_uniform_capacity:
            size->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER?&texel_capacity:
            size->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?&image_capacity:NULL;
        if (!capacity ||
            ((size->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
              size->type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
              size->type==VK_DESCRIPTOR_TYPE_SAMPLER ||
              size->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
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
    pool->storage_image_capacity=storage_image_capacity;
    pool->sampler_capacity=sampler_capacity;
    pool->sampled_image_capacity=sampled_image_capacity;
    pool->storage_texel_capacity=storage_texel_capacity;
    pool->inline_bytes_capacity=inline_bytes_capacity;
    pool->inline_bindings_capacity=inline_bindings_capacity;
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
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: pool->storage_image_used-=count;break;
        case VK_DESCRIPTOR_TYPE_SAMPLER: pool->sampler_used-=count;break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: pool->sampled_image_used-=count;break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: pool->storage_texel_used-=count;break;
        default: break;
        }
    }
    pool->inline_bytes_used-=set->inline_uniform.total_bytes;
    pool->inline_bindings_used-=set->inline_uniform.blocks;
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
        dynamic_uniform_needed=0,texel_needed=0,image_needed=0,input_needed=0,
        storage_image_needed=0,sampler_needed=0,sampled_image_needed=0,storage_texel_needed=0,
        inline_bytes_needed=0,inline_bindings_needed=0;
    for (uint32_t j = 0; j < info->descriptorSetCount; ++j) {
        VkDescriptorSetLayout layout = info->pSetLayouts[j];
        if (!layout || layout->device != d) return INVALID;
        inline_bytes_needed+=layout->inline_uniform.total_bytes;
        inline_bindings_needed+=layout->inline_uniform.blocks;
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
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: storage_image_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_SAMPLER: sampler_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: sampled_image_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: storage_texel_needed+=count;break;
            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: break;
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
        input_needed>pool->input_capacity-pool->input_used ||
        storage_image_needed>pool->storage_image_capacity-pool->storage_image_used ||
        sampler_needed>pool->sampler_capacity-pool->sampler_used ||
        sampled_image_needed>pool->sampled_image_capacity-pool->sampled_image_used ||
        storage_texel_needed>pool->storage_texel_capacity-pool->storage_texel_used ||
        inline_bytes_needed>pool->inline_bytes_capacity-pool->inline_bytes_used ||
        inline_bindings_needed>pool->inline_bindings_capacity-pool->inline_bindings_used)
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    VkDescriptorSet pending = NULL;
    for (uint32_t j = 0; j < info->descriptorSetCount; ++j) {
        VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
        /* One allocation: the structure, then arrays sized from the layout. */
        const uint32_t capacity = info->pSetLayouts[j]->signature.count ?
            info->pSetLayouts[j]->signature.count : 1u;
        const size_t header = (sizeof(struct VkDescriptorSet_T) + 15u) & ~(size_t)15u;
        const size_t per_descriptor = sizeof(VkDescriptorBufferInfo) +
            sizeof(VkDescriptorImageInfo) + sizeof(VkBufferView) + sizeof(VkImage) +
            sizeof(VkBool32);
        VkDescriptorSet set = ps5vk_object_alloc(NULL,
            pool->custom_allocator ? &pool->allocator : NULL,
            header + (size_t)capacity * per_descriptor,
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
        {
            unsigned char *tail = (unsigned char *)set + header;
            memset(tail, 0, (size_t)capacity * per_descriptor);
            set->capacity = capacity;
            set->buffers = (VkDescriptorBufferInfo *)tail; tail += capacity * sizeof(VkDescriptorBufferInfo);
            set->images = (VkDescriptorImageInfo *)tail; tail += capacity * sizeof(VkDescriptorImageInfo);
            set->texel_views = (VkBufferView *)tail; tail += capacity * sizeof(VkBufferView);
            set->image_resources = (VkImage *)tail; tail += capacity * sizeof(VkImage);
            set->defined = (VkBool32 *)tail;
        }
        set->pool = pool; set->signature = info->pSetLayouts[j]->signature;
        set->inline_uniform = info->pSetLayouts[j]->inline_uniform;
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
    pool->storage_image_used+=storage_image_needed;
    pool->sampler_used+=sampler_needed;
    pool->sampled_image_used+=sampled_image_needed;
    pool->storage_texel_used+=storage_texel_needed;
    pool->inline_bytes_used+=inline_bytes_needed;
    pool->inline_bindings_used+=inline_bindings_needed;
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
    /* maxInlineUniformTotalSize and maxPerStageDescriptorInlineUniformBlocks
     * bound the whole pipeline layout, across its sets. */
    static const VkShaderStageFlagBits inline_stages[] = {
        VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    uint32_t inline_bytes = 0, inline_per_stage[6] = {0};
    for (uint32_t j = 0; j < info->setLayoutCount; ++j) {
        const VkDescriptorSetLayout set = info->pSetLayouts[j];
        inline_bytes += set->inline_uniform.total_bytes;
        for (unsigned b = 0; b < PS5VK_MAX_BINDINGS; ++b) {
            if (!set->inline_uniform.bytes[b]) continue;
            for (unsigned s = 0; s < 6; ++s)
                if (set->signature.binding[b].stages & inline_stages[s])
                    ++inline_per_stage[s];
        }
    }
    if (inline_bytes > PS5VK_MAX_INLINE_UNIFORM_TOTAL_BYTES)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for (unsigned s = 0; s < 6; ++s)
        if (inline_per_stage[s] > PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_STAGE)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkPipelineLayout layout = alloc(d, a, sizeof(*layout), &saved, &custom);
    if (!layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(layout, 0, sizeof(*layout));
    layout->device = d; layout->allocator = saved; layout->custom_allocator = custom;
    layout->set_count = info->setLayoutCount;
    for (uint32_t j = 0; j < info->setLayoutCount; ++j)
        layout->sets[j] = info->pSetLayouts[j]->signature;
    const VkShaderStageFlags supported = VK_SHADER_STAGE_COMPUTE_BIT |
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT;
    VkShaderStageFlags push_stages_seen = 0;
    for (uint32_t j = 0; j < info->pushConstantRangeCount; ++j) {
        const VkPushConstantRange *range = &info->pPushConstantRanges[j];
        if (!range->stageFlags || (range->stageFlags & ~supported) || !range->size ||
            (range->stageFlags & push_stages_seen) ||
            (range->offset & 3u) || (range->size & 3u) ||
            range->offset >= PS5VK_MAX_PUSH_CONSTANT_BYTES ||
            range->size > PS5VK_MAX_PUSH_CONSTANT_BYTES - range->offset) {
            ps5vk_object_free(layout, &saved, custom); return INVALID;
        }
        /* VUID00292 applies to stage membership, not byte-range overlap. */
        push_stages_seen |= range->stageFlags;
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

/* Descriptor update templates (VK_KHR_descriptor_update_template). A template
 * is a recorded list of VkWriteDescriptorSet shapes: an update gathers each
 * entry's elements from pData at offset + k * stride and applies them through
 * vkUpdateDescriptorSets, so templates and direct writes share one validation
 * and one storage path. Only DESCRIPTOR_SET templates exist here; push
 * descriptors are not implemented. */
static VkBool32 template_image_type(VkDescriptorType type)
{
    return type == VK_DESCRIPTOR_TYPE_SAMPLER ||
        type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
        type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
        type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
static VkBool32 template_texel_type(VkDescriptorType type)
{
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
        type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}
static VkBool32 template_buffer_type(VkDescriptorType type)
{
    VkDescriptorType base = ps5vk_base_buffer_descriptor_type(type);
    return base == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || base == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplateKHR(VkDevice d,
    const VkDescriptorUpdateTemplateCreateInfo *info, const VkAllocationCallbacks *a,
    VkDescriptorUpdateTemplate *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO ||
        !info->descriptorUpdateEntryCount || !info->pDescriptorUpdateEntries)
        return INVALID;
    if (info->pNext || info->flags ||
        info->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkDescriptorSetLayout layout = info->descriptorSetLayout;
    if (!layout || layout->device != d) return INVALID;
    uint32_t slots[PS5VK_MAX_DESCRIPTORS];
    for (uint32_t j = 0; j < info->descriptorUpdateEntryCount; ++j) {
        const VkDescriptorUpdateTemplateEntry *e = &info->pDescriptorUpdateEntries[j];
        /* Each entry is a legal write range of the layout: the starting
         * binding's type, then rollover through same-typed bindings. */
        if (e->dstBinding >= PS5VK_MAX_BINDINGS ||
            layout->signature.type[e->dstBinding] != e->descriptorType ||
            !(template_image_type(e->descriptorType) || template_texel_type(e->descriptorType) ||
              template_buffer_type(e->descriptorType)) ||
            !signature_indices(&layout->signature, e->dstBinding, e->dstArrayElement,
                               e->descriptorCount, slots))
            return INVALID;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkDescriptorUpdateTemplate t = alloc(d, a, sizeof(*t) +
        (size_t)info->descriptorUpdateEntryCount * sizeof(VkDescriptorUpdateTemplateEntry),
        &saved, &custom);
    if (!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->device = d; t->allocator = saved; t->custom_allocator = custom;
    t->signature = layout->signature;
    t->entry_count = info->descriptorUpdateEntryCount;
    memcpy(t->entries, info->pDescriptorUpdateEntries,
           t->entry_count * sizeof(VkDescriptorUpdateTemplateEntry));
    ++d->descriptor_objects; *out = t;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplateKHR(VkDevice d,
    VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !t || t->device != d) return;
    /* An update copies descriptor values; nothing keeps the template's address. */
    VkAllocationCallbacks saved = t->allocator; VkBool32 custom = t->custom_allocator;
    --d->descriptor_objects; ps5vk_object_free(t, &saved, custom);
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplateKHR(VkDevice d,
    VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data)
{
    if (!d) return;
    /* The set must be compatible with the template's layout: the same
     * canonical signature the template was validated against. */
    if (!set || !set->pool || set->pool->device != d || !t || t->device != d || !data ||
        memcmp(&set->signature, &t->signature, sizeof(set->signature))) {
        ++d->lifetime_errors; return;
    }
    const unsigned char *bytes = data;
    /* Entries are applied in bounded chunks so that a 1024-element entry
     * needs no 1024-element stack copy. A chunk starts where the previous
     * one ended, following the same consecutive-binding rollover. */
    enum { CHUNK = 64 };
    for (uint32_t j = 0; j < t->entry_count; ++j) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[j];
        uint32_t binding = e->dstBinding, element = e->dstArrayElement;
        for (uint32_t done = 0; done < e->descriptorCount;) {
            const uint32_t n = e->descriptorCount - done < CHUNK ? e->descriptorCount - done : CHUNK;
            VkDescriptorImageInfo images[CHUNK];
            VkDescriptorBufferInfo buffers[CHUNK];
            VkBufferView views[CHUNK];
            while (binding < PS5VK_MAX_BINDINGS && element == set->signature.binding[binding].count) {
                ++binding; element = 0;
            }
            if (binding >= PS5VK_MAX_BINDINGS) { ++d->lifetime_errors; return; }
            VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set, .dstBinding = binding, .dstArrayElement = element,
                .descriptorCount = n, .descriptorType = e->descriptorType};
            /* The application's pData is not necessarily aligned for these
             * structures, so every element is copied, never dereferenced. */
            for (uint32_t k = 0; k < n; ++k) {
                const unsigned char *item = bytes + e->offset + (size_t)(done + k) * e->stride;
                if (template_image_type(e->descriptorType))
                    memcpy(&images[k], item, sizeof(images[k]));
                else if (template_texel_type(e->descriptorType))
                    memcpy(&views[k], item, sizeof(views[k]));
                else
                    memcpy(&buffers[k], item, sizeof(buffers[k]));
            }
            if (template_image_type(e->descriptorType)) w.pImageInfo = images;
            else if (template_texel_type(e->descriptorType)) w.pTexelBufferView = views;
            else w.pBufferInfo = buffers;
            unsigned before = d->lifetime_errors;
            vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
            /* Like a write array, an invalid entry stops the update; entries
             * already applied stay applied. */
            if (d->lifetime_errors != before) return;
            /* Advance past the n descriptors just written. */
            for (uint32_t k = 0; k < n; ++k) {
                while (binding < PS5VK_MAX_BINDINGS && element == set->signature.binding[binding].count) {
                    ++binding; element = 0;
                }
                ++element;
            }
            done += n;
        }
    }
}
