#include "compute_commands.h"
#include <string.h>

static void sh(uint32_t *words, size_t *n, uint32_t reg,
               const uint32_t *values, size_t count)
{
    /* Native AGC direct-SH writer uses type zero, also for CS register range. */
    words[(*n)++] = UINT32_C(0xc0007600) | ((uint32_t)count << 16);
    words[(*n)++] = (reg - UINT32_C(0xb000)) / 4;
    memcpy(words + *n, values, count * sizeof(*values));
    *n += count;
}
size_t ps5vk_compute_commands(uint32_t *words, size_t capacity,
                             const struct ps5vk_compute_addresses *a)
{
    if (!words || !a || capacity < PS5VK_COMPUTE_COMMAND_CAPACITY ||
        !a->code || !a->descriptor_table || !a->completion || !a->readback ||
        (a->readback & 3u) || a->readback > (UINT64_C(1) << 48) - 16 ||
        (a->code & 255u) || (a->descriptor_table & 15u) ||
        (a->completion & 7u) || (a->code >> 48) ||
        (a->descriptor_table >> 48) || (a->completion >> 48) ||
        (a->code >> 32) != (a->descriptor_table >> 32))
        return 0;
    /* Use a local packet so rejection leaves caller memory unchanged. */
    uint32_t packet[PS5VK_COMPUTE_COMMAND_CAPACITY];
    size_t n = 0;
    const uint32_t start[] = {0, 0, 0};
    const uint32_t threads[] = {64, 1, 1};
    const uint32_t program[] = {(uint32_t)(a->code >> 8),
                               (uint32_t)(a->code >> 40)};
    /* bootstrap compute profile metadata: wave32, 3 VGPR, float mode 192, mem_ordered,
     * user SGPR=2, TGID XYZ + TG_SIZE enabled; scratch/LDS disabled.
     * SGPR allocation field is unused on GFX10. */
    const uint32_t resources[] = {UINT32_C(0x400c0000), UINT32_C(0x784)};
    const uint32_t zero[] = {0};
    const uint32_t user[] = {0, (uint32_t)a->descriptor_table};
    sh(packet, &n, 0xb810, start, 3);
    sh(packet, &n, 0xb81c, threads, 3);
    sh(packet, &n, 0xb830, program, 2);
    sh(packet, &n, 0xb848, resources, 2);
    sh(packet, &n, 0xb854, zero, 1);
    sh(packet, &n, 0xb8a0, zero, 1); /* COMPUTE_PGM_RSRC3 */
    sh(packet, &n, 0xb900, user, 2);
    /* GFX10 compute preamble: make shader destinations explicit. These
     * are shader-stage scheduling masks, not privileged hardware harvesting.
     * Hardware validation must establish this native context's behavior. */
    const uint32_t destinations[] = {UINT32_MAX, UINT32_MAX};
    const uint32_t inactive[] = {0, 0};
    const uint32_t accum[] = {0, 0, 0, 0};
    sh(packet, &n, 0xb858, destinations, 2);
    sh(packet, &n, 0xb864, inactive, 2);
    sh(packet, &n, 0xb890, accum, 4);
    packet[n++] = UINT32_C(0xc0031500); /* Native DCB DISPATCH_DIRECT */
    packet[n++] = 16; packet[n++] = 1; packet[n++] = 1;
    packet[n++] = UINT32_C(0x8041); /* native order mode + CS enable + wave32 */
    /* Wait for prior compute waves before the following cache/EOP operation.
     * Mesa ac_cmdbuf.h selects event index 4 for CS_PARTIAL_FLUSH (event 7). */
    packet[n++] = UINT32_C(0xc0004600);
    packet[n++] = UINT32_C(0x00000407);
    /* Mesa COPY_DATA: ME register source, TC_L2 destination, confirmed
     * 32-bit write. Caller owns a separate 16-byte readback region. */
    const uint32_t registers[] = {0xb830, 0xb81c, 0xb800};
    for (size_t i = 0; i < 3; ++i) {
        uint64_t destination = a->readback + i * 4;
        packet[n++] = UINT32_C(0xc0044000);
        packet[n++] = UINT32_C(0x00100200);
        packet[n++] = registers[i] / 4;
        packet[n++] = 0;
        packet[n++] = (uint32_t)destination;
        packet[n++] = (uint32_t)(destination >> 32);
    }
    /* Lab-validated DMA_DATA immediate zero, blocking ME, destination L2.
     * See lab research/gpu/AGC_DMA_DATA.md; touches only owned control word. */
    packet[n++] = UINT32_C(0xc0055000);
    packet[n++] = UINT32_C(0xc0300000);
    packet[n++] = 0;
    packet[n++] = 0;
    packet[n++] = (uint32_t)(a->readback + 12);
    packet[n++] = (uint32_t)((a->readback + 12) >> 32);
    packet[n++] = 4;
    /* GFX10 RELEASE_MEM GCR: GLM WB/INV, GLV/GL1 INV, GL2 WB/INV,
     * forward sequence. Fields from pinned Mesa pkt3.json RELEASE_MEM_OP.
     * Native gfx1013 visibility still requires measurement. */
    const uint32_t release[] = {
        UINT32_C(0xc0064900), UINT32_C(0x0070f528), UINT32_C(0x42010000),
        (uint32_t)a->completion, (uint32_t)(a->completion >> 32),
        (uint32_t)PS5VK_COMPLETION_VALUE, (uint32_t)(PS5VK_COMPLETION_VALUE >> 32), 0
    };
    memcpy(packet + n, release, sizeof(release));
    n += sizeof(release) / sizeof(*release);
    memcpy(words, packet, n * sizeof(*words));
    return n;
}
