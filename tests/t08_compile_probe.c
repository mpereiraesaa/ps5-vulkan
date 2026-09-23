/* Host-only compiler probe for T08 shader contracts. */
#include "psbc_compile.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "subgroup") && strcmp(argv[1], "ubo")))
        return 2;
    FILE *file = fopen(argv[2], "rb");
    if (!file) return 3;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 3; }
    long length = ftell(file);
    if (length <= 0 || (length & 3) || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return 3;
    }
    void *spirv = malloc((size_t)length);
    if (!spirv) { fclose(file); return 4; }
    if (fread(spirv, 1, (size_t)length, file) != (size_t)length) {
        fclose(file);
        free(spirv);
        return 3;
    }
    fclose(file);

    PsbcCompileOptions options = {0};
    options.target = PSBC_TARGET_PS5;
    options.stage = PSBC_STAGE_COMPUTE;
    options.entrypoint = "main";
    options.optimise = true;
    options.address32_hi = 2;
    options.static_descriptor_use = true;
    options.descriptor_binding_count = !strcmp(argv[1], "ubo") ? 2 : 1;
    options.descriptor_bindings[0] = (PsbcDescriptorBinding){
        .set = 0, .binding = 0, .type = options.descriptor_binding_count == 2
            ? PSBC_DESCRIPTOR_UNIFORM_BUFFER : PSBC_DESCRIPTOR_STORAGE_BUFFER,
        .array_size = 1, .offset = 0, .stride = 16
    };
    if (options.descriptor_binding_count == 2)
        options.descriptor_bindings[1] = (PsbcDescriptorBinding){
            .set = 0, .binding = 1, .type = PSBC_DESCRIPTOR_STORAGE_BUFFER,
            .array_size = 1, .offset = 16, .stride = 16
        };
    options.enable_int16 = !strcmp(argv[3], "int16");
    options.enable_int8 = !strcmp(argv[3], "int8");

    PsbcShaderOutput output = {0};
    const PsbcResult result = psbc_compile_shader(
        spirv, (size_t)length, &options, &output);
    unsigned long long hash = UINT64_C(1469598103934665603);
    for (size_t n = 0; n < output.machine_code_size; ++n) {
        hash ^= ((const uint8_t *)output.machine_code)[n];
        hash *= UINT64_C(1099511628211);
    }
    printf("result=%d code_bytes=%zu descriptors=%u fnv64=%016llx\n", result,
           output.machine_code_size, output.metadata.descriptor_binding_count,
           hash);
    if (result == PSBC_RESULT_OK && (!output.machine_code ||
        !output.machine_code_size || output.metadata.descriptor_binding_count !=
        options.descriptor_binding_count)) {
        psbc_free_output(&output);
        free(spirv);
        return 5;
    }
    psbc_free_output(&output);
    free(spirv);
    return result == PSBC_RESULT_OK ||
        result == PSBC_RESULT_UNSUPPORTED_CAPABILITY ? 0 : 1;
}
