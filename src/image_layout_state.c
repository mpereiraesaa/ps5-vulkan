#include "image_layout_state.h"
/* The layouts this transaction models. The depth/stencil family is listed
 * whole; whether a member means anything for a given aspect is decided by
 * ps5vk_layout_for_aspect, so a colour image still cannot enter a depth
 * layout and a stencil aspect cannot enter a DEPTH_* one. */
static int supported(VkImageLayout l)
{
    return l==VK_IMAGE_LAYOUT_GENERAL || l==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
        l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
        ps5vk_layout_is_mixed_depth_stencil(l) ||
        ps5vk_layout_is_separate_aspect(l);
}
static VkImageAspectFlags image_aspects(VkImage image)
{
    return ps5vk_format_aspects(image->info.format);
}
static VkImageLayout committed(VkImage image,VkImageAspectFlags aspect)
{
    return aspect==VK_IMAGE_ASPECT_STENCIL_BIT?image->stencil_layout:image->layout;
}
static int find(const struct ps5vk_layout_state *s,VkImage image,VkImageAspectFlags aspect)
{
    for(unsigned i=0;i<s->count;++i)
        if(s->entries[i].image==image && s->entries[i].aspect==aspect)return (int)i;
    return -1;
}
/* The aspect set a call may name: exactly the image's own aspect for a
 * single-aspect format, any non-empty subset of DEPTH|STENCIL for a combined
 * one. Nothing outside the format's aspects is ever accepted. */
static int aspects_valid(VkImage image,VkImageAspectFlags aspects)
{
    const VkImageAspectFlags all=image_aspects(image);
    return aspects && !(aspects & ~all) &&
        (all==PS5VK_DEPTH_STENCIL_ASPECTS || aspects==all);
}
static const VkImageAspectFlags order[3]={
    VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_ASPECT_STENCIL_BIT};
VkResult ps5vk_layout_current(const struct ps5vk_layout_state *s,VkImage image,
    VkImageAspectFlags aspect,VkImageLayout *out)
{
    if(!s || !image || !out || s->count>PS5VK_LAYOUT_IMAGES ||
       (aspect!=VK_IMAGE_ASPECT_COLOR_BIT && aspect!=VK_IMAGE_ASPECT_DEPTH_BIT &&
        aspect!=VK_IMAGE_ASPECT_STENCIL_BIT) || !(image_aspects(image)&aspect))
        return VK_ERROR_UNKNOWN;
    const int i=find(s,image,aspect);
    *out=i>=0?s->entries[i].current:committed(image,aspect);
    return VK_SUCCESS;
}
VkResult ps5vk_layout_require_aspects(const struct ps5vk_layout_state *s,VkImage image,
    VkImageAspectFlags aspects,VkImageLayout expected)
{
    if(!s || !image || !supported(expected) || s->count>PS5VK_LAYOUT_IMAGES ||
       !aspects_valid(image,aspects) ||
       (expected==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && !image->swapchain_owned))
        return VK_ERROR_UNKNOWN;
    for(unsigned k=0;k<3;++k) {
        if(!(aspects&order[k]))continue;
        VkImageLayout current;
        if(ps5vk_layout_current(s,image,order[k],&current)!=VK_SUCCESS ||
           !ps5vk_layouts_match_for_aspect(current,expected,order[k]))return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}
VkResult ps5vk_layout_require(const struct ps5vk_layout_state *s,VkImage image,VkImageLayout expected)
{
    if(!image)return VK_ERROR_UNKNOWN;
    return ps5vk_layout_require_aspects(s,image,image_aspects(image),expected);
}
VkResult ps5vk_layout_transition_aspects(struct ps5vk_layout_state *s,VkImage image,
    VkImageAspectFlags aspects,VkImageLayout old,VkImageLayout next)
{
    if(!s || !image || !supported(next) ||
        (!supported(old) && old!=VK_IMAGE_LAYOUT_UNDEFINED) || s->count>PS5VK_LAYOUT_IMAGES ||
        !aspects_valid(image,aspects) ||
        ((old==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
          next==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) && !image->swapchain_owned))
        return VK_ERROR_UNKNOWN;
    /* Validate every named aspect before touching any entry. */
    int index[3]={-1,-1,-1};
    unsigned added=0;
    for(unsigned k=0;k<3;++k) {
        if(!(aspects&order[k]))continue;
        VkImageLayout projected;
        if(!ps5vk_layout_for_aspect(next,order[k],&projected) ||
           (old!=VK_IMAGE_LAYOUT_UNDEFINED && !ps5vk_layout_for_aspect(old,order[k],&projected)))
            return VK_ERROR_UNKNOWN;
        index[k]=find(s,image,order[k]);
        const VkImageLayout current=index[k]>=0?s->entries[index[k]].current:
            committed(image,order[k]);
        /* UNDEFINED discards the aspect's previous contents, so it matches
         * whatever the aspect was in. */
        if(old!=VK_IMAGE_LAYOUT_UNDEFINED &&
           !ps5vk_layouts_match_for_aspect(current,old,order[k]))return VK_ERROR_UNKNOWN;
        if(index[k]<0)++added;
    }
    if(added>PS5VK_LAYOUT_IMAGES-s->count)return VK_ERROR_TOO_MANY_OBJECTS;
    for(unsigned k=0;k<3;++k) {
        if(!(aspects&order[k]))continue;
        int i=index[k];
        if(i<0) {
            const VkImageLayout initial=committed(image,order[k]);
            i=(int)s->count++;
            s->entries[i]=(struct ps5vk_layout_entry){image,order[k],initial,initial};
        }
        s->entries[i].current=next;
    }
    return VK_SUCCESS;
}
VkResult ps5vk_layout_transition(struct ps5vk_layout_state *s,VkImage image,VkImageLayout old,VkImageLayout next)
{
    if(!image)return VK_ERROR_UNKNOWN;
    return ps5vk_layout_transition_aspects(s,image,image_aspects(image),old,next);
}
VkResult ps5vk_layout_commit(struct ps5vk_layout_state *s)
{
    if(!s || s->count>PS5VK_LAYOUT_IMAGES)return VK_ERROR_UNKNOWN;
    for(unsigned i=0;i<s->count;++i)
        if(!s->entries[i].image ||
           committed(s->entries[i].image,s->entries[i].aspect)!=s->entries[i].initial)
            return VK_ERROR_UNKNOWN;
    for(unsigned i=0;i<s->count;++i) {
        VkImage image=s->entries[i].image;
        if(s->entries[i].aspect==VK_IMAGE_ASPECT_STENCIL_BIT) {
            image->stencil_layout=s->entries[i].current;
            continue;
        }
        /* Native transactions describe whole images. Keep the committed
         * subresource table coherent even when this role normally routes its
         * barriers through the frontend executor. */
        if(image->subresource_layouts) {
            size_t count=(size_t)image->info.mipLevels*image->info.arrayLayers;
            for(size_t j=0;j<count;++j)
                image->subresource_layouts[j]=s->entries[i].current;
        }
        image->layout=s->entries[i].current;
    }
    s->count=0;
    return VK_SUCCESS;
}
