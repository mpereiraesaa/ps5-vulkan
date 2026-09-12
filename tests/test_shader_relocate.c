#include "shader_relocate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t word(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int main(void)
{
    unsigned char original[1024], image[1024], saved[1024]; memset(original, 0x5a, sizeof(original));
    struct ps5vk_shader_relocation r[] = {{0, 256, 32, 1}, {4, 256, 32, 2}};
    memcpy(image, original, sizeof(image));
    assert(!ps5vk_shader_relocate(image, sizeof(image), 0x1ffffff00, r, 2));
    assert(word(image) == 32 && word(image + 4) == 2); /* Carry into high half. */
    assert(!memcmp(image + 8, original + 8, sizeof(image) - 8));
    assert(!ps5vk_shader_relocate(image, sizeof(image), 0x300000000, r, 2));
    assert(word(image) == 288 && word(image + 4) == 3); /* Uses stored addends, not patched data. */
    memcpy(saved, image, sizeof(image)); r[1].type = 3;
    assert(ps5vk_shader_relocate(image, sizeof(image), 0x100000000, r, 2));
    assert(!memcmp(saved, image, sizeof(image))); r[1].type = 2;
    r[1].offset = 0; assert(ps5vk_shader_relocate(image, sizeof(image), 0x100000000, r, 2));
    r[1].offset = 4; r[1].addend = UINT32_MAX;
    assert(ps5vk_shader_relocate(image, sizeof(image), 0x100000000, r, 2));
    r[1].addend = 32; r[1].offset = 1024;
    assert(ps5vk_shader_relocate(image, sizeof(image), 0x100000000, r, 2));
    assert(!memcmp(saved, image, sizeof(image)));
    puts("Shader image relocation: pass (host addresses, not GPU execution)");
}
