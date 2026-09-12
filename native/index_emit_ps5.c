#include "index_emit_ps5.h"
#include "ps5_platform.h"
VkResult ps5vk_native_emit_index(uint32_t **cursor,uint32_t capacity,
    const struct ps5vk_index_fetch *fetch,uint32_t count,uint64_t modifier,ps5vk_emit_index_fn emit)
{
    if(!cursor || !*cursor || !fetch || !emit || !modifier ||
        (fetch->element_bytes!=2 && fetch->element_bytes!=4))return VK_ERROR_UNKNOWN;
    if(!count)return VK_SUCCESS;
    const uint64_t limit=UINT64_C(1)<<48;
    if(!fetch->address || fetch->address>=limit || fetch->address%fetch->element_bytes ||
        count>fetch->available_count ||
        (uint64_t)count*fetch->element_bytes>limit-fetch->address)return VK_ERROR_UNKNOWN;
    if(capacity<9)return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *start=*cursor;
    /* Mesa GFX9+: SET_UCONFIG_REG_INDEX, VGT_INDEX_TYPE 0x3090c,
     * index selector 2. Never use legacy PKT3_INDEX_TYPE on gfx1013. */
    start[0]=0xc0017a00u;start[1]=0x20000243u;
    start[2]=fetch->element_bytes==4?1:0;
    struct ps5_agc_command_buffer writer={.bottom=start+3,.top=start+9,
        .up=start+3,.down=start+9};
    (void)emit(&writer,count,(const void *)(uintptr_t)fetch->address,modifier);
    /* Xash3D's audited writer contract: exactly six DWORDs. */
    if(writer.up!=start+9 || writer.down!=start+9)return VK_ERROR_UNKNOWN;
    *cursor=start+9;return VK_SUCCESS;
}
