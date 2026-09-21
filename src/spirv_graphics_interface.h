#ifndef PS5VK_SPIRV_GRAPHICS_INTERFACE_H
#define PS5VK_SPIRV_GRAPHICS_INTERFACE_H
#include "graphics_program.h"
#include "graphics_stages.h"
/* Profile validation, NOT a complete SPIR-V validator. Accepts smooth float32
 * scalar/vector varyings at whole locations and the bounded clip/cull distance
 * declarations of graphics_stages.h; rejects unsupported interfaces. */
int ps5vk_spirv_graphics_interface(const struct ps5vk_graphics_key *);
/* Validated TCS entrypoint OutputVertices; zero on unsupported/malformed input.
 * Independent of the pipeline input patchControlPoints. */
unsigned ps5vk_spirv_tess_output_points(const struct ps5vk_graphics_module_key *);
/* Resolved stage-pair output count; rejects missing/conflicting execution modes. */
unsigned ps5vk_spirv_tess_pair_output_points(const struct ps5vk_graphics_key *);
/* Declared component widths of the gl_ClipDistance (built-in 3) and
 * gl_CullDistance (built-in 4) arrays in one pre-raster module's entry point
 * interface; zero for a built-in the stage does not declare at all.
 *
 * A declaration is not usage. A front end emits the default gl_PerVertex block
 * with unused one-element clip and cull arrays for shaders that touch neither,
 * and only the compiled metadata can say whether a distance is really written.
 * These widths therefore exist to bound what this profile will accept, not to
 * claim that a feature is consumed. Returns 0 for a module this profile cannot
 * describe, so a caller never reads half-valid counts. */
int ps5vk_spirv_stage_distance_declarations(const struct ps5vk_graphics_module_key *,
                                            unsigned *clip_distances,
                                            unsigned *cull_distances);
/* The same widths as PIXEL INPUTS: the declared component count of the
 * gl_ClipDistance (3) and gl_CullDistance (4) arrays a fragment module reads
 * from its predecessor, or zero for a built-in it does not declare. A read is
 * split by component count, so a stage that declares one array of four and uses
 * only two of its components reports four here and the pipeline still bounds it
 * against what the pre-raster stage exports.
 *
 * Like the export widths above, this is a declaration fact and not a claim that
 * the pixel stage receives the values: the native path refuses the pipeline
 * until the compiler describes the read and the AGC linker can map the
 * attribute to the exported register. Returns 0 for a module this profile
 * cannot describe. */
int ps5vk_spirv_stage_distance_reads(const struct ps5vk_graphics_module_key *,
                                     unsigned *clip_reads,unsigned *cull_reads);
/* Whether one GEOMETRY module writes gl_ViewportIndex (built-in 10): nonzero
 * when it declares the output, zero for a module that does not write it and for
 * a module this profile cannot describe.
 *
 * A declaration is what the profile can describe, not permission to use it.
 * Core Vulkan lets only a geometry stage select a viewport, and the selection
 * is meaningful only when the pipeline programs more than one bank, so the
 * adapter refuses a pipeline that writes an index unless the logical device
 * enabled multiViewport - decided on the enabled mask, never on this fact. */
int ps5vk_spirv_stage_viewport_index(const struct ps5vk_graphics_module_key *);
#endif
