#include "image_layout_state.h"
#include <assert.h>
#include <string.h>
void test_depth_stencil_aspects(void);
int main(void)
{
    struct VkImage_T image={0},other={0};struct ps5vk_layout_state s={0};
    assert(ps5vk_layout_require(&s,&image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)!=VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED && s.count==1);
    assert(ps5vk_layout_require(&s,&image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)!=VK_SUCCESS);
    assert(s.count==1 && s.entries[0].current==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&other,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL)==VK_SUCCESS);
    /* Preparation rollback consists of discarding this local transaction. */
    struct ps5vk_layout_state discard=s;(void)discard;
    assert(image.layout==VK_IMAGE_LAYOUT_UNDEFINED && other.layout==VK_IMAGE_LAYOUT_UNDEFINED);
    other.layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    assert(ps5vk_layout_commit(&s)!=VK_SUCCESS && image.layout==VK_IMAGE_LAYOUT_UNDEFINED && s.count==2);
    other.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_layout_commit(&s)==VK_SUCCESS && !s.count);
    assert(image.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && other.layout==VK_IMAGE_LAYOUT_GENERAL);
    assert(ps5vk_layout_require(&s,&image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_UNDEFINED)!=VK_SUCCESS);
    struct VkImage_T many[PS5VK_LAYOUT_IMAGES+1]={0};
    for(unsigned i=0;i<PS5VK_LAYOUT_IMAGES;++i)
        assert(ps5vk_layout_transition(&s,&many[i],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&many[PS5VK_LAYOUT_IMAGES],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL)!=VK_SUCCESS);
    assert(s.count==PS5VK_LAYOUT_IMAGES);
    /* A discard transition remains valid even when a prior layout exists. */
    assert(ps5vk_layout_transition(&s,&many[0],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    /* Render-pass target layouts are pending until completion, just like the
     * texture upload transaction. Failed/stale commits change neither target. */
    struct VkImage_T color={0},depth={.info={.format=VK_FORMAT_D32_SFLOAT}};struct ps5vk_layout_state targets={0};
    assert(ps5vk_layout_transition(&targets,&color,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&targets,&depth,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(color.layout==VK_IMAGE_LAYOUT_UNDEFINED && depth.layout==VK_IMAGE_LAYOUT_UNDEFINED);
    assert(ps5vk_layout_require(&targets,&color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_require(&targets,&depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    depth.layout=VK_IMAGE_LAYOUT_GENERAL;
    assert(ps5vk_layout_commit(&targets)!=VK_SUCCESS && color.layout==VK_IMAGE_LAYOUT_UNDEFINED);
    depth.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_layout_commit(&targets)==VK_SUCCESS);
    assert(color.layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && depth.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    assert(ps5vk_layout_transition(&targets,&color,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&targets)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&targets,&color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)!=VK_SUCCESS);
    /* Whole-image native commits keep optional subresource state coherent. */
    VkImageLayout cells[6]={0};
    struct VkImage_T layered={.info={.mipLevels=2,.arrayLayers=3},.subresource_layouts=cells};
    struct ps5vk_layout_state upload={0};
    assert(ps5vk_layout_transition(&upload,&layered,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&upload)==VK_SUCCESS);
    for(unsigned i=0;i<6;++i)assert(cells[i]==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange selected={VK_IMAGE_ASPECT_COLOR_BIT,1,1,2,1};
    assert(ps5vk_image_layout_transition(&layered,&selected,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
    assert(ps5vk_layout_require(&upload,&layered,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)!=VK_SUCCESS);
    assert(ps5vk_layout_transition(&upload,&layered,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&upload)==VK_SUCCESS);
    for(unsigned i=0;i<6;++i)assert(cells[i]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    test_depth_stencil_aspects();
}

/* Per-aspect state of a combined depth/stencil image
 * (VK_KHR_separate_depth_stencil_layouts): each aspect is its own entry, a
 * barrier naming one aspect never moves the other, layouts are matched per
 * aspect, and every refused call leaves the transaction byte-for-byte as it
 * was. */
enum {
    D=VK_IMAGE_ASPECT_DEPTH_BIT, S=VK_IMAGE_ASPECT_STENCIL_BIT,
    DS=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,
};
#define DSA VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
#define DSR VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
#define DA VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
#define DR VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
#define SA VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL
#define SR VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL
#define DR_SA VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
#define DA_SR VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL
#define SRC VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
#define DST VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
#define UNDEF VK_IMAGE_LAYOUT_UNDEFINED

static VkImageLayout current(const struct ps5vk_layout_state *s,VkImage image,VkImageAspectFlags aspect)
{
    VkImageLayout layout=VK_IMAGE_LAYOUT_MAX_ENUM;
    assert(ps5vk_layout_current(s,image,aspect,&layout)==VK_SUCCESS);
    return layout;
}

/* A refused call must not change a single byte of the transaction. */
static void refused(struct ps5vk_layout_state *s,VkImage image,VkImageAspectFlags aspects,
    VkImageLayout old,VkImageLayout next)
{
    struct ps5vk_layout_state before=*s;
    assert(ps5vk_layout_transition_aspects(s,image,aspects,old,next)!=VK_SUCCESS);
    assert(!memcmp(&before,s,sizeof(before)));
}

static void projection(void)
{
    VkImageLayout p;
    /* The combined and mixed layouts project onto each aspect. */
    assert(ps5vk_layout_for_aspect(DSA,D,&p) && p==DA);
    assert(ps5vk_layout_for_aspect(DSA,S,&p) && p==SA);
    assert(ps5vk_layout_for_aspect(DSR,D,&p) && p==DR);
    assert(ps5vk_layout_for_aspect(DSR,S,&p) && p==SR);
    assert(ps5vk_layout_for_aspect(DR_SA,D,&p) && p==DR);
    assert(ps5vk_layout_for_aspect(DR_SA,S,&p) && p==SA);
    assert(ps5vk_layout_for_aspect(DA_SR,D,&p) && p==DA);
    assert(ps5vk_layout_for_aspect(DA_SR,S,&p) && p==SR);
    /* A separate layout names its own aspect only. */
    assert(ps5vk_layout_for_aspect(DA,D,&p) && p==DA);
    assert(!ps5vk_layout_for_aspect(DA,S,&p) && !ps5vk_layout_for_aspect(DR,S,&p));
    assert(!ps5vk_layout_for_aspect(SA,D,&p) && !ps5vk_layout_for_aspect(SR,D,&p));
    /* Colour aspects never take a depth/stencil layout and vice versa. */
    assert(!ps5vk_layout_for_aspect(DSA,VK_IMAGE_ASPECT_COLOR_BIT,&p));
    assert(!ps5vk_layout_for_aspect(DA,VK_IMAGE_ASPECT_COLOR_BIT,&p));
    assert(!ps5vk_layout_for_aspect(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,D,&p));
    assert(!ps5vk_layout_for_aspect(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,S,&p));
    /* Aspect-neutral layouts are themselves for every aspect. */
    assert(ps5vk_layout_for_aspect(SRC,S,&p) && p==SRC);
    assert(ps5vk_layout_for_aspect(VK_IMAGE_LAYOUT_GENERAL,D,&p) && p==VK_IMAGE_LAYOUT_GENERAL);
    /* The aspect argument is exactly one bit. */
    assert(!ps5vk_layout_for_aspect(DSA,DS,&p) && !ps5vk_layout_for_aspect(DSA,0,&p));
    /* The matching rule. */
    assert(ps5vk_layouts_match_for_aspect(DSA,DA,D) && ps5vk_layouts_match_for_aspect(DSA,DA_SR,D));
    assert(ps5vk_layouts_match_for_aspect(DSA,DR_SA,S) && !ps5vk_layouts_match_for_aspect(DSA,DR_SA,D));
    assert(!ps5vk_layouts_match_for_aspect(DSA,DSR,S) && !ps5vk_layouts_match_for_aspect(DA,SA,S));
    /* Format aspects. */
    assert(ps5vk_format_aspects(VK_FORMAT_D32_SFLOAT_S8_UINT)==DS);
    assert(ps5vk_format_aspects(VK_FORMAT_D24_UNORM_S8_UINT)==DS);
    assert(ps5vk_format_aspects(VK_FORMAT_D32_SFLOAT)==D);
    assert(ps5vk_format_aspects(VK_FORMAT_S8_UINT)==S);
    assert(ps5vk_format_aspects(VK_FORMAT_R8G8B8A8_UNORM)==VK_IMAGE_ASPECT_COLOR_BIT);
    struct ps5vk_depth_stencil_layouts pair;
    assert(ps5vk_depth_stencil_layouts_from(DSA,DS,&pair) && pair.depth==DSA && pair.stencil==DSA);
    assert(ps5vk_depth_stencil_layouts_from(DA,D,&pair) && pair.depth==DA && pair.stencil==UNDEF);
    assert(!ps5vk_depth_stencil_layouts_from(DA,DS,&pair));
    assert(ps5vk_layout_aspect_attachment_writable(DR_SA,S) &&
           !ps5vk_layout_aspect_attachment_writable(DR_SA,D));
    assert(!ps5vk_layout_aspect_attachment_writable(SRC,D));
}

void test_depth_stencil_aspects(void)
{
    projection();
    struct VkImage_T ds={.info={.format=VK_FORMAT_D32_SFLOAT_S8_UINT}};
    struct VkImage_T depth={.info={.format=VK_FORMAT_D32_SFLOAT}};
    struct VkImage_T colour={.info={.format=VK_FORMAT_R8G8B8A8_UNORM}};
    struct ps5vk_layout_state s={0};

    /* A whole-image transition moves both aspects and takes two entries. */
    assert(ps5vk_layout_transition(&s,&ds,UNDEF,DSA)==VK_SUCCESS);
    assert(s.count==2 && current(&s,&ds,D)==DSA && current(&s,&ds,S)==DSA);
    assert(ps5vk_layout_require(&s,&ds,DSA)==VK_SUCCESS);
    /* The committed image is untouched until completion is proven. */
    assert(ds.layout==UNDEF && ds.stencil_layout==UNDEF);

    /* Depth-only transition: the separate layout matches the combined one for
     * the depth aspect, and the stencil aspect stays where it was. */
    assert(ps5vk_layout_transition_aspects(&s,&ds,D,DA,SRC)==VK_SUCCESS);
    assert(current(&s,&ds,D)==SRC && current(&s,&ds,S)==DSA);
    assert(ps5vk_layout_require_aspects(&s,&ds,S,SA)==VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&s,&ds,S,DSA)==VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&s,&ds,D,SRC)==VK_SUCCESS);
    assert(ps5vk_layout_require(&s,&ds,DSA)!=VK_SUCCESS);
    assert(ps5vk_layout_require(&s,&ds,SRC)!=VK_SUCCESS);

    /* Stencil-only: the depth aspect keeps TRANSFER_SRC. */
    assert(ps5vk_layout_transition_aspects(&s,&ds,S,SA,SRC)==VK_SUCCESS);
    assert(current(&s,&ds,D)==SRC && current(&s,&ds,S)==SRC);
    assert(ps5vk_layout_require(&s,&ds,SRC)==VK_SUCCESS);

    /* Reverse order: stencil back first, then depth, each alone. */
    assert(ps5vk_layout_transition_aspects(&s,&ds,S,SRC,SA)==VK_SUCCESS);
    assert(current(&s,&ds,D)==SRC && current(&s,&ds,S)==SA);
    assert(ps5vk_layout_transition_aspects(&s,&ds,D,SRC,DA)==VK_SUCCESS);
    assert(current(&s,&ds,D)==DA && current(&s,&ds,S)==SA);
    /* The two separate layouts together are the combined attachment layout. */
    assert(ps5vk_layout_require(&s,&ds,DSA)==VK_SUCCESS);
    assert(s.count==2);

    /* Wrong oldLayout, per aspect, is refused and changes nothing. */
    refused(&s,&ds,D,SRC,DR);
    refused(&s,&ds,S,SR,SRC);
    refused(&s,&ds,D,DR,SRC);                 /* DR does not match DA */
    /* A whole-image barrier whose old layout matches depth but not stencil is
     * refused for both: no partial update of the depth aspect. */
    assert(ps5vk_layout_transition_aspects(&s,&ds,S,SA,SR)==VK_SUCCESS);
    refused(&s,&ds,DS,DSA,SRC);
    assert(current(&s,&ds,D)==DA && current(&s,&ds,S)==SR);
    /* ...and the mixed layout that does describe it is accepted. */
    assert(ps5vk_layout_transition_aspects(&s,&ds,DS,DA_SR,DSR)==VK_SUCCESS);
    assert(current(&s,&ds,D)==DSR && current(&s,&ds,S)==DSR);
    assert(ps5vk_layout_require_aspects(&s,&ds,D,DR)==VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&s,&ds,S,SR)==VK_SUCCESS);
    assert(ps5vk_layout_transition_aspects(&s,&ds,DS,DSR,DR_SA)==VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&s,&ds,D,DR)==VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&s,&ds,S,SA)==VK_SUCCESS);

    /* A layout that names the other aspect, or no aspect at all, is refused. */
    refused(&s,&ds,S,UNDEF,DA);
    refused(&s,&ds,D,UNDEF,SR);
    refused(&s,&ds,DS,UNDEF,DA);
    refused(&s,&ds,S,DR,SRC);
    refused(&s,&ds,DS,UNDEF,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    refused(&s,&ds,DS,UNDEF,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    refused(&s,&ds,DS,UNDEF,UNDEF);
    /* Aspect masks the format does not carry, or none. */
    refused(&s,&ds,0,UNDEF,SRC);
    refused(&s,&ds,VK_IMAGE_ASPECT_COLOR_BIT,UNDEF,SRC);
    refused(&s,&ds,DS|VK_IMAGE_ASPECT_COLOR_BIT,UNDEF,SRC);
    refused(&s,&depth,S,UNDEF,SRC);
    refused(&s,&depth,DS,UNDEF,SRC);
    refused(&s,&colour,D,UNDEF,SRC);
    /* A depth-only image and a colour image keep their one-aspect meaning. */
    refused(&s,&colour,VK_IMAGE_ASPECT_COLOR_BIT,UNDEF,DSA);
    refused(&s,&colour,VK_IMAGE_ASPECT_COLOR_BIT,UNDEF,DA);
    assert(ps5vk_layout_transition_aspects(&s,&depth,D,UNDEF,DA)==VK_SUCCESS);
    /* DEPTH_ATTACHMENT and DEPTH_STENCIL_ATTACHMENT are one state for depth. */
    assert(ps5vk_layout_require(&s,&depth,DSA)==VK_SUCCESS);
    assert(ps5vk_layout_transition(&s,&depth,DSA,SRC)==VK_SUCCESS);
    VkImageLayout ignored;
    assert(ps5vk_layout_current(&s,&depth,S,&ignored)!=VK_SUCCESS);
    assert(ps5vk_layout_current(&s,&ds,DS,&ignored)!=VK_SUCCESS);

    /* Commit is per aspect and all-or-nothing: a stale stencil snapshot
     * refuses the whole commit, including the depth half and the other
     * images. */
    ds.stencil_layout=VK_IMAGE_LAYOUT_GENERAL;
    struct ps5vk_layout_state before=s;
    assert(ps5vk_layout_commit(&s)!=VK_SUCCESS);
    assert(!memcmp(&before,&s,sizeof(s)));
    assert(ds.layout==UNDEF && depth.layout==UNDEF);
    ds.stencil_layout=UNDEF;
    assert(ps5vk_layout_commit(&s)==VK_SUCCESS && !s.count);
    assert(ds.layout==DR_SA && ds.stencil_layout==DR_SA);
    assert(depth.layout==SRC && depth.stencil_layout==UNDEF);

    /* After commit the committed per-aspect state drives matching. */
    assert(ps5vk_layout_transition_aspects(&s,&ds,S,SA,SRC)==VK_SUCCESS);
    assert(ps5vk_layout_commit(&s)==VK_SUCCESS);
    assert(ds.layout==DR_SA && ds.stencil_layout==SRC);
    assert(ps5vk_layout_require_aspects(&s,&ds,D,DR)==VK_SUCCESS);
    assert(ps5vk_layout_require_aspects(&s,&ds,S,SRC)==VK_SUCCESS);
    assert(ps5vk_layout_require(&s,&ds,SRC)!=VK_SUCCESS);

    /* Mid-submission failure: preparation works on a copy and only a fully
     * prepared submission adopts it, so an operation refused after two good
     * ones leaves neither the transaction nor the committed image changed. */
    struct ps5vk_layout_state prepared=s,staged=s;
    assert(ps5vk_layout_transition_aspects(&staged,&ds,D,DR,SRC)==VK_SUCCESS);
    assert(ps5vk_layout_transition_aspects(&staged,&ds,S,SRC,SA)==VK_SUCCESS);
    assert(ps5vk_layout_transition_aspects(&staged,&ds,S,SR,SRC)!=VK_SUCCESS);
    assert(!memcmp(&prepared,&s,sizeof(s)));
    assert(ds.layout==DR_SA && ds.stencil_layout==SRC);

    /* Capacity counts entries, not images: a combined whole-image transition
     * that needs two free entries when only one is left takes neither. */
    struct ps5vk_layout_state full={0};
    static struct VkImage_T filler[PS5VK_LAYOUT_IMAGES];
    for(unsigned i=0;i<PS5VK_LAYOUT_IMAGES-1u;++i)
        assert(ps5vk_layout_transition(&full,&filler[i],UNDEF,VK_IMAGE_LAYOUT_GENERAL)==VK_SUCCESS);
    struct VkImage_T fresh={.info={.format=VK_FORMAT_D32_SFLOAT_S8_UINT}};
    before=full;
    assert(ps5vk_layout_transition(&full,&fresh,UNDEF,DSA)==VK_ERROR_TOO_MANY_OBJECTS);
    assert(!memcmp(&before,&full,sizeof(full)));
    assert(ps5vk_layout_transition_aspects(&full,&fresh,S,UNDEF,SA)==VK_SUCCESS);
    assert(full.count==PS5VK_LAYOUT_IMAGES);
    assert(ps5vk_layout_transition_aspects(&full,&fresh,S,SA,SRC)==VK_SUCCESS);
    assert(ps5vk_layout_transition_aspects(&full,&fresh,D,UNDEF,DA)==VK_ERROR_TOO_MANY_OBJECTS);
}
