#ifndef PS5VK_TRANSFORM_FEEDBACK_H
#define PS5VK_TRANSFORM_FEEDBACK_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* VK_EXT_transform_feedback (DXVK262-T14): the SPIR-V capture interface.
 *
 * The pinned compiler describes a capture program with at most four buffers
 * and four vertex streams (PsbcShaderMetadata streamout_* arrays), so four is
 * the width of every array below. The limits a caller passes may be narrower:
 * they are the properties the device would report, and a module that needs
 * more than they allow is outside the profile rather than malformed.
 *
 * Nothing here advertises or enables the extension. */
enum { PS5VK_XFB_ABI_BUFFERS = 4, PS5VK_XFB_ABI_STREAMS = 4 };

struct ps5vk_xfb_limits {
    uint32_t max_streams;
    uint32_t max_buffers;
    uint32_t max_buffer_data_size;
    uint32_t max_buffer_data_stride;
    uint32_t max_stream_data_size;
    /* A stream other than zero needs geometryStreams. */
    VkBool32 geometry_streams;
    /* Without it, a stage that emits to several streams must output points. */
    VkBool32 streams_lines_triangles;
};

/* What one entry point captures, in bytes. */
struct ps5vk_xfb_interface {
    uint32_t captures;               /* captured outputs and block members */
    uint32_t buffers_mask;           /* XfbBuffer indices that are written */
    uint32_t captured_streams_mask;  /* streams that own a captured output */
    uint32_t emitted_streams_mask;   /* streams a geometry stage emits to */
    uint32_t strides[PS5VK_XFB_ABI_BUFFERS];
    uint32_t buffer_stream[PS5VK_XFB_ABI_BUFFERS];
    /* The Vulkan "buffer data size": the largest Offset plus output size. */
    uint32_t buffer_data_size[PS5VK_XFB_ABI_BUFFERS];
    /* Sum of the buffer data sizes of the buffers a stream writes. */
    uint32_t stream_bytes[PS5VK_XFB_ABI_STREAMS];
};

enum {
    PS5VK_XFB_NONE = 0,        /* the entry point does not declare Xfb */
    PS5VK_XFB_CAPTURES = 1,    /* a valid capture interface inside the limits */
    PS5VK_XFB_MALFORMED = -1,  /* violates the SPIR-V/Vulkan capture rules */
    PS5VK_XFB_EXCEEDS = -2,    /* valid, but needs more than the limits allow */
};

/* Reflect the capture interface of the entry point whose result id is `entry`.
 * `words` must be a module that already passed shader-module validation (well
 * formed instruction lengths). `out` is cleared first and filled only for
 * PS5VK_XFB_CAPTURES. */
int ps5vk_xfb_reflect(const uint32_t *words, size_t count, uint32_t entry,
                      const struct ps5vk_xfb_limits *limits,
                      struct ps5vk_xfb_interface *out);
#endif
