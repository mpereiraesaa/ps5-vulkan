#ifndef PS5VK_DESCRIPTOR_CAPACITY_H
#define PS5VK_DESCRIPTOR_CAPACITY_H

/* Implementation bounds, not a claim of Vulkan minimum-limit conformance.
 * PS5VK_MAX_DESCRIPTORS is one set's capacity (maxPerSetDescriptors) and the
 * descriptor budget of one pipeline stage; PS5VK_MAX_BINDINGS matches the
 * compiler's PSBC_MAX_DESCRIPTOR_BINDINGS and the 64-bit per-set
 * used-binding masks. Set storage is sized from each layout, never from this
 * bound. Kept free of other includes so limit headers can name it. */
enum { PS5VK_MAX_BINDINGS = 64, PS5VK_MAX_DESCRIPTORS = 1024, PS5VK_MAX_SETS = 4 };

#endif
