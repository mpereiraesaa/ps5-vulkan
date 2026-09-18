#include "draw_emit_ps5.h"
#include "ps5_platform.h"
#include <assert.h>
static unsigned calls, index_calls;
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
    ++index_calls;
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
    /* The UC block is the linked three registers unless the pre-raster program
     * is a merged vertex+geometry program, which adds its GE allocation. */
    struct ps5vk_draw_state state = {.cx_count = 87, .uc_count = 3, .modifier = 5};
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
        .base_vertex_slot=0,.start_instance_slot=UINT32_MAX,.draw_id_slot=UINT32_MAX,.view_index_slot=UINT32_MAX,
        .lds_slot=1,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    state.sh_count=10;cursor=commands;calls=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)==VK_SUCCESS);
    assert(commands[2]==10 && commands[3]==0x8c && commands[4]==2);
    assert(commands[5]==11 && commands[6]==0); /* No LLPC table address in base vertex. */
    assert(commands[7]==0xc && commands[8]==2 && commands[9]==0 && commands[10]==0);
    /* Shader draw-parameter contract at the packet level: a non-indexed
     * runtime draw publishes firstVertex as the base vertex and firstInstance
     * as the base instance into the compiler-declared slots. The pinned CTS
     * requires exactly this for the non-indexed base_vertex cases and for
     * base_instance, so a change here must be deliberate. */
    state.runtime=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=3,.fragment_count=2,
        .base_vertex_slot=0,.start_instance_slot=1,.draw_id_slot=UINT32_MAX,.view_index_slot=UINT32_MAX,.lds_slot=2,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    state.sh_count=10;cursor=commands;calls=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)==VK_SUCCESS);
    assert(commands[5]==11 && commands[6]==13);
    /* DrawIndex on the runtime path. The slot the compiler declared for the
     * built-in has to carry the sequence index of the draw inside its command;
     * ps5vk refuses multi-draw, so every draw it executes is the first one and
     * the value is zero. The word is written from the draw itself, not left at
     * the block's zero fill, so a collision with any other declared slot is a
     * fail-closed error rather than a silent overwrite. */
    state.runtime=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=4,.fragment_count=2,
        .base_vertex_slot=0,.start_instance_slot=1,.draw_id_slot=2,.view_index_slot=UINT32_MAX,.lds_slot=3,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    state.sh_count=10;cursor=commands;calls=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)==VK_SUCCESS);
    assert(commands[3]==0x8c && commands[4]==4);
    assert(commands[5]==11 && commands[6]==13 && commands[7]==0 && commands[8]==0);
    state.runtime.draw_id_slot=1; /* start-instance collision */
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)==
        VK_ERROR_FEATURE_NOT_PRESENT);
    assert(cursor==commands && !calls);
    state.runtime.draw_id_slot=4; /* outside the declared vertex block */
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)==
        VK_ERROR_FEATURE_NOT_PRESENT);
    assert(cursor==commands && !calls);
    /* The integer vertex-format probe uses the real runtime vertex-table ABI. */
    const struct ps5vk_runtime_draw_abi vertex_format_abi={.enabled=1,.vertex_count=3,.fragment_count=2,
        .base_vertex_slot=1,.start_instance_slot=UINT32_MAX,
        .draw_id_slot=UINT32_MAX,.view_index_slot=UINT32_MAX,
        .vertex_buffer_valid=1,.vertex_buffer_slot=0,.vertex_buffer_usage_mask=1,.lds_slot=2,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    state.runtime=vertex_format_abi;
    cursor=commands;calls=0;op.type=PS5VK_DRAW;op.instance_count=1;op.first_vertex=0;
    assert(ps5vk_native_emit_vertex_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800)==VK_SUCCESS && cursor>commands && calls);
    /* An indexed draw on the runtime path is the combination the emitter used
     * to refuse outright. It now runs through the same prepared vertex table
     * and the same index emitter as the offline path, so what this pins at the
     * packet level is the shader-visible contract: the compiler-declared
     * base-vertex slot carries the signed vertexOffset (-2, two's complement)
     * and the start-instance slot keeps its own word, while the vertex index
     * still comes from the fetched index buffer instead of a DrawIndexAuto. */
    const struct ps5vk_runtime_draw_abi indexed_abi={.enabled=1,.vertex_count=4,.fragment_count=2,
        .base_vertex_slot=2,.start_instance_slot=3,
        .draw_id_slot=UINT32_MAX,.view_index_slot=UINT32_MAX,
        .vertex_buffer_valid=1,.vertex_buffer_slot=0,.vertex_buffer_usage_mask=1,.lds_slot=1,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    const uint32_t no_tables[4]={0,0,0,0};
    state.runtime=indexed_abi;
    cursor=commands;calls=0;index_calls=0;op.type=PS5VK_DRAW_INDEXED;op.index_count=6;
    op.vertex_offset=-2;op.first_instance=3;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,no_tables,NULL,&indices,draw_index)==VK_SUCCESS && calls==6 && index_calls==1);
    assert(cursor==commands+24);
    assert(commands[3]==0x8c && commands[4]==4);
    assert(commands[5]==0x567800 && commands[6]==0);
    assert(commands[7]==UINT32_MAX-1 && commands[8]==3);
    assert(commands[13]==0xc0002f00 && commands[14]==1);
    assert(commands[15]==0xc0017a00 && commands[16]==0x20000243 && commands[17]==0);
    for(unsigned i=0;i<6;++i)assert(commands[18+i]==i);
    /* Fail-closed fetch shapes. The batch is already several packets long when
     * the index emitter looks at the fetch, so the emitter's contract is the
     * one that matters: the caller's cursor only advances on full success (it
     * discards the whole unsubmitted batch otherwise) and the index callback
     * never runs. Partial words in the discarded batch are expected. */
    struct ps5vk_index_fetch bad=indices;
    cursor=commands;calls=0;index_calls=0;bad.element_bytes=3;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,no_tables,NULL,&bad,draw_index)!=VK_SUCCESS && cursor==commands && !index_calls);
    bad=indices;bad.address=0x12340001;cursor=commands;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,no_tables,NULL,&bad,draw_index)!=VK_SUCCESS && cursor==commands && !index_calls);
    bad=indices;bad.available_count=5;cursor=commands;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,no_tables,NULL,&bad,draw_index)!=VK_SUCCESS && cursor==commands && !index_calls);
    /* Without the audited index callback the combination is refused before a
     * single word is written. */
    cursor=commands;calls=0;index_calls=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,no_tables,NULL,&indices,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    /* The offline entry point shares that emitter, so the same malformed fetch
     * is refused there without advancing its caller either. */
    cursor=commands;
    assert(ps5vk_native_emit_indexed_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x123400,0x567800,&bad,draw_index)!=VK_SUCCESS && cursor==commands && !index_calls);
    /* Vulkan zero-count draws still have no rasterization side effects. */
    op.index_count=0;cursor=commands;calls=0;index_calls=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,no_tables,NULL,&indices,draw_index)==VK_SUCCESS && cursor==commands && !calls);
    op.index_count=6;state.runtime=vertex_format_abi;
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
        0x567800,tables,NULL,NULL,NULL)==VK_SUCCESS && calls==6);
    assert(commands[8]==0xc && commands[9]==4);
    for(unsigned s=0;s<4;++s)assert(commands[10+3-s]==tables[s]);
    cursor=commands;calls=0;tables[3]=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,tables,NULL,NULL,NULL)!=VK_SUCCESS && !calls && cursor==commands);
    tables[3]=0x40000;
    state.runtime.fragment_descriptor_slot[3]=3;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0x567800,tables,NULL,NULL,NULL)!=VK_SUCCESS && !calls && cursor==commands);
    state.runtime.fragment_descriptor_slot[3]=0;
    state.runtime.vertex_buffer_valid=0;state.runtime.vertex_buffer_usage_mask=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&state,&state,sizeof(state),&op,
        0,tables,NULL,NULL,NULL)==VK_SUCCESS); /* procedural VS + sampled FS */
    state.runtime.fragment_descriptor_valid[0]=0;
    op.type=PS5VK_DRAW;
    state.runtime=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=2,.fragment_count=2,
        .base_vertex_slot=0,.start_instance_slot=UINT32_MAX,.draw_id_slot=UINT32_MAX,.view_index_slot=UINT32_MAX,
        .lds_slot=1,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    cursor=commands;calls=0;state.runtime.lds_slot=0;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)!=VK_SUCCESS);
    assert(cursor==commands && !calls);
    state.runtime.lds_slot=2;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0x123400)!=VK_SUCCESS);
    assert(cursor==commands && !calls);
    state.runtime.lds_slot=1;state.sh_count=PS5VK_DRAW_SH_CAPACITY+1;
    assert(ps5vk_native_emit_draw(&cursor,64,&state,&state,sizeof(state),&op,0)!=VK_SUCCESS);
    assert(cursor==commands && !calls);

    /* --- T02-C3b: one prepared draw re-emitted per view -------------------
     * A multiview subpass re-emits the same prepared draw once per view of its
     * view mask. What this emitter owns at the packet level is where the view's
     * layer selection lands (after the prepared target block, before the draw
     * packet), that only the words the layer actually moves are written, and
     * that the ViewIndex the vertex stage reads is that view's. The prepared
     * draw's own register block is never rewritten: the view words belong to
     * the emission that carried them.
     *
     * The targets below are the pinned builders' exact shapes, taken from the
     * tables the emitter is written against (tests/test_targets_ps5.c pins
     * those tables against targets the real builders produce). */
    struct ps5vk_target_registers color_prepared={0},color_layer={0};
    color_prepared.count=color_layer.count=PS5_COLOR_REGISTER_COUNT;
    for(unsigned i=0;i<PS5_COLOR_REGISTER_COUNT;++i) {
        color_prepared.registers[i]=(ps5_agc_register){ps5vk_color_target_offsets[i],0x1000u+i};
        color_layer.registers[i]=color_prepared.registers[i];
    }
    color_layer.registers[0].value=0x2000u;     /* 0x318: the address low word */
    color_layer.registers[10].value=0x0003u;    /* 0x390: the address high byte */
    struct ps5vk_target_registers depth_prepared={0},depth_layer={0};
    depth_prepared.count=depth_layer.count=PS5_DEPTH_REGISTER_COUNT;
    for(unsigned i=0;i<PS5_DEPTH_REGISTER_COUNT;++i) {
        depth_prepared.registers[i]=(ps5_agc_register){ps5vk_depth_target_offsets[i],0x0100u+i};
        depth_layer.registers[i]=depth_prepared.registers[i];
    }
    /* The D32 plan's constant DB_RENDER_CONTROL word at 0x200 (index 21) stays
     * identical: the layer moves the two address halves at 0x012/0x014 and
     * 0x01a/0x01c and nothing else. */
    depth_layer.registers[7].value=0x0200u;
    depth_layer.registers[9].value=0x0200u;
    depth_layer.registers[14].value=0x0004u;
    depth_layer.registers[15].value=0x0004u;
    assert(depth_prepared.registers[21].offset==0x200u &&
           depth_prepared.registers[21].value==depth_layer.registers[21].value);
    struct ps5vk_draw_state view_state={.cx_count=87,.uc_count=3,.modifier=5,.sh_count=10};
    view_state.runtime=(struct ps5vk_runtime_draw_abi){.enabled=1,.vertex_count=5,.fragment_count=2,
        .fragment_view_index_valid=1,.fragment_view_index_slot=1,
        .base_vertex_slot=0,.start_instance_slot=1,.draw_id_slot=UINT32_MAX,.view_index_slot=2,
        .vertex_buffer_valid=1,.vertex_buffer_slot=3,.vertex_buffer_usage_mask=1,
        .lds_slot=4,.lds_value=0,
        .vertex_push_slot=UINT32_MAX,.fragment_push_slot=UINT32_MAX};
    struct ps5vk_operation view_op={.type=PS5VK_DRAW,.vertex_count=3,.instance_count=1,
        .first_vertex=11,.first_instance=13};
    /* Colour only: the two colour address words, one packet each, between the
     * prepared target block and the draw packet. */
    struct ps5vk_view_emit view={.view_index=2,.prepared_color=&color_prepared,
        .view_color=&color_layer,.prepared_depth=NULL,.view_depth=NULL};
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&view,NULL,NULL)==VK_SUCCESS && calls==6);
    assert(cursor==commands+25);
    assert(commands[0]==87 && commands[1]==3 && commands[2]==10);
    assert(commands[3]==0xc0016900 && commands[4]==0x318 && commands[5]==0x2000);
    assert(commands[6]==0xc0016900 && commands[7]==0x390 && commands[8]==0x0003);
    /* The draw parameters are the ones every runtime draw publishes, with the
     * compiler-declared ViewIndex slot carrying this view rather than zero. */
    assert(commands[9]==0x8c && commands[10]==5);
    assert(commands[11]==11 && commands[12]==13 && commands[13]==2 && commands[14]==0x567800);
    assert(commands[15]==0 && commands[16]==0xc && commands[17]==2);
    assert(commands[18]==0 && commands[19]==2); /* fragment ViewIndex, same replay */
    /* Colour and depth together: the depth layer's four carried words follow,
     * and the depth target's own 0x200 word is NOT among them - the D32 shape
     * always contains it and the layer never moves it, so a multiview depth
     * draw is representable exactly as the colour one is. */
    struct ps5vk_view_emit layered={.view_index=1,.prepared_color=&color_prepared,
        .view_color=&color_layer,.prepared_depth=&depth_prepared,.view_depth=&depth_layer};
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&layered,NULL,NULL)==VK_SUCCESS && calls==6);
    assert(cursor==commands+37);
    assert(commands[3]==0xc0016900 && commands[4]==0x318 && commands[5]==0x2000);
    assert(commands[6]==0xc0016900 && commands[7]==0x390 && commands[8]==0x0003);
    assert(commands[9]==0xc0016900 && commands[10]==0x012 && commands[11]==0x0200);
    assert(commands[12]==0xc0016900 && commands[13]==0x014 && commands[14]==0x0200);
    assert(commands[15]==0xc0016900 && commands[16]==0x01a && commands[17]==0x0004);
    assert(commands[18]==0xc0016900 && commands[19]==0x01c && commands[20]==0x0004);
    /* No packet's offset word is the D32 plan's own 0x200: the layer moves the
     * address words and leaves the DB_RENDER_CONTROL word where the builder put
     * it, and the pipeline state stays the last writer of 0x200. */
    for(unsigned p=0;p<6;++p)assert(commands[4+3u*p]!=0x200u);
    assert(commands[21]==0x8c && commands[22]==5);
    /* A view whose layer is already the prepared target carries no word at all,
     * which is what keeps the multiview-disabled emission byte-identical. */
    struct ps5vk_view_emit same={.view_index=1,.prepared_color=&color_prepared,
        .view_color=&color_prepared,.prepared_depth=&depth_prepared,.view_depth=&depth_prepared};
    cursor=commands;calls=0;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&same,NULL,NULL)==VK_SUCCESS && calls==6);
    assert(cursor==commands+19);
    assert(commands[3]==0x8c && commands[4]==5 && commands[5]==11 && commands[6]==13);
    assert(commands[7]==1 && commands[8]==0x567800);
    /* Fail-closed views. Each is refused BEFORE the emission writes its first
     * word, so the caller's scratch still holds the sentinel it was filled
     * with - the historic emitter's own late failures (capacity or callback)
     * are the only ones that can leave partial words there, and those are not
     * exercised here. */
    struct ps5vk_target_registers mutated_nonaddress=depth_layer,
        mutated_owned=depth_layer,wrong_count=color_layer,arbitrary={0},arbitrary_layer={0};
    mutated_nonaddress.registers[7].value=depth_prepared.registers[7].value;  /* 0x012 back */
    mutated_nonaddress.registers[5].value=0x9999u;      /* 0x007: the pinned size word */
    mutated_owned.registers[7].value=depth_prepared.registers[7].value;
    mutated_owned.registers[9].value=depth_prepared.registers[9].value;
    mutated_owned.registers[14].value=depth_prepared.registers[14].value;
    mutated_owned.registers[15].value=depth_prepared.registers[15].value;
    mutated_owned.registers[21].value=depth_prepared.registers[21].value^1u; /* 0x200 moved */
    wrong_count.count=PS5_COLOR_REGISTER_COUNT-1u;
    arbitrary.count=arbitrary_layer.count=3;
    arbitrary.registers[0]=(ps5_agc_register){0x300u,0x00010000u};
    arbitrary.registers[1]=(ps5_agc_register){0x301u,0x0000000bu};
    arbitrary.registers[2]=(ps5_agc_register){0x310u,0x00000042u};
    arbitrary_layer=arbitrary;
    arbitrary_layer.registers[0].value=0x00030000u;
    struct ps5vk_view_emit refused=view;
    refused.view_color=&wrong_count;
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&refused,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
    refused=view;refused.prepared_color=&arbitrary;refused.view_color=&arbitrary_layer;
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&refused,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
    refused=layered;refused.view_depth=&mutated_nonaddress;
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&refused,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
    refused=layered;refused.view_depth=&mutated_owned;
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&refused,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
    refused=view;refused.view_index=PS5VK_MAX_VIEW_MASK_VIEWS;
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&refused,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
    refused=layered;refused.prepared_depth=NULL;
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&refused,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
    /* A view the caller's scratch is too small for is refused in the same
     * preflight, before the emission writes anything. */
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,20,&view_state,&view_state,sizeof(view_state),
        &view_op,0x567800,no_tables,&layered,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
    struct ps5vk_draw_state no_metadata=view_state;
    no_metadata.runtime.enabled=0;
    cursor=commands;calls=0;for(unsigned i=0;i<64;++i)commands[i]=0x5a5a5a5au;
    assert(ps5vk_native_emit_runtime_draw(&cursor,64,&no_metadata,&no_metadata,sizeof(no_metadata),
        &view_op,0x567800,no_tables,&view,NULL,NULL)!=VK_SUCCESS && cursor==commands && !calls);
    for(unsigned i=0;i<64;++i)assert(commands[i]==0x5a5a5a5au);
}
