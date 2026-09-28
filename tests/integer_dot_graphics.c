/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host graphics compiler and merged-stage resource evidence; no GPU execution.
 */
#include "runtime_graphics_compiler.h"
#include "spirv_graphics_interface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct ps5vk_graphics_module_key read_module(const char *path)
{
    struct ps5vk_graphics_module_key m = {0};
    if (!strcmp(path, "-")) return m;
    FILE *f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long bytes = ftell(f); rewind(f);
    assert(bytes >= 20 && !(bytes % 4));
    uint32_t *words = malloc((size_t)bytes); assert(words);
    assert(fread(words, 1, (size_t)bytes, f) == (size_t)bytes); fclose(f);
    m.words = words; m.word_count = (size_t)bytes / 4; m.entry = "main";
    return m;
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    struct ps5vk_set_signature set = {.count = 1};
    set.binding[0] = (struct ps5vk_binding){.count = 1, .stages = VK_SHADER_STAGE_ALL_GRAPHICS};
    set.type[0] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (unsigned b = 1; b < PS5VK_MAX_BINDINGS; ++b) set.binding[b].first = 1;
    struct ps5vk_graphics_key key = {
        .vertex = read_module(argv[1]), .tess_control = read_module(argv[2]),
        .tess_eval = read_module(argv[3]), .geometry = read_module(argv[4]),
        .fragment = read_module(argv[5]), .descriptor_set_count = 1, .descriptor_sets = &set,
        .feature_mask = PS5VK_FEATURE_GEOMETRY_SHADER | PS5VK_FEATURE_TESSELLATION_SHADER,
        .color_format = {VK_FORMAT_B8G8R8A8_UNORM}, .color_attachment_count = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .color_write_mask = {15}};
    key.topology = key.tess_control.words ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    key.patch_control_points = key.tess_control.words ? 3 : 0;
    assert(ps5vk_spirv_graphics_interface(&key));
    const void *out = NULL;
    assert(ps5vk_runtime_graphics_compile(NULL, &key, &out) == VK_SUCCESS && out);
    const struct ps5vk_runtime_graphics_program *p = out;
    assert(p->fragment.machine_code_size);
    if (key.tess_control.words) assert(p->hull.machine_code_size && p->domain.machine_code_size);
    else assert(p->vertex.machine_code_size);
    printf("%u %u %u %u\n", !!p->vertex.metadata.descriptor_set_valid[0],
           !!p->hull.metadata.descriptor_set_valid[0], !!p->domain.metadata.descriptor_set_valid[0],
           !!p->fragment.metadata.descriptor_set_valid[0]);
    ps5vk_runtime_graphics_free(NULL, out);
    free((void *)key.vertex.words); free((void *)key.tess_control.words);
    free((void *)key.tess_eval.words); free((void *)key.geometry.words); free((void *)key.fragment.words);
    return 0;
}
