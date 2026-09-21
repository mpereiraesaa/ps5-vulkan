#include "graphics_program.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    uint32_t vs[5]={0x07230203,1,2,3,0}, fs[5]={0x07230203,4,5,6,0};
    int payload;
    struct ps5vk_graphics_program programs[2]={{.key={
        .vertex={.words=vs,.word_count=5,.entry="main"},
        .fragment={.words=fs,.word_count=5,.entry="main"},
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .color_write_mask={15}},.backend_data=&payload}};
    struct ps5vk_graphics_library library={programs,1};
    struct ps5vk_graphics_key key=programs[0].key;
    const struct ps5vk_graphics_program *out;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_SUCCESS && out==programs);
    /* No alias between distinct enabled blend contracts; ignored disabled
     * state must not make a formerly matching library record disappear. */
    key.src_color_blend_factor[0]=VK_BLEND_FACTOR_SRC_ALPHA;
    key.blend_constants[0]=0.5f;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_SUCCESS);
    key=programs[0].key;key.blend_enable[0]=VK_TRUE;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    programs[0].key.blend_enable[0]=VK_TRUE;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_SUCCESS);
#define CHECK_BLEND_FIELD(field,value) do { \
    key=programs[0].key; key.field=(value); \
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out); \
} while(0)
    CHECK_BLEND_FIELD(src_color_blend_factor[0],VK_BLEND_FACTOR_SRC_ALPHA);
    CHECK_BLEND_FIELD(dst_color_blend_factor[0],VK_BLEND_FACTOR_ONE);
    CHECK_BLEND_FIELD(color_blend_op[0],VK_BLEND_OP_SUBTRACT);
    CHECK_BLEND_FIELD(src_alpha_blend_factor[0],VK_BLEND_FACTOR_SRC_ALPHA);
    CHECK_BLEND_FIELD(dst_alpha_blend_factor[0],VK_BLEND_FACTOR_ONE);
    CHECK_BLEND_FIELD(alpha_blend_op[0],VK_BLEND_OP_MAX);
    for(unsigned i=0;i<4;++i) CHECK_BLEND_FIELD(blend_constants[i],0.5f);
#undef CHECK_BLEND_FIELD
    programs[0].key.blend_enable[0]=VK_FALSE;key=programs[0].key;
    uint32_t changed[5]={0x07230203,4,5,7,0}; key.fragment.words=changed;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    key=programs[0].key; key.color_format[0]=VK_FORMAT_R8G8B8A8_UNORM;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    key=programs[0].key; key.vertex.entry="different";
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    key=programs[0].key; key.descriptor_set_count=1;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    key=programs[0].key; programs[1]=programs[0]; library.count=2;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_UNKNOWN && !out);
    library.count=1;
    VkVertexInputBindingDescription bindings[2]={{0,24,VK_VERTEX_INPUT_RATE_VERTEX},{3,8,VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription attributes[2]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32_SFLOAT,12}};
    programs[0].key.vertex_binding_count=2;programs[0].key.vertex_bindings=bindings;
    programs[0].key.vertex_attribute_count=2;programs[0].key.vertex_attributes=attributes;
    key=programs[0].key;
    VkVertexInputBindingDescription reordered_b[2]={bindings[1],bindings[0]};
    VkVertexInputAttributeDescription reordered_a[2]={attributes[1],attributes[0]};
    key.vertex_bindings=reordered_b;key.vertex_attributes=reordered_a;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_SUCCESS);
    reordered_b[1].stride=28;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    reordered_b[1]=bindings[0];reordered_b[1].inputRate=VK_VERTEX_INPUT_RATE_INSTANCE;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    reordered_b[1]=bindings[0];reordered_a[0].offset=8;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    reordered_a[0]=attributes[1];reordered_a[0].format=VK_FORMAT_R32G32_SFLOAT;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    reordered_a[0]=attributes[1];reordered_a[0].binding=3;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    reordered_a[0].binding=9;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_UNKNOWN);
    reordered_a[0]=attributes[0];
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_UNKNOWN);
    reordered_a[0]=attributes[1];reordered_b[0]=bindings[0];
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_UNKNOWN);
    reordered_b[0]=bindings[1];key.vertex_bindings=NULL;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_UNKNOWN);
    key=programs[0].key;key.vertex_attribute_count=33;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_UNKNOWN);
    struct ps5vk_set_signature signature={.count=1};
    signature.binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_FRAGMENT_BIT};
    signature.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for(unsigned i=1;i<PS5VK_MAX_BINDINGS;++i)signature.binding[i].first=1;
    programs[0].key.descriptor_set_count=1;programs[0].key.descriptor_sets=&signature;
    struct ps5vk_set_signature requested=signature;
    key=programs[0].key;key.descriptor_sets=&requested;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_SUCCESS);
    requested.type[0]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    requested=signature;requested.binding[0].stages=VK_SHADER_STAGE_VERTEX_BIT;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    requested=signature;requested.count=2;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    requested=signature;requested.binding[1].first=0;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    key.descriptor_sets=NULL;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    key.descriptor_set_count=0;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT);
    /* The tessellation pair belongs to the identity, together with the patch
     * control points the pair was compiled for: a record without a pair must
     * never satisfy a pipeline that asks for one, the pair's own modules are
     * compared rather than merely present, and two runs that differ only in the
     * patch count are different programs. */
    library.count=1;
    uint32_t tcs[5]={0x07230203,7,8,9,0}, tes[5]={0x07230203,10,11,12,0};
    uint32_t other_tes[5]={0x07230203,10,11,13,0};
    programs[0].key.tess_control=(struct ps5vk_graphics_module_key){
        .words=tcs,.word_count=5,.entry="main"};
    programs[0].key.tess_eval=(struct ps5vk_graphics_module_key){
        .words=tes,.word_count=5,.entry="main"};
    programs[0].key.patch_control_points=3;
    key=programs[0].key;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_SUCCESS && out==programs);
    key.patch_control_points=4;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    key=programs[0].key;key.tess_eval.words=other_tes;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    key=programs[0].key;
    key.tess_control=(struct ps5vk_graphics_module_key){0};
    key.tess_eval=(struct ps5vk_graphics_module_key){0};
    key.patch_control_points=0;
    assert(ps5vk_graphics_resolve(&library,&key,&out)==VK_ERROR_FEATURE_NOT_PRESENT && !out);
    puts("Graphics library exact-pair resolution: structural host fixtures only");
}
