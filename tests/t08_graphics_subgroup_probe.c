/* Host-only T08 compiler probe: compare subgroup and control machine code. */
#include "psbc_compile.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    PsbcStage stage;
    if (!strcmp(argv[1], "vertex"))
        stage = PSBC_STAGE_VERTEX;
    else if (!strcmp(argv[1], "fragment"))
        stage = PSBC_STAGE_FRAGMENT;
    else
        return 2;

    FILE *file = fopen(argv[2], "rb");
    if (!file)
        return 3;
    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return 3;
    }
    long length = ftell(file);
    if (length <= 0 || (length & 3) || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return 3;
    }
    void *spirv = malloc((size_t)length);
    if (!spirv) {
        fclose(file);
        return 4;
    }
    if (fread(spirv, 1, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(spirv);
        return 3;
    }
    fclose(file);

    /* Match the basic graphics context the shipping adapter supplies. A
     * standalone vertex compile without NGG can discard the shader output,
     * leaving the same ISA as a no-Broadcast control. */
    PsbcCompileOptions options = {0};
    options.target = PSBC_TARGET_PS5;
    options.stage = stage;
    options.entrypoint = "main";
    options.optimise = true;
    options.address32_hi = 2;
    options.primitive_type = 4; /* triangle list */
    options.rasterization_samples = 1;
    options.static_descriptor_use = true;
    options.descriptor_binding_count = 1;
    options.descriptor_bindings[0] = (PsbcDescriptorBinding){
        .set = 0, .binding = 0, .type = PSBC_DESCRIPTOR_STORAGE_BUFFER,
        .array_size = 1, .offset = 0, .stride = 16
    };
    if (stage == PSBC_STAGE_VERTEX) {
        options.ngg = true;
        options.omit_implicit_primitive_id = true;
    } else {
        options.spi_shader_col_format = 9; /* one unblended RGBA export */
    }

    PsbcShaderOutput output = {0};
    PsbcResult result = psbc_compile_shader(
        spirv, (size_t)length, &options, &output);
    unsigned long long hash = UINT64_C(1469598103934665603);
    for (size_t n = 0; n < output.machine_code_size; ++n) {
        hash ^= ((const uint8_t *)output.machine_code)[n];
        hash *= UINT64_C(1099511628211);
    }
    printf("result=%d code_bytes=%zu descriptors=%u fnv64=%016llx\n",
           result, output.machine_code_size,
           output.metadata.descriptor_binding_count, hash);
    int valid = result == PSBC_RESULT_OK && output.machine_code &&
                output.machine_code_size &&
                output.metadata.descriptor_binding_count == 1;
    psbc_free_output(&output);
    free(spirv);
    return valid ? 0 : 1;
}
