/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Programmable graphics stage contract shared by the SPIR-V interface policy,
 * the graphics program key and the native metadata adapter. It carries the
 * profile's own declarations, not Vulkan conformance claims: a value is here
 * because a bounded implementation exists for it, and everything else stays
 * fail-closed.
 */
#ifndef PS5VK_GRAPHICS_STAGES_H
#define PS5VK_GRAPHICS_STAGES_H
#include <vulkan/vulkan_core.h>

/* Distance limits for the pre-raster stage.
 *
 * The gfx1013 pre-raster stage exports at most two packed position registers
 * past POS0, and the pinned compiler writes clip and cull distances into those
 * two registers four components at a time (third_party/psbc-reference,
 * src/amd/common/nir/ac_nir_prerast_utils.c: the two CCDIST exports). The
 * combined count is therefore the binding bound, not just each feature's own
 * Vulkan floor: a module that declares nine distances, or eight clip plus one
 * cull, cannot be delivered by this profile and is refused instead of being
 * silently truncated. Each value below is the Vulkan 1.0 mandatory floor, which
 * is also the widest declaration the two exported registers can carry. */
enum {
    PS5VK_MAX_CLIP_DISTANCES = 8,
    PS5VK_MAX_CULL_DISTANCES = 8,
    PS5VK_MAX_COMBINED_CLIP_CULL_DISTANCES = 8,
    /* Vulkan's mandatory floor for maxTessellationPatchSize. A tessellation
     * pipeline's patch control points are validated against it even while the
     * stage itself is refused for want of a loadable compiler package. */
    PS5VK_MAX_PATCH_CONTROL_POINTS = 32
};

#endif
