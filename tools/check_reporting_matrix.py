#!/usr/bin/env python3
"""Build the spec -> reported -> evidence -> CTS matrix for device reporting.

Inputs, all of them machine-readable and pinned in this repository:

* the reported values, dumped through the public Vulkan query entry points by
  ``tools/dump_device_reporting.c`` (which builds its platform with the same
  initializer the console platform uses);
* the normative core tables in ``conformance_inventory/core_target.json``,
  extracted from the pinned Khronos Vulkan-Docs revision;
* the frozen upstream selection in ``cts/upstream/manifest.json`` for the
  applicable-CTS column.

The output is ``conformance_inventory/reporting_matrix.json``: one row per
mandatory limit and per mandatory format-table cell rule, each carrying the
Vulkan 1.0 requirement, the reported value, the evidence pointer and the verdict.
A verdict is never "pass" unless the reported value satisfies the requirement;
anything this tool cannot decide is recorded as ``not-audited`` with the reason,
never silently accepted.

Usage:
    python3 tools/check_reporting_matrix.py            # write the matrix
    python3 tools/check_reporting_matrix.py --check     # fail on drift
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

VK_IMAGE_TILING_OPTIMAL = 0
VK_IMAGE_USAGE_SAMPLED_BIT = 0x00000004
VK_IMAGE_USAGE_STORAGE_BIT = 0x00000008
VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x00000010
VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x00000020
VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x00000040
VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x00000080

ROOT = Path(__file__).resolve().parents[1]
CORE_TARGET = ROOT / "conformance_inventory/core_target.json"
MANIFEST = ROOT / "cts/upstream/manifest.json"
MATRIX = ROOT / "conformance_inventory/reporting_matrix.json"
DUMP = ROOT / "tools/dump_device_reporting.c"
PLATFORM = ROOT / "native/platform_ps5.c"
PROFILE_HEADER = ROOT / "src/device_profile_report.h"
DUMP_BINARY = ROOT / "build/tests/dump_device_reporting"

# Feature bits reported as VK_FALSE and the code path that enforces that, as a
# file plus a token the tool verifies is still present. A citation that no
# longer resolves marks the row not-audited instead of satisfied, so the matrix
# cannot keep claiming a gate that has moved or disappeared.
FEATURE_GATES = {
    "imageCubeArray": ("src/vk_image_view.c",
                       "!(d->enabled_features&PS5VK_FEATURE_IMAGE_CUBE_ARRAY)",
                       "cube-array views require the enabled feature bit"),
    "textureCompressionBC": ("src/texture_format.c",
                             "PS5VK_TEXTURE_COMPRESSION_BC_DIAGNOSTIC",
                             "BC format roles require the default-off diagnostic switch"),
    # independentBlend was promoted on 2026-09-22: the platform reports the bit,
    # the profile advertises two colour attachments, and both upstream leaves
    # that require the feature pass, so it is no longer a gated VK_FALSE report.
    "sampleRateShading": ("src/vk_graphics_pipeline.c", "m->sampleShadingEnable",
                          "sample shading state is rejected"),
    "logicOp": ("src/color_attachment_contract.c", "state->logicOpEnable",
                "logicOpEnable is rejected"),
    "depthClamp": ("src/vk_graphics_pipeline.c", "r->depthClampEnable",
                   "depthClampEnable is rejected"),
    "fillModeNonSolid": ("src/vk_graphics_pipeline.c", "r->polygonMode != VK_POLYGON_MODE_FILL",
                         "only VK_POLYGON_MODE_FILL is accepted"),
    "wideLines": ("src/vk_graphics_pipeline.c", "r->lineWidth != 1.0f",
                  "only line width 1.0 is accepted"),
    "largePoints": ("src/physical_device_profile.h",
                    "limits->pointSizeRange[0] = PS5VK_REQUIRED_POINT_SIZE;",
                    "point size is fixed at 1.0"),
    "alphaToOne": ("src/vk_graphics_pipeline.c", "m->alphaToOneEnable",
                   "alphaToOneEnable is rejected"),
    "depthBounds": ("src/vk_graphics_pipeline.c", "depth->depthBoundsTestEnable",
                    "depthBoundsTestEnable is rejected"),
    "samplerAnisotropy": ("src/vk_sampler.c", "info->anisotropyEnable",
                          "anisotropyEnable is rejected"),
    "pipelineStatisticsQuery": ("src/vk_query_pool.c", "pipelineStatisticsQuery is reported false.",
                                "pipeline statistics query pools are rejected"),
    # Both halves of the distance interface are implemented and hardware-
    # witnessed: the pre-raster export (static and dynamically indexed), and now
    # the pixel stage reading the interpolated distance - the eleven-case witness
    # verifies case 10 with expected=4096 covered=4096 foreign=0 wrong_color=0 and
    # digest f50dd9368fee6cc9, and a described read is delivered in the shipping
    # profile as well. The features stay unreported because their remaining
    # obligations are not complete: the advertised distance limits and the
    # negotiation row, and the CTS leaves that exercise the pixel read inside an
    # acceptance run (the pinned upstream clipping module gates the
    # fragment-shader-read variant on these same two features,
    # vktClippingTests.cpp requireFeatures()).
    "shaderClipDistance": ("native/runtime_graphics_compiler.c",
                           "!ps5vk_runtime_graphics_distance_reads_described(",
                           "the pre-raster export is implemented and hardware-witnessed for static "
                           "indices, and the dynamically indexed write is measured on hardware "
                           "(its image is byte-identical to the statically indexed quadrant); the "
                           "fragment stage's read of the same distances is verified on hardware too "
                           "(eleven-case witness, expected=4096 covered=4096 foreign=0 wrong_color=0, "
                           "digest f50dd9368fee6cc9) and the shipping profile delivers it; the "
                           "feature stays unreported until its distance limits and the applicable CTS "
                           "acceptance are published"),
    "shaderCullDistance": ("native/runtime_graphics_compiler.c",
                           "!ps5vk_runtime_graphics_distance_reads_described(",
                           "the pre-raster export is implemented and hardware-witnessed for static "
                           "indices, and the dynamically indexed write is measured on hardware "
                           "(its image is byte-identical to the statically indexed quadrant); the "
                           "fragment stage's read of the same distances is verified on hardware too "
                           "(eleven-case witness, expected=4096 covered=4096 foreign=0 wrong_color=0, "
                           "digest f50dd9368fee6cc9) and the shipping profile delivers it; the "
                           "feature stays unreported until its distance limits and the applicable CTS "
                           "acceptance are published"),
    "shaderResourceResidency": ("src/vk_queue.c", "VK_QUEUE_SPARSE_BINDING_BIT",
                                "no queue advertises sparse binding"),
    "sparseBinding": ("src/vk_queue.c", "VK_QUEUE_SPARSE_BINDING_BIT",
                      "no queue advertises sparse binding"),
    "sparseResidencyBuffer": ("src/vk_memory.c", "info->flags",
                              "image creation flags are rejected"),
    "sparseResidencyImage2D": ("src/vk_memory.c", "info->flags",
                               "image creation flags are rejected"),
    "sparseResidencyImage3D": ("src/vk_memory.c", "info->flags",
                               "image creation flags are rejected"),
    "sparseResidency2Samples": ("src/vk_memory.c", "info->flags",
                                "image creation flags are rejected"),
    "sparseResidency4Samples": ("src/vk_memory.c", "info->flags",
                                "image creation flags are rejected"),
    "sparseResidency8Samples": ("src/vk_memory.c", "info->flags",
                                "image creation flags are rejected"),
    "sparseResidency16Samples": ("src/vk_memory.c", "info->flags",
                                 "image creation flags are rejected"),
    "sparseResidencyAliased": ("src/vk_memory.c", "info->flags",
                               "image creation flags are rejected"),
    "variableMultisampleRate": ("src/graphics_limits.h",
                                "framebufferColorSampleCounts=VK_SAMPLE_COUNT_1_BIT",
                                "one framebuffer sample count"),
    # The gate moved and became exact: secondary command buffers now exist, so
    # the honest citation is the inheritance validation that refuses a
    # secondary claiming to inherit a query, not the old blanket rejection.
    "inheritedQueries": ("src/vk_command.c",
                         "i->occlusionQueryEnable || i->queryFlags || i->pipelineStatistics",
                         "a secondary cannot inherit a query"),
    "multiViewport": ("src/graphics_limits.h", "maxViewports=1",
                      "one viewport is accepted"),
}

# True feature bits are an executable promise, so each one needs code evidence
# and an applicable upstream oracle.  An unlisted true bit is a hard violation;
# it may never become green merely because the dump started reporting it.
ADVERTISED_FEATURES = {
    "robustBufferAccess": {
        "citations": (
            ("src/descriptor_encode.c", "out[3] = 0x31016fac;"),
            # The bound that makes the vertex fetch a bounded access moved with
            # the descriptor-window change: the descriptor now covers the whole
            # bound buffer, so a structure-of-arrays attribute is reachable, and
            # the plan is what bounds how many vertices a draw may address.
            ("src/vertex_fetch.c", "1+(bytes-fetch->attribute_extent)/fetch->stride"),
        ),
        "detail": ("raw storage/uniform descriptors carry the byte span and the "
                   "vertex plan bounds the addressable vertex count against the "
                   "bound data's extent"),
        "cts": ("dEQP-VK.info.device_mandatory_features",),
    },
    # DXVK262-T03. Each feature is reported behind its platform bit and its
    # execution path is cited; the applicable upstream leaves are the ones the
    # frozen selection accepts for it.
    "drawIndirectFirstInstance": {
        "profiles": ("graphics",),
        "citations": (
            ("src/vk_indirect.c",
             "if (command.firstInstance && !first_instance_enabled) return INVALID"),
            ("native/draw_emit_ps5.c", "ps5vk_draw_base_instance(op)"),
            ("native/platform_ps5.c", "PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE |"),
        ),
        "detail": ("a non-zero firstInstance in a resolved indirect command is delivered to "
                   "the start-instance user SGPR when the feature is enabled and refused otherwise"),
        "cts": ("dEQP-VK.draw.renderpass.shader_draw_parameters.base_instance.draw_indirect_first_instance",
                "dEQP-VK.draw.renderpass.shader_draw_parameters.base_instance.draw_indexed_indirect_first_instance"),
    },
    "multiDrawIndirect": {
        "profiles": ("graphics",),
        "citations": (
            ("src/vk_indirect.c", "VkResult ps5vk_indirect_resolve_command("),
            ("native/graphics_queue_ps5.c", "PS5VK_MULTI_DRAW_EXPANDED"),
            ("src/vk_internal.h", "PS5VK_MULTI_DRAW_INDIRECT_COUNT = 65535"),
        ),
        "detail": ("every command of a vkCmdDraw*Indirect call is resolved at the queue head and "
                   "emitted in order with its own DrawIndex over a bounded arena chain; "
                   "maxDrawIndirectCount is the core floor derived from the platform mask"),
        "cts": ("dEQP-VK.draw.renderpass.shader_draw_parameters.draw_index.draw",
                "dEQP-VK.draw.renderpass.shader_draw_parameters.draw_index.draw_instanced",
                "dEQP-VK.draw.renderpass.shader_draw_parameters.draw_index.draw_indexed",
                "dEQP-VK.draw.renderpass.shader_draw_parameters.draw_index.draw_indexed_instanced"),
    },
    "fullDrawIndexUint32": {
        "profiles": ("graphics",),
        "citations": (
            ("src/index_fetch.c", "case VK_INDEX_TYPE_UINT32:size=4;break;"),
            ("native/index_emit_ps5.c", "start[2]=fetch->element_bytes==4?1:0;"),
            ("src/graphics_limits.h", "maxDrawIndexedIndexValue=UINT32_MAX"),
        ),
        "detail": ("uint32 index buffers are fetched by the VGT at 32 bits with 64-bit host range "
                   "arithmetic and a 32-bit BaseVertex add, so the full index range is executable "
                   "and maxDrawIndexedIndexValue reports 2^32-1"),
        "cts": ("dEQP-VK.info.device_mandatory_features",),
    },
}

ADVERTISED_FEATURES["uniformBufferStandardLayout"] = {
    "citations": (
        ("native/platform_ps5.c", "PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT"),
        ("src/vk_device.c", "VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME"),
        ("src/vk_pipeline.c", "PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT"),
        ("src/spirv_ubo_layout.c", "ps5vk_spirv_validate_ubo_layout"),
    ),
    "detail": ("the Vulkan 1.0 KHR query and create route enables bounded std430 uniform "
               "buffer layout validation before compiler lowering"),
    "cts": ("dEQP-VK.ubo.single_basic_array.std430.uint.vertex",),
}

_MEMORY_MODEL_VOLATILE_CTS = tuple(
    "dEQP-VK.spirv_assembly.instruction.compute.opatomic_storage_buffer_volatile." + operation
    for operation in ("compex", "iadd", "idec", "iinc", "isub", "load", "store"))
ADVERTISED_FEATURES["vulkanMemoryModel"] = {
    "citations": (
        ("native/platform_ps5.c", "platform->supported_features |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL;"),
        ("src/vk_device.c", "VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME"),
        ("src/vk_pipeline.c", "PS5VK_FEATURE_VULKAN_MEMORY_MODEL"),
        ("src/ps5vk_compiler.c", "PS5VK_FEATURE_VULKAN_MEMORY_MODEL"),
    ),
    "detail": ("the Vulkan 1.0 KHR query and opt-in route enables VulkanKHR volatile "
               "queue-family atomics; seven unchanged upstream atomic oracles and a bounded "
               "GPU producer/consumer witness passed"),
    "cts": _MEMORY_MODEL_VOLATILE_CTS,
}

ADVERTISED_FEATURES["bufferDeviceAddress"] = {
    "citations": (
        ("native/platform_ps5.c",
         "platform->supported_features |= PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;"),
        ("src/vk_device.c", "VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME"),
        ("src/vk_memory.c", "vkGetBufferDeviceAddressKHR"),
        ("src/vk_pipeline.c", "PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS"),
        ("src/ps5vk_compiler.c", "enable_physical_storage_buffer_addresses"),
        ("src/descriptor_encode.c", "storage_image_descriptor"),
    ),
    "detail": ("the Vulkan 1.0 KHR query/create route enables bounded physical "
               "storage-buffer addressing; two unchanged original compute "
               "CTS leaves and an independent GPU address witness passed"),
    "cts": (
        "dEQP-VK.binding_model.buffer_device_address.set0.depth1.basessbo.load.nostore.single.std140.comp",
        "dEQP-VK.binding_model.buffer_device_address.set0.depth1.basessbo.load.nostore.single.std140.comp_offset_nonzero",
    ),
}

# DXVK262-T04. The applicable upstream oracle for both distance features is the
# pinned clipping module's user-defined family, which the frozen selection lists
# in full for the shapes this device can run: vertex-only, the two indexing modes
# and the fragment-stage read, with the clip counts reaching the reported
# maxClipDistances of eight. These names are generated the same way the factory
# composes them, so the matrix and cts/upstream/manifest.json cannot drift.
def _clip_distance_cts_paths() -> tuple[str, ...]:
    paths: list[str] = []
    for group in ("clip_distance", "clip_cull_distance"):
        for suffix in ("", "_dynamic_index"):
            for clip in range(1, 9):
                if group == "clip_cull_distance":
                    cull = min(8, 8 - clip)
                    leaf = f"{clip}_{cull}" if cull else str(clip)
                else:
                    leaf = str(clip)
                for read in ("", "_fragmentshader_read"):
                    paths.append(f"dEQP-VK.clipping.user_defined.{group}{suffix}.vert."
                                 f"{leaf}{read}")
    # The complementarity and misc leaves are NOT listed: the pinned binary does
    # not report them when they are filtered by the name the module's
    # construction implies (measured twice - absent from both the ps5log
    # transcript and the QPA), so they stay diagnostics in the selection and are
    # not claimed as evidence for the feature.
    return tuple(sorted(paths))


_CLIP_DISTANCE_CTS = _clip_distance_cts_paths()
_CLIP_DISTANCE_CITATIONS = (
    ("native/runtime_graphics_compiler.c",
     "ps5vk_runtime_graphics_distance_reads_described(&p->vertex.metadata,"),
    ("src/spirv_graphics_interface.c", "fs.clip_distance_reads>previous->clip_distances"),
    ("src/physical_device_profile.h",
     "limits->maxClipDistances = PS5VK_REQUIRED_CLIP_DISTANCES;"),
    ("native/platform_ps5.c", "PS5VK_FEATURE_SHADER_CLIP_DISTANCE |"),
    ("tests/test_clip_cull_witness.c", "PS5VK_CLIP_CULL_PIXEL_READ"),
)
def _geometry_cts_paths() -> tuple[str, ...]:
    """The geometry leaves the frozen selection accepts.

    Read from the manifest rather than listed twice: the matrix's
    applicable-CTS column has to be the same selection the payload runs, and a
    leaf that is demoted back to a diagnostic must disappear from the feature's
    evidence at the same time.
    """
    manifest = json.loads(MANIFEST.read_text())
    return tuple(sorted(case["path"] for case in manifest["cases"]
                        if "geometryShader" in " ".join(case.get("features_required", []))))


def _geometry_citations() -> tuple:
    return (
        ("src/vk_device.c", "PS5VK_FEATURE_GEOMETRY_SHADER"),
        ("native/platform_ps5.c", "PS5VK_FEATURE_GEOMETRY_SHADER"),
        ("src/graphics_program.h", "ps5vk_agc_primitive_needs_geometry"),
        ("src/spirv_graphics_interface.c", "ps5vk_topology_input_vertices"),
        ("src/graphics_limits.h", "limits->maxGeometryOutputVertices=256;"),
        ("native/runtime_graphics_compiler.c", "ps5vk_graphics_has_geometry(key)"),
        ("tests/test_geometry_witness.c", "PS5VK_GEOMETRY_LINES"),
    )


ADVERTISED_FEATURES["geometryShader"] = {
    "profiles": ("graphics",),
    "citations": _geometry_citations(),
    "detail": ("the merged vertex+geometry pre-raster program is compiled, packaged and run "
               "on the graphics path: the ES->GS input handoff it reads, the triangle, point "
               "and line input families under their own input assemblies, gl_InvocationID, "
               "gl_PrimitiveIDIn and the five mandatory minima are hardware-witnessed by the "
               "nineteen-case geometry witness (shipping-mode run 20260917T183522678Z, "
               "strict_verified, cases=19, gpu_readback, lifecycle_ok, including case 15 with "
               "expected=1024 covered=1024 and the points/lines cases at 218/218 and 467/467), "
               "and the profile reports the five geometry limits at the Vulkan floor this "
               "profile exercised in src/graphics_limits.h"),
    "cts": _geometry_cts_paths(),
}
ADVERTISED_FEATURES["tessellationShader"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/tess_profile.h", "PS5VK_FEATURE_TESSELLATION_SHADER"),
        ("native/platform_ps5.c", "ps5vk_native_tess_profile("),
        ("tools/dump_device_reporting.c", "ps5vk_native_tess_profile(platform)"),
    ),
    "detail": ("default native runtime-graphics profile: linked LS/HS and TES/TES+GS "
               "execution and eight limits are independently hardware-witnessed; "
               "the default integrated candidate passes 403 original upstream cases "
               "with strict identity and clean closure (TESSELLATION_STATUS.md). "
               "This host query is not GPU evidence or Vulkan conformance."),
    # The integrated receipt is documented separately; do not pretend it is
    # already part of the historical frozen canonical selection.
    "cts": (),
}
ADVERTISED_FEATURES["shaderClipDistance"] = {
    "profiles": ("graphics",),
    "citations": _CLIP_DISTANCE_CITATIONS,
    "detail": ("the pre-raster stage exports its clip distances through the packed position "
               "registers and the fragment stage reads the interpolated value; both halves are "
               "implemented, bounded by the two registers (maxClipDistances, maxCullDistances and "
               "maxCombinedClipAndCullDistances are reported at the Vulkan floor of eight) and "
               "measured on hardware by the eleven-case clip/cull witness, whose pixel-read case "
               "verifies expected=4096 covered=4096 foreign=0 wrong_color=0 with digest "
               "f50dd9368fee6cc9"),
    "cts": _CLIP_DISTANCE_CTS,
}
ADVERTISED_FEATURES["shaderCullDistance"] = {
    "profiles": ("graphics",),
    "citations": _CLIP_DISTANCE_CITATIONS,
    "detail": ("the same export and pixel-read path carries the cull distances, and the cull rule "
               "is per half-space rather than per vertex - the witness's cull cases prove a "
               "primitive is discarded only when a half-space is negative at every vertex, and "
               "the combined budget (maxCombinedClipAndCullDistances) is the same eight "
               "components"),
    "cts": _CLIP_DISTANCE_CTS,
}
ADVERTISED_FEATURES["fragmentStoresAndAtomics"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/platform_ps5.c", "PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS"),
        ("native/runtime_graphics_compiler.c", "fragment store/atomic needs both EXEC_ON_HIER_FAIL"),
        ("native/upload_commands_ps5.h", "VK_ACCESS_SHADER_WRITE_BIT"),
        ("native/fragment_store_probe.c", "PS5VK_FRAGMENT_STORE_READBACK"),
    ),
    "detail": ("fragment-stage storage writes and atomics are compiled and delivered through "
               "the descriptor table; the deterministic native witness separates a zero-write "
               "control from exactly 4096 fragment writes with 30 guard words intact, and both "
               "unchanged upstream frag_side_effects kill oracles pass in the integrated "
               "306-case hardware run"),
    "cts": (
        "dEQP-VK.rasterization.frag_side_effects.color_at_beginning.kill",
        "dEQP-VK.rasterization.frag_side_effects.color_at_end.kill",
    ),
}

ADVERTISED_FEATURES["dualSrcBlend"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/platform_ps5.c", "PS5VK_FEATURE_DUAL_SRC_BLEND"),
        ("native/runtime_graphics_compiler.c", "ps5vk_blend_equation(key->color_blend_op"),
        ("native/draw_state_ps5.c", "0x08e"),
        ("src/texture_format.c", "PS5VK_FORMAT_CAP_COLOR_ATTACHMENT_BLEND"),
        ("native/dual_source_probe.c", "PS5VK_DUAL_SOURCE_READBACK"),
    ),
    "detail": ("the fragment module's secondary export is packaged as the exact 0x44/0xff "
               "pair, the front end and the compiler refuse a SRC1 equation without the enabled "
               "feature and the proven export, the runtime serves the whole GFX1013 blend "
               "contract with partial colour write masks carried in the render-target block, and "
               "the deterministic native witness separates the primary export from the blended "
               "value; all 98 applicable upstream dual-source blend leaves passed in the "
               "404-case hardware run"),
    "cts": (
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_1mcc_max_alpha_1mca_ca_min-color_1mca_sa_rsub_alpha_s1a_dc_add-color_1mca_1mcc_min_alpha_1msa_1ms1a_add-color_s1c_da_max_alpha_dc_1msc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_1mdc_min_alpha_ca_1ms1a_min-color_o_s1a_add_alpha_s1a_ca_add-color_sas_1mca_add_alpha_1msc_sa_sub-color_sc_1msc_max_alpha_1msc_sas_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_1mdc_rsub_alpha_da_1ms1c_add-color_cc_ca_add_alpha_da_sas_max-color_z_1mcc_min_alpha_o_z_min-color_ca_s1c_add_alpha_1msc_s1a_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_1msc_sub_alpha_s1c_s1a_sub-color_cc_cc_max_alpha_sc_1msc_add-color_z_sas_sub_alpha_cc_sc_sub-color_z_1msa_min_alpha_z_dc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_ca_max_alpha_sa_cc_add-color_o_cc_min_alpha_1mda_1ms1c_max-color_z_1msa_max_alpha_1mda_da_rsub-color_sc_1mca_add_alpha_sc_1mca_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_cc_max_alpha_sas_cc_add-color_dc_1ms1a_rsub_alpha_sa_1mca_sub-color_1msc_cc_rsub_alpha_cc_o_sub-color_s1c_1msa_rsub_alpha_1mda_ca_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_da_min_alpha_1ms1c_1mda_sub-color_dc_s1a_add_alpha_ca_sas_min-color_da_1mca_rsub_alpha_da_ca_min-color_o_cc_rsub_alpha_1mca_sc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mca_s1c_rsub_alpha_1mca_o_rsub-color_1msc_s1a_rsub_alpha_da_cc_max-color_o_1msc_sub_alpha_sas_da_max-color_z_o_min_alpha_cc_dc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mcc_cc_rsub_alpha_1mdc_s1a_add-color_1msa_1msa_sub_alpha_1ms1a_ca_min-color_z_s1c_rsub_alpha_s1c_ca_sub-color_1ms1c_s1c_min_alpha_1mcc_1mdc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mcc_da_sub_alpha_s1a_z_sub-color_sas_da_max_alpha_z_1mcc_add-color_sas_da_rsub_alpha_sc_1ms1c_min-color_da_1mdc_sub_alpha_1ms1c_1msa_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mcc_s1c_max_alpha_da_sc_add-color_dc_1mcc_sub_alpha_s1a_o_sub-color_1ms1a_da_sub_alpha_cc_da_max-color_1msa_s1a_max_alpha_ca_s1a_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_1mdc_rsub_alpha_sc_da_max-color_sa_sc_rsub_alpha_sc_1ms1a_sub-color_1ms1a_sc_add_alpha_1ms1a_o_add-color_1mca_1ms1a_max_alpha_sa_1mca_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_1mdc_sub_alpha_s1a_1ms1c_rsub-color_1mca_o_rsub_alpha_1mca_ca_min-color_ca_s1c_add_alpha_dc_1ms1c_sub-color_ca_1ms1a_min_alpha_sc_sc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_1ms1a_add_alpha_1mcc_1mca_max-color_sc_dc_sub_alpha_1msc_1ms1a_max-color_1ms1a_sa_max_alpha_da_1ms1c_add-color_1ms1a_dc_max_alpha_1ms1c_s1c_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_1msa_max_alpha_1mca_z_sub-color_da_1msc_rsub_alpha_1mda_1ms1c_add-color_1msa_1mdc_max_alpha_da_sas_min-color_cc_dc_sub_alpha_1mda_sas_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_da_sub_alpha_sc_ca_add-color_1msa_z_min_alpha_1mca_1mcc_min-color_o_sa_add_alpha_1mda_dc_rsub-color_sc_1mcc_min_alpha_s1a_z_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_s1a_min_alpha_1msa_sc_sub-color_1msa_o_rsub_alpha_da_z_add-color_1msc_s1c_rsub_alpha_1mda_s1a_max-color_s1c_cc_add_alpha_sas_ca_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_sa_sub_alpha_1ms1c_1mdc_sub-color_o_1mca_add_alpha_cc_cc_add-color_s1a_cc_sub_alpha_ca_cc_min-color_cc_sas_min_alpha_sa_z_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_sc_rsub_alpha_s1c_o_max-color_da_ca_add_alpha_z_1msc_add-color_1mca_1ms1a_add_alpha_o_1mda_max-color_1ms1a_1msc_rsub_alpha_dc_sas_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mda_z_add_alpha_sas_1mca_min-color_cc_s1c_add_alpha_sc_o_sub-color_z_1mda_min_alpha_1mda_s1a_sub-color_s1c_sc_min_alpha_o_o_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1mdc_1mda_rsub_alpha_1mca_1mcc_min-color_dc_o_rsub_alpha_sa_z_add-color_1msc_da_max_alpha_1mca_1mca_sub-color_sa_1ms1a_sub_alpha_1msa_sc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1a_ca_add_alpha_1msa_cc_max-color_s1c_z_sub_alpha_sa_sc_add-color_da_da_max_alpha_s1c_cc_rsub-color_da_s1a_max_alpha_da_o_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1a_cc_max_alpha_1msa_1mca_sub-color_o_1mdc_max_alpha_1mda_1ms1c_rsub-color_sas_da_max_alpha_1msa_1msc_add-color_sc_1msc_add_alpha_sas_1ms1a_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1a_o_rsub_alpha_o_sas_max-color_z_1msa_min_alpha_dc_sc_rsub-color_sc_1mda_add_alpha_1ms1c_cc_max-color_s1a_1ms1c_max_alpha_1mca_o_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1a_o_sub_alpha_sc_z_min-color_da_o_add_alpha_1msc_sa_min-color_1mdc_1mda_sub_alpha_sas_1mdc_max-color_1mdc_1msa_max_alpha_o_1msc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1a_sa_add_alpha_o_1ms1a_max-color_sa_1ms1c_add_alpha_s1a_s1c_max-color_sc_1mdc_add_alpha_1ms1a_1mdc_sub-color_da_sa_sub_alpha_1mcc_sc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1a_sa_max_alpha_sas_sas_min-color_1ms1c_1msa_sub_alpha_1msc_o_add-color_sa_sa_rsub_alpha_cc_cc_add-color_da_da_add_alpha_s1c_da_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1a_z_sub_alpha_1mdc_s1a_min-color_1mda_1mcc_max_alpha_1msc_o_max-color_1ms1a_1mcc_min_alpha_1mcc_s1c_max-color_1mcc_1ms1a_add_alpha_sa_1mca_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1c_1mda_add_alpha_cc_1mca_min-color_da_o_sub_alpha_da_1mda_max-color_z_1mcc_sub_alpha_sc_cc_sub-color_1mca_1ms1a_max_alpha_cc_dc_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1c_1ms1c_max_alpha_1mdc_z_sub-color_sc_z_max_alpha_1ms1c_sas_sub-color_1msc_1msc_min_alpha_s1a_ca_min-color_1msc_1msc_add_alpha_ca_da_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1c_s1c_min_alpha_1ms1c_cc_add-color_sas_sas_max_alpha_1mca_dc_min-color_1msc_1ms1c_min_alpha_dc_1mdc_add-color_1mdc_s1a_rsub_alpha_o_1mda_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1ms1c_sa_rsub_alpha_1mda_s1c_sub-color_o_ca_min_alpha_sa_da_add-color_sa_da_min_alpha_s1c_s1c_max-color_z_s1a_max_alpha_1msa_cc_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msa_1mca_add_alpha_da_dc_min-color_1msa_cc_rsub_alpha_1msa_1mcc_max-color_dc_dc_add_alpha_dc_dc_min-color_1mda_1ms1a_add_alpha_sc_sa_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msa_1mca_add_alpha_dc_1ms1c_max-color_1msc_sc_sub_alpha_sa_s1c_rsub-color_o_1mcc_rsub_alpha_1mdc_s1c_rsub-color_ca_1mcc_sub_alpha_sas_1mca_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msa_1mda_max_alpha_1ms1c_o_rsub-color_1mda_s1a_rsub_alpha_1mca_sas_add-color_s1c_1mca_add_alpha_cc_ca_max-color_s1c_1mcc_max_alpha_s1a_o_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msa_1mdc_max_alpha_s1a_ca_max-color_1mda_cc_min_alpha_sas_dc_sub-color_1ms1a_sc_sub_alpha_z_dc_max-color_sc_dc_sub_alpha_s1c_o_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msc_1ms1a_add_alpha_1mdc_1msa_sub-color_dc_1ms1c_rsub_alpha_z_1mdc_sub-color_ca_1ms1c_min_alpha_sas_ca_rsub-color_1ms1c_s1c_add_alpha_z_1mda_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msc_1ms1a_sub_alpha_1mda_1mda_sub-color_1ms1a_ca_min_alpha_o_s1a_max-color_s1c_da_add_alpha_1ms1a_ca_max-color_sc_sa_add_alpha_z_o_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msc_1ms1c_sub_alpha_da_z_min-color_sa_cc_max_alpha_sc_sa_min-color_o_s1c_sub_alpha_1msa_sa_add-color_sa_1mda_rsub_alpha_cc_sc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_1msc_sas_sub_alpha_s1a_1mda_add-color_sa_1mcc_min_alpha_cc_1mcc_sub-color_dc_1ms1a_sub_alpha_1mca_z_max-color_1msc_1msa_max_alpha_sc_s1c_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_ca_1ms1a_max_alpha_da_1mda_sub-color_dc_ca_max_alpha_1msc_1msa_add-color_1mdc_1ms1a_min_alpha_1mda_1mda_min-color_1ms1c_1msc_max_alpha_1mca_1msc_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_ca_1ms1a_sub_alpha_1msa_1mca_sub-color_1msc_da_max_alpha_o_da_add-color_s1c_s1a_max_alpha_dc_1ms1a_sub-color_s1a_z_sub_alpha_1msa_1msc_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_ca_1ms1a_sub_alpha_1msa_1mdc_sub-color_1mda_sas_add_alpha_o_ca_add-color_sa_1mdc_sub_alpha_o_1mca_rsub-color_s1c_1msa_rsub_alpha_1msa_1mca_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_ca_1msa_add_alpha_dc_1ms1a_add-color_da_cc_rsub_alpha_1ms1a_s1a_max-color_sas_z_min_alpha_1mca_da_add-color_1msc_ca_min_alpha_1mdc_sc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_ca_1msa_max_alpha_s1a_1mda_sub-color_s1a_sc_add_alpha_dc_1mca_max-color_sas_s1a_add_alpha_1msa_sas_min-color_1ms1c_1msc_sub_alpha_sc_sas_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_ca_ca_rsub_alpha_1msa_s1c_rsub-color_dc_1ms1a_min_alpha_1ms1a_cc_rsub-color_ca_ca_add_alpha_s1c_sc_add-color_o_1ms1c_sub_alpha_z_1mda_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_1mca_add_alpha_o_1ms1a_sub-color_1mcc_1msc_max_alpha_1mdc_sas_sub-color_ca_1mdc_min_alpha_z_1mdc_max-color_1ms1c_1mdc_min_alpha_dc_o_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_1mcc_max_alpha_z_o_add-color_sa_s1a_max_alpha_1msa_dc_min-color_sc_cc_add_alpha_dc_1msa_sub-color_1ms1a_o_max_alpha_1ms1a_sc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_1mcc_sub_alpha_z_1mca_sub-color_sa_da_min_alpha_s1c_ca_add-color_1ms1a_sa_max_alpha_1ms1a_cc_sub-color_dc_ca_add_alpha_cc_1ms1a_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_1mdc_add_alpha_sc_1mda_add-color_sc_1mca_rsub_alpha_z_1mdc_max-color_sa_1mca_sub_alpha_sc_s1c_max-color_sas_s1a_min_alpha_da_1ms1c_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_1msc_rsub_alpha_sc_1mdc_sub-color_1ms1c_sas_sub_alpha_s1c_sas_max-color_dc_sa_sub_alpha_sa_1msa_add-color_s1c_sc_add_alpha_z_o_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_1msc_sub_alpha_z_1mcc_min-color_1msc_1ms1c_add_alpha_1mda_1mdc_sub-color_ca_sas_rsub_alpha_cc_1ms1c_max-color_1ms1c_1ms1c_rsub_alpha_da_s1a_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_ca_sub_alpha_1ms1c_da_add-color_ca_dc_sub_alpha_s1c_sc_add-color_sc_sa_min_alpha_1ms1c_1mda_min-color_1ms1c_dc_rsub_alpha_1msc_1msc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_cc_sa_sub_alpha_z_dc_rsub-color_s1a_1mdc_sub_alpha_1msc_1mdc_min-color_1mcc_ca_sub_alpha_ca_z_min-color_1mdc_s1c_min_alpha_s1c_1mdc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_da_1ms1a_rsub_alpha_da_s1a_max-color_o_sc_max_alpha_1mcc_1msc_sub-color_1msc_1mcc_max_alpha_s1c_1mca_sub-color_ca_1mcc_max_alpha_s1a_dc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_da_ca_max_alpha_da_1mdc_rsub-color_sa_1msc_sub_alpha_sc_1mca_sub-color_1ms1c_s1c_add_alpha_s1c_dc_rsub-color_da_1mda_add_alpha_s1c_1msa_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_da_o_add_alpha_1msa_1mca_sub-color_cc_1msc_min_alpha_1msa_s1a_add-color_1mca_sc_min_alpha_1msc_1ms1c_add-color_1ms1c_1mcc_add_alpha_1mdc_o_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_da_z_rsub_alpha_s1a_s1a_rsub-color_s1c_1msa_rsub_alpha_1mda_sc_add-color_cc_1mcc_min_alpha_sas_da_add-color_1mcc_1msc_sub_alpha_da_z_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_dc_1mca_min_alpha_1msa_1msc_sub-color_s1a_1msc_rsub_alpha_dc_dc_max-color_sa_1mda_sub_alpha_z_da_max-color_dc_sc_max_alpha_dc_1ms1c_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_dc_1msa_sub_alpha_1mca_da_rsub-color_z_cc_add_alpha_sa_dc_add-color_s1a_1ms1a_rsub_alpha_1mca_s1c_min-color_1mdc_z_min_alpha_sc_1mcc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_dc_ca_rsub_alpha_dc_s1a_rsub-color_cc_da_min_alpha_ca_1ms1a_max-color_1msc_1mdc_max_alpha_cc_sa_rsub-color_da_o_sub_alpha_z_dc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_1mdc_rsub_alpha_1mca_1mcc_rsub-color_1mcc_1ms1a_add_alpha_1msa_1ms1c_rsub-color_1msa_1mda_max_alpha_1msc_sa_min-color_1ms1a_sc_max_alpha_1mca_cc_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_1msc_max_alpha_1ms1a_1mca_add-color_1mdc_s1c_min_alpha_ca_dc_sub-color_1mdc_s1c_sub_alpha_z_sc_min-color_ca_1mca_rsub_alpha_s1a_1ms1a_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_ca_add_alpha_z_1msa_sub-color_z_1mcc_add_alpha_1mcc_1mca_sub-color_1msa_da_rsub_alpha_cc_1ms1a_add-color_cc_1mcc_sub_alpha_1mda_1ms1c_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_cc_add_alpha_o_s1c_add-color_1mdc_1mcc_min_alpha_1ms1a_1mcc_sub-color_sas_1msa_sub_alpha_1ms1c_1mda_add-color_1msa_o_add_alpha_dc_sc_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_cc_min_alpha_da_sc_max-color_1mda_s1a_add_alpha_da_1mda_rsub-color_dc_s1a_rsub_alpha_da_1mcc_rsub-color_cc_dc_min_alpha_1msa_sas_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_cc_min_alpha_sas_o_min-color_o_1msa_add_alpha_1mdc_s1a_max-color_1ms1a_1msc_add_alpha_cc_1mcc_max-color_1msa_cc_max_alpha_sas_da_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_sa_max_alpha_da_ca_add-color_z_1ms1c_add_alpha_sc_sas_rsub-color_1mdc_cc_min_alpha_dc_ca_min-color_1ms1a_1msc_max_alpha_1msa_ca_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_o_sas_rsub_alpha_1msc_1mcc_rsub-color_z_s1a_sub_alpha_da_s1c_add-color_1mda_sc_add_alpha_z_z_rsub-color_1ms1a_sc_sub_alpha_sa_1msa_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1a_1mda_max_alpha_s1c_1msa_rsub-color_ca_1ms1a_add_alpha_1mda_1msc_min-color_z_s1a_add_alpha_1mdc_1mcc_add-color_s1c_1mda_add_alpha_1ms1a_o_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1a_1ms1a_rsub_alpha_sc_dc_rsub-color_1msa_dc_sub_alpha_sc_z_min-color_da_z_add_alpha_1mdc_ca_max-color_1mcc_s1c_rsub_alpha_1ms1a_dc_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1a_1ms1c_add_alpha_1mcc_1mda_sub-color_ca_1ms1a_max_alpha_1ms1c_s1c_min-color_da_sc_sub_alpha_sc_1mcc_min-color_1mda_dc_max_alpha_ca_s1c_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1a_cc_rsub_alpha_1msc_1mcc_min-color_sc_1mdc_add_alpha_da_ca_min-color_1mcc_1mda_max_alpha_1ms1c_s1a_min-color_s1c_cc_sub_alpha_ca_1mda_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1a_da_min_alpha_1msa_1msa_min-color_da_s1a_rsub_alpha_1msc_z_add-color_ca_sc_sub_alpha_cc_s1a_max-color_1mca_1mcc_add_alpha_1msa_s1c_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1a_s1a_sub_alpha_sc_1msa_rsub-color_sc_1mcc_add_alpha_s1a_1ms1c_rsub-color_1mdc_ca_rsub_alpha_1mda_1ms1c_rsub-color_1ms1a_1msc_min_alpha_o_sas_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1a_s1c_rsub_alpha_sa_sas_max-color_z_1msa_min_alpha_sas_s1c_rsub-color_1mdc_1msa_rsub_alpha_sc_s1a_min-color_1mdc_sa_min_alpha_1mca_1mcc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1c_1mda_add_alpha_s1c_ca_add-color_1mca_z_max_alpha_dc_1mcc_max-color_sa_dc_max_alpha_1ms1c_o_sub-color_1mcc_1msc_rsub_alpha_da_1mcc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1c_1msa_rsub_alpha_ca_z_rsub-color_1ms1c_s1a_max_alpha_z_1msc_add-color_1mda_1mcc_add_alpha_1msc_1mda_max-color_1ms1c_o_max_alpha_s1a_1msc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1c_sa_min_alpha_1msc_dc_min-color_1mdc_1mca_sub_alpha_s1a_1msc_max-color_sas_ca_max_alpha_1ms1c_sas_sub-color_1msc_sas_max_alpha_1mcc_da_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_s1c_sc_rsub_alpha_1msc_1ms1a_max-color_1ms1c_1mda_rsub_alpha_z_1mcc_max-color_z_sas_sub_alpha_1ms1c_s1c_sub-color_1mdc_s1c_min_alpha_sa_1mdc_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sa_1mdc_add_alpha_cc_1ms1c_sub-color_1msa_z_max_alpha_da_1mda_rsub-color_1msa_1msc_rsub_alpha_1mcc_o_min-color_1ms1c_cc_add_alpha_dc_da_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sa_1mdc_rsub_alpha_1mda_cc_sub-color_1msc_z_max_alpha_o_s1c_sub-color_1ms1a_1msc_sub_alpha_ca_sa_sub-color_ca_ca_max_alpha_cc_s1a_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sa_cc_add_alpha_sc_sc_add-color_dc_da_max_alpha_dc_s1a_max-color_sa_1mca_sub_alpha_1mca_1ms1c_add-color_1msa_1msa_rsub_alpha_1mda_1mcc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sa_cc_rsub_alpha_o_1msa_max-color_1ms1c_dc_sub_alpha_1msa_o_min-color_sc_cc_min_alpha_sc_1msc_min-color_1msc_sa_rsub_alpha_o_z_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sas_1mdc_rsub_alpha_s1a_z_sub-color_1msc_sc_min_alpha_s1a_sc_sub-color_sas_z_max_alpha_1msc_da_min-color_s1c_dc_rsub_alpha_o_1mcc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sas_1ms1c_sub_alpha_1mda_cc_add-color_da_cc_rsub_alpha_z_1ms1a_add-color_s1c_1mcc_max_alpha_1mca_s1a_rsub-color_cc_dc_max_alpha_1mcc_s1a_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sas_s1a_add_alpha_sas_1ms1a_max-color_1msa_sas_rsub_alpha_s1a_1mca_sub-color_1mcc_1ms1a_add_alpha_sc_s1a_min-color_ca_1ms1c_max_alpha_1mca_1mcc_add",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sas_s1c_add_alpha_1mca_1mca_sub-color_1mdc_sc_max_alpha_1msa_s1c_rsub-color_1msa_1mdc_max_alpha_1mca_1mdc_max-color_s1c_ca_min_alpha_1ms1c_1msc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sas_z_max_alpha_1mcc_1msc_min-color_1msa_1msc_min_alpha_ca_s1a_add-color_1mda_1msc_max_alpha_dc_s1a_rsub-color_s1c_s1c_add_alpha_s1c_z_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sc_1mca_max_alpha_1ms1c_1mdc_sub-color_ca_1mda_sub_alpha_ca_o_rsub-color_cc_dc_add_alpha_ca_1msa_min-color_1ms1c_1mcc_max_alpha_sas_1mdc_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sc_1ms1c_rsub_alpha_1msc_s1a_rsub-color_1ms1a_1msc_max_alpha_1mda_sc_sub-color_1msa_dc_min_alpha_1msa_1mca_add-color_da_1mcc_rsub_alpha_1ms1c_sa_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sc_sa_min_alpha_cc_sc_rsub-color_1mcc_1ms1a_add_alpha_sa_da_rsub-color_1mda_sa_min_alpha_s1a_dc_sub-color_sa_z_min_alpha_sc_1mcc_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_sc_sc_add_alpha_cc_cc_add-color_1ms1c_ca_sub_alpha_1msa_1mda_max-color_da_1mdc_sub_alpha_1mdc_1mda_rsub-color_1msa_1msa_min_alpha_1mca_1ms1c_rsub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_z_1mcc_sub_alpha_1mdc_sa_sub-color_s1a_s1a_rsub_alpha_cc_z_add-color_s1c_s1a_rsub_alpha_dc_1mca_add-color_1mdc_1ms1c_max_alpha_s1a_dc_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_z_1msa_rsub_alpha_1msc_z_add-color_s1c_1ms1c_min_alpha_s1a_dc_max-color_1ms1a_o_max_alpha_1mca_dc_rsub-color_sc_dc_min_alpha_sas_1ms1a_max",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_z_s1c_min_alpha_1mcc_s1c_rsub-color_ca_1mca_add_alpha_cc_1ms1a_min-color_ca_1ms1c_rsub_alpha_sa_sas_min-color_1ms1c_s1a_add_alpha_1mda_1ms1a_min",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_z_sa_rsub_alpha_o_1ms1a_sub-color_1ms1c_1ms1c_min_alpha_sa_s1a_max-color_sa_cc_sub_alpha_sc_1mdc_min-color_o_1mca_add_alpha_da_ca_sub",
        "dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states.color_z_sc_add_alpha_1ms1c_sa_min-color_dc_1mca_add_alpha_z_1mca_max-color_1ms1c_sa_max_alpha_1mcc_sc_sub-color_s1c_1mda_add_alpha_s1c_1mda_add",
    ),
}

# DXVK262-T05, promoted 2026-09-21 on physical-console evidence. Each feature
# DXVK262-T06 independentBlend, promoted 2026-09-22. The two upstream leaves
# that require it both pass; the citations name the per-attachment contract end
# to end.
ADVERTISED_FEATURES["independentBlend"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/platform_ps5.c", "PS5VK_FEATURE_INDEPENDENT_BLEND"),
        ("native/runtime_graphics_compiler.c", "key->color_attachment_count!=PS5VK_MAX_COLOR_ATTACHMENTS"),
        ("native/draw_state_ps5.c", "ps5vk_color_attachment_offsets[attachment]"),
        ("src/color_attachment_contract.h", "PS5VK_MAX_COLOR_ATTACHMENTS = 2"),
        ("src/graphics_limits.h", "limits->maxColorAttachments=PS5VK_MAX_COLOR_ATTACHMENTS"),
        ("native/two_mrt_probe.c", "PS5VK_TWO_MRT_READBACK"),
    ),
    "detail": ("the render pass, the framebuffer, the pipeline key and the native "
               "per-target programming each carry one colour target per attachment with its "
               "own CB_COLORn block, its own blend control and its own CB_TARGET_MASK nibble, "
               "and the readback copies every attachment into its own buffer; the runtime "
               "compiler admits a two-target pipeline only when the device carries the "
               "capability, and the private two-MRT witness recorded one draw writing two "
               "attachments with different values before the upstream leaves were selected. "
               "Both attachment_write_mask attachment_count_2 suballocation leaves passed in "
               "the 464-case hardware run"),
    "cts": (
        "dEQP-VK.renderpass.suballocation.attachment_write_mask.attachment_count_2.start_index_0",
        "dEQP-VK.renderpass.suballocation.attachment_write_mask.attachment_count_2.start_index_1",
    ),
}

# DXVK262-T06 sampleRateShading, promoted 2026-09-23 on physical-console
# evidence. The pixel stage publishes the sample positions and the position at
# the iterated sample, the barrier that publishes colour to the texture path
# waits for a confirmed writeback, and the feature's own oracle passes at both
# served counts for the shapes this profile renders.
ADVERTISED_FEATURES["sampleRateShading"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/platform_ps5.c", "PS5VK_FEATURE_SAMPLE_RATE_SHADING"),
        ("native/draw_state_ps5.c", "PA_SC_AA_SAMPLE_LOCS_PIXEL"),
        ("native/draw_state_ps5.c", "POS_FLOAT_LOCATION"),
        ("src/graphics_sync.c", "ps5vk_graphics_color_to_texture_wait"),
        ("src/sample_rate_contract.h", "VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT"),
        ("native/runtime_graphics_compiler.c", "sample_shading_enable"),
    ),
    "detail": ("the raster stage programs MSAA_ENABLE, the sixteen "
               "PA_SC_AA_SAMPLE_LOCS_PIXEL_* words carrying Vulkan's standard 4x pattern and "
               "the sample distance that pattern asks for, and the pixel stage publishes "
               "SPI_BARYC_CNTL POS_FLOAT_LOCATION=2 whenever the wave iterates per sample, so "
               "fract(gl_FragCoord.xy) is the SAMPLE's position and every sample of a pixel "
               "receives a different one; the barrier that publishes a colour attachment to the "
               "texture path stores a completion token and waits for it, because the colour "
               "block writes back asynchronously and a read issued behind the bare event saw "
               "tiles it had not written yet"),
    # DXVK262-T06, promoted 2026-09-23. The feature's own oracle - the leaves whose
    # checkSupport requires DEVICE_CORE_FEATURE_SAMPLE_RATE_SHADING - passes for the
    # triangle and quad shapes at both served counts, three measured runs in a row
    # inside the 494-case acceptance selection; the point and line shapes the same
    # oracle selects are refused by this profile's pipeline resolver and stay
    # diagnostics (plain-point-line-pipeline-refused).
    "cts": tuple(sorted([
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_0.samples_2.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_0.samples_4.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_25.samples_2.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_25.samples_4.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_5.samples_2.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_5.samples_4.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_75.samples_2.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_75.samples_4.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_1_0.samples_2.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_1_0.samples_4.primitive_triangle",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_0.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_0.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_25.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_25.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_5.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_5.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_75.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_0_75.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_1_0.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_disabled.min_1_0.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_0.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_0.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_25.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_25.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_5.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_5.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_75.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_0_75.samples_4.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_1_0.samples_2.quad",
        "dEQP-VK.pipeline.monolithic.multisample.min_sample_shading_enabled.min_1_0.samples_4.quad",
    ])),
}

# DXVK262-T05, promoted 2026-09-21 on physical-console evidence. Each feature
# names the state its draw programs and the upstream leaves that exercise it,
# all of them in the frozen acceptance selection.
ADVERTISED_FEATURES["depthClamp"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/draw_state_ps5.c", "raster->depth_clamp"),
        ("src/vk_graphics_pipeline.c", "r->depthClampEnable && !(d->enabled_features & PS5VK_FEATURE_DEPTH_CLAMP)"),
    ),
    "detail": ("the draw programs PA_CL_CLIP_CNTL ZCLIP_NEAR/FAR_DISABLE from the pipeline's "
               "snapshot, so the viewport depth range becomes the clamp interval, and pipeline "
               "creation refuses depthClampEnable unless the logical device enabled the feature; "
               "all eight applicable upstream leaves pass, the six draw.renderpass ones reading "
               "their result back through the DEPTH-aspect 64KB_Z_X readback"),
    "cts": tuple(sorted(['dEQP-VK.clipping.clip_volume.depth_clamp.triangle_list', 'dEQP-VK.clipping.clip_volume.depth_clamp.triangle_strip', 'dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat', 'dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat_clamp_four_viewports', 'dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat_clamp_input_negative', 'dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat_clamp_input_positive', 'dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat_depth_bias_clamp_input_negative', 'dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat_depth_bias_clamp_input_positive'])),
}
ADVERTISED_FEATURES["depthBiasClamp"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/draw_state_ps5.c", "raster->depth_bias_enable"),
        ("src/vk_graphics_pipeline.c", "r->depthBiasClamp != 0.0f"),
    ),
    "detail": ("the polygon-offset block carries the clamp the draw snapshot recorded, and a "
               "non-zero static clamp is refused unless the logical device enabled the feature; "
               "its only two applicable upstream oracles both pass, while "
               "dynamic_state.monolithic.rs_state.depth_bias_clamp stays out for a different "
               "reason - it needs a stencil-bearing attachment format this profile does not offer"),
    "cts": tuple(sorted(['dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat_depth_bias_clamp_input_negative', 'dEQP-VK.draw.renderpass.depth_clamp.d32_sfloat_depth_bias_clamp_input_positive'])),
}
ADVERTISED_FEATURES["multiViewport"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/draw_state_ps5.c", "viewport_count"),
        ("src/vk_graphics_pipeline.c", "vp->viewportCount > 1 && !(d->enabled_features & PS5VK_FEATURE_MULTI_VIEWPORT)"),
    ),
    "detail": ("sixteen viewport and scissor banks are written per draw and selected by "
               "gl_ViewportIndex from a geometry stage, and a pipeline naming more than one "
               "viewport is refused unless the logical device enabled the feature; all "
               "twenty-two applicable upstream leaves pass, the six draw.renderpass.scissor ones "
               "clearing their target through the transfer destination and reading the rendered "
               "result back from the same image"),
    "cts": tuple(sorted(['dEQP-VK.draw.renderpass.scissor.16_dynamic_scissors', 'dEQP-VK.draw.renderpass.scissor.16_static_scissors', 'dEQP-VK.draw.renderpass.scissor.dynamic_scissor_mix', 'dEQP-VK.draw.renderpass.scissor.dynamic_scissor_out_of_order_updates', 'dEQP-VK.draw.renderpass.scissor.dynamic_scissor_updates_between_draws', 'dEQP-VK.draw.renderpass.scissor.two_static_scissors_one_quad', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_1', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_10', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_11', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_12', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_13', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_14', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_15', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_16', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_2', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_3', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_4', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_5', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_6', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_7', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_8', 'dEQP-VK.fragment_ops.scissor.multi_viewport.scissor_9'])),
}
ADVERTISED_FEATURES["fillModeNonSolid"] = {
    "profiles": ("graphics",),
    "citations": (
        ("native/draw_state_ps5.c", "hardware_polygon_type"),
        ("src/vk_graphics_pipeline.c", "!(d->enabled_features & PS5VK_FEATURE_FILL_MODE_NON_SOLID)"),
    ),
    "detail": ("POLYMODE_FRONT/BACK_PTYPE with DUAL_MODE and KEEP_TOGETHER_ENABLE program the "
               "LINE and POINT polygon modes, and any other mode, or these two without the "
               "enabled feature, are refused at pipeline creation; all twenty-eight rasterization "
               "culling leaves pass, sixteen of them the _line and _point variants. The "
               "twenty-ninth applicable leaf, the amber line_continuity one, is not runnable on "
               "this profile and not a defect in this feature: Amber demands host-coherent memory "
               "this device does not advertise"),
    "cts": tuple(sorted(['dEQP-VK.rasterization.culling.back_triangle_strip', 'dEQP-VK.rasterization.culling.back_triangle_strip_line', 'dEQP-VK.rasterization.culling.back_triangle_strip_point', 'dEQP-VK.rasterization.culling.back_triangle_strip_reverse', 'dEQP-VK.rasterization.culling.back_triangle_strip_reverse_line', 'dEQP-VK.rasterization.culling.back_triangle_strip_reverse_point', 'dEQP-VK.rasterization.culling.back_triangles', 'dEQP-VK.rasterization.culling.back_triangles_line', 'dEQP-VK.rasterization.culling.back_triangles_point', 'dEQP-VK.rasterization.culling.back_triangles_reverse', 'dEQP-VK.rasterization.culling.back_triangles_reverse_line', 'dEQP-VK.rasterization.culling.back_triangles_reverse_point', 'dEQP-VK.rasterization.culling.both_triangle_strip', 'dEQP-VK.rasterization.culling.both_triangle_strip_reverse', 'dEQP-VK.rasterization.culling.both_triangles', 'dEQP-VK.rasterization.culling.both_triangles_reverse', 'dEQP-VK.rasterization.culling.front_triangle_strip', 'dEQP-VK.rasterization.culling.front_triangle_strip_line', 'dEQP-VK.rasterization.culling.front_triangle_strip_point', 'dEQP-VK.rasterization.culling.front_triangle_strip_reverse', 'dEQP-VK.rasterization.culling.front_triangle_strip_reverse_line', 'dEQP-VK.rasterization.culling.front_triangle_strip_reverse_point', 'dEQP-VK.rasterization.culling.front_triangles', 'dEQP-VK.rasterization.culling.front_triangles_line', 'dEQP-VK.rasterization.culling.front_triangles_point', 'dEQP-VK.rasterization.culling.front_triangles_reverse', 'dEQP-VK.rasterization.culling.front_triangles_reverse_line', 'dEQP-VK.rasterization.culling.front_triangles_reverse_point'])),
}

# Every non-advertised VkPhysicalDeviceFeatures member shares one fail-closed
# device-negotiation gate.  Vulkan valid usage prevents an application from
# relying on a false feature without requesting it; the driver's obligation is
# to report it false and refuse device creation when it is requested.  The C
# regression walks every VkBool32 member, so this generic citation is stronger
# than inventing an object-level rejection branch for compiler-side features.
FALSE_CORE_FEATURE_GATE = (
    "src/vk_device.c",
    "if (!entry || !(supported & entry->bit)) return VK_ERROR_FEATURE_NOT_PRESENT;",
    "tests/test_vk_device.c",
    "for(size_t offset=0;offset<sizeof(features);offset+=sizeof(VkBool32))",
)


def evaluate_feature(name: str, value: bool, profile: str = "graphics") -> tuple[str, str]:
    advertised = ADVERTISED_FEATURES.get(name)
    # A feature reviewed for the graphics execution path is reported false by
    # the compute-only build, which has no render pass or draw at all; that
    # false report is the honest one for that profile, not a lost advertisement.
    if (advertised and not value and "profiles" in advertised and
            profile not in advertised["profiles"]):
        return ("satisfied", f"{profile}-only build: the graphics execution path this feature "
                "needs is not built into this profile")
    if value:
        if not advertised:
            return "violation", "advertised true without a reviewed implementation contract"
        missing = []
        for where, token in advertised["citations"]:
            path = ROOT / where
            if not path.is_file() or token not in path.read_text():
                missing.append(f"{where}:{token}")
        if missing:
            return "violation", "advertised implementation citations missing: " + ", ".join(missing)
        return "satisfied", advertised["detail"]
    if advertised:
        return "violation", "required reviewed feature is no longer advertised"
    if name in ("vulkanMemoryModelDeviceScope", "bufferDeviceAddress"):
        token = ("PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE" if
                 name == "vulkanMemoryModelDeviceScope" else
                 "PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS")
        source = (ROOT / "src/vk_device.c").read_text()
        if token not in source or "return VK_ERROR_FEATURE_NOT_PRESENT;" not in source:
            return "not-audited", "extension feature request gate citation is missing"
        return ("satisfied", "the KHR feature query reports false on the shipping "
                "platform and vkCreateDevice refuses a request for the unsupported bit")
    if name in FEATURE_GATES:
        where, token, reason = FEATURE_GATES[name]
        path = ROOT / where
        if not path.is_file() or token not in path.read_text():
            return "not-audited", f"citation missing: {where} no longer contains {token!r}"
        return "satisfied", f"{reason} ({where})"
    if name in FEATURE_ABSENT_FORMAT_FAMILY:
        where, tokens = FEATURE_ABSENT_FORMAT_FAMILY[name]
        text = (ROOT / where).read_text()
        present = [token for token in tokens if token in text]
        if present:
            return "violation", f"{where} mentions {present}, so the family may exist"
        return "satisfied", f"no {tokens[0]} family in {where}"
    gate_file, gate_token, test_file, test_token = FALSE_CORE_FEATURE_GATE
    missing = []
    for where, token in ((gate_file, gate_token), (test_file, test_token)):
        path = ROOT / where
        if not path.is_file() or token not in path.read_text():
            missing.append(f"{where}:{token}")
    if missing:
        return "not-audited", "generic false-feature gate citation missing: " + ", ".join(missing)
    return ("satisfied", "reported false and any request for this VkPhysicalDeviceFeatures "
            f"member is rejected before device creation ({gate_file}; exhaustive {test_file})")

# Feature bits backed by the absence of an entire format family in the format
# table rather than by an explicit rejection branch.
FEATURE_ABSENT_FORMAT_FAMILY = {
    "textureCompressionETC2": ("src/graphics_formats.h", ("VK_FORMAT_ETC2", "VK_FORMAT_EAC")),
    "textureCompressionASTC_LDR": ("src/graphics_formats.h", ("VK_FORMAT_ASTC",)),
}

# Limits whose requirement depends on an advertised feature. The values are the
# ones the pinned CTS applies when the gating feature is NOT supported
# (vktApiFeatureInfo.cpp, "unsupportedFeatureTable", lines 319-360); with the
# feature supported the base table entry applies instead. They are transcribed,
# not inferred, so `requirement_source` in the matrix can cite them.
CTS_GATED_OFF_REQUIREMENT = {
    "sparseAddressSpaceSize": ("sparseBinding", 0),
    "maxTessellationGenerationLevel": ("tessellationShader", 0),
    "maxTessellationPatchSize": ("tessellationShader", 0),
    "maxTessellationControlPerVertexInputComponents": ("tessellationShader", 0),
    "maxTessellationControlPerVertexOutputComponents": ("tessellationShader", 0),
    "maxTessellationControlPerPatchOutputComponents": ("tessellationShader", 0),
    "maxTessellationControlTotalOutputComponents": ("tessellationShader", 0),
    "maxTessellationEvaluationInputComponents": ("tessellationShader", 0),
    "maxTessellationEvaluationOutputComponents": ("tessellationShader", 0),
    "maxGeometryShaderInvocations": ("geometryShader", 0),
    "maxGeometryInputComponents": ("geometryShader", 0),
    "maxGeometryOutputComponents": ("geometryShader", 0),
    "maxGeometryOutputVertices": ("geometryShader", 0),
    "maxGeometryTotalOutputComponents": ("geometryShader", 0),
    # While dualSrcBlend is not advertised the limit may be 0; once it is
    # advertised the core table's own floor of 1 applies (see the promotion of
    # 2026-09-21, which reports 1 on the graphics profile).
    "maxFragmentDualSrcAttachments": ("dualSrcBlend", 0),
    "maxDrawIndexedIndexValue": ("fullDrawIndexUint32", (1 << 24) - 1),
    "maxDrawIndirectCount": ("multiDrawIndirect", 1),
    "maxSamplerAnisotropy": ("samplerAnisotropy", 1.0),
    "maxViewports": ("multiViewport", 1),
    "minTexelGatherOffset": ("shaderImageGatherExtended", 0),
    "maxTexelGatherOffset": ("shaderImageGatherExtended", 0),
    "minInterpolationOffset": ("sampleRateShading", 0.0),
    "maxInterpolationOffset": ("sampleRateShading", 0.0),
    "subPixelInterpolationOffsetBits": ("sampleRateShading", 0),
    "storageImageSampleCounts": ("shaderStorageImageMultisample", 1),
    "maxClipDistances": ("shaderClipDistance", 0),
    "maxCullDistances": ("shaderCullDistance", 0),
    "maxCombinedClipAndCullDistances": ("shaderClipDistance", 0),
    "pointSizeRange": ("largePoints", 1.0),
    "pointSizeGranularity": ("largePoints", 0.0),
    "lineWidthRange": ("wideLines", 1.0),
    "lineWidthGranularity": ("wideLines", 0.0),
}

# Limits this profile deliberately reports below the Vulkan 1.0 floor. Each is a
# real restriction of the executable frontend, verified against the cited code
# path; the value is NOT inflated and the gap is a documented blocker rather
# than a claim. A violation that is not in this table fails the gate.
KNOWN_BLOCKERS = {
    "maxImageDimension1D": "compute-only build: graphics limits are not applied to this profile",
    # DXVK262-T06 independentBlend describes two colour attachments through the
    # whole ABI and programmes one CB_COLORn block per target, but the profile
    # does not advertise the second one until a native witness writes and reads
    # back two targets; the DXVK profile asks for four either way.
    "maxColorAttachments": "one color attachment per render pass; a second needs the native witness",
    "maxFragmentOutputAttachments": "one color attachment per render pass; a second needs the native witness",
    "maxFragmentCombinedOutputResources": "one color attachment plus the single sampled descriptor",
    "maxVertexInputBindings": "compute-only build: graphics limits are not applied; graphics supports 16 bindings (VERTEX_INPUT.md)",
    # The graphics profile reports the Vulkan 1.0 floors for these four; only the
    # compute-only build, which applies no graphics limits, stays below them.
    "maxPerStageDescriptorSamplers": "compute-only build: graphics limits are not applied; graphics reports the floor of 16",
    "maxDescriptorSetSamplers": "compute-only build: graphics limits are not applied; graphics reports the floor of 96",
    "maxPerStageDescriptorSampledImages": "compute-only build: graphics limits are not applied; graphics reports the floor of 16",
    "maxDescriptorSetSampledImages": "compute-only build: graphics limits are not applied; graphics reports the floor of 96",
    "maxPerStageDescriptorStorageImages": "bounded R32_UINT storage-image route has not qualified the Vulkan descriptor-count floor",
    "maxDescriptorSetStorageImages": "bounded R32_UINT storage-image route has not qualified the Vulkan descriptor-count floor",
    "maxPerStageDescriptorInputAttachments": "no input attachment support (subpass dependencies rejected)",
    "maxDescriptorSetInputAttachments": "no input attachment support (subpass dependencies rejected)",
    "maxMemoryAllocationCount": "allocator policy: heap size divided by the minimum allocation charge",
    "maxSamplerLodBias": "vkCreateSampler encodes and bounds signed mipLodBias to the reported interval",
    "minTexelOffset": "no evidence the compiler/sampler path implements texel offsets",
    "maxTexelOffset": "no evidence the compiler/sampler path implements texel offsets",
    "storageImageSampleCounts": "compute-only profile has no image objects; graphics supports one-sample R32_UINT storage images",
    # The four framebuffer sample-count limits follow the platform mask: the
    # 2026-09-23 sampleRateShading promotion reports 1x/2x/4x on the graphics
    # profile, so only the compute-only build (which applies no graphics
    # limits) stays at the single-sample value.
    "framebufferColorSampleCounts": "compute-only build: the graphics profile reports 1x/2x/4x",
    "framebufferDepthSampleCounts": "compute-only build: the graphics profile reports 1x/2x/4x",
    "framebufferStencilSampleCounts": "compute-only build: the graphics profile reports 1x/2x/4x",
    "framebufferNoAttachmentsSampleCounts": "compute-only build: the graphics profile reports 1x/2x/4x",
    # The interpolation-offset limits are the CTS-gated half of
    # sampleRateShading: while the feature is unreported the CTS leaves them out
    # and the relaxed floor applies, and the 2026-09-23 promotion brings the
    # core table's own floors (+/-0.5 and 4 bits) into scope. This profile
    # reports 0: no path lowers an interpolation offset - the fragment
    # interface declares position, sample id and the per-sample read, not an
    # offset - so the value is not inflated and the gap is named here rather
    # than hidden behind the feature's promotion.
    "maxInterpolationOffset": "no interpolation-offset path is implemented or measured",
    "minInterpolationOffset": "no interpolation-offset path is implemented or measured",
    "subPixelInterpolationOffsetBits": "no interpolation-offset path is implemented or measured",
    "sampledImageColorSampleCounts": "single-sample sampling only: multisampled sampled images are not supported",
    "sampledImageDepthSampleCounts": "single-sample sampling only: multisampled sampled images are not supported",
    "sampledImageStencilSampleCounts": "single-sample sampling only: multisampled sampled images are not supported",
    "maxImageDimension2D": "compute-only build: graphics limits are not applied to this profile",
    "maxImageDimension3D": "compute-only build: graphics limits are not applied to this profile",
    "maxImageDimensionCube": "compute-only build: graphics limits are not applied to this profile",
    "maxImageArrayLayers": "compute-only build: graphics limits are not applied to this profile",
    "maxFramebufferWidth": "compute-only build: graphics limits are not applied to this profile",
    "maxFramebufferHeight": "compute-only build: graphics limits are not applied to this profile",
    "maxVertexInputAttributes": "compute-only build: graphics limits are not applied to this profile",
    "maxVertexInputAttributeOffset": "compute-only build: graphics limits are not applied to this profile",
    "maxVertexInputBindingStride": "compute-only build: graphics limits are not applied to this profile",
    "maxDrawIndexedIndexValue": "compute-only build: graphics limits are not applied to this profile",
    "maxViewports": "compute-only build: graphics limits are not applied to this profile",
    "subPixelPrecisionBits": "compute-only build: graphics limits are not applied to this profile",
    "maxSamplerAllocationCount": "compute-only build: graphics limits are not applied to this profile",
    "maxSamplerAnisotropy": "compute-only build: graphics limits are not applied to this profile",
    "maxStorageBufferRange": "compute-only build: heap budget is 64 MiB, below the 128 MiB floor",
    "maxViewportDimensions": "compute-only build: graphics limits are not applied to this profile",
    "viewportBoundsRange": "compute-only build: graphics limits are not applied to this profile",
}


def load_dump(profile: str) -> dict:
    if not DUMP_BINARY.is_file():
        raise SystemExit(f"{DUMP_BINARY} is missing; run `make check` first")
    args = [str(DUMP_BINARY)] + (["--compute"] if profile == "compute" else [])
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def core_value(row: dict):
    """The value the Vulkan 1.0 core requires, with the tag that produced it."""
    for value in row.get("values", []):
        tags = [str(tag) for tag in value.get("tags", [])]
        if not tags or "core" in tags:
            return value, ("core" if tags else "untagged-core")
    return None, None


def requirement_floor(row: dict):
    value, tag = core_value(row)
    return (value.get("value") if value else None), tag


def evaluate_limit(row: dict, reported, features: dict) -> tuple[str, str]:
    """Return (verdict, detail) for one row of the limit table."""
    kind = row.get("limit_type")
    value, tag = requirement_floor(row)
    raw_value = value
    if isinstance(value, str):
        value = _decode_requirement(value)
    gated = CTS_GATED_OFF_REQUIREMENT.get(row["limit"])
    # A gated-off floor applies only while the gating feature is reported
    # false; once the feature is advertised the limit must meet the core table's
    # own requirement below, so a platform cannot keep the feature's relaxed
    # floor after promoting the feature.
    if gated and features.get(gated[0]) is True:
        gated = None
    if gated:
        gate, floor = gated
        source = f"cts-gated-off:{gate}"
        if reported is None:
            return "not-audited", f"{source}: reported value unavailable"
        if isinstance(floor, float) or isinstance(reported, list):
            observed = reported if isinstance(reported, list) else [reported]
            required = floor if isinstance(floor, list) else [floor] * len(observed)
            bad = [element for element, want in zip(observed, required)
                   if isinstance(element, (int, float)) and element < want]
            verdict = "violation" if bad else "satisfied"
            return verdict, f"{source}: reported {reported} must be >= {floor}"
        verdict = "violation" if reported < floor else "satisfied"
        return verdict, f"{source}: reported {reported} must be >= {floor}"
    if value is None:
        if kind in ("implementation-dependent", "recommendation", "unspecified", "none", "Boolean"):
            return "not-applicable", f"specification leaves this limit ({kind}) to the implementation"
        return "not-audited", f"requirement text not decoded ({raw_value!r})"
    if reported is None:
        return "not-audited", "reported value unavailable"
    return _evaluate_limit_floor(row, reported, value, tag, kind)


def _evaluate_limit_floor(row: dict, reported, value, tag: str, kind: str) -> tuple[str, str]:
    if isinstance(value, str) and value.startswith("ename:"):
        expected = value.split(":", 1)[1]
        if str(reported) not in ("0", "1"):
            return "not-audited", f"{tag}: non-boolean report {reported!r} for {value}"
        return "satisfied", f"{tag}: reported {reported} for required {expected}"
    if isinstance(value, list):
        if not isinstance(reported, list) or len(reported) != len(value):
            return "not-audited", f"{tag}: tuple requirement {value} vs {reported}"
        verdict = "satisfied"
        for index, required in enumerate(value):
            if required is None:
                continue
            observed = reported[index]
            if not isinstance(observed, (int, float)):
                return "not-audited", f"{tag}: non-numeric reported element {observed!r}"
            # Range limits are expressed as the capability floor per element:
            # a reported range must be at least as wide as the required one.
            if required >= 0 and observed < required:
                verdict = "violation"
            if required < 0 and observed > required:
                verdict = "violation"
        return verdict, f"{tag}: required {value}, reported {reported}"
    if isinstance(value, int) and kind == "min" and isinstance(reported, int) and \
            row.get("required_kind") == "enum":
        # Bitmask limits such as the sample-count sets: the report must contain
        # every required bit.
        missing = value & ~reported
        return ("violation" if missing else "satisfied",
                f"{tag}: reported mask {reported:#x} must include {value:#x}")
    if not isinstance(value, (int, float)) or not isinstance(reported, (int, float)):
        return "not-audited", f"{tag}: non-numeric requirement {value!r} vs reported {reported!r}"
    if kind == "max":
        return ("satisfied" if reported <= value else "violation",
                f"{tag}: reported {reported} must be <= {value}")
    if kind == "min":
        return ("satisfied" if reported >= value else "violation",
                f"{tag}: reported {reported} must be >= {value}")
    if kind == "implementation-dependent":
        return "not-audited", f"implementation dependent ({tag}: {value})"
    if kind in ("fixed point increment", "max") or "granularity" in str(row.get("limit")) or "alignment" in str(row.get("limit")):
        return ("satisfied" if reported <= value else "violation",
                f"{tag}: reported {reported} must be <= {value}")
    return "not-audited", f"unhandled limit type {kind!r}"


def _decode_requirement(value: str):
    """Decode the spec table's text form: '(a,b)' tuples and enum bit lists."""
    text = value.strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    if "ename:" in text:
        bits = 0
        for name in re.findall(r"ename:(VK_[A-Z0-9_]+)", text):
            if name in SAMPLE_COUNT_BITS:
                bits |= SAMPLE_COUNT_BITS[name]
            else:
                return None
        return bits if bits else None
    parts = [part.strip() for part in text.split(",")]
    numbers = []
    for part in parts:
        # Requirements such as "64.0 - ULP" or "256.0 - pname:x" keep the
        # leading numeric bound; the spec's own conservative reading is that
        # value.
        part = part.split(" ")[0]
        try:
            numbers.append(int(part))
        except ValueError:
            try:
                numbers.append(float(part))
            except ValueError:
                return None
    if len(numbers) == 1:
        return numbers[0]
    return numbers


def evaluate_formats(core: dict, dump: dict) -> list[dict]:
    """Evaluate the mandatory format tables rule by rule.

    Every rule from the pinned specification's mandatory-format section is
    classified: per-format cells ("must be supported on the named format"),
    class cells ("at least some of the named formats"), the depth/stencil
    any-of rule, the bufferFeatures negative-scope rule, the compressed-family
    table-choice rules and the feature-gated scope rules. A missing mandatory
    capability is recorded as a blocker with the reason, never as support.
    """
    reported = {int(key): value for key, value in dump["formats"].items()}
    rows: list[dict] = []
    for table in core["formats"]["tables"]:
        legend = table.get("symbol_legend", {})
        annotations = table.get("annotations", [])
        groups: dict[tuple[str, str], list[str]] = {}
        for row in table["rows"]:
            format_name = row.get("format") or row.get("name")
            for cell in row["cells"]:
                scope = cell["scope"]
                feature = cell["feature"]
                if not cell["required"]:
                    continue
                if cell.get("guard"):
                    rows.append({"kind": "format-cell", "table": table["anchor"],
                                 "format": format_name, "scope": scope, "feature": feature,
                                 "verdict": "not-applicable",
                                 "detail": f"guard {cell['guard']} is not advertised"})
                    continue
                symbol = cell.get("symbol")
                rule = legend.get(symbol, "")
                if "at least some of the named formats" in rule:
                    groups.setdefault((symbol, scope, feature), []).append(format_name)
                    continue
                if "with some caveats or preconditions" in rule:
                    rows.append({"kind": "format-cell", "table": table["anchor"],
                                 "format": format_name, "scope": scope, "feature": feature,
                                 "verdict": "not-audited",
                                 "detail": f"conditional rule: {rule}"})
                    continue
                entry = _format_entry(reported, format_name)
                bit = FEATURE_BITS.get(feature)
                if entry is None:
                    verdict = "blocker"
                    detail = ("mandatory format is not implemented by this profile "
                              f"({rule or 'per-format rule'})")
                elif not bit:
                    verdict, detail = "not-audited", f"unknown feature bit {feature}"
                elif entry.get(scope, 0) & bit:
                    verdict, detail = "satisfied", f"{format_name} reports {feature} in {scope}"
                else:
                    verdict = "blocker"
                    detail = (f"advertised format {format_name} lacks mandatory {feature} "
                              f"in {scope} (capability not implemented)")
                rows.append({"kind": "format-cell", "table": table["anchor"],
                             "format": format_name, "scope": scope, "feature": feature,
                             "symbol": symbol, "verdict": verdict, "detail": detail})
        for (symbol, scope, feature), formats in sorted(groups.items()):
            bit = FEATURE_BITS.get(feature)
            supported = []
            for format_name in formats:
                entry = _format_entry(reported, format_name)
                if entry and bit and (entry.get(scope, 0) & bit):
                    supported.append(format_name)
            if supported:
                verdict, detail = "satisfied", f"supported by {supported[:3]}"
            else:
                verdict = "blocker"
                detail = (f"no format of this class reports {feature} in {scope} "
                          "(family not implemented)")
            rows.append({"kind": "format-class-rule", "table": table["anchor"],
                         "symbol": symbol, "scope": scope, "feature": feature,
                         "formats": formats, "verdict": verdict, "detail": detail})

        for annotation in annotations:
            kind = annotation.get("kind")
            if kind == "any-of-formats-rule":
                feature = (annotation.get("features") or [""])[0]
                bit = FEATURE_BITS.get(feature, 0)
                options = annotation.get("format_options") or []
                # The rule names two independent pairs in one sentence; the
                # conservative reading is that at least one named format per
                # pair must support the feature, and this profile advertises
                # D32_SFLOAT, which satisfies both pairs.
                satisfied = [name for name in options
                             if (_format_entry(reported, name) or {}).get("optimalTilingFeatures", 0) & bit]
                verdict = "satisfied" if satisfied else "blocker"
                rows.append({"kind": "format-any-of-rule", "table": table["anchor"],
                             "feature": feature, "formats": options, "verdict": verdict,
                             "detail": (f"supported by {satisfied[:2]}" if satisfied else
                                        "no depth/stencil format with the mandatory feature")})
            elif kind == "negative-scope-rule":
                offenders = []
                for row in table["rows"]:
                    name = row.get("format") or row.get("name")
                    entry = _format_entry(reported, name)
                    if entry and entry.get("bufferFeatures", 0):
                        offenders.append(name)
                rows.append({"kind": "format-negative-scope-rule", "table": table["anchor"],
                             "verdict": "violation" if offenders else "satisfied",
                             "detail": ("reported bufferFeatures for depth/stencil formats: "
                                        f"{offenders[:3]}" if offenders else
                                        "no depth/stencil format reports buffer features")})
            elif kind == "table-choice-rule":
                features = [name for name in (annotation.get("features") or [])]
                families = ["formats-mandatory-features-bcn", "formats-mandatory-features-etc",
                            "formats-mandatory-features-astc"]
                complete = []
                for family in families:
                    source = next((t for t in core["formats"]["tables"] if t["anchor"] == family), None)
                    if not source:
                        continue
                    names = [row.get("format") or row.get("name") for row in source["rows"]]
                    if names and all(
                            all((_format_entry(reported, name) or {}).get("optimalTilingFeatures", 0)
                                & FEATURE_BITS.get(feature, 0) for feature in features)
                            for name in names):
                        complete.append(family)
                verdict = "satisfied" if complete else "blocker"
                rows.append({"kind": "format-table-choice-rule", "table": table["anchor"],
                             "verdict": verdict,
                             "detail": (f"complete family {complete}" if complete else
                                        "no compressed format family is implemented")})
            elif kind == "scope-rule":
                rows.append({"kind": "format-scope-rule", "table": table["anchor"],
                             "verdict": "not-applicable",
                             "detail": f"requires {annotation.get('condition') or annotation.get('features')}"})
    return rows


def _format_entry(reported: dict, format_name: str | None):
    if not format_name:
        return None
    enum = FORMAT_ENUMS.get(format_name)
    if enum is None:
        return None
    return reported.get(enum)


def evaluate_format_query_consistency(dump: dict) -> list[dict]:
    """The two format query paths must agree with each other.

    `vkGetPhysicalDeviceFormatProperties` describes the features a format
    supports per tiling scope; `vkGetPhysicalDeviceImageFormatProperties` must
    answer VK_SUCCESS exactly for the (format, tiling, usage) combinations those
    features allow. Any disagreement is a reporting defect, not a capability
    question, so it is a violation.
    """
    scope_bits = {
        VK_IMAGE_USAGE_SAMPLED_BIT: ("optimalTilingFeatures", "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT"),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT: ("optimalTilingFeatures", "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT"),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT: ("optimalTilingFeatures", "VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT"),
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT: ("optimalTilingFeatures", "VK_FORMAT_FEATURE_TRANSFER_SRC_BIT"),
        VK_IMAGE_USAGE_TRANSFER_DST_BIT: ("optimalTilingFeatures", "VK_FORMAT_FEATURE_TRANSFER_DST_BIT"),
        VK_IMAGE_USAGE_STORAGE_BIT: ("optimalTilingFeatures", "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT"),
    }
    rows = []
    for query in dump.get("imageFormatProperties", []):
        tiling = query["tiling"]
        usage = query["usage"]
        entry = dump["formats"].get(str(query["format"]))
        if entry is None or usage not in scope_bits:
            continue
        scope, feature = scope_bits[usage]
        scope = "optimalTilingFeatures" if tiling == 0 else "linearTilingFeatures"
        bit = FEATURE_BITS.get(feature, 0)
        expected = bool(entry.get(scope, 0) & bit) if tiling == 0 else bool(entry.get("linearTilingFeatures", 0) & bit)
        actual = query["result"] == 0
        rows.append({
            "kind": "format-query-consistency",
            "format": query["format"], "tiling": tiling, "usage": usage,
            "reported_features": entry.get(scope, 0),
            "expected_supported": expected, "reported_supported": actual,
            "verdict": "satisfied" if expected == actual else "violation",
            "detail": (f"tiling {tiling} usage {usage:#x}: features {entry.get(scope, 0):#x} "
                       f"implies supported={expected}, query answered supported={actual}"),
        })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail instead of rewriting when the matrix is out of date")
    args = parser.parse_args()

    core = json.loads(CORE_TARGET.read_text())
    manifest = json.loads(MANIFEST.read_text())
    dumps = {"graphics": load_dump("graphics"), "compute": load_dump("compute")}

    limits = []
    for row in core["limits"]["rows"]:
        name = row["limit"]
        for profile, dump in dumps.items():
            reported = dump["limits"].get(name)
            if name not in LIMIT_MEMBERS:
                verdict, detail = ("not-applicable",
                                   "not a Vulkan 1.0 VkPhysicalDeviceLimits member "
                                   "(pinned khronos-vulkan-registry header)")
            else:
                verdict, detail = evaluate_limit(row, reported, dump["features"])
                if verdict == "violation":
                    reason = KNOWN_BLOCKERS.get(name)
                    if reason:
                        verdict = "blocker"
                        detail = f"{detail}; blocker: {reason}"
            limits.append({"kind": "limit", "limit": name, "profile": profile,
                           "reported": reported, "verdict": verdict, "detail": detail})

    features = []
    for profile, dump in dumps.items():
        for name, value in sorted(dump["features"].items()):
            verdict, detail = evaluate_feature(name, value, profile)
            features.append({"kind": "feature", "feature": name, "profile": profile,
                             "reported": value, "verdict": verdict, "detail": detail})
        for name in ("uniformBufferStandardLayout", "vulkanMemoryModel",
                     "vulkanMemoryModelDeviceScope", "bufferDeviceAddress"):
            value = dump["extensionFeatures"][name]
            verdict, detail = evaluate_feature(name, value, profile)
            features.append({"kind": "extension-feature", "feature": name,
                             "profile": profile, "reported": value,
                             "verdict": verdict, "detail": detail})

    formats = []
    for profile, dump in dumps.items():
        for row in evaluate_formats(core, dump):
            row["profile"] = profile
            formats.append(row)

    shaders = []
    for profile, dump in dumps.items():
        for row in evaluate_shader_capabilities(dump):
            row = dict(row)
            row["profile"] = profile
            shaders.append(row)
        for row in evaluate_shader_precision(dump):
            row = dict(row)
            row["profile"] = profile
            shaders.append(row)

    format_queries = []
    for profile, dump in dumps.items():
        for row in evaluate_format_query_consistency(dump):
            row = dict(row)
            row["profile"] = profile
            format_queries.append(row)

    selected = [case["path"] for case in manifest["cases"]]
    diagnostics = [case["path"] for case in manifest.get("diagnostics", [])]

    # Applicable-CTS column. The limit rows are exactly what the diagnostic
    # device-query cases check; no acceptance case depends on a value this
    # increment touched, so the acceptance selection is unchanged.
    limit_cases = [case for case in diagnostics if case.startswith("dEQP-VK.info.")]
    for row in limits:
        row["applicable_cts"] = {"cases": limit_cases, "status": "diagnostic"}
    for row in formats:
        row["applicable_cts"] = {"cases": [], "status": "not-selected",
                                 "note": "mandatory format families are not implemented; no selected case covers them"}
        if (row.get("format") == "VK_FORMAT_R32_UINT" and
                row.get("feature") == "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT" and
                row.get("scope") == "optimalTilingFeatures" and
                row.get("profile") == "graphics"):
            row["applicable_cts"] = {
                "cases": [case["path"] for case in manifest.get("cases", [])
                          if case["category"] == "t08-buffer-device-address-base"],
                "status": "acceptance",
                "note": "original compute BDA leaves exercise the bounded R32_UINT storage image",
            }
    for row in features:
        advertised = ADVERTISED_FEATURES.get(row["feature"])
        cases = list(advertised["cts"]) if advertised else []
        row["applicable_cts"] = {
            "cases": cases,
            "status": "acceptance" if cases else "not-selected",
            "note": ("selected original upstream reporting oracle" if cases
                     else "no selected case requires this bit"),
        }
    for row in shaders:
        row["applicable_cts"] = {"cases": [], "status": "not-selected",
                                 "note": "no original CTS case for this capability is selected here"}

    matrix = {
        "schema": "ps5vk-reporting-matrix/1",
        "reported_source": {
            "tool": "tools/dump_device_reporting.c",
            "profile_initializer": "src/device_profile_report.h",
            "native_graphics_overlay": "native/tess_profile.h",
            "native_consumer": "native/platform_ps5.c",
        },
        "normative_source": {
            "inventory": "conformance_inventory/core_target.json",
            "limit_rows": len(core["limits"]["rows"]),
            "format_tables": [table["anchor"] for table in core["formats"]["tables"]],
        },
        "applicable_cts_selection": {
            "manifest": "cts/upstream/manifest.json",
            "cases": selected,
            "diagnostics": diagnostics,
            "note": "Only original upstream cases whose prerequisites the reported "
                    "profile satisfies belong here; anything else stays a diagnostic.",
        },
        "profiles": {profile: {"deviceName": dump["deviceName"], "apiVersion": dump["apiVersion"],
                               "vendorID": dump["vendorID"], "deviceID": dump["deviceID"],
                               "multiview_query": dump["multiviewQuery"],
                               "standard_ubo_query": dump["standardUBOQuery"],
                               "memory_model_query": dump["memoryModelQuery"],
                               "buffer_device_address_query": dump["bufferDeviceAddressQuery"]}
                     for profile, dump in dumps.items()},
        "limits": limits,
        "features": features,
        "formats": formats,
        "format_query_consistency": format_queries,
        "shader_capabilities": shaders,
    }

    summary = {}
    for section in ("limits", "features", "formats", "format_query_consistency", "shader_capabilities"):
        counts = {}
        for row in matrix[section]:
            counts[row["verdict"]] = counts.get(row["verdict"], 0) + 1
        summary[section] = counts
    matrix["summary"] = summary

    text = json.dumps(matrix, indent=2, sort_keys=True) + "\n"
    if args.check:
        current = MATRIX.read_text() if MATRIX.is_file() else ""
        if current != text:
            print("reporting matrix is out of date; re-run without --check", file=sys.stderr)
            return 1
    else:
        MATRIX.write_text(text)

    for section in ("limits", "features", "formats", "format_query_consistency", "shader_capabilities"):
        print(f"{section}: " + ", ".join(f"{k}={v}" for k, v in sorted(summary[section].items())))
    violations = [row for section in ("limits", "features", "formats",
                                      "format_query_consistency", "shader_capabilities")
                  for row in matrix[section]
                  if row["verdict"] == "violation"]
    blockers = [row for section in ("limits", "formats", "format_query_consistency", "shader_capabilities") for row in matrix[section]
                if row["verdict"] == "blocker"]
    print(f"blockers: {len(blockers)} (documented below-floor reports, not claims)")
    print(f"unresolved violations: {len(violations)}")
    for row in violations[:20]:
        key = row.get("limit") or f"{row.get('feature')} ({row.get('scope')})"
        print(f"  {row['kind']:18s} {row['profile']:8s} {key:38s} {row['detail'][:90]}")
    return 1 if violations else 0


# VkFormat enum values and feature-bit values are taken from the pinned
# registry header, not retyped by hand.
def _load_registry_constants() -> tuple[dict, dict]:
    header = (ROOT / "third_party/vulkan-headers/include/vulkan/vulkan_core.h")
    text = header.read_text()
    formats: dict[str, int] = {}
    for name, value in re.findall(r"\n\s*(VK_FORMAT_[A-Z0-9_]+)\s*=\s*(\d+)\s*,", text):
        formats.setdefault(name, int(value))
    bits: dict[str, int] = {}
    for name, value in re.findall(r"\n\s*(VK_FORMAT_FEATURE_[A-Z0-9_]+_BIT[A-Z_]*)\s*=\s*(0x[0-9a-fA-F]+|\d+)", text):
        bits.setdefault(name, int(value, 0))
    return formats, bits


FORMAT_ENUMS, FEATURE_BITS = _load_registry_constants()


def _load_sample_count_bits() -> dict[str, int]:
    text = (ROOT / "third_party/vulkan-headers/include/vulkan/vulkan_core.h").read_text()
    bits: dict[str, int] = {}
    for name, value in re.findall(r"\n\s*(VK_SAMPLE_COUNT_[A-Z0-9_]+_BIT)\s*=\s*(0x[0-9a-fA-F]+|\d+)", text):
        bits.setdefault(name, int(value, 0))
    if not bits:
        raise SystemExit("VK_SAMPLE_COUNT_*_BIT not found in the pinned registry header")
    return bits


SAMPLE_COUNT_BITS = _load_sample_count_bits()


def _load_required_floors() -> dict[str, int]:
    """The floors this repository declares in physical_device_profile.h."""
    text = (ROOT / "src/physical_device_profile.h").read_text()
    floors = {name: int(value) for name, value in
              re.findall(r"#define (PS5VK_REQUIRED_[A-Z_]+) (\d+)u", text)}
    for name in ("PS5VK_REQUIRED_SUBTEXEL_BITS", "PS5VK_REQUIRED_MIPMAP_PRECISION_BITS",
                 "PS5VK_REQUIRED_INTERFACE_COMPONENTS", "PS5VK_REQUIRED_SAMPLE_MASK_WORDS"):
        if name not in floors:
            raise SystemExit(f"{name} is not declared in src/physical_device_profile.h")
    return floors


REQUIRED_FLOORS = _load_required_floors()


# SPIR-V capability enumerants handled by the shader feature gate. These are
# stable SPIR-V registry values; the names are confirmed against the pinned
# compiler header when that optional checkout is present (it is absent in CI,
# where a hard dependency would make the gate environment-dependent).
SPIRV_CAPABILITY_NAMES = {
    22: "Int16",
    39: "Int8",
    4433: "StorageBuffer16BitAccess",
    4434: "UniformAndStorageBuffer16BitAccess",
    4448: "StorageBuffer8BitAccess",
    4449: "UniformAndStorageBuffer8BitAccess",
    5345: "VulkanMemoryModel",
    5346: "VulkanMemoryModelDeviceScope",
    5347: "PhysicalStorageBufferAddresses",
}


def _verify_spirv_capabilities() -> None:
    """Fail if the pinned enumerants are not real, when the header is present."""
    header = ROOT / "third_party/psbc-reference/src/compiler/spirv/spirv.h"
    if not header.is_file():
        return
    present = {int(value) for _name, value in
               re.findall(r"\n\s*(SpvCapability[A-Za-z0-9_]+)\s*=\s*(\d+)\s*,", header.read_text())}
    missing = sorted(number for number in SPIRV_CAPABILITY_NAMES if number not in present)
    if missing:
        raise SystemExit(f"SPIR-V capability numbers {missing} are not in {header}")


SPIRV_CAPABILITIES = dict(SPIRV_CAPABILITY_NAMES)
_verify_spirv_capabilities()

# The SPIR-V capabilities the frontend's shader feature gate handles, and the
# advertisement each one corresponds to. Anything not listed here has no
# frontend gate, so it is recorded as not-audited rather than assumed rejected.
# Keyed by enumerant so registry renames of the same capability cannot silently
# unmap the rule.
SHADER_CAPABILITY_ADVERTISEMENT = {
    4433: ("extension", "storageBuffer16BitAccess"),
    4448: ("extension", "storageBuffer8BitAccess"),
    22: ("core", "shaderInt16"),
    39: ("core", None),
    4434: ("extension", "uniformAndStorageBuffer16BitAccess"),
    4449: ("extension", "uniformAndStorageBuffer8BitAccess"),
    5345: ("extension", "vulkanMemoryModel"),
    5346: ("extension", "vulkanMemoryModelDeviceScope"),
    5347: ("extension", "bufferDeviceAddress"),
}


def evaluate_shader_capabilities(dump: dict) -> list[dict]:
    """Compare the frontend's SPIR-V capability gate with what is advertised."""
    source = (ROOT / "src/vk_pipeline.c").read_text()
    function = re.search(r"static int spirv_narrow_requirements\(.*?\n\}", source, re.DOTALL)
    rows: list[dict] = []
    if not function:
        return [{"kind": "shader-capability", "verdict": "not-audited",
                 "detail": "spirv_narrow_requirements not found in src/vk_pipeline.c"}]
    for case in re.finditer(r"case (\d+)u:\s*(.*?)(break;|return 0;)", function.group(0), re.DOTALL):
        number, body, terminator = int(case.group(1)), case.group(2), case.group(3)
        name = SPIRV_CAPABILITIES.get(number, f"capability-{number}")
        advertisement = SHADER_CAPABILITY_ADVERTISEMENT.get(number)
        if terminator == "return 0;":
            action, required = "reject", None
        else:
            feature_bits = list(dict.fromkeys(re.findall(r"PS5VK_FEATURE_[A-Z0-9_]+", body)))
            action, required = (("requires-extension-feature", " | ".join(feature_bits))
                                if feature_bits else ("accept", None))
        if advertisement is None:
            rows.append({"kind": "shader-capability", "capability": name, "number": number,
                         "action": action, "verdict": "not-audited",
                         "detail": "gate present but no advertised feature is mapped to it"})
            continue
        where, field = advertisement
        advertised = (dump["extensionFeatures"].get(field, False) if where == "extension"
                      else dump["features"].get(field, False) if field else False)
        if action == "requires-extension-feature":
            rows.append({"kind": "shader-capability", "capability": name, "number": number,
                         "action": action, "file": "src/vk_pipeline.c",
                         "advertised": bool(advertised), "verdict": "satisfied",
                         "detail": f"accepted only when {required} is advertised "
                                   f"(advertised={bool(advertised)}), so the gate and the "
                                   "advertisement cannot disagree"})
            continue
        accepted = action != "reject"
        verdict = "satisfied" if accepted == bool(advertised) else "violation"
        rows.append({"kind": "shader-capability", "capability": name, "number": number,
                     "action": action, "file": "src/vk_pipeline.c",
                     "advertised": bool(advertised), "verdict": verdict,
                     "detail": f"frontend {'accepts' if accepted else 'rejects'} "
                               f"({required or 'no feature required'}); advertised={bool(advertised)}"})
    return rows


def evaluate_shader_precision(dump: dict) -> list[dict]:
    """What this profile advertises about shader precision, and its basis.

    Vulkan 1.0 has no float-control feature bits: precision-related reporting is
    the mandatory limit floors plus the absence of any float-control extension.
    Anything stronger (a claimed precision mode per stage) would require
    VK_KHR_shader_float_controls properties, which are not advertised.
    """
    source = (ROOT / "src/vk_pipeline.c").read_text()
    rows = [{
        "kind": "shader-precision",
        "subject": "float-control extensions",
        "advertised": [name for name in dump.get("extensions", [])
                       if "float_controls" in name or "float16" in name],
        "verdict": "satisfied",
        "detail": "no float-control or float16 extension is advertised, so no per-stage "
                  "precision mode is claimed beyond the mandatory limit floors",
    }, {
        "kind": "shader-precision",
        "subject": "texture and mipmap precision floors",
        "advertised": [dump["limits"]["subTexelPrecisionBits"], dump["limits"]["mipmapPrecisionBits"]],
        "verdict": "satisfied" if dump["limits"]["subTexelPrecisionBits"] >= REQUIRED_FLOORS["PS5VK_REQUIRED_SUBTEXEL_BITS"]
                   and dump["limits"]["mipmapPrecisionBits"] >= REQUIRED_FLOORS["PS5VK_REQUIRED_MIPMAP_PRECISION_BITS"]
                   else "violation",
        "detail": "reported as the specification floor; the frontend does not quantize "
                  "texture coordinates itself",
    }, {
        "kind": "shader-precision",
        "subject": "compiler-emitted float/ieee mode",
        "advertised": "per-program metadata only",
        "verdict": "satisfied" if "p->float_mode > 255 || p->ieee_mode > 1" in source
                   else "not-audited",
        "file": "src/vk_pipeline.c",
        "detail": "PSBC/ACO records float_mode and ieee_mode per compiled program and the "
                  "pipeline gate validates them; they are not exposed as device capabilities",
    }, {
        "kind": "shader-precision",
        "subject": "relaxed precision / signed zero / denorm modes",
        "advertised": "not advertised",
        "verdict": "not-audited",
        "detail": "no VK_KHR_shader_float_controls or Vulkan 1.2 float-control property is "
                  "reported, so no mode claim exists to compare against PSBC/ACO; the "
                  "compiler's behaviour for each SPIR-V mode is not measured here",
    }]
    return rows


def _load_limit_members() -> set[str]:
    """VkPhysicalDeviceLimits members, straight from the pinned registry."""
    text = (ROOT / "third_party/vulkan-headers/include/vulkan/vulkan_core.h").read_text()
    match = re.search(r"typedef struct VkPhysicalDeviceLimits \{(.*?)\} VkPhysicalDeviceLimits;",
                      text, re.DOTALL)
    if not match:
        raise SystemExit("VkPhysicalDeviceLimits not found in the pinned registry header")
    body = match.group(1)
    names = set()
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("//") or line.startswith("/*") or line.startswith("*"):
            continue
        for token in re.findall(r"\b([a-z][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*;", line):
            names.add(token)
    return names


LIMIT_MEMBERS = _load_limit_members()


if __name__ == "__main__":
    raise SystemExit(main())
