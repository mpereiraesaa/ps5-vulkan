#include "dispatch_encode.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    struct ps5vk_compiled_program p = {.gfx = 1013, .code_words = 80, .wave_size = 32,
        .local_size = {64, 1, 1}, .vgprs = 3, .sgprs = 10, .float_mode = 192,
        .mem_ordered = 1, .user_sgprs = 2, .tg_size = 1, .tgid = {1, 1, 1},
        .descriptor_count = 2, .descriptors = {{0, 1, 0, 0}, {0, 0, 0, 4}}};
    struct ps5vk_dispatch_encoding d = {.program = &p,
        .addresses = {0x100004000, 0x100008000, 0x200000000, 0x200000040},
        .groups = {16, 1, 1}, .completion_value = PS5VK_COMPLETION_VALUE};
    uint32_t original[96] = {0}, result[96] = {0}, saved[96];
    size_t n = ps5vk_compute_commands(original, 96, &d.addresses);
    assert(n == ps5vk_dispatch_encode(result, 96, &d));
    assert(!memcmp(original, result, n * 4)); /* Exact bootstrap compute profile regression. */
    p.local_size[0] = 8; p.local_size[1] = 4; p.local_size[2] = 2;
    p.vgprs = 17; p.ieee_mode = 1; p.tidig_components = 2; p.tgid[2] = 0;
    d.groups[0] = 3; d.groups[1] = 7; d.groups[2] = 2;
    d.completion_value = UINT64_C(0x123456789abcdef0);
    assert(ps5vk_dispatch_encode(result, 96, &d) == n);
    assert(result[7] == 8 && result[8] == 4 && result[9] == 2);
    assert(result[16] == (0x400c0000u | (1u << 23) | 2));
    assert(result[17] == ((0x784u & ~(1u << 9)) | (2u << 11)));
    assert(result[43] == 3 && result[44] == 7 && result[45] == 2);
    assert(result[n - 3] == 0x9abcdef0 && result[n - 2] == 0x12345678);
    assert(result[n - 7] == 0x0070f528); /* No legacy GCR regression. */
    d.groups[0] = 0; assert(ps5vk_dispatch_encode(result, 96, &d) == n && result[43] == 0);
    memcpy(saved, result, sizeof(saved));
    d.completion_value = 0; assert(!ps5vk_dispatch_encode(result, 96, &d));
    d.completion_value = 2; d.groups[0] = 65536; assert(!ps5vk_dispatch_encode(result, 96, &d));
    d.groups[0] = 1; p.vgprs = 257; assert(!ps5vk_dispatch_encode(result, 96, &d));
    p.vgprs = 3; d.addresses.descriptor_table = d.addresses.code;
    assert(!ps5vk_dispatch_encode(result, 96, &d));
    d.addresses.descriptor_table = 0x100008000; d.addresses.code = 0x1ffffff00;
    assert(!ps5vk_dispatch_encode(result, 96, &d)); /* ISA crosses high-address window. */
    d.addresses.code = 0x100004000; d.addresses.readback = d.addresses.completion;
    assert(!ps5vk_dispatch_encode(result, 96, &d));
    assert(!memcmp(saved, result, sizeof(saved)));

    /* Verify PSBC / ACO GFX10.3 ABI: user_sgprs = 3, wgp_mode = 1 */
    p.user_sgprs = 3;
    p.wgp_mode = 1;
    d.addresses.readback = 0x200000040;
    uint32_t psbc_result[96] = {0};
    assert(!ps5vk_dispatch_encode(psbc_result, 96, &d)); /* PSBC address32_hi is 2. */
    d.addresses.code = 0x200004000;
    d.addresses.descriptor_table = 0x200008000;
    d.completion_value = PS5VK_COMPLETION_VALUE;
    size_t n_psbc = ps5vk_dispatch_encode(psbc_result, 96, &d);
    assert(n_psbc == n + 1);
    assert((psbc_result[16] & (1u << 29)) != 0); /* wgp_mode enabled */
    assert((psbc_result[17] & 0x7e) == (3u << 1)); /* user_sgprs = 3 */
    assert(psbc_result[24] == 0xc0037600); /* sh 0xb900 count 3 */
    assert(psbc_result[25] == 0x240);
    assert(psbc_result[26] == 0); /* s0 */
    assert(psbc_result[27] == 0); /* s1 */
    assert(psbc_result[28] == (uint32_t)d.addresses.descriptor_table); /* s2 */
    /* Check completion value and trailer */
    assert(psbc_result[n_psbc - 3] == (uint32_t)PS5VK_COMPLETION_VALUE);
    assert(psbc_result[n_psbc - 2] == (uint32_t)(PS5VK_COMPLETION_VALUE >> 32));

    puts("Parameterized dispatch encoding: pass (host packets only)");
}
