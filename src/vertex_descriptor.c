#include "vertex_descriptor.h"
#include <string.h>
int ps5vk_vertex_descriptor(uint32_t out[4],uint64_t address,uint64_t bytes,
                            uint32_t stride,uint32_t attribute_extent)
{
    const uint64_t limit=UINT64_C(1)<<48;
    if(!out || !address || address%4 || address>=limit || !bytes || bytes>limit-address ||
       !stride || stride>0x3fff || stride%4 || !attribute_extent || attribute_extent%4 ||
       attribute_extent>stride || bytes<stride || bytes/stride>UINT32_MAX)return -1;
    uint32_t words[4]={(uint32_t)address,(uint32_t)(address>>32)|(stride<<16),
                      (uint32_t)(bytes/stride),UINT32_C(0x11014fac)};
    /* Gears' validated gfx1013 typed/structured SRD control word. NUM_RECORDS
     * is a record count, unlike the stride-zero compute byte-span descriptor.
     * Typed load formats/attribute offsets come from the compiled VS. */
    memcpy(out,words,sizeof(words));return 0;
}
int ps5vk_vertex_table_address(uint64_t shader_address,uint64_t table_address,uint32_t table_bytes)
{
    const uint64_t limit=UINT64_C(1)<<48;
    return shader_address && shader_address<limit && table_address && table_address<limit &&
        table_address%16==0 && table_bytes && table_bytes%16==0 && table_bytes<=limit-table_address &&
        (shader_address>>32)==(table_address>>32) &&
        (shader_address>>32)==((table_address+table_bytes-1)>>32);
}
