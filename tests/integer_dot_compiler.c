/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual driver compiler adapter, with owned runtime-input integer-dot SPIR-V.
 * This proves compilation and metadata only, never GPU numerical results. */
#include "ps5vk_compiler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    assert(argc > 1);
    struct VkPipelineLayout_T layout = {.set_count = 1};
    layout.sets[0].count = 4;
    for (unsigned b = 0; b < PS5VK_MAX_BINDINGS; ++b) {
        layout.sets[0].binding[b].first = b < 4 ? b : 4;
        if (b < 4) {
            layout.sets[0].binding[b].count = 1;
            layout.sets[0].binding[b].stages = VK_SHADER_STAGE_COMPUTE_BIT;
            layout.sets[0].type[b] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
    }
    for (int file = 1; file < argc; ++file) {
        FILE *f = fopen(argv[file], "rb"); assert(f);
        assert(!fseek(f, 0, SEEK_END));
        long size = ftell(f); assert(size >= 20 && !(size % 4));
        rewind(f);
        uint32_t *words = malloc((size_t)size); assert(words);
        assert(fread(words, 1, (size_t)size, f) == (size_t)size); fclose(f);
        struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
        VkResult result = ps5vk_runtime_compile_compute(words, (size_t)size / 4,
            "main", &layout, NULL, &program, &code);
        assert(result == VK_SUCCESS && code && program.code_words);
        assert(program.gfx == 1013 && program.wave_size == 32);
        assert(program.local_size[0] == 64 && program.local_size[1] == 1 && program.local_size[2] == 1);
        assert(program.descriptor_set_mask == 1);
        unsigned used = 0;
        for (unsigned i = 0; i < program.descriptor_count; ++i) {
            assert(program.descriptors[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            assert(program.descriptors[i].set == 0 && program.descriptors[i].binding < 4);
            used |= 1u << program.descriptors[i].binding;
        }
        assert((used & 11u) == 11u);  /* Both runtime inputs and the result. */
        printf("DOT_COMPILED %s words=%zu bindings=%u\n", argv[file], program.code_words, used);
        free(code); free(words);
    }
    return 0;
}
