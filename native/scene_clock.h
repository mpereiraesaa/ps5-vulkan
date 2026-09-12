#ifndef PS5VK_SCENE_CLOCK_H
#define PS5VK_SCENE_CLOCK_H
#include <stdint.h>
/* Bounded phase avoids growing float angles and depends on elapsed time, not
 * FPS. Integer modulo precedes conversion, including after long uptimes. */
static inline float ps5vk_scene_angle(uint64_t elapsed_ns)
{
    return (float)(elapsed_ns % UINT64_C(6000000000)) *
           (6.2831853071795864769f / 6000000000.0f);
}
static inline unsigned ps5vk_scene_pattern(uint64_t elapsed_ns)
{
    return (unsigned)((elapsed_ns / UINT64_C(2000000000)) % 3);
}
#endif
