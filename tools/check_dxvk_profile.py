#!/usr/bin/env python3
"""Join the pinned DXVK 2.6.2 profile to ps5vk's current evidence.

This is intentionally fail-closed. A required value without public reporting,
reviewed implementation or native execution evidence remains a blocker. CTS
mapping and runs are regression evidence: an unmapped or unrun leaf does not
block, while an observed applicable failure does.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "conformance_inventory/dxvk_v262_profile.json"
EVIDENCE = ROOT / "conformance_inventory/dxvk_v262_evidence.json"
REPORTING = ROOT / "conformance_inventory/reporting_matrix.json"
CORE_REQUIREMENTS = ROOT / "conformance_inventory/requirements.json"
OUTPUT = ROOT / "conformance_inventory/dxvk_v262_matrix.json"
DEVICE_SOURCE = ROOT / "src/vk_device.c"
PLATFORM_SOURCE = ROOT / "native/platform_ps5.c"
VULKAN_HEADER = ROOT / "third_party/vulkan-headers/include/vulkan/vulkan_core.h"
SCHEMA = "ps5vk-dxvk-matrix/1"
MULTIVIEW_FIELDS = {
    "feature:VkPhysicalDeviceVulkan11Features:multiview": "multiview",
    "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewViewCount": "maxMultiviewViewCount",
    "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewInstanceIndex": "maxMultiviewInstanceIndex",
}
STANDARD_UBO_ID = "feature:VkPhysicalDeviceVulkan12Features:uniformBufferStandardLayout"
MEMORY_MODEL_IDS = {
    "feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModel": "vulkanMemoryModel",
    "feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModelDeviceScope":
        "vulkanMemoryModelDeviceScope",
}
BDA_ID = "feature:VkPhysicalDeviceVulkan12Features:bufferDeviceAddress"
HOST_QUERY_RESET_ID = "feature:VkPhysicalDeviceVulkan12Features:hostQueryReset"
SAMPLER_MIRROR_CLAMP_ID = "feature:VkPhysicalDeviceVulkan12Features:samplerMirrorClampToEdge"
DEVICE_SCOPE_ID = "feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModelDeviceScope"
TIMELINE_ID = "feature:VkPhysicalDeviceVulkan12Features:timelineSemaphore"
TIMELINE_DIFFERENCE_ID = "property:VkPhysicalDeviceVulkan12Properties:maxTimelineSemaphoreValueDifference"
SEPARATE_DEPTH_STENCIL_ID = "feature:VkPhysicalDeviceVulkan12Features:separateDepthStencilLayouts"
# Single-feature extension routes: a promoted Vulkan 1.2/1.3 feature that this
# Vulkan 1.0 device carries through one extension and that extension's own
# feature structure. The reporting dump publishes every route's queried value
# under extension_route_queries, the capability probe logs it as
# DXVK262_EXTENSION_ROUTE_QUERY, and one table drives both joins. A new route
# is one entry here, one in tools/verify_dxvk_probe.py and one query in the
# probe and the dump.
EXTENSION_ROUTES = {
    # DXVK262-T14: both members of the extension's own feature structure. The
    # capture witness is their execution evidence (dxvk_v262_evidence.json).
    "feature:VkPhysicalDeviceTransformFeedbackFeaturesEXT:transformFeedback": {
        "extension": "VK_EXT_transform_feedback",
        "field": "transformFeedback",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "src/vk_xfb_commands.c",
                 "native/graphics_queue_ps5.c", "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed EXT feature query and opt-in; geometry-stage capture compiles "
                   "to the ordered no-GDS streamout and each begin/end session loads and "
                   "stores its counters, with DrawIndirectByteCount and stream queries."),
    },
    "feature:VkPhysicalDeviceTransformFeedbackFeaturesEXT:geometryStreams": {
        "extension": "VK_EXT_transform_feedback",
        "field": "geometryStreams",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "src/vk_transform_feedback.c",
                 "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed EXT feature query and opt-in; streams 1-3 capture into their "
                   "own buffers and a non-zero stream needs the enabled feature."),
    },
    "feature:VkPhysicalDeviceVulkan13Features:shaderDemoteToHelperInvocation": {
        "extension": "VK_EXT_shader_demote_to_helper_invocation",
        "field": "shaderDemoteToHelperInvocation",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "native/draw_state_ps5.c",
                 "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed EXT feature query and opt-in; the pinned compiler lowers every "
                   "demote spelling to a helper-preserving demote and the draw gives "
                   "export-free removing programs their export memory."),
    },
    "feature:VkPhysicalDeviceVulkan13Features:shaderTerminateInvocation": {
        "extension": "VK_KHR_shader_terminate_invocation",
        "field": "shaderTerminateInvocation",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "native/draw_state_ps5.c",
                 "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed KHR feature query and opt-in; OpTerminateInvocation compiles "
                   "to a terminating program whose removed pixels write no depth or stencil."),
    },
    "feature:VkPhysicalDeviceVulkan13Features:synchronization2": {
        "extension": "VK_KHR_synchronization2",
        "field": "synchronization2",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "src/vk_sync2.c",
                 "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed KHR feature query and opt-in; barrier2, event2 and submit2 "
                   "convert onto the Vulkan 1.0 barrier and submit routes. Timestamp2 is "
                   "refused with every timestamp (timestampValidBits is zero)."),
    },
    "feature:VkPhysicalDeviceVulkan12Features:imagelessFramebuffer": {
        "extension": "VK_KHR_imageless_framebuffer",
        "field": "imagelessFramebuffer",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "src/vk_framebuffer.c",
                 "src/vk_command.c", "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed KHR feature query and opt-in with the maintenance2 and "
                   "image_format_list dependencies; attachments bind at render-pass begin "
                   "through VkRenderPassAttachmentBeginInfo."),
    },
    "feature:VkPhysicalDeviceRobustness2FeaturesEXT:robustBufferAccess2": {
        "extension": "VK_EXT_robustness2",
        "field": "robustBufferAccess2",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "src/vertex_fetch.c",
                 "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed EXT feature query and opt-in; the compiler bounds buffer descriptors by "
                   "their exact range, and non-indexed draws past a vertex buffer read zero "
                   "through the vertex descriptor record count."),
    },
    "feature:VkPhysicalDeviceRobustness2FeaturesEXT:nullDescriptor": {
        "extension": "VK_EXT_robustness2",
        "field": "nullDescriptor",
        "refs": ["native/platform_ps5.c", "src/vk_device.c", "src/vk_descriptor.c",
                 "conformance_inventory/reporting_matrix.json"],
        "detail": ("Reviewed EXT feature query and opt-in; VK_NULL_HANDLE descriptors and vertex "
                   "buffers become all-zero records that read zero."),
    },
}
DIAGNOSTIC_IMPLEMENTATIONS = {
    "feature:VkPhysicalDeviceVulkan12Features:samplerMirrorClampToEdge": (
        ("src/vk_device.c", "PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE"),
        ("src/vk_sampler.c", "VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE"),
    ),
}


def diagnostic_implementation(identifier: str) -> dict | None:
    citations = DIAGNOSTIC_IMPLEMENTATIONS.get(identifier)
    if citations is None:
        return None
    missing = [f"{path}:{token}" for path, token in citations
               if not (ROOT / path).is_file() or token not in (ROOT / path).read_text()]
    return {"state": "missing" if missing else "implemented",
            "refs": sorted({path for path, _ in citations}),
            "detail": ("Missing reviewed diagnostic implementation: " + ", ".join(missing)
                       if missing else "Bounded implementation exists in a diagnostic build; "
                       "public API and native evidence remain independent, and observed CTS failures stay visible.")}


DEVICE_SCOPE_IMPLEMENTATION_TOKENS = (
    ("src/vk_device.c", "PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE"),
    ("src/vk_pipeline.c", "case 5346u: /* VulkanMemoryModelDeviceScope */"),
    ("src/ps5vk_compiler.c", "opts.enable_vulkan_memory_model_device_scope"),
)


def memory_model_axes(row: dict, query: dict, extensions: set[str],
                      feature_reports: dict[str, dict]) -> tuple[dict, dict] | None:
    field = MEMORY_MODEL_IDS.get(row["id"])
    if field is None:
        return None
    if query.get("route") != "VK_KHR_vulkan_memory_model":
        raise ValueError("memory model public query route is absent")
    value = query.get(field)
    base = query.get("vulkanMemoryModel")
    scope = query.get("vulkanMemoryModelDeviceScope")
    if (not isinstance(value, bool) or not isinstance(base, bool) or
            not isinstance(scope, bool) or (scope and not base)):
        raise ValueError("invalid memory model public query value")
    extension = "VK_KHR_vulkan_memory_model" in extensions
    report = feature_reports.get(field, {})
    implemented = (value and extension and report.get("kind") == "extension-feature" and
                   report.get("reported") is True and report.get("verdict") == "satisfied")
    scope_missing = (
        [f"{path}:{token}" for path, token in DEVICE_SCOPE_IMPLEMENTATION_TOKENS
         if not (ROOT / path).is_file() or token not in (ROOT / path).read_text()]
        if row["id"] == DEVICE_SCOPE_ID else []
    )
    implemented = implemented and not scope_missing
    return ({"state": "satisfied" if value and extension else "blocker",
             "observed": value and extension, "expected": row["expected"],
             "via": "VK_KHR_vulkan_memory_model" if extension else None,
             "detail": "KHR feature query on Vulkan 1.0; the Vulkan 1.2 aggregate is unadvertised."},
            {"state": "implemented" if implemented else "missing",
             "refs": ["src/vk_device.c", "src/vk_pipeline.c", "src/ps5vk_compiler.c",
                      "native/runtime_graphics_compiler.c",
                      "conformance_inventory/reporting_matrix.json"],
            "detail": (("Missing reviewed DeviceScope implementation: " +
                        ", ".join(scope_missing)) if scope_missing else
                       "Reviewed public KHR query and device-creation opt-in; DeviceScope is "
                       "independently gated in compute and graphics compilation."
                       if row["id"] == DEVICE_SCOPE_ID else
                       "The base model and DeviceScope are independently gated; "
                       "compute and graphics compiler options follow each SPIR-V module's memory model.")})


def buffer_address_axes(row: dict, query: dict, extensions: set[str],
                        feature_reports: dict[str, dict]) -> tuple[dict, dict] | None:
    if row["id"] != BDA_ID:
        return None
    if query.get("route") != "VK_KHR_buffer_device_address":
        raise ValueError("buffer device address public query route is absent")
    value = query.get("bufferDeviceAddress")
    capture_replay = query.get("bufferDeviceAddressCaptureReplay")
    multi_device = query.get("bufferDeviceAddressMultiDevice")
    if (not all(isinstance(v, bool) for v in (value, capture_replay, multi_device)) or
            capture_replay or multi_device):
        raise ValueError("invalid buffer device address public query")
    route = ("VK_KHR_buffer_device_address" in extensions and
             "VK_KHR_device_group" in extensions)
    report = feature_reports.get("bufferDeviceAddress", {})
    implemented = (value and route and report.get("kind") == "extension-feature" and
                   report.get("reported") is True and report.get("verdict") == "satisfied")
    return ({"state": "satisfied" if value and route else "blocker",
             "observed": value and route, "expected": row["expected"],
             "via": "VK_KHR_buffer_device_address" if route else None,
             "detail": "Equivalent KHR feature on Vulkan 1.0; the Vulkan 1.2 aggregate is unadvertised."},
            {"state": "implemented" if implemented else "missing",
             "refs": ["src/vk_device.c", "src/vk_memory.c", "src/vk_pipeline.c",
                      "src/ps5vk_compiler.c", "conformance_inventory/reporting_matrix.json"],
             "detail": "Reviewed KHR feature query, opt-in, address binding and compute compilation."})


def standard_ubo_axes(row: dict, query: dict, extensions: set[str],
                      feature_reports: dict[str, dict]) -> tuple[dict, dict] | None:
    if row["id"] != STANDARD_UBO_ID:
        return None
    if query.get("route") != "VK_KHR_uniform_buffer_standard_layout" or (
            "VK_KHR_uniform_buffer_standard_layout" not in extensions):
        raise ValueError("standard UBO public query route is absent")
    value = query.get("uniformBufferStandardLayout")
    if not isinstance(value, bool):
        raise ValueError("invalid standard UBO public query value")
    report = feature_reports.get("uniformBufferStandardLayout", {})
    implemented = (value and report.get("kind") == "extension-feature" and
                   report.get("reported") is True and report.get("verdict") == "satisfied")
    return ({"state": "satisfied" if value else "blocker", "observed": value,
             "expected": row["expected"], "via": "VK_KHR_uniform_buffer_standard_layout",
             "detail": "Equivalent KHR field; aggregate Vulkan 1.2 structures remain unadvertised."},
            {"state": "implemented" if implemented else "missing",
             "refs": ["src/vk_device.c", "src/spirv_ubo_layout.c",
                      "conformance_inventory/reporting_matrix.json"],
             "detail": "Reviewed KHR feature query, opt-in and layout validation."})



def host_query_reset_axes(row: dict, query: dict, extensions: set[str],
                          feature_reports: dict[str, dict]) -> tuple[dict, dict] | None:
    if row["id"] != HOST_QUERY_RESET_ID:
        return None
    if query.get("route") != "VK_EXT_host_query_reset":
        raise ValueError("host query reset public query route is absent")
    value = query.get("hostQueryReset")
    if not isinstance(value, bool):
        raise ValueError("invalid host query reset public query value")
    extension = "VK_EXT_host_query_reset" in extensions
    report = feature_reports.get("hostQueryReset", {})
    implemented = (value and extension and report.get("kind") == "extension-feature" and
                   report.get("reported") is True and report.get("verdict") == "satisfied")
    return ({"state": "satisfied" if value and extension else "blocker",
             "observed": value and extension, "expected": row["expected"],
             "via": "VK_EXT_host_query_reset" if extension else None,
             "detail": "EXT feature query on Vulkan 1.0; the Vulkan 1.2 aggregate is unadvertised."},
            {"state": "implemented" if implemented else "missing",
             "refs": ["native/platform_ps5.c", "src/vk_device.c", "src/vk_query_pool.c",
                      "conformance_inventory/reporting_matrix.json"],
             "detail": "Reviewed EXT feature query, opt-in, range and pending-use validation."})


def extension_route_axes(row: dict, queries: dict, extensions: set[str],
                         feature_reports: dict[str, dict]) -> tuple[dict, dict] | None:
    """The api and implementation axes of a row in EXTENSION_ROUTES."""
    route = EXTENSION_ROUTES.get(row["id"])
    if route is None:
        return None
    extension, field = route["extension"], route["field"]
    query = queries.get(extension)
    # One extension's feature structure may carry several routed fields: the
    # query must hold exactly those fields, each a boolean.
    fields = {entry["field"] for entry in EXTENSION_ROUTES.values()
              if entry["extension"] == extension}
    if (not isinstance(query, dict) or set(query) != fields or
            not all(isinstance(value, bool) for value in query.values())):
        raise ValueError(f"{extension} public query route is absent")
    value = query[field]
    if not isinstance(value, bool):
        raise ValueError(f"invalid {extension} public query value")
    enumerated = extension in extensions
    report = feature_reports.get(field, {})
    implemented = (value and enumerated and report.get("kind") == "extension-feature" and
                   report.get("reported") is True and report.get("verdict") == "satisfied")
    return ({"state": "satisfied" if value and enumerated else "blocker",
             "observed": value and enumerated, "expected": row["expected"],
             "via": extension if enumerated else None,
             "detail": (f"{extension} feature query on Vulkan 1.0." if
                        row["container"].endswith("EXT") or row["container"].endswith("KHR")
                        else f"{extension} feature query on Vulkan 1.0; the "
                        f"{row['container']} aggregate is unadvertised.")},
            {"state": "implemented" if implemented else "missing",
             "refs": route["refs"], "detail": route["detail"]})


def sampler_mirror_clamp_axes(row: dict, extensions: set[str]) -> tuple[dict, dict] | None:
    if (row["id"] != SAMPLER_MIRROR_CLAMP_ID or
            "VK_KHR_sampler_mirror_clamp_to_edge" not in extensions):
        return None
    return ({"state": "satisfied", "observed": True, "expected": row["expected"],
             "via": "VK_KHR_sampler_mirror_clamp_to_edge",
             "detail": "KHR device extension on Vulkan 1.0; the Vulkan 1.2 aggregate is unadvertised."},
            {"state": "implemented", "refs": ["native/platform_ps5.c", "src/vk_device.c",
                                               "src/vk_sampler.c"],
             "detail": "Public KHR enumeration and device opt-in use the native mirror-clamp sampler path."})

def timeline_axes(row: dict, query: dict, extensions: set[str],
                  feature_reports: dict[str, dict]) -> tuple[dict, dict] | None:
    """timelineSemaphore and maxTimelineSemaphoreValueDifference through
    VK_KHR_timeline_semaphore on Vulkan 1.0 (DXVK262-T09)."""
    if row["id"] not in (TIMELINE_ID, TIMELINE_DIFFERENCE_ID):
        return None
    if query.get("route") != "VK_KHR_timeline_semaphore":
        raise ValueError("timeline semaphore public query route is absent")
    feature = query.get("timelineSemaphore")
    difference = query.get("maxTimelineSemaphoreValueDifference")
    if (not isinstance(feature, bool) or isinstance(difference, bool) or
            not isinstance(difference, int) or difference < 0 or
            (difference and not feature) or (feature and not difference)):
        raise ValueError("invalid timeline semaphore public query")
    route = "VK_KHR_timeline_semaphore" in extensions and feature
    observed = feature if row["id"] == TIMELINE_ID else difference
    satisfied = route and (observed >= row["expected"] if row["id"] == TIMELINE_DIFFERENCE_ID
                           else observed is True)
    report = feature_reports.get("timelineSemaphore", {})
    implemented = (satisfied and report.get("kind") == "extension-feature" and
                   report.get("reported") is True and report.get("verdict") == "satisfied")
    return ({"state": "satisfied" if satisfied else "blocker",
             "observed": observed if route else None, "expected": row["expected"],
             "via": "VK_KHR_timeline_semaphore" if route else None,
             "detail": "Equivalent KHR query on Vulkan 1.0; the Vulkan 1.2 aggregate is unadvertised."},
            {"state": "implemented" if implemented else "missing",
             "refs": ["src/vk_device.c", "src/vk_sync.c", "src/vk_queue.c",
                      "conformance_inventory/reporting_matrix.json"],
             "detail": "Reviewed KHR query, opt-in, frontend payload and full 64-bit comparisons."})


def separate_depth_stencil_axes(row: dict, query: dict, extensions: set[str],
                                feature_reports: dict[str, dict]) -> tuple[dict, dict] | None:
    """separateDepthStencilLayouts through VK_KHR_separate_depth_stencil_layouts
    and its registry route (maintenance2, create_renderpass2) on Vulkan 1.0."""
    if row["id"] != SEPARATE_DEPTH_STENCIL_ID:
        return None
    if query.get("route") != "VK_KHR_separate_depth_stencil_layouts":
        raise ValueError("separate depth/stencil layouts public query route is absent")
    value = query.get("separateDepthStencilLayouts")
    if not isinstance(value, bool):
        raise ValueError("invalid separate depth/stencil layouts public query")
    route = {"VK_KHR_separate_depth_stencil_layouts", "VK_KHR_create_renderpass2",
             "VK_KHR_maintenance2", "VK_KHR_multiview"} <= extensions
    satisfied = value and route
    report = feature_reports.get("separateDepthStencilLayouts", {})
    implemented = (satisfied and report.get("kind") == "extension-feature" and
                   report.get("reported") is True and report.get("verdict") == "satisfied")
    return ({"state": "satisfied" if satisfied else "blocker", "observed": satisfied,
             "expected": row["expected"],
             "via": "VK_KHR_separate_depth_stencil_layouts" if satisfied else None,
             "detail": "Equivalent KHR feature on Vulkan 1.0; the Vulkan 1.2 aggregate is unadvertised."},
            {"state": "implemented" if implemented else "missing",
             "refs": ["src/vk_device.c", "src/vk_render_pass.c", "src/image_layout_state.c",
                      "native/graphics_queue_ps5.c", "conformance_inventory/reporting_matrix.json"],
             "detail": "Reviewed per-aspect layout state, render pass 2 stencil layouts, "
                       "barriers, load/store and readback for D32_SFLOAT_S8_UINT."})



def multiview_axes(row: dict, query: dict, extensions: set[str]) -> tuple[dict, dict] | None:
    """Resolve only the three reviewed core/KHR equivalent semantics.

    The route stays visible and the separate apiVersion row stays blocked.
    Query values come from the compiled public-entry-point reporting fixture,
    never from the expected DXVK floor or a hand-authored evidence override.
    """
    field = MULTIVIEW_FIELDS.get(row["id"])
    if field is None:
        return None
    if query.get("route") != "VK_KHR_multiview" or "VK_KHR_multiview" not in extensions:
        raise ValueError("multiview reporting route is absent or unsupported")
    value = query.get(field)
    if ((field == "multiview" and not isinstance(value, bool)) or
            (field != "multiview" and (isinstance(value, bool) or not isinstance(value, int) or value < 0))):
        raise ValueError("invalid multiview public query value")
    satisfied = value >= row["expected"]
    return ({"state": "satisfied" if satisfied else "blocker", "observed": value,
             "expected": row["expected"], "via": "VK_KHR_multiview",
             "detail": "Equivalent KHR field; Vulkan 1.2 aggregate structs and API 1.3 remain unadvertised."},
            {"state": "implemented" if satisfied else "missing",
             "refs": ["src/vk_device.c", "src/vk_render_pass.c", "native/graphics_queue_ps5.c"],
             "detail": "Reviewed multiview execution; a strict native witness is required, "
                       "and an observed applicable CTS failure blocks."})


def canonical(value: object) -> str:
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def version_tuple(value: str) -> tuple[int, int, int]:
    parts = value.split(".")
    if len(parts) != 3 or any(not part.isdigit() for part in parts):
        raise ValueError(f"invalid Vulkan version {value!r}")
    return tuple(int(part) for part in parts)  # type: ignore[return-value]


def decode_vk_version(value: int) -> tuple[int, int, int]:
    return ((value >> 22) & 0x7f, (value >> 12) & 0x3ff, value & 0xfff)


def implemented_device_extensions() -> set[str]:
    definitions = dict(re.findall(
        r'^#define\s+(VK_[A-Z0-9_]+_EXTENSION_NAME)\s+"([^"]+)"',
        VULKAN_HEADER.read_text(), re.MULTILINE))
    source_text = DEVICE_SOURCE.read_text()
    start = source_text.index("VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties")
    end = source_text.index("VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties", start)
    tokens = set(re.findall(r'VK_[A-Z0-9_]+_EXTENSION_NAME',
                            source_text[start:end]))
    # The capability probe creates a Vulkan instance without VK_KHR_surface.
    # Its device-extension count therefore excludes the surface-dependent WSI
    # route, even when a native graphics build can offer it to a surface-enabled
    # instance. Keep the conditional guard explicit so an unconditional
    # advertisement cannot silently inherit this probe exception.
    swapchain_token = "VK_KHR_SWAPCHAIN_EXTENSION_NAME"
    if swapchain_token in tokens:
        surface_gate = source_text.index("static int swapchain_supported(")
        if ("if (swapchain_supported(p))" not in source_text[start:end] or
                "p->instance->surface_extension_enabled" not in
                source_text[surface_gate:start]):
            raise ValueError("VK_KHR_swapchain lacks its instance-surface gate")
        tokens.remove(swapchain_token)
    # The extension is implemented only when the native platform advertises
    # the feature that makes its public query and device-create route usable.
    if "PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT" not in PLATFORM_SOURCE.read_text():
        tokens.discard("VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME")
    missing = sorted(token for token in tokens if token not in definitions)
    if missing:
        raise ValueError("unresolved device extension macros: " + ", ".join(missing))
    # Count only bits assigned to the native platform's supported-features
    # mask, keeping the capability probe aligned with the ordinary build.
    platform_source = (ROOT / "native/platform_ps5.c").read_text()
    # A default-off measurement build is not the shipping capability probe.
    # Strip only this explicitly named conditional block, and fail closed if
    # its preprocessor boundary is malformed rather than counting its bits.
    for name in (


        "PS5VK_DESCRIPTOR_UPDATE_TEMPLATE_DIAGNOSTIC",

        "PS5VK_DXVK_RENDER_DIAGNOSTIC",


        "PS5VK_DXVK_ROUTES_DIAGNOSTIC",

        "PS5VK_SHADER_INT16_DIAGNOSTIC",

        "PS5VK_MAINTENANCE4_DIAGNOSTIC",

        "PS5VK_HOST_COHERENT_DIAGNOSTIC",

    ):
        guard = f"#if defined({name}) && {name}"
        if guard in platform_source:
            pattern = re.compile(r"^" + re.escape(guard) + r"\n.*?^#endif\s*$",
                                 re.MULTILINE | re.DOTALL)
            blocks = list(pattern.finditer(platform_source))
            if (len(blocks) != 1 or
                    re.search(r"^#(?:if|ifdef|ifndef|elif|else)\b",
                              blocks[0].group()[len(guard):],
                              re.MULTILINE)):
                raise ValueError(f"malformed {name} guard")
            platform_source = pattern.sub("", platform_source)
    platform_source = re.sub(r"/\*.*?\*/|//[^\n]*", "", platform_source, flags=re.DOTALL)
    assignments = re.findall(r"platform->supported_features(?:_t09)?\s*(?:\|=|=)\s*(.*?);",
                             platform_source, re.DOTALL)
    shipping_bits = {bit for assignment in assignments
                     for bit in re.findall(r"PS5VK_(?:T09_)?FEATURE_[A-Z0-9_]+", assignment)}
    gates = {
        "VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME": {
            "PS5VK_FEATURE_STORAGE_BUFFER_8BIT", "PS5VK_FEATURE_STORAGE_BUFFER_16BIT"},
        "VK_KHR_8BIT_STORAGE_EXTENSION_NAME": {"PS5VK_FEATURE_STORAGE_BUFFER_8BIT"},
        "VK_KHR_16BIT_STORAGE_EXTENSION_NAME": {"PS5VK_FEATURE_STORAGE_BUFFER_16BIT"},
        "VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME": {
            "PS5VK_FEATURE_SHADER_DRAW_PARAMETERS"},
        "VK_KHR_MULTIVIEW_EXTENSION_NAME": {"PS5VK_FEATURE_MULTIVIEW"},
        "VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME": {
            "PS5VK_FEATURE_VULKAN_MEMORY_MODEL"},
        "VK_KHR_DEVICE_GROUP_EXTENSION_NAME": {
            "PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS"},
        "VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME": {
            "PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS"},
        "VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME": {
            "PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT"},

        "VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_HOST_QUERY_RESET"},
        "VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE"},

        "VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE"},
        "VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS"},
        "VK_KHR_MAINTENANCE_2_EXTENSION_NAME": {"PS5VK_T09_FEATURE_MAINTENANCE2"},
        "VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_CREATE_RENDERPASS2"},
        "VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK"},
        "VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION"},
        "VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION"},
        "VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_GET_MEMORY_REQUIREMENTS2"},
        "VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_DEDICATED_ALLOCATION"},
        "VK_KHR_BIND_MEMORY_2_EXTENSION_NAME": {"PS5VK_T09_FEATURE_BIND_MEMORY2"},
        "VK_KHR_MAINTENANCE_4_EXTENSION_NAME": {"PS5VK_T09_FEATURE_MAINTENANCE4"},
        "VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_DESCRIPTOR_UPDATE_TEMPLATE"},
        "VK_EXT_ROBUSTNESS_2_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2", "PS5VK_T09_FEATURE_NULL_DESCRIPTOR"},
        # DXVK262-T10 recording routes, unadvertised until a native witness.
        "VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE"},
        "VK_KHR_MAINTENANCE_1_EXTENSION_NAME": {"PS5VK_T09_FEATURE_MAINTENANCE1"},
        "VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME": {"PS5VK_T09_FEATURE_COPY_COMMANDS2"},
        "VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_DEPTH_STENCIL_RESOLVE"},
        "VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME": {"PS5VK_T09_FEATURE_DYNAMIC_RENDERING"},
        "VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_FORMAT_FEATURE_FLAGS2"},
        "VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_IMAGE_FORMAT_LIST"},
        "VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_SYNCHRONIZATION2"},
        "VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME": {
            "PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER"},

    }
    unmapped = sorted(tokens - gates.keys())
    if unmapped:
        raise ValueError("unmapped conditional device extension macros: " + ", ".join(unmapped))
    return {definitions[token] for token in tokens if gates[token] & shipping_bits}


def core_indexes(requirements: dict) -> tuple[dict[str, list[dict]],
                                               dict[str, list[dict]],
                                               dict[str, list[dict]]]:
    feature_index: dict[str, list[dict]] = {}
    property_index: dict[str, list[dict]] = {}
    extension_index: dict[str, list[dict]] = {}
    for row in requirements.get("requirements", []):
        for name in row.get("features", []):
            feature_index.setdefault(name, []).append(row)
        for name in row.get("limits", []):
            property_index.setdefault(name, []).append(row)
        for name in row.get("extensions", []):
            extension_index.setdefault(name, []).append(row)
    return feature_index, property_index, extension_index


# CTS is regression evidence, not a readiness gate.  A capability that is
# exposed, implemented and strictly native-witnessed is ready whether or not a
# CTS leaf is mapped or has run.  CTS still records two positive routes:
#   cts-pass          original leaves inside the frozen regression selection;
#   cts-focused-pass  original leaves that passed a focused run of an exact
#                     artifact and case list outside that selection, every
#                     named case Pass (NotSupported, Skip or Fail never count);
# and an observed applicable failure, cts-fail, stays visible and blocks the
# row until it is explained or fixed.  No route relaxes the API or native axes.
CTS_PASS = ("cts-pass", "cts-focused-pass")
CTS_BLOCKING = ("cts-fail",)
SHA256 = re.compile(r"[0-9a-f]{64}")
CASE_NAME = re.compile(r"dEQP-VK(?:\.[A-Za-z0-9_\-]+)+")


def _refs(override: dict, what: str) -> list[str]:
    refs = override.get("refs")
    if (not isinstance(refs, list) or not refs or
            any(not isinstance(ref, str) or not ref for ref in refs)):
        raise ValueError(f"{what} needs non-empty refs")
    return refs


def _run_ids(override: dict, what: str) -> list[str]:
    runs = override.get("run_ids")
    if (not isinstance(runs, list) or not runs or
            any(not isinstance(run, str) or not run for run in runs)):
        raise ValueError(f"{what} needs at least one run id")
    return runs


def _artifact(override: dict, key: str, what: str) -> str:
    value = override.get(key)
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise ValueError(f"{what} needs a SHA-256 {key}")
    return value


def focused_cts(override: dict, selected_cases: set[str],
                diagnostic_cases: set[str]) -> dict:
    claimed = override.get("cases")
    if (not isinstance(claimed, list) or not claimed or len(set(claimed)) != len(claimed) or
            any(not isinstance(case, str) or not CASE_NAME.fullmatch(case)
                for case in claimed)):
        raise ValueError("focused CTS evidence needs unique exact dEQP-VK leaf names")
    if set(claimed) & diagnostic_cases:
        raise ValueError("focused CTS evidence names a case the frozen package keeps "
                         "as a diagnostic; reconcile the manifest first")
    if set(claimed) <= selected_cases:
        raise ValueError("every focused case is in the frozen selection; use cts-pass")
    if override.get("result") != {"Pass": len(claimed)}:
        raise ValueError("focused CTS result must be exactly Pass for every named case")
    return {"run_ids": _run_ids(override, "focused CTS evidence"),
            "artifact_sha256": _artifact(override, "artifact_sha256", "focused CTS evidence"),
            "case_list_sha256": _artifact(override, "case_list_sha256",
                                          "focused CTS evidence"),
            "result": override["result"]}


def cts_join(rows: list[dict], override: dict | None,
             selected_cases: set[str], diagnostic_cases: set[str]) -> dict:
    cases = sorted({case for row in rows for case in row.get("cts", {}).get("cases", [])})
    requirement_ids = sorted({row["id"] for row in rows})
    if override:
        state = override.get("state")
        extra: dict = {}
        if state == "cts-focused-pass":
            extra = focused_cts(override, selected_cases, diagnostic_cases)
            claimed = override["cases"]
        elif state in ("cts-pass", "cts-fail"):
            claimed = override.get("cases", [])
            allowed = selected_cases if state == "cts-pass" else diagnostic_cases
            if not claimed or not set(claimed).issubset(allowed):
                raise ValueError("CTS evidence names a case outside the current upstream "
                                 f"{'selection' if state == 'cts-pass' else 'diagnostics'}")
        else:
            raise ValueError(f"unsupported CTS evidence state {state!r}")
        return {
            "state": state, "cases": claimed,
            "mapped_cases": cases, "related_requirement_ids": requirement_ids,
            "refs": override.get("refs", []), "note": override.get("note", ""),
            **extra,
        }
    return {
        "state": "mapped-not-run" if cases else "not-mapped",
        "cases": [], "mapped_cases": cases,
        "related_requirement_ids": requirement_ids,
        "refs": [],
    }


def validate_native(identifier: str, native: dict) -> None:
    state = native.get("state")
    if state not in ("native-evidence", "witnessed-blocker",
                     "reported-not-executed", "not-run"):
        raise ValueError(f"invalid native state for {identifier}")
    if state == "native-evidence":
        # Per-capability execution evidence is bound to an exact artifact.
        what = f"native evidence for {identifier}"
        _run_ids(native, what)
        _artifact(native, "artifact_sha256", what)
        _refs(native, what)


def row_ready(row: dict) -> bool:
    return (row["api"]["state"] == "satisfied" and
            row["implementation"]["state"] == "implemented" and
            row["cts"]["state"] not in CTS_BLOCKING and
            row["native"]["state"] == "native-evidence")


def implementation_for(row: dict, feature_reports: dict[str, dict],
                       extensions: set[str], current_api: tuple[int, int, int]) -> dict:
    kind = row["kind"]
    if kind == "api-version":
        ok = current_api >= version_tuple(row["expected"])
        return {"state": "implemented" if ok else "missing",
                "refs": ["src/physical_device_profile.h", "src/vk_device.c"]}
    if kind == "extension":
        ok = row["name"] in extensions
        return {"state": "implemented" if ok else "missing",
                "refs": ["src/vk_device.c"]}
    if kind == "feature" and row["container"] == "VkPhysicalDeviceFeatures":
        report = feature_reports.get(row["name"])
        ok = bool(report and report.get("reported") is True and
                  report.get("verdict") == "satisfied")
        return {"state": "implemented" if ok else "missing",
                "refs": ["conformance_inventory/reporting_matrix.json"],
                "detail": report.get("detail") if report else "feature is absent from the public reporting matrix"}
    return {"state": "missing", "refs": [],
            "detail": "No reviewed Vulkan 1.1+ or extension-feature implementation is advertised."}


def api_for(row: dict, feature_reports: dict[str, dict], extensions: set[str],
            current_api: tuple[int, int, int]) -> dict:
    kind = row["kind"]
    if kind == "api-version":
        expected = version_tuple(row["expected"])
        return {"state": "satisfied" if current_api >= expected else "blocker",
                "observed": ".".join(map(str, current_api)), "expected": row["expected"]}
    if kind == "extension":
        observed = 1 if row["name"] in extensions else 0
        return {"state": "satisfied" if observed >= row["expected"] else "blocker",
                "observed": observed, "expected": row["expected"]}
    if kind == "feature" and row["container"] == "VkPhysicalDeviceFeatures":
        report = feature_reports.get(row["name"])
        observed = bool(report and report.get("reported") is True)
        return {"state": "satisfied" if observed == row["expected"] else "blocker",
                "observed": observed, "expected": row["expected"]}
    return {"state": "blocker", "observed": None, "expected": row["expected"],
            "detail": "The public device reports Vulkan 1.0 and does not expose this profile structure."}


def generate() -> dict:
    profile = json.loads(PROFILE.read_text())
    evidence = json.loads(EVIDENCE.read_text())
    reporting = json.loads(REPORTING.read_text())
    requirements = json.loads(CORE_REQUIREMENTS.read_text())
    if evidence.get("profile_id") != profile["profile"]["id"]:
        raise ValueError("DXVK evidence profile id mismatch")
    overrides = evidence.get("requirements", {})
    profile_ids = {row["id"] for row in profile["requirements"]}
    unknown = sorted(set(overrides) - profile_ids)
    if unknown:
        raise ValueError("evidence names unknown DXVK requirements: " + ", ".join(unknown))
    probe = evidence.get("capability_probe")
    probe_satisfied: set[str] = set()
    if probe:
        probe_satisfied = set(probe.get("satisfied_ids", []))
        runs = probe.get("runs", [])
        if (probe.get("requirements") != len(profile_ids) or
                probe.get("device_api") != "1.0.0" or
                probe.get("transport") != "ps5log/1" or
                probe.get("verifier") != "tools/verify_dxvk_probe.py" or
                len(runs) < 1 or
                not re.fullmatch(r"[0-9a-f]{64}", probe.get("artifact_sha256", "")) or
                any(not run.get("id") or
                    not re.fullmatch(r"[0-9a-f]{64}", run.get("log_sha256", ""))
                    for run in runs) or
                not probe_satisfied.issubset(profile_ids)):
            raise ValueError("invalid DXVK native capability-probe evidence")

    current_api = decode_vk_version(reporting["profiles"]["graphics"]["apiVersion"])
    extensions = implemented_device_extensions()
    if probe and (probe["device_api"] != ".".join(map(str, current_api)) or
                  probe.get("device_extensions") != len(extensions)):
        raise ValueError("DXVK native capability probe no longer matches public reporting")
    feature_reports = {row["feature"]: row for row in reporting["features"]
                       if row.get("profile") == "graphics"}
    feature_index, property_index, extension_index = core_indexes(requirements)
    selected_cases = set(reporting.get("applicable_cts_selection", {}).get("cases", []))
    diagnostic_cases = set(reporting.get("applicable_cts_selection", {}).get("diagnostics", []))
    rows = []
    for requirement in profile["requirements"]:
        identifier = requirement["id"]
        override = overrides.get(identifier, {})
        if requirement["kind"] == "feature":
            related = feature_index.get(requirement["name"], [])
        elif requirement["kind"] == "property":
            related = property_index.get(requirement["name"], [])
        elif requirement["kind"] == "extension":
            related = extension_index.get(requirement["name"], [])
        else:
            related = []
        api = api_for(requirement, feature_reports, extensions, current_api)
        implementation = implementation_for(
            requirement, feature_reports, extensions, current_api)
        multiview = multiview_axes(requirement,
            reporting["profiles"]["graphics"].get("multiview_query", {}), extensions)
        if multiview is not None:
            api, implementation = multiview
        standard_ubo = standard_ubo_axes(requirement,
            reporting["profiles"]["graphics"].get("standard_ubo_query", {}),
            extensions, feature_reports)
        if standard_ubo is not None:
            api, implementation = standard_ubo
        memory_model = memory_model_axes(requirement,
            reporting["profiles"]["graphics"].get("memory_model_query", {}),
            extensions, feature_reports)
        if memory_model is not None:
            api, implementation = memory_model
        buffer_address = buffer_address_axes(requirement,
            reporting["profiles"]["graphics"].get("buffer_device_address_query", {}),
            extensions, feature_reports)
        if buffer_address is not None:
            api, implementation = buffer_address

        host_query_reset = host_query_reset_axes(requirement,
            reporting["profiles"]["graphics"].get("host_query_reset_query", {}),
            extensions, feature_reports)
        if host_query_reset is not None:
            api, implementation = host_query_reset
        sampler_mirror_clamp = sampler_mirror_clamp_axes(requirement, extensions)
        if sampler_mirror_clamp is not None:
            api, implementation = sampler_mirror_clamp

        timeline = timeline_axes(requirement,
            reporting["profiles"]["graphics"].get("timeline_semaphore_query", {}),
            extensions, feature_reports)
        if timeline is not None:
            api, implementation = timeline
        separate = separate_depth_stencil_axes(requirement,
            reporting["profiles"]["graphics"].get("separate_depth_stencil_layouts_query", {}),
            extensions, feature_reports)
        if separate is not None:
            api, implementation = separate
        routed = extension_route_axes(requirement,
            reporting["profiles"]["graphics"].get("extension_route_queries", {}),
            extensions, feature_reports)
        if routed is not None:
            api, implementation = routed

        diagnostic = diagnostic_implementation(identifier)
        if diagnostic is not None and sampler_mirror_clamp is None:
            implementation = diagnostic

        cts = cts_join(related, override.get("cts"), selected_cases, diagnostic_cases)
        if "native" in override:
            native = override["native"]
        elif probe:
            native = {
                "state": ("reported-not-executed" if identifier in probe_satisfied
                          else "witnessed-blocker"),
                "run_ids": [run["id"] for run in probe["runs"]],
                "artifact_sha256": probe["artifact_sha256"],
                "refs": ["VALIDATION.md#dxvk-262-public-abi-capability-probe"],
                "note": ("The native query reports the requested value, but the probe "
                         "does not execute the capability." if identifier in probe_satisfied
                         else "Exact native query evidence witnessed the current blocker."),
            }
        else:
            native = {"state": "not-run", "refs": []}
        validate_native(identifier, native)
        row = {
            **requirement,
            "api": api,
            "implementation": implementation,
            "cts": cts,
            "native": native,
        }
        row["verdict"] = "satisfied" if row_ready(row) else "blocker"
        rows.append(row)

    if probe:
        api_satisfied = {row["id"] for row in rows
                         if row["api"]["state"] == "satisfied"}
        if api_satisfied != probe_satisfied:
            raise ValueError("DXVK native capability probe satisfied set drift")

    dimensions = {}
    for name, success in (("api", ("satisfied",)), ("implementation", ("implemented",)),
                          ("native", ("native-evidence",))):
        satisfied = sum(row[name]["state"] in success for row in rows)
        dimensions[name] = {"satisfied": satisfied,
                            "blocker": len(rows) - satisfied}
    # CTS is evidence, not a gate: report passes and observed failures, and
    # leave the rest as "no-evidence" instead of calling it a blocker.
    passed = sum(row["cts"]["state"] in CTS_PASS for row in rows)
    failed = sum(row["cts"]["state"] in CTS_BLOCKING for row in rows)
    dimensions["cts"] = {"pass": passed, "fail": failed,
                         "no-evidence": len(rows) - passed - failed}
    return {
        "schema": SCHEMA,
        "profile": profile["profile"],
        "source": profile["source"],
        "policy": {
            "ready_rule": ("api=satisfied AND implementation=implemented AND "
                           "native=native-evidence AND cts not in cts_blocking_states"),
            "cts_blocking_states": list(CTS_BLOCKING),
            "cts_scope": ("regression evidence per capability (frozen selection or a "
                          "focused run); a missing, unmapped or unrun leaf never blocks, "
                          "an observed applicable failure always does; never whole-suite "
                          "CTS or conformance"),
            "missing_evidence": "blocker",
            "scope": "DXVK v2.6.2 D3D11 feature level 11_0 baseline only",
        },
        "current_driver": {
            "api_version": ".".join(map(str, current_api)),
            "device_extensions": sorted(extensions),
            "reported_source": reporting["reported_source"],
            "native_probe": probe,
        },
        "summary": {
            "requirements": len(rows),
            "satisfied": sum(row["verdict"] == "satisfied" for row in rows),
            "blocker": sum(row["verdict"] == "blocker" for row in rows),
            "dimensions": dimensions,
        },
        "requirements": rows,
    }


def validate(document: dict) -> None:
    if document.get("schema") != SCHEMA:
        raise ValueError("DXVK matrix schema mismatch")
    rows = document.get("requirements", [])
    ids = [row.get("id") for row in rows]
    if len(ids) != len(set(ids)):
        raise ValueError("DXVK matrix has duplicate ids")
    summary = document.get("summary", {})
    if summary.get("requirements") != len(rows):
        raise ValueError("DXVK matrix count drift")
    for row in rows:
        if (row.get("verdict") == "satisfied") != row_ready(row):
            raise ValueError(f"non-fail-closed verdict for {row.get('id')}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    document = generate()
    validate(document)
    rendered = canonical(document)
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_text() != rendered:
            raise ValueError("checked-in DXVK v2.6.2 matrix is stale")
    else:
        OUTPUT.write_text(rendered)
    summary = document["summary"]
    print(f"DXVK v2.6.2 matrix: {summary['satisfied']}/{summary['requirements']} "
          f"ready, {summary['blocker']} blockers")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError, KeyError) as error:
        print(f"check_dxvk_profile.py: {error}", file=sys.stderr)
        raise SystemExit(1)
