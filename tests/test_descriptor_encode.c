#include "descriptor_encode.h"
#include "texture_descriptor.h"
#include "vk_image.h"
#include "vk_sampler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Isolated address-resolution fixture. Actual binding lifetime/range validation
 * is tested by test_vk_memory; this test is not evidence of GPU mappings. */
VkResult ps5vk_buffer_span(VkDevice d, VkBuffer b, VkDeviceSize offset,
                          VkDeviceSize range, void **address, VkDeviceSize *size)
{
    (void)d;
    if (!b) return VK_ERROR_UNKNOWN;
    *address = (void *)((uintptr_t)b + offset); *size = range;
    return VK_SUCCESS;
}
VkResult ps5vk_image_span(VkDevice d, VkImage image, void **address,
                          VkDeviceSize *bytes)
{
    (void)d;
    if (!image || !image->memory) return VK_ERROR_UNKNOWN;
    *address = (void *)(uintptr_t)UINT64_C(0x200001000);
    *bytes = image->requirements.size;
    return VK_SUCCESS;
}
int main(void)
{
    struct VkDevice_T device = {0};
    struct VkDescriptorPool_T pool = {.device = &device};
    struct VkDescriptorSet_T set = {.pool = &pool};
    set.signature.binding[0] = (struct ps5vk_binding){1, 0, VK_SHADER_STAGE_COMPUTE_BIT};
    set.signature.binding[1] = (struct ps5vk_binding){1, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    set.signature.type[0]=set.signature.type[1]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    set.defined[0] = set.defined[1] = VK_TRUE;
    set.buffers[0] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x100004000, 128, 4096};
    set.buffers[1] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x200006000, 256, 2048};
    struct ps5vk_compiled_program p = {.gfx = 1013, .descriptor_count = 2,
        .descriptors = {{0, 1, 0, 0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                        {0, 0, 0, 8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}};
    uint32_t table[16], saved[16]; memset(table, 0xab, sizeof(table));
    VkDeviceSize dynamic[PS5VK_MAX_DESCRIPTORS]={0};
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, dynamic, table, 16) == VK_SUCCESS);
    assert(table[0] == 0x6100 && table[1] == 2 && table[2] == 2048);
    assert(table[8] == 0x4080 && table[9] == 1 && table[10] == 4096);
    assert(table[3] == 0x31016fac && table[11] == 0x31016fac);
    assert(!table[4] && !table[7] && table[12] == 0xabababab);
    set.signature.type[1]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    p.descriptors[0].type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    dynamic[0]=256;
    assert(ps5vk_descriptor_encode(&device,&p,0,&set,dynamic,table,16)==VK_SUCCESS);
    assert(table[0]==0x6200 && table[1]==2 && table[2]==2048);
    dynamic[0]=UINT64_MAX;
    assert(ps5vk_descriptor_encode(&device,&p,0,&set,dynamic,table,16)!=VK_SUCCESS);
    dynamic[0]=0;set.signature.type[1]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    p.descriptors[0].type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    assert(ps5vk_descriptor_encode(&device,&p,0,&set,dynamic,table,16)==VK_SUCCESS);
    memcpy(saved, table, sizeof(table));
    set.signature.binding[0].count = 2;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, dynamic, table, 16) == VK_SUCCESS);
    assert(!memcmp(saved, table, sizeof(table))); /* Scalar shader uses element zero. */
    set.signature.binding[0].count = 1;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, dynamic, table, 11) != VK_SUCCESS);
    set.defined[0] = VK_FALSE;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, dynamic, table, 16) != VK_SUCCESS);
    set.defined[0] = VK_TRUE; set.buffers[0].range = UINT64_C(1) << 32;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, dynamic, table, 16) != VK_SUCCESS);
    set.buffers[0].range = 4096; p.descriptors[1].table_dword = 0;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, dynamic, table, 16) != VK_SUCCESS);
    assert(!memcmp(saved, table, sizeof(table)));
    struct VkBufferView_T view={.device=&device,.buffer=(VkBuffer)(uintptr_t)0x200004000,
        .format=VK_FORMAT_R32_UINT,.offset=64,.range=256};
    struct VkDescriptorSet_T texel={.pool=&pool,.defined={VK_TRUE},.texel_views={&view}};
    texel.signature.binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_COMPUTE_BIT};
    texel.signature.type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    struct ps5vk_compiled_program typed={.gfx=1013,.descriptor_count=1,
        .descriptors={{1,0,0,0,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER}}};
    memset(table,0,sizeof(table));
    assert(ps5vk_descriptor_encode(&device,&typed,1,&texel,dynamic,table,16)==VK_SUCCESS);
    assert(table[0]==0x4040 && table[1]==0x00040002 && table[2]==64 &&
        table[3]==0x11014204);
    /* GFX10 buffer SRD: R32_UINT plus Vulkan identity completion (X,0,0,1).
     * This exact field regression prevents the hardware's scalar replication
     * (X,X,X,X), which violates texelFetch's (R,0,0,1) result. */
    assert(((table[3] >> 12) & 0x7fu) == 20u);
    assert(((table[3] >> 0) & 7u) == 4u);
    assert(((table[3] >> 3) & 7u) == 0u);
    assert(((table[3] >> 6) & 7u) == 0u);
    assert(((table[3] >> 9) & 7u) == 1u);
    /* A four-component row must encode its own GFX10 word and the
     * (X,Y,Z,W) completion. Without this, an RGBA8 texel buffer would be read
     * back through the one-component (X,0,0,1) mapping, or through another
     * row's format word, and no console run of the R32 path would notice. */
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    memset(table, 0, sizeof(table));
    assert(ps5vk_descriptor_encode(&device, &typed, 1, &texel, dynamic, table, 16) == VK_SUCCESS);
    assert(table[0] == 0x4040 && table[1] == 0x00040002 && table[2] == 64);
    assert(((table[3] >> 12) & 0x7fu) == 56u);
    assert(((table[3] >> 0) & 7u) == 4u);
    assert(((table[3] >> 3) & 7u) == 5u);
    assert(((table[3] >> 6) & 7u) == 6u);
    assert(((table[3] >> 9) & 7u) == 7u);
    assert((table[3] & ~UINT32_C(0x7ffff)) == UINT32_C(0x11000000));
    view.format = VK_FORMAT_R8G8B8A8_SINT;
    assert(ps5vk_descriptor_encode(&device, &typed, 1, &texel, dynamic, table, 16) == VK_SUCCESS);
    assert(((table[3] >> 12) & 0x7fu) == 61u);
    /* A format without the implemented role is refused before any mutation:
     * the sRGB member of the same byte size, the BGRA row whose completion
     * would differ, and an unknown format. */
    const VkFormat refused[] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
                                 (VkFormat)0x7fffffff };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        view.format = refused[i];
        memset(table, 0xab, sizeof(table));
        assert(ps5vk_descriptor_encode(&device, &typed, 1, &texel, dynamic, table, 16) != VK_SUCCESS);
        for (unsigned j = 0; j < 16; ++j) assert(table[j] == 0xabababab);
    }
    /* Pinned BDA output: R32_UINT 8x8, padded 256-byte rows, one UAV T#
     * followed by the original SSBO. A buffer-width declaration for binding
     * one would overlap the eight-DWORD image record. */
    struct VkImage_T image = {.device=&device,
        .memory=(VkDeviceMemory)(uintptr_t)1,
        .layout=VK_IMAGE_LAYOUT_GENERAL,
        .requirements={.size=2048},
        .info={.format=VK_FORMAT_R32_UINT,.imageType=VK_IMAGE_TYPE_2D,
            .extent={8,8,1},.mipLevels=1,.arrayLayers=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
                VK_IMAGE_USAGE_TRANSFER_DST_BIT}};
    struct VkImageView_T image_view={.device=&device,.image=&image,
        .format=VK_FORMAT_R32_UINT,.view_type=VK_IMAGE_VIEW_TYPE_2D,
        .range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    struct VkDescriptorSet_T storage={.pool=&pool,
        .defined={VK_TRUE,VK_TRUE},.image_resources={&image}};
    storage.signature.binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_COMPUTE_BIT};
    storage.signature.binding[1]=(struct ps5vk_binding){1,1,VK_SHADER_STAGE_COMPUTE_BIT};
    storage.signature.type[0]=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    storage.signature.type[1]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storage.images[0]=(VkDescriptorImageInfo){VK_NULL_HANDLE,&image_view,VK_IMAGE_LAYOUT_GENERAL};
    storage.buffers[1]=(VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x200006000,0,256};
    struct ps5vk_compiled_program storage_program={.gfx=1013,.descriptor_count=2,
        .descriptors={{0,0,0,0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                      {0,1,0,8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}};
    uint32_t image_table[16];
    memset(image_table,0,sizeof(image_table));
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)==VK_SUCCESS);
    assert(image_table[0]==0x2000010 && image_table[1]==0xc1400000 &&
        image_table[2]==0x8001c001 && image_table[3]==0x90000204 &&
        image_table[4]==63 && image_table[5]==0x00400000);
    assert(image_table[8]==0x6000 && image_table[9]==2 && image_table[10]==256);
    storage_program.descriptors[1].table_dword=4;
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)!=VK_SUCCESS);
    storage_program.descriptors[1].table_dword=8;
    image.layout=VK_IMAGE_LAYOUT_UNDEFINED;
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)!=VK_SUCCESS);
    image.layout=VK_IMAGE_LAYOUT_GENERAL;
    storage.images[0].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)!=VK_SUCCESS);
    storage.images[0].imageLayout=VK_IMAGE_LAYOUT_GENERAL;

    /* VK_EXT_robustness2 nullDescriptor. Without the device feature a null
     * handle is refused before any word changes. With it, each null role is
     * an all-zero record of its own width, and the live records beside it are
     * encoded exactly as before. */
    const VkDescriptorBufferInfo null_buffer={VK_NULL_HANDLE,0,VK_WHOLE_SIZE};
    uint32_t record[4];
    memset(record,0xab,sizeof(record));
    assert(ps5vk_buffer_descriptor(&device,&null_buffer,0,record)!=VK_SUCCESS);
    for(unsigned i=0;i<4;++i)assert(record[i]==0xabababab);
    storage.images[0]=(VkDescriptorImageInfo){VK_NULL_HANDLE,VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_UNDEFINED};
    storage.image_resources[0]=VK_NULL_HANDLE;
    storage.buffers[1]=null_buffer;
    memset(image_table,0xab,sizeof(image_table));
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)!=VK_SUCCESS);
    for(unsigned i=0;i<16;++i)assert(image_table[i]==0xabababab);
    device.enabled_features_t09|=PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
    assert(ps5vk_buffer_descriptor(&device,&null_buffer,0,record)==VK_SUCCESS);
    for(unsigned i=0;i<4;++i)assert(record[i]==0);
    /* A dynamic offset bound for a null dynamic buffer is not in the record. */
    memset(record,0xab,sizeof(record));
    assert(ps5vk_buffer_descriptor(&device,&null_buffer,4096,record)==VK_SUCCESS);
    for(unsigned i=0;i<4;++i)assert(record[i]==0);
    /* VUID-VkDescriptorBufferInfo-buffer-02999: offset 0, VK_WHOLE_SIZE. */
    const VkDescriptorBufferInfo null_offset={VK_NULL_HANDLE,256,VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo null_range={VK_NULL_HANDLE,0,256};
    memset(record,0xab,sizeof(record));
    assert(ps5vk_buffer_descriptor(&device,&null_offset,0,record)!=VK_SUCCESS);
    assert(ps5vk_buffer_descriptor(&device,&null_range,0,record)!=VK_SUCCESS);
    for(unsigned i=0;i<4;++i)assert(record[i]==0xabababab);
    memset(image_table,0xab,sizeof(image_table));
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)==VK_SUCCESS);
    for(unsigned i=0;i<12;++i)assert(image_table[i]==0);
    assert(image_table[12]==0xabababab);
    /* A live storage image beside a null buffer keeps its exact T#. */
    storage.images[0]=(VkDescriptorImageInfo){VK_NULL_HANDLE,&image_view,VK_IMAGE_LAYOUT_GENERAL};
    storage.image_resources[0]=&image;
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)==VK_SUCCESS);
    assert(image_table[0]==0x2000010 && image_table[3]==0x90000204);
    for(unsigned i=8;i<12;++i)assert(image_table[i]==0);
    /* A view without its recorded resource is still a broken record, not a
     * null one. */
    storage.image_resources[0]=VK_NULL_HANDLE;
    assert(ps5vk_descriptor_encode(&device,&storage_program,0,&storage,dynamic,
        image_table,16)!=VK_SUCCESS);
    texel.texel_views[0]=VK_NULL_HANDLE;
    memset(table,0xab,sizeof(table));
    assert(ps5vk_descriptor_encode(&device,&typed,1,&texel,dynamic,table,16)==VK_SUCCESS);
    for(unsigned i=0;i<4;++i)assert(table[i]==0);
    assert(table[4]==0xabababab);
    device.enabled_features_t09&=~(uint32_t)PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
    assert(ps5vk_descriptor_encode(&device,&typed,1,&texel,dynamic,table,16)!=VK_SUCCESS);

    /* robustBufferAccess2: NUM_RECORDS is the descriptor range rounded up to
     * the reported four-byte alignment, only on a device that enabled it;
     * robustBufferAccess alone keeps the exact byte extent. */
    const VkDescriptorBufferInfo ranged={(VkBuffer)(uintptr_t)0x100004000,64,4093};
    assert(ps5vk_buffer_descriptor(&device,&ranged,0,record)==VK_SUCCESS);
    assert(record[0]==0x4040 && record[1]==1 && record[2]==4093 &&
        record[3]==0x31016fac);
    device.enabled_features_t09|=PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2;
    const VkDeviceSize ranges[][2]={{4093,4096},{4094,4096},{4095,4096},{4096,4096},
        {1,4},{4097,4100}};
    for(unsigned i=0;i<sizeof(ranges)/sizeof(ranges[0]);++i) {
        const VkDescriptorBufferInfo r={(VkBuffer)(uintptr_t)0x100004000,64,ranges[i][0]};
        assert(ps5vk_buffer_descriptor(&device,&r,0,record)==VK_SUCCESS);
        assert(record[0]==0x4040 && record[1]==1 && record[2]==ranges[i][1] &&
            record[3]==0x31016fac);
    }
    /* A dynamic offset moves the base, never the rounded extent. */
    assert(ps5vk_buffer_descriptor(&device,&ranged,256,record)==VK_SUCCESS);
    assert(record[0]==0x4140 && record[2]==4096);
    /* The rounded extent is still bounded to 32 bits. */
    const VkDescriptorBufferInfo huge={(VkBuffer)(uintptr_t)0x100004000,0,UINT32_MAX};
    assert(ps5vk_buffer_descriptor(&device,&huge,0,record)!=VK_SUCCESS);
    device.enabled_features_t09&=~(uint32_t)PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2;
    assert(ps5vk_buffer_descriptor(&device,&huge,0,record)==VK_SUCCESS);
    assert(record[2]==UINT32_MAX);
    /* DXVK's DXBC forms. A separate SAMPLER is the sampler's own four words;
     * a separate SAMPLED_IMAGE is exactly the combined record's T#; the two
     * texel roles share one V# but each needs its own implemented role. */
    struct VkImage_T sampled_image={.device=&device,
        .memory=(VkDeviceMemory)(uintptr_t)1,.requirements={.size=4096},
        .info={.format=VK_FORMAT_R8G8B8A8_UNORM,.imageType=VK_IMAGE_TYPE_2D,
            .extent={8,4,1},.mipLevels=1,.arrayLayers=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT}};
    struct VkImageView_T sampled_view={.device=&device,.image=&sampled_image,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.view_type=VK_IMAGE_VIEW_TYPE_2D,
        .range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    struct VkSampler_T sampler={.device=&device,.words={0x11111111u,0x22222222u,
        0x33333333u,0x44444444u}};
    struct VkBufferView_T typed_view={.device=&device,.buffer=(VkBuffer)(uintptr_t)0x200004000,
        .format=VK_FORMAT_R32_UINT,.offset=64,.range=256};
    struct VkDescriptorSet_T separate={.pool=&pool,
        .defined={VK_TRUE,VK_TRUE,VK_TRUE,VK_TRUE},
        .image_resources={VK_NULL_HANDLE,&sampled_image},
        .texel_views={VK_NULL_HANDLE,VK_NULL_HANDLE,&typed_view,&typed_view}};
    const VkDescriptorType separate_types[4]={VK_DESCRIPTOR_TYPE_SAMPLER,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    for(uint32_t b=0;b<4;++b) {
        separate.signature.binding[b]=(struct ps5vk_binding){1,b,VK_SHADER_STAGE_COMPUTE_BIT};
        separate.signature.type[b]=separate_types[b];
    }
    separate.images[0]=(VkDescriptorImageInfo){&sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
    separate.images[1]=(VkDescriptorImageInfo){VK_NULL_HANDLE,&sampled_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    /* Canonical offsets: S# at dword 0, T# at 4, uniform V# at 12. */
    struct ps5vk_compiled_program separate_program={.gfx=1013,.descriptor_count=3,
        .descriptors={{0,0,0,0,VK_DESCRIPTOR_TYPE_SAMPLER},
                      {0,1,0,4,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
                      {0,2,0,12,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER}}};
    uint32_t separate_table[20], combined[12];
    memset(separate_table,0xab,sizeof(separate_table));
    assert(ps5vk_descriptor_encode(&device,&separate_program,0,&separate,dynamic,
        separate_table,20)==VK_SUCCESS);
    assert(!memcmp(separate_table,sampler.words,16));
    assert(ps5vk_texture_descriptor(&device,&sampled_view,&sampler,combined)==VK_SUCCESS);
    assert(!memcmp(separate_table+4,combined,32));
    assert(separate_table[4]==0x2000010 && ((separate_table[6]>>14)&0x3fffu)==3u);
    assert(separate_table[12]==0x4040 && separate_table[13]==0x00040002 &&
        separate_table[14]==64 && separate_table[15]==0x11014204);
    assert(separate_table[16]==0xabababab);
    /* A sampled-image record in the wrong layout, over an image without
     * SAMPLED usage or with a missing sampler is refused, table unchanged. */
    memcpy(saved,separate_table,sizeof(saved));
    separate.images[1].imageLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    assert(ps5vk_descriptor_encode(&device,&separate_program,0,&separate,dynamic,
        separate_table,20)!=VK_SUCCESS);
    separate.images[1].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sampled_image.info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(ps5vk_descriptor_encode(&device,&separate_program,0,&separate,dynamic,
        separate_table,20)!=VK_SUCCESS);
    assert(ps5vk_sampled_image_descriptor(&device,&sampled_view,combined)==
        VK_ERROR_FEATURE_NOT_PRESENT);
    sampled_image.info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    separate.images[0].sampler=VK_NULL_HANDLE;
    assert(ps5vk_descriptor_encode(&device,&separate_program,0,&separate,dynamic,
        separate_table,20)!=VK_SUCCESS);
    separate.images[0].sampler=&sampler;
    assert(!memcmp(saved,separate_table,sizeof(saved)));
    /* The storage role is a distinct implemented capability. R32_UINT carries
     * it: the imageStore V# is the texelFetch record. R32_SINT carries only
     * the uniform role and is refused, table unchanged. */
    struct ps5vk_compiled_program storage_texel_program={.gfx=1013,.descriptor_count=1,
        .descriptors={{0,3,0,16,VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER}}};
    assert(ps5vk_texture_format_capabilities(VK_FORMAT_R32_UINT) &
        PS5VK_FORMAT_CAP_STORAGE_TEXEL_BUFFER);
    assert(ps5vk_descriptor_encode(&device,&storage_texel_program,0,&separate,dynamic,
        separate_table,20)==VK_SUCCESS);
    assert(separate_table[16]==0x4040 && separate_table[17]==0x00040002 &&
        separate_table[18]==64 && separate_table[19]==0x11014204);
    typed_view.format=VK_FORMAT_R32_SINT;
    memcpy(saved,separate_table,sizeof(saved));
    assert(ps5vk_descriptor_encode(&device,&storage_texel_program,0,&separate,dynamic,
        separate_table,20)!=VK_SUCCESS);
    assert(!memcmp(saved,separate_table,sizeof(saved)));
    typed_view.format=VK_FORMAT_R32_UINT;
    /* A compiled type the compute path has no record for is refused. */
    separate_program.descriptors[0].type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    separate.signature.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    assert(ps5vk_descriptor_encode(&device,&separate_program,0,&separate,dynamic,
        separate_table,20)!=VK_SUCCESS);
    puts("Compiler-ordered raw descriptor table: pass (host only)");
}
