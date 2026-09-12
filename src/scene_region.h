#ifndef PS5VK_SCENE_REGION_H
#define PS5VK_SCENE_REGION_H
#include <stddef.h>
#include <stdint.h>
struct ps5vk_scene_region { uint32_t black,red,green,blue,unexpected; };
/* Only the fixture's 1920x1080, 32bpp, single-mip/sample 64KB_R_X surface.
 * Full 128x128 visible blocks; no within-block coordinate addressing. */
int ps5vk_scene_region_scan(const uint32_t *,size_t,unsigned,unsigned,
                           struct ps5vk_scene_region *);
#endif
