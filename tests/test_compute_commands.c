#include "compute_commands.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    uint32_t words[PS5VK_COMPUTE_COMMAND_CAPACITY];
    struct ps5vk_compute_addresses a = {0x100004000, 0x100008000, 0x200000000, 0x200000040};
    size_t n = ps5vk_compute_commands(words, PS5VK_COMPUTE_COMMAND_CAPACITY, &a);
    assert(n == 82);
    assert(words[0] == 0xc0037600 && words[1] == 0x204);
    assert(words[7] == 64 && words[8] == 1 && words[9] == 1);
    assert(words[12] == (uint32_t)(a.code >> 8) && words[13] == 0);
    assert(words[42] == 0xc0031500);
    assert(words[43] == 16 && words[46] == 0x8041);
    assert(words[47] == 0xc0004600 && words[48] == 0x407);
    assert(words[49] == 0xc0044000 && words[50] == 0x100200);
    assert(words[51] == 0xb830 / 4);
    assert(words[53] == 0x40 && words[54] == 2);
    assert(words[67] == 0xc0055000 && words[68] == 0xc0300000);
    assert(words[69] == 0 && words[71] == 0x4c && words[73] == 4);
    assert(words[n - 15] == 0xc0055000 && words[n - 8] == 0xc0064900);
    assert(words[n - 5] == 0 && words[n - 4] == 2);
    assert(words[n - 3] == 0x2468ace0 && words[n - 2] == 0x13579bdf);
    assert(words[n - 7] == 0x0070f528); /* GFX10 forward GCR writeback */
    uint32_t snapshot[PS5VK_COMPUTE_COMMAND_CAPACITY];
    memcpy(snapshot, words, sizeof(words));
    assert(!ps5vk_compute_commands(words, 1, &a));
    assert(!memcmp(snapshot, words, sizeof(words)));
    a.descriptor_table += UINT64_C(0x100000000);
    assert(!ps5vk_compute_commands(words, PS5VK_COMPUTE_COMMAND_CAPACITY, &a));
    a.descriptor_table = 0x100008001;
    assert(!ps5vk_compute_commands(words, PS5VK_COMPUTE_COMMAND_CAPACITY, &a));
    assert(!ps5vk_compute_commands(words, PS5VK_COMPUTE_COMMAND_CAPACITY, 0));
    return 0;
}
