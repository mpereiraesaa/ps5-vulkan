#include "compute_check.h"
#include <assert.h>

int main(void)
{
    struct ps5vk_compute_buffer input, output;
    ps5vk_compute_prepare(&input, &output);
    assert(ps5vk_compute_expected(0) == 7);
    assert(ps5vk_compute_expected(1) == 4);
    struct ps5vk_compute_result r = ps5vk_compute_check(&input, &output);
    assert(r.output_errors == PS5VK_ELEMENTS);
    assert(!r.input_errors && !r.guard_errors);
    /* This is an oracle test, explicitly not a GPU execution test. */
    for (size_t i = 0; i < PS5VK_ELEMENTS; ++i)
        output.values[i] = (uint32_t)((uint64_t)input.values[i] * 3 + 7);
    r = ps5vk_compute_check(&input, &output);
    assert(!r.output_errors && r.first_output_error == SIZE_MAX);
    for (size_t i = 0; i < PS5VK_ELEMENTS; ++i) {
        output.values[i] ^= 1;
        r = ps5vk_compute_check(&input, &output);
        assert(r.output_errors == 1 && r.first_output_error == i);
        output.values[i] ^= 1;
        input.values[i] ^= 1;
        assert(ps5vk_compute_check(&input, &output).input_errors == 1);
        input.values[i] ^= 1;
    }
    for (size_t i = 0; i < PS5VK_GUARD_WORDS; ++i) {
        uint32_t *words[] = {&input.before[i], &input.after[i],
                            &output.before[i], &output.after[i]};
        for (size_t j = 0; j < 4; ++j) {
            *words[j] ^= 1;
            assert(ps5vk_compute_check(&input, &output).guard_errors == 1);
            *words[j] ^= 1;
        }
    }
    return 0;
}
