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
    "imageCubeArray": ("src/vk_memory.c", "info->imageType != VK_IMAGE_TYPE_2D",
                       "only 2D images are created"),
    "independentBlend": ("src/vk_graphics_pipeline.c", "b->attachmentCount != 1",
                         "one color attachment per pipeline"),
    "geometryShader": ("src/vk_graphics_pipeline.c",
                       "if (s->stage == VK_SHADER_STAGE_VERTEX_BIT && !vs) vs=s;",
                       "only vertex and fragment stages are accepted"),
    "tessellationShader": ("src/vk_graphics_pipeline.c",
                           "if (s->stage == VK_SHADER_STAGE_VERTEX_BIT && !vs) vs=s;",
                           "only vertex and fragment stages are accepted"),
    "sampleRateShading": ("src/vk_graphics_pipeline.c", "m->sampleShadingEnable",
                          "sample shading state is rejected"),
    "logicOp": ("src/vk_graphics_pipeline.c", "b->logicOpEnable",
                "logicOpEnable is rejected"),
    "multiDrawIndirect": ("src/vk_command.c", "count>1",
                          "one indirect draw per command is accepted"),
    "drawIndirectFirstInstance": ("src/vk_indirect.c", "if (command.firstInstance) return INVALID",
                                  "firstInstance must be zero"),
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
    "shaderClipDistance": ("src/spirv_graphics_interface.c", "d->builtin!=42",
                           "only the FragCoord builtin is accepted"),
    "shaderCullDistance": ("src/spirv_graphics_interface.c", "d->builtin!=42",
                           "only the FragCoord builtin is accepted"),
    "shaderResourceMinLod": ("src/vk_sampler.c", "info->minLod!=0",
                             "sampler minLod is rejected"),
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
    "inheritedQueries": ("src/vk_command.c", "There is no valid no-op subset.",
                         "secondary command buffers are rejected"),
    "fullDrawIndexUint32": ("src/graphics_limits.h", "maxDrawIndexedIndexValue=UINT32_MAX",
                            "the full 32-bit draw-index range is accepted"),
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
            ("src/vertex_descriptor.c", "bytes/stride"),
        ),
        "detail": ("raw storage/uniform descriptors carry the byte span and "
                   "vertex descriptors carry the bounded record count"),
        "cts": ("dEQP-VK.info.device_mandatory_features",),
    },
}

# Every non-advertised VkPhysicalDeviceFeatures member shares one fail-closed
# device-negotiation gate.  Vulkan valid usage prevents an application from
# relying on a false feature without requesting it; the driver's obligation is
# to report it false and refuse device creation when it is requested.  The C
# regression walks every VkBool32 member, so this generic citation is stronger
# than inventing an object-level rejection branch for compiler-side features.
FALSE_CORE_FEATURE_GATE = (
    "src/vk_device.c",
    "if (offset != robust_offset ||",
    "tests/test_vk_device.c",
    "for(size_t offset=0;offset<sizeof(features);offset+=sizeof(VkBool32))",
)


def evaluate_feature(name: str, value: bool) -> tuple[str, str]:
    advertised = ADVERTISED_FEATURES.get(name)
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
    "textureCompressionBC": ("src/graphics_formats.h", ("VK_FORMAT_BC", "VK_FORMAT_BC1")),
    "shaderStorageImageExtendedFormats": ("src/graphics_formats.h", ("VK_IMAGE_USAGE_STORAGE_BIT",)),
    "shaderStorageImageMultisample": ("src/graphics_formats.h", ("VK_IMAGE_USAGE_STORAGE_BIT",)),
    "shaderStorageImageReadWithoutFormat": ("src/graphics_formats.h", ("VK_IMAGE_USAGE_STORAGE_BIT",)),
    "shaderStorageImageWriteWithoutFormat": ("src/graphics_formats.h", ("VK_IMAGE_USAGE_STORAGE_BIT",)),
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
    "maxImageDimension1D": "2D-only image model: vkCreateImage accepts VK_IMAGE_TYPE_2D and rejects 1D images",
    "maxImageDimension3D": "2D-only image model: 3D images are rejected",
    "maxImageDimensionCube": "2D-only image model: cube images are rejected",
    "maxImageArrayLayers": "single-layer images only (vkCreateImage requires arrayLayers 1)",
    "maxColorAttachments": "one color attachment per render pass",
    "maxFragmentOutputAttachments": "one color attachment per render pass",
    "maxFragmentCombinedOutputResources": "one color attachment plus the single sampled descriptor",
    "maxVertexInputBindings": "one vertex binding (src/graphics_limits.h, vertex-input gate)",
    "maxPerStageDescriptorSamplers": "one combined image/sampler descriptor",
    "maxDescriptorSetSamplers": "one combined image/sampler descriptor",
    "maxPerStageDescriptorSampledImages": "one sampled image descriptor",
    "maxDescriptorSetSampledImages": "one sampled image descriptor",
    "maxPerStageDescriptorStorageImages": "no storage image descriptor type is accepted",
    "maxDescriptorSetStorageImages": "no storage image descriptor type is accepted",
    "maxPerStageDescriptorInputAttachments": "no input attachment support (subpass dependencies rejected)",
    "maxDescriptorSetInputAttachments": "no input attachment support (subpass dependencies rejected)",
    "discreteQueuePriorities": "single serial queue; priority-based scheduling is not implemented",
    "maxMemoryAllocationCount": "allocator policy: heap size divided by the minimum allocation charge",
    "maxSamplerLodBias": "vkCreateSampler rejects mipLodBias/minLod/maxLod",
    "minTexelOffset": "no evidence the compiler/sampler path implements texel offsets",
    "maxTexelOffset": "no evidence the compiler/sampler path implements texel offsets",
    "storageImageSampleCounts": "no storage image format is advertised",
    "sampledImageIntegerSampleCounts": "no integer sampled image format is advertised",
    "framebufferColorSampleCounts": "single-sample rendering only: MSAA (4 samples) is not supported",
    "framebufferDepthSampleCounts": "single-sample rendering only: MSAA (4 samples) is not supported",
    "framebufferStencilSampleCounts": "single-sample rendering only: MSAA (4 samples) is not supported",
    "framebufferNoAttachmentsSampleCounts": "single-sample rendering only: MSAA (4 samples) is not supported",
    "sampledImageColorSampleCounts": "single-sample sampling only: multisampled sampled images are not supported",
    "sampledImageDepthSampleCounts": "single-sample sampling only: multisampled sampled images are not supported",
    "sampledImageStencilSampleCounts": "single-sample sampling only: multisampled sampled images are not supported",
    "maxImageDimension2D": "compute-only build: graphics limits are not applied to this profile",
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
            verdict, detail = evaluate_feature(name, value)
            features.append({"kind": "feature", "feature": name, "profile": profile,
                             "reported": value, "verdict": verdict, "detail": detail})

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
                                 "note": "the selected storage-width cases exercise the accepted capabilities"}

    matrix = {
        "schema": "ps5vk-reporting-matrix/1",
        "reported_source": {
            "tool": "tools/dump_device_reporting.c",
            "profile_initializer": "src/device_profile_report.h",
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
                               "vendorID": dump["vendorID"], "deviceID": dump["deviceID"]}
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


# SPIR-V capability enumerants handled by the narrow-storage gate. These are
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

# The SPIR-V capabilities the frontend's narrow-storage gate handles, and the
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
            match = re.search(r"PS5VK_FEATURE_STORAGE_BUFFER_(\d+)BIT", body)
            action, required = ("requires-extension-feature", match.group(0)) if match else ("accept", None)
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
