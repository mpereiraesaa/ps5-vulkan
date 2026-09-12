#include "compute_check.h"

static const uint32_t guard = UINT32_C(0xa59c37e1);
uint32_t ps5vk_compute_input(size_t i)
{
    /* Includes zero, UINT32_MAX and many wraparound cases. */
    if (i == 0) return 0;
    if (i == 1) return UINT32_MAX;
    return (uint32_t)i * UINT32_C(0x9e3779b9);
}
uint32_t ps5vk_compute_expected(size_t i)
{
    return ps5vk_compute_input(i) * UINT32_C(3) + UINT32_C(7);
}
void ps5vk_compute_prepare(struct ps5vk_compute_buffer *input,
                          struct ps5vk_compute_buffer *output)
{
    for (size_t i = 0; i < PS5VK_GUARD_WORDS; ++i) {
        input->before[i] = input->after[i] = guard;
        output->before[i] = output->after[i] = guard;
    }
    for (size_t i = 0; i < PS5VK_ELEMENTS; ++i) {
        input->values[i] = ps5vk_compute_input(i);
        /* Guaranteed mismatch if the GPU never writes this element. */
        output->values[i] = ~ps5vk_compute_expected(i);
    }
}
struct ps5vk_compute_result ps5vk_compute_check(
    const struct ps5vk_compute_buffer *input,
    const struct ps5vk_compute_buffer *output)
{
    struct ps5vk_compute_result result = {0, 0, 0, SIZE_MAX};
    for (size_t i = 0; i < PS5VK_ELEMENTS; ++i) {
        if (input->values[i] != ps5vk_compute_input(i)) ++result.input_errors;
        if (output->values[i] != ps5vk_compute_expected(i)) {
            if (!result.output_errors) result.first_output_error = i;
            ++result.output_errors;
        }
    }
    for (size_t i = 0; i < PS5VK_GUARD_WORDS; ++i) {
        result.guard_errors += input->before[i] != guard;
        result.guard_errors += input->after[i] != guard;
        result.guard_errors += output->before[i] != guard;
        result.guard_errors += output->after[i] != guard;
    }
    return result;
}
