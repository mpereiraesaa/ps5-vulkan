#include "draw_emit_ps5.h"
#include "ps5_platform.h"
#include <assert.h>
static unsigned calls;
static uint32_t *indirect(void *opaque, const void *r, uint32_t count)
{
    struct ps5_agc_command_buffer *w = opaque; assert(r && count); ++calls;
    assert(w->top - w->up >= 1); *w->up++ = count; return w->up;
}
uint32_t *sceAgcDcbSetCxRegistersIndirect(void *w, const void *r, uint32_t n) { return indirect(w,r,n); }
uint32_t *sceAgcDcbSetUcRegistersIndirect(void *w, const void *r, uint32_t n) { return indirect(w,r,n); }
uint32_t *sceAgcDcbSetShRegistersIndirect(void *w, const void *r, uint32_t n) { return indirect(w,r,n); }
uint32_t *sceAgcCbSetShRegisterRangeDirect(void *opaque, uint32_t offset, const uint32_t *v, uint32_t n)
{
    struct ps5_agc_command_buffer *w = opaque; ++calls;
    assert(w->top-w->up >= n+2); *w->up++ = offset; *w->up++ = n;
    for (uint32_t i=0; i<n; ++i) *w->up++ = v[i];
    return w->up;
}
uint32_t *sceAgcDcbDrawIndexAuto(void *opaque, uint32_t n, uint64_t modifier)
{
    struct ps5_agc_command_buffer *w = opaque; ++calls;
    assert(w->top-w->up >= 3); *w->up++ = n; *w->up++ = (uint32_t)modifier; *w->up++ = 0;
    return w->up;
}
static uint32_t *draw_index(void *opaque,uint32_t n,const void *address,uint64_t modifier)
{
    struct ps5_agc_command_buffer *w=opaque;
    assert(n==6 && (uintptr_t)address==0x12340000 && modifier==5);++calls;
    assert(w->top-w->up==6);
    for(unsigned i=0;i<6;++i)*w->up++=i;
    return w->up;
}
int main(void)
{
    struct ps5vk_draw_state replay={.cx_count=3,.cx={{0x094,0x800003c0},{0x095,0x021c0780},{0x292,0x22}}};
    uint32_t packet[8]={0},*at=packet;
    assert(ps5vk_native_emit_scissor_replay(&at,6,&replay)==VK_ERROR_OUT_OF_HOST_MEMORY && at==packet && !packet[0]);
    assert(ps5vk_native_emit_scissor_replay(&at,7,&replay)==VK_SUCCESS && at==packet+7);
    assert(packet[0]==0xc0026900 && packet[1]==0x094 && packet[2]==0x800003c0 && packet[3]==0x021c0780);
    assert(packet[4]==0xc0016900 && packet[5]==0x292 && packet[6]==0x22 && packet[7]==0);
    at=packet;replay.cx_count=2;
    assert(ps5vk_native_emit_scissor_replay(&at,8,&replay)==VK_ERROR_UNKNOWN && at==packet);
    struct ps5vk_draw_state state = {.cx_count = 87, .modifier = 5};
    struct ps5vk_operation op = {.type = PS5VK_DRAW, .vertex_count = 3, .instance_count = 7,
        .first_vertex = 11, .first_instance = 13};
    uint32_t commands[64] = {0}, *cursor = commands;
    assert(ps5vk_native_emit_draw(&cursor, 64, &state, &state, sizeof(state), &op, 0x123400) == VK_SUCCESS);
    assert(calls == 6 && cursor == commands+16);
    assert(commands[3] == 0x8c && commands[5] == 0x123400 && commands[6] == 11 && commands[7] == 13);
    assert(commands[8] == 0xc && commands[10] == 0x123400);
    assert(commands[11] == 0xc0002f00 && commands[12] == 7 && commands[13] == 3);
    cursor = commands; calls = 0; op.instance_count = 0;
    assert(ps5vk_native_emit_draw(&cursor, 64, &state, &state, sizeof(state), &op, 0) == VK_SUCCESS);
    assert(cursor == commands && !calls);
    op.instance_count = 1;
    assert(ps5vk_native_emit_draw(&cursor, 15, &state, &state, sizeof(state), &op, 0) != VK_SUCCESS);
    assert(cursor == commands);
    calls = 0;
    assert(ps5vk_native_emit_draw(&cursor, 64, &state, commands, sizeof(commands), &op, 0) != VK_SUCCESS && !calls);
    cursor=commands;calls=0;op.instance_count=7;
    assert(ps5vk_native_emit_vertex_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400,0x567800)==VK_SUCCESS);
    assert(calls==6 && cursor==commands+17);
    assert(commands[3]==0x8c && commands[4]==4 && commands[5]==0x123400);
    assert(commands[6]==0x567800 && commands[7]==11 && commands[8]==13);
    assert(commands[9]==0xc && commands[11]==0x123400);
    assert(commands[12]==0xc0002f00 && commands[13]==7 && commands[14]==3);
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_vertex_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400,3)!=VK_SUCCESS);
    assert(cursor==commands && !calls);
    assert(ps5vk_native_emit_vertex_draw(&cursor,16,&state,&state,sizeof(state),&op,0x123400,0x567800)!=VK_SUCCESS);
    assert(cursor==commands);
    cursor=commands;calls=0;
    op.type=PS5VK_DRAW_INDEXED;op.index_count=6;op.vertex_offset=-2;
    struct ps5vk_index_fetch indices={0x12340000,6,2};
    assert(ps5vk_native_emit_indexed_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800,&indices,draw_index)==VK_SUCCESS);
    assert(cursor==commands+23 && calls==6);
    assert(commands[7]==UINT32_MAX-1 && commands[8]==13);
    assert(commands[14]==0xc0017a00 && commands[15]==0x20000243 && commands[16]==0);
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_textured_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800,0x900000,&indices,draw_index)==VK_SUCCESS);
    assert(cursor==commands+24 && calls==6);
    assert(commands[9]==0xc && commands[10]==2 && commands[11]==0x123400 && commands[12]==0x900000);
    assert(commands[13]==0xc0002f00 && commands[15]==0xc0017a00);
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_textured_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800,0x900001,&indices,draw_index)!=VK_SUCCESS && !calls && cursor==commands);
    op.type=PS5VK_DRAW;
    assert(ps5vk_native_emit_textured_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800,0x900000,NULL,NULL)==VK_SUCCESS && cursor==commands+18);
    state.runtime=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=2,.fragment_count=2,
        .base_vertex_slot=0,.start_instance_slot=UINT32_MAX,.lds_slot=1,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    state.sh_count=10;cursor=commands;calls=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)==VK_SUCCESS);
    assert(commands[2]==10 && commands[3]==0x8c && commands[4]==2);
    assert(commands[5]==11 && commands[6]==0); /* No LLPC table address in base vertex. */
    assert(commands[7]==0xc && commands[8]==2 && commands[9]==0 && commands[10]==0);
    /* The integer vertex-format probe uses the real runtime vertex-table ABI
     * but deliberately isolates it from the still-unsupported combination of
     * runtime shaders with indexed emission. */
    state.runtime=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=3,.fragment_count=2,
        .base_vertex_slot=1,.start_instance_slot=UINT32_MAX,
        .vertex_buffer_valid=1,.vertex_buffer_slot=0,.vertex_buffer_usage_mask=1,.lds_slot=2,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    cursor=commands;calls=0;op.type=PS5VK_DRAW;op.instance_count=1;op.first_vertex=0;
    assert(ps5vk_native_emit_vertex_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800)==VK_SUCCESS && cursor>commands && calls);
    cursor=commands;calls=0;op.type=PS5VK_DRAW_INDEXED;op.index_count=6;
    assert(ps5vk_native_emit_indexed_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800,&indices,draw_index)==VK_ERROR_FEATURE_NOT_PRESENT);
    assert(cursor==commands && !calls);
    op.type=PS5VK_DRAW;state.runtime.fragment_descriptor_valid[0]=1;
    state.runtime.fragment_descriptor_slot[0]=0;
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_textured_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800,0x900000,NULL,NULL)==VK_SUCCESS);
    assert(commands[8]==0xc && commands[9]==2 && commands[10]==0x900000);
    state.runtime.fragment_count=4;
    uint32_t tables[4]={0x10000,0x20000,0x30000,0x40000};
    for(unsigned s=0;s<4;++s) {
        state.runtime.fragment_descriptor_valid[s]=1;
        state.runtime.fragment_descriptor_slot[s]=3-s;
    }
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,tables,NULL,NULL)==VK_SUCCESS && calls==6);
    assert(commands[8]==0xc && commands[9]==4);
    for(unsigned s=0;s<4;++s)assert(commands[10+3-s]==tables[s]);
    cursor=commands;calls=0;tables[3]=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,tables,NULL,NULL)!=VK_SUCCESS && !calls && cursor==commands);
    tables[3]=0x40000;
    state.runtime.fragment_descriptor_slot[3]=3;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,tables,NULL,NULL)!=VK_SUCCESS && !calls && cursor==commands);
    state.runtime.fragment_descriptor_slot[3]=0;
    state.runtime.vertex_buffer_valid=0;state.runtime.vertex_buffer_usage_mask=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0,tables,NULL,NULL)==VK_SUCCESS); /* procedural VS + sampled FS */
    state.runtime.fragment_descriptor_valid[0]=0;
    op.type=PS5VK_DRAW;
    state.runtime=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=2,.fragment_count=2,
        .base_vertex_slot=0,.start_instance_slot=UINT32_MAX,.lds_slot=1,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    cursor=commands;calls=0;state.runtime.lds_slot=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)!=VK_SUCCESS);
    assert(cursor==commands && !calls);
    state.runtime.lds_slot=2;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)!=VK_SUCCESS);
    assert(cursor==commands && !calls);
    state.runtime.lds_slot=1;state.sh_count=17;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0)!=VK_SUCCESS);
    assert(cursor==commands && !calls);
}
