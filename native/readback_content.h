#ifndef PS5VK_READBACK_CONTENT_H
#define PS5VK_READBACK_CONTENT_H
#include <stddef.h>
#include <stdint.h>
struct ps5vk_readback_content {
    uint64_t pixels,nonblack,opaque,gray128;
    uint32_t hash;
};
/* Packed RGBA8/BGRA8; no padding or uninitialized capacity may be included.
 * Statistics are observations, not an image-correctness oracle. */
static inline int ps5vk_readback_content(const void *data,size_t bytes,
    struct ps5vk_readback_content *out)
{
    if(!data || !out || !bytes || bytes%4)return -1;
    const unsigned char *p=data;
    struct ps5vk_readback_content r={.pixels=bytes/4,.hash=2166136261u};
    for(size_t i=0;i<bytes;i+=4) {
        r.nonblack+=(p[i]|p[i+1]|p[i+2])!=0;
        r.opaque+=p[i+3]==255;
        r.gray128+=p[i]==128 && p[i+1]==128 && p[i+2]==128 && p[i+3]==255;
        for(unsigned c=0;c<4;++c)r.hash=(r.hash^p[i+c])*16777619u;
    }
    *out=r;return 0;
}
#endif
