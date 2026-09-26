/* The transform feedback session layout and packets (native/xfb_ps5.h,
 * DXVK262-T14): the five-record table the capture program reads, and the
 * CP DMA dword copy that loads and stores the counters. */
#include "xfb_ps5.h"
#include <assert.h>

int main(void)
{
    /* The control block the compiler's global streamout lowering addresses:
     * offsets at 0, generated primitives at 16, emitted at 32. */
    assert(PS5VK_XFB_CONTROL_OFFSET == 0 && PS5VK_XFB_GENERATED_OFFSET == 16 &&
           PS5VK_XFB_EMITTED_OFFSET == 32 && PS5VK_XFB_CONTROL_BYTES == 64);
    assert(PS5VK_XFB_TABLE_OFFSET + 16 * PS5VK_XFB_TABLE_RECORDS <= PS5VK_XFB_SESSION_BYTES &&
           PS5VK_XFB_TABLE_OFFSET % 16 == 0);

    uint32_t table[20];
    const uint64_t address[4] = {UINT64_C(0x212345600), 0, UINT64_C(0x2abcdef00), 0};
    const uint32_t bytes[4] = {320, 0, 64, 0};
    assert(ps5vk_xfb_table(table, address, bytes, UINT64_C(0x200001000)));
    assert(table[0] == 0x12345600u && table[1] == 2u && table[2] == 320u &&
           table[3] == PS5VK_XFB_RECORD_FORMAT);
    /* An unbound slot is the null record: zero range, nothing written. */
    for (unsigned w = 4; w < 8; ++w) assert(!table[w]);
    assert(table[8] == 0xabcdef00u && table[9] == 2u && table[10] == 64u);
    assert(table[16] == 0x00001000u && table[17] == 2u && table[18] == 64u &&
           table[19] == PS5VK_XFB_RECORD_FORMAT);
    /* An unaligned base or range, or an address past 48 bits, is refused. */
    uint32_t record[4];
    assert(!ps5vk_xfb_record(record, UINT64_C(0x200000002), 16));
    assert(!ps5vk_xfb_record(record, UINT64_C(0x200000000), 18));
    assert(!ps5vk_xfb_record(record, UINT64_C(1) << 48, 16));
    assert(!ps5vk_xfb_record(record, 0, 16));

    uint32_t copy[PS5VK_XFB_DMA_WORDS];
    assert(ps5vk_xfb_copy_dword(copy, UINT64_C(0x2000000a0), UINT64_C(0x123400004)));
    assert(copy[0] == 0xc0055000u && copy[1] == 0xe0300000u &&
           copy[2] == 0xa0u && copy[3] == 2u && copy[4] == 0x23400004u && copy[5] == 1u &&
           copy[6] == 4u);
    assert(!ps5vk_xfb_copy_dword(copy, 2, 8) && !ps5vk_xfb_copy_dword(copy, 0, 8) &&
           !ps5vk_xfb_copy_dword(copy, 8, UINT64_C(1) << 48));
    assert(PS5VK_XFB_TICKET_OFFSET == 48 && PS5VK_XFB_UNORDERED_OFFSET == 52 &&
           PS5VK_XFB_UNORDERED_OFFSET + 4 <= PS5VK_XFB_CONTROL_BYTES);
    assert(ps5vk_xfb_zero_dword(copy, UINT64_C(0x200000030)));
    assert(copy[0] == 0xc0055000u && copy[1] == 0xc0300000u && !copy[2] && !copy[3] &&
           copy[4] == 0x30u && copy[5] == 2u && copy[6] == 4u);
    assert(!ps5vk_xfb_zero_dword(copy, 2) && !ps5vk_xfb_zero_dword(copy, 0));
    return 0;
}
