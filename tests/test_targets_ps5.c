#include "targets_ps5.h"
#include "presentation_format_ps5.h"
#include "graphics_limits.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uintptr_t base = UINT64_C(0x100020000);
static VkDeviceSize span_bytes = 131072;
VkResult ps5vk_image_span(VkDevice d, VkImage image, void **address, VkDeviceSize *bytes)
{ (void)d; (void)image; *address=(void *)base; *bytes=span_bytes; return VK_SUCCESS; }
int main(void)
{
    assert(ps5vk_native_video_format(VK_FORMAT_B8G8R8A8_UNORM)==UINT64_C(0x8000000000000000));
    assert(ps5vk_native_video_format(VK_FORMAT_B8G8R8A8_UNORM)!=UINT64_C(0x8000000022000000));
    assert(!ps5vk_native_video_format(VK_FORMAT_R8G8B8A8_UNORM));
    assert(!ps5vk_native_video_format(VK_FORMAT_D32_SFLOAT));
    assert(!ps5vk_native_video_format(VK_FORMAT_UNDEFINED));
    struct VkDevice_T device = {0};
    struct VkImage_T image = {.info={.imageType=VK_IMAGE_TYPE_2D, .format=VK_FORMAT_D32_SFLOAT,
        .extent={32,32,1}, .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL, .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}};
    struct VkImageView_T view = {.device=&device, .image=&image, .format=VK_FORMAT_D32_SFLOAT,
        .range={VK_IMAGE_ASPECT_DEPTH_BIT, 0,1,0,1}};
    struct ps5vk_target_registers target;
    assert(ps5vk_native_target(&device, &view, NULL, &target) == VK_SUCCESS && target.count == 22);
    assert(target.registers[7].offset == 0x12 && target.registers[7].value == (base >> 8));
    base += 64;
    assert(ps5vk_native_target(&device, &view, NULL, &target) == VK_ERROR_UNKNOWN && !target.count);
    base -= 64; view.format=image.info.format=VK_FORMAT_R8G8B8A8_UNORM;
    assert(ps5vk_native_target(&device, &view, NULL, &target) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    const uint32_t offsets[16]={0x318,0x31b,0x31c,0x31d,0x31e,0x31f,0x321,0x323,
        0x324,0x325,0x390,0x398,0x3a0,0x3a8,0x3b0,0x3b8};
    ps5_agc_register defaults[16];
    for(unsigned i=0;i<16;++i)defaults[i]=(ps5_agc_register){offsets[i],0};
    view.format=image.info.format=VK_FORMAT_B8G8R8A8_UNORM;
    image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;view.range.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_SUCCESS);
    assert(target.registers[2].offset==0x31c && (target.registers[2].value & 0x1800)==0x0800);

    /* --- multisampled colour target (DXVK262-T06) -----------------------
     * The sample geometry rides in CB_COLOR0_ATTRIB (context offset 0x31d) as
     * the log2 fields the shared builder clears, and the storage each target
     * addresses scales with the count: 1x keeps the builder's zeroes and the
     * old footprint, 2x and 4x state log2 of the count in NUM_SAMPLES and
     * NUM_FRAGMENTS and are sized with that many sample planes. The counts
     * exist only on a platform that carries the feature bit, and the depth
     * role has no multisampled target at all. */
    {
        const VkDeviceSize saved_span = span_bytes;
        const VkSampleCountFlagBits saved_samples = image.info.samples;
        VkMemoryRequirements single_requirements, sampled_requirements;
        assert(ps5vk_native_image_requirements(&device,&image.info,&single_requirements)==VK_SUCCESS);
        assert(single_requirements.size==131072 && (target.registers[3].value &
            PS5VK_COLOR_ATTRIB_SAMPLE_FIELDS_MASK)==0);
        image.info.samples = VK_SAMPLE_COUNT_4_BIT;
        assert(ps5vk_native_image_requirements(&device,&image.info,&sampled_requirements)==
            VK_ERROR_FORMAT_NOT_SUPPORTED);
        assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_ERROR_FORMAT_NOT_SUPPORTED);
        device.platform_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        span_bytes = 4 * 131072;
        assert(ps5vk_native_image_requirements(&device,&image.info,&sampled_requirements)==VK_SUCCESS);
        assert(sampled_requirements.size==262144 &&
               sampled_requirements.alignment==single_requirements.alignment);
        assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_SUCCESS);
        assert(target.count==16u && target.registers[3].offset==0x31d);
        assert((target.registers[3].value & PS5VK_COLOR_ATTRIB_SAMPLE_FIELDS_MASK)==
               ps5vk_color_attrib_sample_fields(VK_SAMPLE_COUNT_4_BIT));
        image.info.samples = VK_SAMPLE_COUNT_2_BIT;
        assert(ps5vk_native_image_requirements(&device,&image.info,&sampled_requirements)==VK_SUCCESS);
        assert(sampled_requirements.size==131072);
        assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_SUCCESS);
        assert((target.registers[3].value & PS5VK_COLOR_ATTRIB_SAMPLE_FIELDS_MASK)==
               ps5vk_color_attrib_sample_fields(VK_SAMPLE_COUNT_2_BIT));
        /* The role combinations the pinned multisample oracle builds are backed
         * by the same storage equation: the colour attachment with its readback
         * source, and the per-sample fetch form that adds the input role. A
         * role the oracle never asks for keeps the refusal. */
        image.info.samples = VK_SAMPLE_COUNT_4_BIT;
        image.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        assert(ps5vk_native_image_requirements(&device,&image.info,&sampled_requirements)==VK_SUCCESS);
        assert(sampled_requirements.size==262144);
        assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_SUCCESS);
        image.info.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        assert(ps5vk_native_image_requirements(&device,&image.info,&sampled_requirements)==VK_SUCCESS);
        assert(sampled_requirements.size==262144);
        assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_SUCCESS);
        assert((target.registers[3].value & PS5VK_COLOR_ATTRIB_SAMPLE_FIELDS_MASK)==
               ps5vk_color_attrib_sample_fields(VK_SAMPLE_COUNT_4_BIT));
        image.info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        assert(ps5vk_native_image_requirements(&device,&image.info,&sampled_requirements)==
            VK_ERROR_FORMAT_NOT_SUPPORTED);
        image.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        /* 8x is outside the envelope this profile is built for. */
        image.info.samples = VK_SAMPLE_COUNT_8_BIT;
        assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_ERROR_FORMAT_NOT_SUPPORTED);
        /* A multisampled depth surface has no target on this path: the
         * requirements path refuses the storage first, which is the refusal
         * the target reports. */
        image.info.samples = VK_SAMPLE_COUNT_4_BIT;
        image.info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        view.format = image.info.format = VK_FORMAT_D32_SFLOAT;
        view.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        assert(ps5vk_native_target(&device,&view,NULL,&target)==VK_ERROR_FORMAT_NOT_SUPPORTED);
        view.format = image.info.format = VK_FORMAT_B8G8R8A8_UNORM;
        view.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image.info.samples = saved_samples;
        span_bytes = saved_span;
        device.platform_features &= ~(uint32_t)PS5VK_FEATURE_SAMPLE_RATE_SHADING;
        /* Rebuild the single-sample reference target the layer section below
         * compares against: every refusal above zeroed it. */
        assert(ps5vk_native_target(&device,&view,defaults,&target)==VK_SUCCESS);
    }

    /* --- slice A measurement: is a chosen layer addressable at all? --------
     * The multiview question is whether the pinned target path can be pointed
     * at one layer of a multi-layer surface. These are the host half of that
     * measurement: the per-layer footprint, the fail-closed edges, and the
     * register arithmetic that must hold for base + layer * footprint to mean
     * "that layer" - with the depth and color address encodings compared so a
     * mixed surface cannot use one stride for both. */
    span_bytes = 4 * 131072;
    image.info.arrayLayers = 4;
    VkDeviceSize footprint = 0;
    assert(ps5vk_native_layer_footprint(&device, &image, &footprint) == VK_SUCCESS);
    assert(footprint == 131072);                    /* 128 KiB-aligned color layer */
    view.range.baseArrayLayer = 0;
    struct ps5vk_target_registers layer0, layer1;
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer0) == VK_SUCCESS);
    assert(layer0.count == target.count && !memcmp(layer0.registers, target.registers,
        target.count * sizeof(target.registers[0])));
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) == VK_SUCCESS);
    /* Exactly the address-carrying registers move, and by exactly one layer:
     * the color builder writes address>>8 at register 0 and (address>>40)&0xff
     * at register 2 alongside its swap bits. */
    {
        unsigned differences = 0;
        for (unsigned i = 0; i < target.count; ++i) {
            if (layer0.registers[i].value == layer1.registers[i].value) continue;
            ++differences;
            const uint32_t delta = layer1.registers[i].value - layer0.registers[i].value;
            assert(layer0.registers[i].offset == layer1.registers[i].offset);
            assert(delta == (uint32_t)(footprint >> 8));   /* no 40-bit carry here */
        }
        assert(differences == 1);
        assert(layer0.registers[0].offset == layer1.registers[0].offset);
    }
    /* A layer outside the surface, an unaligned layer stride and a surface that
     * cannot hold the layer all stay refused before registers exist. */
    assert(ps5vk_native_layer_target(&device, &view, 4, defaults, &layer1) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !layer1.count);
    span_bytes = 131072 + 64;                       /* one layer plus a fragment */
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) ==
           VK_ERROR_UNKNOWN && !layer1.count);
    span_bytes = 4 * 131072;
    base += 8;                                      /* breaks the 128 KiB rule */
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) ==
           VK_ERROR_UNKNOWN && !layer1.count);
    base -= 8;
    /* The depth layer uses the same base-address mechanism with a 64 KiB
     * alignment and the D32 layout's footprint, so a mixed color+depth surface
     * must take the stricter alignment and its own per-role stride. */
    image.info.format = view.format = VK_FORMAT_D32_SFLOAT;
    image.info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    view.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    const uintptr_t color_base = base;
    assert(ps5vk_native_layer_footprint(&device, &image, &footprint) == VK_SUCCESS &&
           footprint && !(footprint % 65536));
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer0) == VK_SUCCESS);
    assert(ps5vk_native_layer_target(&device, &view, 1, defaults, &layer1) == VK_SUCCESS);
    assert(layer0.count == 22 && layer1.count == layer0.count);
    assert(layer1.registers[7].offset == 0x12 &&
           layer1.registers[7].value == layer0.registers[7].value + (uint32_t)(footprint >> 8));
    base = color_base + 65536;                      /* aligned for depth, not color */
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer1) == VK_SUCCESS);
    image.info.format = view.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    view.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    assert(ps5vk_native_layer_target(&device, &view, 0, defaults, &layer1) != VK_SUCCESS);
    base = color_base;

    /* --- layered attachment storage (T02-C3a) -----------------------------
     * The measurement above established that a layer is addressable, so the
     * storage model has to describe one: a per-layer stride that is the same
     * footprint, an alignment the target builders accept, a total size that is
     * the stride times the layer count with an explicit overflow check, and
     * arrayLayers == 1 unchanged from the single-layer requirements. */
    {
        VkDeviceSize stride = 0, alignment = 0, bytes = 0, rgba_stride = 0;
        assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, 64u, 64u, 3u,
            &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(alignment == 131072u && !(stride % alignment) && bytes == 3u * stride);
        rgba_stride = stride;
        assert(ps5vk_native_layered_storage(VK_FORMAT_B8G8R8A8_UNORM, 64u, 64u, 2u,
            &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(bytes == 2u * stride && alignment == 131072u);
        /* Depth uses the 64KB_Z_X layout alignment, which is why a mixed
         * surface has to take the stricter color alignment for its own
         * layers. */
        assert(ps5vk_native_layered_storage(VK_FORMAT_D32_SFLOAT, 64u, 64u, 2u,
            &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(!(alignment % 65536u) && !(stride % alignment) && bytes == 2u * stride);
        /* One layer reproduces the previous single-layer requirements. */
        assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, 64u, 64u, 1u,
            &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(bytes == stride && stride == rgba_stride && alignment == 131072u);
        /* Unsupported formats, an empty layer count and an overflowing layer
         * count are refused rather than described. */
        assert(ps5vk_native_layered_storage(VK_FORMAT_R8_UNORM, 64u, 64u, 2u,
            &stride, &alignment, &bytes) == VK_ERROR_FORMAT_NOT_SUPPORTED);
        assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, 64u, 64u, 0u,
            &stride, &alignment, &bytes) == VK_ERROR_UNKNOWN);
        assert(ps5vk_native_layered_storage(VK_FORMAT_B8G8R8A8_UNORM,
            PS5VK_MAX_COLOR_DIMENSION + 1u, 64u, 2u, &stride, &alignment, &bytes) ==
            VK_ERROR_FORMAT_NOT_SUPPORTED);
        /* A layer count far beyond the image limit still has to produce the
         * product rather than a wrapped size, and the first count whose product
         * does not fit is refused. */
        assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, 64u, 64u,
            UINT32_MAX, &stride, &alignment, &bytes) == VK_SUCCESS);
        assert(bytes == (VkDeviceSize)rgba_stride * UINT32_MAX);
        assert(ps5vk_native_layered_storage(VK_FORMAT_R8G8B8A8_UNORM, 64u, 64u,
            UINT64_MAX, &stride, &alignment, &bytes) == VK_ERROR_OUT_OF_HOST_MEMORY);
        assert(!stride && !alignment && !bytes);
    }
    /* --- multiview view expansion (T02-C3b) -------------------------------
     * A subpass view mask names the views its draws render. Expanding it has to
     * produce exactly those views in ascending order - a view's layer and the
     * ViewIndex its vertex stage reads have to agree - with multiview disabled
     * staying exactly one view, and it has to refuse before a caller can emit
     * anything at all. */
    {
        uint32_t views[5]={0xdeadbeefu,0xdeadbeefu,0xdeadbeefu,0xdeadbeefu,0xdeadbeefu};
        uint32_t count=0;
        assert(ps5vk_native_view_expand(0u,views,5u,&count)==VK_SUCCESS && count==1u &&
               views[0]==0u && views[1]==0xdeadbeefu);
        assert(ps5vk_native_view_expand(0b111u,views,5u,&count)==VK_SUCCESS && count==3u &&
               views[0]==0u && views[1]==1u && views[2]==2u);
        /* Sparse, and the highest bit a mask can name. */
        assert(ps5vk_native_view_expand(0b101u,views,5u,&count)==VK_SUCCESS && count==2u &&
               views[0]==0u && views[1]==2u);
        assert(ps5vk_native_view_expand(0x80000001u,views,5u,&count)==VK_SUCCESS && count==2u &&
               views[0]==0u && views[1]==31u);
        /* A set the caller cannot hold is refused before anything is written:
         * neither the array nor the count moves. */
        views[0]=0u;views[1]=31u;count=2u;
        assert(ps5vk_native_view_expand(0b101u,views,1u,&count)!=VK_SUCCESS &&
               count==2u && views[0]==0u && views[1]==31u);
        assert(ps5vk_native_view_expand(0u,NULL,5u,&count)==VK_ERROR_UNKNOWN);
        assert(ps5vk_native_view_expand(0u,views,0u,&count)==VK_ERROR_UNKNOWN);
    }
    /* Which layer a view renders into: the view's own range has to carry the
     * view (the multiview obligation an attachment view is created with), and
     * the backing has to have that layer. Both roles are checked against their
     * own view, and the base target the pass is prepared with stays layer zero
     * of the image. */
    {
        base=UINT64_C(0x100020000);span_bytes=4*131072;
        image.info.arrayLayers=4;
        image.info.format=view.format=VK_FORMAT_B8G8R8A8_UNORM;
        image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        view.range=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDeviceSize color_footprint=0;
        assert(ps5vk_native_layer_footprint(&device,&image,&color_footprint)==VK_SUCCESS);
        struct ps5vk_target_registers single,att0,att2,att3;
        assert(ps5vk_native_view_layer_target(&device,&view,0u,defaults,&single)==VK_SUCCESS);
        assert(ps5vk_native_view_layer_target(&device,&view,0u,defaults,&att0)==VK_SUCCESS &&
               !memcmp(&att0,&single,sizeof(att0)));
        /* One layer is one view: a second view is outside this view's range. */
        assert(ps5vk_native_view_layer_target(&device,&view,1u,defaults,&att2)==
               VK_ERROR_FEATURE_NOT_PRESENT && !att2.count);
        /* The multiview attachment shape: base layer zero and one layer per
         * view. It prepares as the single-layer view always did, because the
         * base target is layer zero either way, and each view is selected by
         * the same per-layer arithmetic. */
        view.range.layerCount=4;
        assert(ps5vk_native_target(&device,&view,defaults,&att0)==VK_SUCCESS);
        struct ps5vk_target_registers base_target=att0;
        assert(ps5vk_native_view_layer_target(&device,&view,0u,defaults,&att0)==VK_SUCCESS);
        assert(ps5vk_native_view_layer_target(&device,&view,2u,defaults,&att2)==VK_SUCCESS);
        assert(ps5vk_native_view_layer_target(&device,&view,3u,defaults,&att3)==VK_SUCCESS);
        assert(!memcmp(&att0,&base_target,sizeof(att0)));      /* view zero is the prepared target */
        assert(att2.registers[0].offset==att0.registers[0].offset &&
               att3.registers[0].offset==att0.registers[0].offset);
        assert(att2.registers[0].value==att0.registers[0].value+2u*(uint32_t)(color_footprint>>8));
        assert(att3.registers[0].value==att0.registers[0].value+3u*(uint32_t)(color_footprint>>8));
        /* Outside the view's range, and inside the range but not the backing. */
        assert(ps5vk_native_view_layer_target(&device,&view,4u,defaults,&att3)==
               VK_ERROR_FEATURE_NOT_PRESENT && !att3.count);
        image.info.arrayLayers=3;
        assert(ps5vk_native_view_layer_target(&device,&view,3u,defaults,&att3)!=VK_SUCCESS &&
               !att3.count);
        image.info.arrayLayers=4;
        /* The depth role carries the same obligation from its own view, with
         * its own footprint and alignment. */
        image.info.format=view.format=VK_FORMAT_D32_SFLOAT;
        image.info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        view.range=(VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,2};
        image.info.arrayLayers=2;
        struct ps5vk_target_registers depth0,depth1;
        assert(ps5vk_native_view_layer_target(&device,&view,0u,NULL,&depth0)==VK_SUCCESS &&
               depth0.count==22);
        assert(ps5vk_native_view_layer_target(&device,&view,1u,NULL,&depth1)==VK_SUCCESS);
        assert(depth1.registers[7].offset==0x12 &&
               depth1.registers[7].value>depth0.registers[7].value);
        assert(ps5vk_native_view_layer_target(&device,&view,2u,NULL,&depth1)==
               VK_ERROR_FEATURE_NOT_PRESENT && !depth1.count);
        view.range.layerCount=3;                     /* range says three, backing has two */
        assert(ps5vk_native_view_layer_target(&device,&view,2u,NULL,&depth1)!=VK_SUCCESS &&
               !depth1.count);
        /* A view that does not start at layer zero still has no whole-image
         * target, so it cannot be prepared as one. */
        view.range.baseArrayLayer=1;
        assert(ps5vk_native_target(&device,&view,NULL,&depth1)==VK_ERROR_FEATURE_NOT_PRESENT &&
               !depth1.count);
    }
    /* --- the target shapes the view rule is written against (T02-C3b) -----
     * The emission allows a view to move only the words that carry an
     * attachment address, which is a statement about what the pinned builders
     * produce. Pin it here against targets the real builders make: the exact
     * offset list per role, the two carriers per role, and the fact that moving
     * a layer changes nothing else. A builder that changes shape fails here
     * instead of quietly widening (or breaking) the emission's rule. */
    {
        base=UINT64_C(0x100020000);span_bytes=4*131072;
        image.info.arrayLayers=4;
        image.info.format=view.format=VK_FORMAT_B8G8R8A8_UNORM;
        image.info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        view.range=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,4};
        struct ps5vk_target_registers color_base,color_layer;
        assert(ps5vk_native_target(&device,&view,defaults,&color_base)==VK_SUCCESS &&
               color_base.count==PS5_COLOR_REGISTER_COUNT);
        assert(ps5vk_native_view_layer_target(&device,&view,3u,defaults,&color_layer)==VK_SUCCESS);
        for(unsigned i=0;i<color_base.count;++i)
            assert(color_base.registers[i].offset==ps5vk_color_target_offsets[i] &&
                   color_layer.registers[i].offset==ps5vk_color_target_offsets[i]);
        unsigned color_moves=0;
        for(unsigned i=0;i<color_base.count;++i) {
            if(color_base.registers[i].value==color_layer.registers[i].value)continue;
            ++color_moves;
            assert(ps5vk_target_carrier(color_base.registers[i].offset));
        }
        /* The colour address is two words: 0x318 holds address>>8 and 0x390 holds
         * (address>>40)&0xff, so three layers move exactly 3 * stride >> 8 in the
         * first and nothing in the second at this address. */
        assert(color_moves==1);
        assert(ps5vk_target_carrier(0x318u) && ps5vk_target_carrier(0x390u));
        assert(!ps5vk_target_carrier(0x31cu) && !ps5vk_target_carrier(0x200u));
        image.info.format=view.format=VK_FORMAT_D32_SFLOAT;
        image.info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        view.range=(VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,4};
        struct ps5vk_target_registers depth_base,depth_layer;
        assert(ps5vk_native_target(&device,&view,NULL,&depth_base)==VK_SUCCESS &&
               depth_base.count==PS5_DEPTH_REGISTER_COUNT);
        assert(ps5vk_native_view_layer_target(&device,&view,3u,NULL,&depth_layer)==VK_SUCCESS);
        for(unsigned i=0;i<depth_base.count;++i)
            assert(depth_base.registers[i].offset==ps5vk_depth_target_offsets[i] &&
                   depth_layer.registers[i].offset==ps5vk_depth_target_offsets[i]);
        unsigned depth_moves=0;
        int depth_render_control_equal=0;
        for(unsigned i=0;i<depth_base.count;++i) {
            const uint32_t offset=depth_base.registers[i].offset;
            if(offset==0x200u)
                depth_render_control_equal=
                    depth_base.registers[i].value==depth_layer.registers[i].value;
            if(depth_base.registers[i].value==depth_layer.registers[i].value)continue;
            ++depth_moves;
            assert(ps5vk_target_carrier(offset));
        }
        /* The D32 plan carries offset 0x200 in every target and the layer never
         * moves it: refusing that word would make every multiview depth draw
         * unrepresentable, which is exactly what the emission must not do. */
        assert(depth_render_control_equal);
        assert(depth_moves==2);            /* 0x012/0x014 lo, 0x01a/0x01c hi */
        assert(ps5vk_target_carrier(0x012u) && ps5vk_target_carrier(0x014u) &&
               ps5vk_target_carrier(0x01au) && ps5vk_target_carrier(0x01cu));
        assert(!ps5vk_target_carrier(0x007u) && !ps5vk_target_carrier(0x2deu));
        assert(ps5vk_target_offsets(PS5_COLOR_REGISTER_COUNT)!=NULL &&
               ps5vk_target_offsets(PS5_DEPTH_REGISTER_COUNT)!=NULL &&
               ps5vk_target_offsets(3u)==NULL);
    }
    puts("Target address bridge: host registers only, no submission");
}
