#ifndef PS5VK_SCENE_GEOMETRY_H
#define PS5VK_SCENE_GEOMETRY_H
#include <stdint.h>
enum { PS5VK_SCENE_VERTICES=48, PS5VK_SCENE_INDICES=72 };
struct ps5vk_scene_vertex { float position[3], uv_angle[3]; };
struct ps5vk_scene_draw { uint32_t count, first_index; int32_t base_vertex; };
/* Diagnostic partition only: both plans consume the same ordered 72 indices. */
unsigned ps5vk_scene_draws(int split, struct ps5vk_scene_draw out[2]);
/* Static world-space mesh, independent face UVs. Rotation/projection are shader
 * operations; this helper only duplicates the input angle per vertex. */
int ps5vk_scene_geometry(struct ps5vk_scene_vertex *,uint16_t *,float angle);
/* Diagnostic mesh only: first 3 vertices/indices form a plane whose scene3d
 * shader projection spans (384,216), (1536,216), (960,864) at 1920x1080.
 * Remaining storage is zeroed; caller must draw exactly 3 indices. */
int ps5vk_scene_probe_triangle(struct ps5vk_scene_vertex *,uint16_t *);
enum { PS5VK_SAMPLER_PROBE_CASES=13 };
/* Constant UV ladder, expressed exactly in 1/8192 normalized units. */
int ps5vk_scene_sampler_uv(unsigned test,float *u,int *numerator);
#endif
