#ifndef PS5VK_RUNTIME_FRAGMENT_SHAPE_H
#define PS5VK_RUNTIME_FRAGMENT_SHAPE_H

/* The fragment export shape the compiler proved for a pipeline's pair. The
 * registers alone cannot tell the two two-export shapes apart (the pinned
 * compiler publishes the same pair for dual source and for two colour
 * targets), so the shape is the driver's own classification and travels with
 * the program and the pair. It lives in its own header because the native draw
 * state reads it, and that file must not pull the compiler headers in. */
enum ps5vk_runtime_fragment_export_shape {
    PS5VK_RUNTIME_FRAGMENT_SHAPE_SINGLE=0,
    PS5VK_RUNTIME_FRAGMENT_SHAPE_DUAL=1,
    PS5VK_RUNTIME_FRAGMENT_SHAPE_TWO_MRT=2,
    /* Only the second colour target is written: the module exports Location 1
     * alone, which the pinned compiler publishes as SPI_SHADER_COL_FORMAT=0x9
     * with CB_SHADER_MASK=0xf0 - the format nibbles follow the module's outputs
     * in declaration order while the mask names the attachment each export
     * targets. */
    PS5VK_RUNTIME_FRAGMENT_SHAPE_SECOND_MRT=3
};

#endif
