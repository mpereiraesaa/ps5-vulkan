/* Host diagnostic: expose compiler-produced metadata before native packaging.
 * Success means compilation only, never GPU execution or compatible AGC ABI. */
#include "libpsbc/psbc_compile.h"
#include "runtime_shader.h"
#include <stdio.h>
#include <stdlib.h>

static int inspect(const char *path, PsbcStage stage)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length < 20 || length > 16 * 1024 * 1024 || length % 4 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    uint32_t *words = malloc((size_t)length);
    if (!words) { fclose(file); return 1; }
    size_t got = fread(words, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) { free(words); return 1; }
    PsbcCompileOptions options = {
        .target = PSBC_TARGET_PS5, .stage = stage, .entrypoint = "main",
        .optimise = true, .ngg = stage == PSBC_STAGE_VERTEX, .address32_hi = 2,
        .primitive_type = 4, .rasterization_samples = 1,
        /* This diagnostic's fragment source does not read PrimitiveId. */
        .omit_implicit_primitive_id = true,
    };
    PsbcShaderOutput output = {0};
    PsbcResult result = psbc_compile_shader(words, (size_t)length, &options, &output);
    free(words);
    printf("stage=%u result=%s code_bytes=%zu\n", (unsigned)stage,
           psbc_result_string(result), output.machine_code_size);
    if (result != PSBC_RESULT_OK) { psbc_free_output(&output); return 1; }
    const PsbcShaderMetadata *m = &output.metadata;
    printf("metadata=%u hardware_stage=%u unresolved=%x linkage=%u address_hi=%u "
           "user_sgprs=%u scratch=%u lds_layout=%u\n", m->version,
           (unsigned)m->hardware_stage, m->unresolved_fields, m->linkage_valid,
           m->address32_hi, m->user_sgpr_count, m->scratch_size_per_thread,
           m->ngg_lds_layout_valid);
    printf("user_data lds_slot=%u lds_value=%u base_vertex_valid=%u base_vertex_slot=%u "
           "instance_valid=%u instance_slot=%u\n",m->ngg_lds_layout_user_data_dword,
           m->ngg_lds_layout,m->base_vertex_valid,m->base_vertex_user_data_dword,
           m->start_instance_valid,m->start_instance_user_data_dword);
    for (unsigned i = 0; i < m->context_register_count; ++i)
        printf("cx[%u]=%04x:%08x\n", i, m->context_registers[i].offset, m->context_registers[i].value);
    for (unsigned i = 0; i < m->shader_register_count; ++i)
        printf("sh[%u]=%04x:%08x\n", i, m->shader_registers[i].offset, m->shader_registers[i].value);
    for (unsigned i = 0; i < m->input_semantic_count; ++i)
        printf("input[%u]=%08x\n", i, m->input_semantics[i]);
    for (unsigned i = 0; i < m->output_semantic_count; ++i)
        printf("output[%u]=%08x\n", i, m->output_semantics[i]);
    printf("link_ge=%04x:%08x stages=%04x:%08x user_vgpr=%04x:%08x\n",
           m->linkage_ge_cntl.offset, m->linkage_ge_cntl.value,
           m->linkage_stages_en.offset, m->linkage_stages_en.value,
           m->linkage_user_vgpr_en.offset, m->linkage_user_vgpr_en.value);
    struct ps5vk_runtime_shader arena;
    int package_result=ps5vk_runtime_shader_build(&arena,&output);
    printf("native_header_result=%d bytes=%zu\n",package_result,sizeof(arena));
    psbc_free_output(&output);
    return package_result!=0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s vertex.spv fragment.spv\n", argv[0]); return 2; }
    psbc_init();
    int vertex = inspect(argv[1], PSBC_STAGE_VERTEX);
    int fragment = inspect(argv[2], PSBC_STAGE_FRAGMENT);
    psbc_shutdown();
    return vertex || fragment;
}
