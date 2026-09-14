#include "draw_prepare_ps5.h"
#include "vertex_fetch.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocations, releases, targets;
static VkResult target_rc, flush_rc;
static VkDeviceSize fail_allocation_size;
static uintptr_t latest_address;
static _Alignas(4096) unsigned char storage[4096];
static _Alignas(4) unsigned char vertex_source[32];
static size_t expected_bytes=sizeof(struct ps5vk_draw_state);
static VkResult fetch_rc;
static VkResult texture_rc;
VkResult ps5vk_texture_descriptor(VkDevice d,VkImageView v,VkSampler s,uint32_t out[12])
{(void)d;(void)v;(void)s;for(unsigned i=0;i<12;++i)out[i]=100+i;return texture_rc;}
static const void *fetch_address=vertex_source;
static unsigned fetch_count=1;
VkResult ps5vk_vertex_fetch_used_spans(VkDevice d,const struct ps5vk_graphics_key *k,
    const struct ps5vk_operation *op,uint32_t mask,struct ps5vk_vertex_fetch_table *out)
{
    (void)d;(void)k;(void)op;(void)mask;
    if(fetch_rc==VK_SUCCESS) {
        *out=(struct ps5vk_vertex_fetch_table){.count=fetch_count};
        out->bindings[0]=(struct ps5vk_vertex_fetch){fetch_address,16,4,4};
        if(fetch_count>1)out->bindings[fetch_count-1]=out->bindings[0];
    }
    return fetch_rc;
}
static VkResult allocate(void *c, VkDeviceSize n, void **a, void **b)
{
    (void)c;
    assert(n<=sizeof(storage));
    if(n==fail_allocation_size)return VK_ERROR_OUT_OF_HOST_MEMORY;
    ++allocations; *a = *b = storage; latest_address=(uintptr_t)*a;return VK_SUCCESS;
}
static void release(void *c, void *b) { (void)c; assert(b==storage);++releases; }
static VkResult flush(void *c, void *b, VkDeviceSize o, VkDeviceSize n)
{ (void)c; assert(b && !o && n == expected_bytes); return flush_rc; }
VkResult ps5vk_native_target(VkDevice d, VkImageView v, const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],
    struct ps5vk_target_registers *out)
{ (void)defaults; assert(v->device == d); ++targets; out->count = 16; return target_rc; }
VkResult ps5vk_native_draw_state(VkPipeline p, const VkViewport *viewport,
    const VkRect2D *scissor, const struct ps5vk_target_registers *color,
    const struct ps5vk_target_registers *depth, const VkRect2D *area,
    uint32_t width, uint32_t height, struct ps5vk_draw_state *out)
{
    assert(p && viewport->width == 4 && scissor->extent.width == 4 &&
        color->count == 16 && !depth && area->extent.width == 4 && width == 4 && height == 4);
    *out = (struct ps5vk_draw_state){.cx_count = 87, .modifier = 5}; return VK_SUCCESS;
}
int main(void)
{
    /* Test the allocation bridge independently; target/state behavior has its
     * own tests. These callbacks do not build GPU commands. */
    struct VkDevice_T d = {.memory = {NULL, allocate, release, flush, flush}};
    struct VkImageView_T v = {.device = &d};
    struct VkFramebuffer_T fb = {.device = &d, .width = 4, .height = 4, .attachment_count = 1,
        .attachments = {&v}, .depth_attachment = VK_ATTACHMENT_UNUSED};
    struct VkRenderPass_T pass = {.device = &d};
    struct VkPipeline_T p = {.device = &d};
    struct ps5vk_operation op = {.type = PS5VK_DRAW, .pipeline = &p, .framebuffer = &fb,
        .render_pass = &pass, .viewport = {0,0,4,4,0,1}, .scissor = {{0,0},{4,4}}};
    VkRect2D area = {{0,0},{4,4}};
    struct ps5vk_prepared_draw prepared = {0};
    target_rc = VK_ERROR_FORMAT_NOT_SUPPORTED;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) == target_rc && !allocations);
    target_rc = VK_SUCCESS; flush_rc = VK_ERROR_MEMORY_MAP_FAILED;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) == flush_rc);
    assert(allocations == releases && !prepared.backing);
    flush_rc = VK_SUCCESS;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) == VK_SUCCESS);
    assert(prepared.state->modifier == 5 && prepared.bytes == sizeof(*prepared.state));
    unsigned before = targets;
    assert(ps5vk_native_prepare_draw(&d, &op, &area, NULL, &prepared) != VK_SUCCESS && targets == before);
    ps5vk_native_release_draw(&prepared); ps5vk_native_release_draw(&prepared);
    assert(allocations == releases && !prepared.state);
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+16;
    /* Fixed aligned allocation fixture makes the aperture test deterministic. */
    uint64_t shader_address=latest_address;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(prepared.vertex_table && (uintptr_t)prepared.vertex_table%16==0 && !prepared.vertex_bounce);
    assert(prepared.vertex_table[0]==(uint32_t)(uintptr_t)vertex_source &&
        (prepared.vertex_table[1]&0xffff)==(uintptr_t)vertex_source>>32 &&
        prepared.vertex_table[1]>>16==4 && prepared.vertex_table[2]==4);
    assert(prepared.bytes==expected_bytes);
    ps5vk_native_release_draw(&prepared);
    for(unsigned i=0;i<sizeof(vertex_source);++i)vertex_source[i]=(unsigned char)(i+1);
    fetch_address=vertex_source+1;
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+16+16;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(prepared.vertex_bounce && (uintptr_t)prepared.vertex_bounce%4==0 &&
        prepared.vertex_bounce_bytes==16 && !memcmp(prepared.vertex_bounce,vertex_source+1,16));
    assert(prepared.vertex_table[0]==(uint32_t)(uintptr_t)prepared.vertex_bounce &&
        (prepared.vertex_table[1]&0xffff)==(uintptr_t)prepared.vertex_bounce>>32);
    ps5vk_native_release_draw(&prepared);
    /* An unencodable byte-granular source may need an aligned bounce.  Failure
     * to allocate it is deterministic and leaves no partial prepared draw. */
    unsigned bounce_allocated=allocations;
    fail_allocation_size=expected_bytes;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==
        VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(allocations==bounce_allocated && releases==allocations &&
        !prepared.backing && !prepared.vertex_bounce);
    fail_allocation_size=0;fetch_address=vertex_source;
    /* Compiler mask {0,15} requires two packed SRDs, not sixteen sparse SRDs.
     * Each unaligned source owns its bounce until draw retirement. */
    fetch_count=16;fetch_address=vertex_source+1;
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+32+32;
    assert(ps5vk_native_prepare_vertex_draw_masked(&d,&op,&area,NULL,NULL,shader_address,0x8001,&prepared)==VK_SUCCESS);
    assert(prepared.vertex_bounce_bytes==32);
    assert(prepared.vertex_table[0]!=prepared.vertex_table[4]);
    assert(prepared.vertex_table[6]==4);
    ps5vk_native_release_draw(&prepared);
    assert(allocations==releases);
    fetch_count=1;fetch_address=vertex_source;
    expected_bytes=((sizeof(struct ps5vk_draw_state)+15u)&~(size_t)15u)+16;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address^(UINT64_C(1)<<32),&prepared)==VK_ERROR_MEMORY_MAP_FAILED);
    assert(allocations==releases && !prepared.backing);
    unsigned allocated=allocations;fetch_rc=VK_ERROR_UNKNOWN;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==fetch_rc && allocations==allocated);
    fetch_rc=VK_SUCCESS;
    struct VkDescriptorPool_T pool={.device=&d};
    struct VkDescriptorSet_T set={.pool=&pool,.generation=7};
    set.signature.count=1;set.signature.binding[0].count=1;set.signature.type[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;set.defined[0]=VK_TRUE;
    op.pipeline->set_count=1;op.sets[0]=&set;op.generations[0]=7;
    expected_bytes+=48;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==VK_SUCCESS);
    assert(prepared.texture_table==prepared.vertex_table+4 && prepared.bytes==expected_bytes);
    for(unsigned i=0;i<12;++i)assert(prepared.texture_table[i]==100+i);
    ps5vk_native_release_draw(&prepared);assert(!prepared.texture_table && allocations==releases);
    allocated=allocations;op.generations[0]=6;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)!=VK_SUCCESS && allocations==allocated);
    op.generations[0]=7;texture_rc=VK_ERROR_UNKNOWN;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==texture_rc && allocations==allocated);
    texture_rc=VK_SUCCESS;flush_rc=VK_ERROR_MEMORY_MAP_FAILED;
    assert(ps5vk_native_prepare_vertex_draw(&d,&op,&area,NULL,NULL,shader_address,&prepared)==flush_rc && allocations==releases);
}
