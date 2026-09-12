#ifndef PS5VK_COMPUTE_CHECK_H
#define PS5VK_COMPUTE_CHECK_H
#include <stddef.h>
#include <stdint.h>

enum { PS5VK_ELEMENTS = 1024, PS5VK_GUARD_WORDS = 16 };
struct ps5vk_compute_buffer {
    uint32_t before[PS5VK_GUARD_WORDS];
    uint32_t values[PS5VK_ELEMENTS];
    uint32_t after[PS5VK_GUARD_WORDS];
};
struct ps5vk_compute_result {
    size_t output_errors, input_errors, guard_errors;
    size_t first_output_error;
};
uint32_t ps5vk_compute_input(size_t i);
uint32_t ps5vk_compute_expected(size_t i);
void ps5vk_compute_prepare(struct ps5vk_compute_buffer *input,
                          struct ps5vk_compute_buffer *output);
struct ps5vk_compute_result ps5vk_compute_check(
    const struct ps5vk_compute_buffer *input,
    const struct ps5vk_compute_buffer *output);
#endif
