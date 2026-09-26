#!/usr/bin/env python3
"""Run one DXVK native PS5 payload and write a JSON receipt for its artifact.

The payload must already be deployed; this tool never deploys. It launches the
title, waits for the ps5log/1 run, closes the title, and records the build
identity, every stage marker, the first refusal (DXVK log line, Vulkan call,
result and parameters), the pixel oracle, a crash record if any, and whether
the title stopped cleanly. A refusal is a result, not a tool failure: the tool
fails only when the run cannot be attributed to the artifact or the title does
not stop.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_ps5_native import PROFILE  # noqa: E402

RECORD = re.compile(r"^(\d+)\t(\d+)\t([^\t]+)\t(.*)$")
FIELD = re.compile(r"(\w+)=(\S*)")
STAGE = re.compile(r"^DXVK_NATIVE_STAGE stage=(\S+) state=(\S+) ?(.*)$")
LIMIT = 40


# VkBool32 members in declaration order; DXVK_VK_FEATURES masks index them.
FEATURE_MEMBERS = {
    "core": (
        "robustBufferAccess fullDrawIndexUint32 imageCubeArray independentBlend geometryShader "
        "tessellationShader sampleRateShading dualSrcBlend logicOp multiDrawIndirect "
        "drawIndirectFirstInstance depthClamp depthBiasClamp fillModeNonSolid depthBounds "
        "wideLines largePoints alphaToOne multiViewport samplerAnisotropy textureCompressionETC2 "
        "textureCompressionASTC_LDR textureCompressionBC occlusionQueryPrecise "
        "pipelineStatisticsQuery vertexPipelineStoresAndAtomics fragmentStoresAndAtomics "
        "shaderTessellationAndGeometryPointSize shaderImageGatherExtended "
        "shaderStorageImageExtendedFormats shaderStorageImageMultisample "
        "shaderStorageImageReadWithoutFormat shaderStorageImageWriteWithoutFormat "
        "shaderUniformBufferArrayDynamicIndexing shaderSampledImageArrayDynamicIndexing "
        "shaderStorageBufferArrayDynamicIndexing shaderStorageImageArrayDynamicIndexing "
        "shaderClipDistance shaderCullDistance shaderFloat64 shaderInt64 shaderInt16 "
        "shaderResourceResidency shaderResourceMinLod sparseBinding sparseResidencyBuffer "
        "sparseResidencyImage2D sparseResidencyImage3D sparseResidency2Samples "
        "sparseResidency4Samples sparseResidency8Samples sparseResidency16Samples "
        "sparseResidencyAliased variableMultisampleRate inheritedQueries").split(),
    "vk11": (
        "storageBuffer16BitAccess uniformAndStorageBuffer16BitAccess storagePushConstant16 "
        "storageInputOutput16 multiview multiviewGeometryShader multiviewTessellationShader "
        "variablePointersStorageBuffer variablePointers protectedMemory samplerYcbcrConversion "
        "shaderDrawParameters").split(),
    "vk12": (
        "samplerMirrorClampToEdge drawIndirectCount storageBuffer8BitAccess "
        "uniformAndStorageBuffer8BitAccess storagePushConstant8 shaderBufferInt64Atomics "
        "shaderSharedInt64Atomics shaderFloat16 shaderInt8 descriptorIndexing "
        "shaderInputAttachmentArrayDynamicIndexing shaderUniformTexelBufferArrayDynamicIndexing "
        "shaderStorageTexelBufferArrayDynamicIndexing shaderUniformBufferArrayNonUniformIndexing "
        "shaderSampledImageArrayNonUniformIndexing shaderStorageBufferArrayNonUniformIndexing "
        "shaderStorageImageArrayNonUniformIndexing shaderInputAttachmentArrayNonUniformIndexing "
        "shaderUniformTexelBufferArrayNonUniformIndexing "
        "shaderStorageTexelBufferArrayNonUniformIndexing "
        "descriptorBindingUniformBufferUpdateAfterBind descriptorBindingSampledImageUpdateAfterBind "
        "descriptorBindingStorageImageUpdateAfterBind descriptorBindingStorageBufferUpdateAfterBind "
        "descriptorBindingUniformTexelBufferUpdateAfterBind "
        "descriptorBindingStorageTexelBufferUpdateAfterBind "
        "descriptorBindingUpdateUnusedWhilePending descriptorBindingPartiallyBound "
        "descriptorBindingVariableDescriptorCount runtimeDescriptorArray samplerFilterMinmax "
        "scalarBlockLayout imagelessFramebuffer uniformBufferStandardLayout "
        "shaderSubgroupExtendedTypes separateDepthStencilLayouts hostQueryReset timelineSemaphore "
        "bufferDeviceAddress bufferDeviceAddressCaptureReplay bufferDeviceAddressMultiDevice "
        "vulkanMemoryModel vulkanMemoryModelDeviceScope "
        "vulkanMemoryModelAvailabilityVisibilityChains shaderOutputViewportIndex "
        "shaderOutputLayer subgroupBroadcastDynamicId").split(),
    "vk13": (
        "robustImageAccess inlineUniformBlock descriptorBindingInlineUniformBlockUpdateAfterBind "
        "pipelineCreationCacheControl privateData shaderDemoteToHelperInvocation "
        "shaderTerminateInvocation subgroupSizeControl computeFullSubgroups synchronization2 "
        "textureCompressionASTC_HDR shaderZeroInitializeWorkgroupMemory dynamicRendering "
        "shaderIntegerDotProduct maintenance4").split(),
}
# DXVK 2.6.2 D3D11 feature-level gate. Every feature level needs the baseline
# terms (D3D11Device::GetDeviceFeatures, d3d11_device.cpp:1941-1968, checked by
# DxvkAdapter::checkFeatureSupport); 11_0 additionally needs the fl11_0 terms
# (D3D11DeviceFeatures::GetMaxFeatureLevel, d3d11_features.cpp:377-380).
FL_GATE = {
    "baseline": (
        "core.depthBiasClamp core.depthClamp core.dualSrcBlend core.fillModeNonSolid "
        "core.fullDrawIndexUint32 core.geometryShader core.imageCubeArray core.independentBlend "
        "core.multiViewport core.occlusionQueryPrecise core.sampleRateShading "
        "core.shaderClipDistance core.shaderCullDistance core.shaderImageGatherExtended "
        "core.textureCompressionBC vk12.samplerMirrorClampToEdge "
        "vk13.shaderDemoteToHelperInvocation xfb.transformFeedback xfb.geometryStreams").split(),
    "fl11_0": ("core.drawIndirectFirstInstance core.fragmentStoresAndAtomics "
               "core.multiDrawIndirect core.tessellationShader").split(),
}
# Terms a DIAGNOSTIC source patch removes from the gate.
FL_GATE_RELAXATIONS = {
    "src/d3d11/d3d11_device.cpp:fl-gate-transform-feedback-relaxed":
        ("xfb.transformFeedback", "xfb.geometryStreams"),
    "src/d3d11/d3d11_device.cpp:fl-gate-demote-to-helper-relaxed":
        ("vk13.shaderDemoteToHelperInvocation",),
}


def decode_features(records: list[dict]) -> dict[str, dict[str, bool]]:
    """Decode the first report of each DXVK_VK_FEATURES struct."""
    decoded: dict[str, dict[str, bool]] = {}
    for record in records:
        struct = record.get("struct")
        if struct not in FEATURE_MEMBERS or struct in decoded or "mask" not in record:
            continue
        mask = int(record["mask"], 16)
        decoded[struct] = {name: bool(mask >> index & 1)
                           for index, name in enumerate(FEATURE_MEMBERS[struct])}
    return decoded


def feature_level_gate(summary: dict, patches: list[str]) -> dict | None:
    """Every FL-gate boolean as ps5vk reported it to DXVK (after any DIAGNOSTIC
    translation), which terms a patch relaxed, and which still fail."""
    decoded = decode_features(summary.get("features", []))
    if "core" not in decoded:
        return None
    extensions = {item.split(":")[0] for item in
                  summary.get("extensions", {}).get("vkEnumerateDeviceExtensionProperties", [])}
    relaxed = sorted({term for patch in patches for term in FL_GATE_RELAXATIONS.get(patch, ())})
    gate: dict = {"relaxed_by_diagnostic_patch": relaxed}
    failing = []
    for level, terms in FL_GATE.items():
        values = {}
        for term in terms:
            struct, member = term.split(".")
            if struct == "xfb":
                # DXVK chains the transform feedback struct only when the
                # extension is enumerated; the payload does not decode it.
                value = None if "VK_EXT_transform_feedback" in extensions else False
            else:
                value = decoded.get(struct, {}).get(member, False)
            values[term] = value
            if value is False and term not in relaxed:
                failing.append(term)
        gate[level] = values
    gate["failing"] = failing
    return gate


def fields(text: str) -> dict[str, str]:
    return dict(FIELD.findall(text))


def is_refusal_line(level: str | None, text: str) -> bool:
    """The payload's rule for DXVK log lines that refuse something."""
    return (level == "err" or (level == "warn" and text.startswith("Skipping")) or
            (text.startswith("Required ") and " not supported" in text))


def split_refusal(text: str) -> dict:
    """Parse DXVK_FIRST_REFUSAL. Free-form tails (params=, text=) run to the
    next known key or the end of the line."""
    refusal = {"raw": text}
    for key in ("source", "stage", "call", "level", "result", "last_vk_call"):
        match = re.search(rf"(?:^| ){key}=(\S+)", text)
        if match:
            refusal[key] = match.group(1)
    for key, stop in (("params", None), ("last_vk_params", " text="), ("text", None)):
        match = re.search(rf"(?:^| ){key}=(.*)", text)
        if match:
            value = match.group(1)
            if stop and stop in value:
                value = value[:value.index(stop)]
            refusal[key] = value.strip()
    if "result" in refusal:
        refusal["result"] = int(refusal["result"])
    return refusal


def parse_log(log: str) -> dict:
    """Summarize one payload run from its ps5log/1 records."""
    records = []
    for line in log.splitlines():
        match = RECORD.match(line)
        if match:
            records.append((int(match.group(1)), match.group(3), match.group(4)))
    summary: dict = {
        "records": len(records), "identity": None, "environment": None, "stages": [],
        "last_stage": None, "dxvk_log": {"counts": {}, "errors": [], "warnings": []},
        "vk_refusals": [], "vk_missing": [], "vk_calls_logged": 0, "vk_properties": None,
        "extensions": {}, "loader": [], "ps5vk_diagnostics": [], "first_refusal": None,
        "first_refusal_candidate": None, "oracle": None, "result": None, "crash": None,
        "trace": None, "gpu_hang_suspected": False, "features": [],
        "vk_first_call": None, "vk_last_call": None,
        "compat": {"refusals": [], "translations": []},
    }
    for seq, level, text in records:
        if text.startswith("DXVK_NATIVE_IDENTITY "):
            summary["identity"] = fields(text)
        elif text.startswith("DXVK_NATIVE_ENVIRONMENT "):
            summary["environment"] = text.split(" ", 1)[1]
        elif (match := STAGE.match(text)):
            stage = {"seq": seq, "stage": match.group(1), "state": match.group(2),
                     "detail": match.group(3)}
            summary["stages"].append(stage)
            if stage["state"] == "begin" and stage["stage"] != "shutdown":
                summary["last_stage"] = stage["stage"]
        elif text.startswith("DXVK_LOG "):
            entry = fields(text.split(" text=", 1)[0])
            body = text.split(" text=", 1)[1] if " text=" in text else ""
            counts = summary["dxvk_log"]["counts"]
            counts[entry.get("level", "raw")] = counts.get(entry.get("level", "raw"), 0) + 1
            bucket = {"err": "errors", "warn": "warnings"}.get(entry.get("level"))
            if bucket and len(summary["dxvk_log"][bucket]) < LIMIT:
                summary["dxvk_log"][bucket].append(
                    {"seq": seq, "stage": entry.get("stage"), "text": body})
            if summary["first_refusal_candidate"] is None and is_refusal_line(
                    entry.get("level"), body):
                summary["first_refusal_candidate"] = {
                    "seq": seq, "source": "dxvk_log", "level": entry.get("level"),
                    "stage": entry.get("stage"), "text": body}
        elif text.startswith("DXVK_VK_REFUSAL "):
            head = fields(text.split(" params=")[0])
            refusal = {"seq": seq, "text": text[len("DXVK_VK_REFUSAL "):],
                       "call": head.get("call"), "result": head.get("result"),
                       "stage": head.get("stage")}
            if len(summary["vk_refusals"]) < LIMIT:
                summary["vk_refusals"].append(refusal)
            if summary["first_refusal_candidate"] is None:
                summary["first_refusal_candidate"] = {"source": "vulkan", **refusal}
            if "DEVICE_LOST" in text:
                summary["gpu_hang_suspected"] = True
        elif text.startswith("DXVK_VK_MISSING "):
            summary["vk_missing"].append(fields(text).get("name"))
        elif text.startswith("DXVK_VK_CALL "):
            summary["vk_calls_logged"] += 1
            call = fields(text).get("call")
            summary["vk_first_call"] = summary["vk_first_call"] or call
            summary["vk_last_call"] = call
        elif text.startswith("DXVK_VK_PROPERTIES "):
            summary["vk_properties"] = summary["vk_properties"] or text.split(" ", 1)[1]
        elif text.startswith("DXVK_VK_EXTENSIONS "):
            entry = fields(text)
            summary["extensions"].setdefault(entry.get("call"), []).extend(
                item for item in entry.get("list", "").split(",") if item and item != "none")
        elif text.startswith("DXVK_LOADER "):
            summary["loader"].append(text[len("DXVK_LOADER "):])
        elif text.startswith("DXVK_FIRST_REFUSAL "):
            body = text[len("DXVK_FIRST_REFUSAL "):]
            summary["first_refusal"] = None if body == "source=none" else split_refusal(body)
        elif text.startswith("DXVK_ORACLE "):
            oracle = fields(text)
            summary["oracle"] = {
                "checked": int(oracle["checked"]), "mismatches": int(oracle["mismatches"]),
                "checksum": oracle["checksum"], "expected_checksum": oracle["expected_checksum"],
                "first": oracle.get("first")}
        elif text.startswith("DXVK_NATIVE_RESULT "):
            summary["result"] = fields(text)
        elif text.startswith("DXVK_NATIVE_CRASH "):
            summary["crash"] = text[len("DXVK_NATIVE_CRASH "):]
        elif text.startswith("DXVK_COMPAT_REFUSAL "):
            summary["compat"]["refusals"].append(fields(text).get("feature"))
        elif text.startswith("DXVK_COMPAT_"):
            if len(summary["compat"]["translations"]) < LIMIT:
                summary["compat"]["translations"].append(text)
        elif text.startswith("DXVK_VK_FEATURES "):
            summary["features"].append(fields(text))
        elif text.startswith("DXVK_VK_TRACE "):
            summary["trace"] = {key: int(value) for key, value in fields(text).items()}
        elif ("site=" in text or text.startswith("PS5VK_")) and \
                len(summary["ps5vk_diagnostics"]) < LIMIT:
            summary["ps5vk_diagnostics"].append({"seq": seq, "level": level, "text": text})
        if "VK_ERROR_DEVICE_LOST" in text or "DeviceLost" in text:
            summary["gpu_hang_suspected"] = True
    return summary


def check_identity(summary: dict, artifact: dict) -> list[str]:
    """Return the mismatches between the run's identity event and the artifact."""
    identity = summary.get("identity") or {}
    problems = []
    for key in ("variant", "label", "dxvk_commit", "ps5vk_commit", "eboot_sha256"):
        if identity.get(key) != str(artifact.get(key)):
            problems.append(f"{key}: run={identity.get(key)} artifact={artifact.get(key)}")
    return problems


def vulkan13_acceptance(summary: dict, artifact: dict, finalized: bool,
                        lifecycle_ok: bool) -> dict:
    """Consumer acceptance, not a Vulkan conformance claim or presentation test."""
    problems = check_identity(summary, artifact)
    identity = summary.get("identity") or {}
    if (artifact.get("variant") != "unmodified" or artifact.get("diagnostic") is not False or
            artifact.get("label") != "UNMODIFIED" or artifact.get("dxvk_source_patches") != [] or
            artifact.get("sdk_switches") != [] or artifact.get("integration") is not None or
            artifact.get("sdk_rebuilt") is not True or artifact.get("ps5vk_dirty") is not False):
        problems.append("artifact is not the unmodified shipping-SDK route")
    for key, expected in {"diagnostic": "0", "patches": "none", "compat_layer": "0",
                          "integration": "none", "sdk_switches": "none", "ps5vk_dirty": "0"}.items():
        if identity.get(key) != expected:
            problems.append(f"run identity {key} is not {expected}")
    version = re.search(r"\bapiVersion=(\d+)\.(\d+)\.(\d+)\b",
                        summary.get("vk_properties") or "")
    if not version or tuple(map(int, version.groups())) < (1, 3, 0):
        problems.append("device did not report Vulkan 1.3 or later")
    result = summary.get("result") or {}
    if any(result.get(key) != value for key, value in {
            "outcome": "rendered", "create_hr": "0x00000000", "feature_level": "0xb000",
            "device_refs": "0", "context_refs": "0"}.items()):
        problems.append("D3D11 FL11_0 render and release did not complete")
    oracle = summary.get("oracle") or {}
    if any(oracle.get(key) != value for key, value in {
            "checked": 4096, "mismatches": 0, "checksum": "6e17a4c5",
            "expected_checksum": "6e17a4c5"}.items()):
        problems.append("4096-pixel oracle did not match the fixed workload")
    if not any(stage["stage"] == "shutdown" and stage["state"] == "ok"
               for stage in summary.get("stages", [])):
        problems.append("shutdown completion missing")
    if not finalized or not lifecycle_ok:
        problems.append("telemetry or title lifecycle did not complete cleanly")
    if (summary.get("crash") or summary.get("gpu_hang_suspected") or
            summary.get("compat", {}).get("translations") or
            summary.get("compat", {}).get("refusals")):
        problems.append("crash, GPU hang or compatibility translation observed")
    if summary.get("first_refusal") or (summary.get("trace") or {}).get("refusals") != 0:
        problems.append("driver or consumer refusal observed, or trace completion missing")
    return {"passed": not problems, "problems": problems}


def receipt_for(summary: dict, artifact: dict, run_receipt: dict | None,
                log_sha: str, lifecycle_ok: bool) -> dict:
    result = summary.get("result") or {}
    return {
        "profile": PROFILE + "-run",
        "variant": artifact["variant"],
        "label": artifact["label"],
        "diagnostic": artifact["diagnostic"],
        "dxvk_commit": artifact["dxvk_commit"],
        "dxvk_source_patches": artifact["dxvk_source_patches"],
        "ps5vk_commit": artifact["ps5vk_commit"],
        "eboot_sha256": artifact["eboot_sha256"],
        "identity_mismatches": check_identity(summary, artifact),
        "run_id": (run_receipt or {}).get("run_id"),
        "log_sha256": log_sha,
        "log_finalized": bool(run_receipt and run_receipt.get("bye")
                              and run_receipt.get("clean")),
        "outcome": result.get("outcome") or ("crash" if summary["crash"] else "incomplete"),
        # Innermost stage that began, including DXVK-internal stages.
        "last_stage": summary["last_stage"] or result.get("last_stage"),
        "first_refusal": summary["first_refusal"] or summary["first_refusal_candidate"],
        "fl_gate": feature_level_gate(summary, list(artifact.get("dxvk_source_patches", []))),
        "oracle": summary["oracle"],
        "vk_first_call": summary["vk_first_call"],
        "vk_last_call": summary["vk_last_call"],
        "shutdown": next((s["detail"] for s in summary["stages"]
                          if s["stage"] == "shutdown" and s["state"] == "ok"), None),
        "sdk_switches": artifact.get("sdk_switches", []),
        "patch_list_sha256": artifact.get("patch_list_sha256"),
        "crash": summary["crash"],
        "gpu_hang_suspected": summary["gpu_hang_suspected"],
        "lifecycle_ok": lifecycle_ok,
        "summary": summary,
        "vulkan13_acceptance": vulkan13_acceptance(
            summary, artifact, bool(run_receipt and run_receipt.get("bye")
                                    and run_receipt.get("clean")), lifecycle_ok),
    }


def wait_for_run(runs_dir: Path, known: set[str], timeout: float) -> tuple[Path | None, bool]:
    """Return the newest new log and whether the server finalized it."""
    deadline = time.monotonic() + timeout
    newest = None
    while time.monotonic() < deadline:
        candidates = sorted(path for path in runs_dir.glob("*_PPSA99994_ps5vk_*.log")
                            if path.name not in known)
        if candidates:
            newest = candidates[-1]
            if newest.with_suffix(".json").is_file():
                return newest, True
        time.sleep(1.0)
    return newest, False


def main() -> int:
    from run_consumer import close_and_confirm, control, running

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--require-vulkan13", action="store_true",
                        help="Require unmodified DXVK on the shipping SDK, Vulkan 1.3, "
                             "the fixed pixel oracle and clean lifecycle")
    args = parser.parse_args()
    artifact = json.loads(args.artifact.read_text())
    eboot = args.dist / "eboot.bin"
    if (artifact.get("profile") != PROFILE or
            hashlib.sha256(eboot.read_bytes()).hexdigest() != artifact.get("eboot_sha256")):
        raise RuntimeError("artifact identity mismatch")
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch while a title is active")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    launched = False
    log_path, finalized = None, False
    try:
        control("launch", args.host)
        launched = True
        log_path, finalized = wait_for_run(args.runs_dir, known, args.timeout)
    finally:
        lifecycle_ok = (close_and_confirm(args.host) if launched else
                        running(args.host) == "none")
    if log_path is None:
        receipt = {"profile": PROFILE + "-run", "variant": artifact["variant"],
                   "label": artifact["label"], "eboot_sha256": artifact["eboot_sha256"],
                   "outcome": "no-log", "lifecycle_ok": lifecycle_ok}
    else:
        if not finalized:
            # The server finalizes on disconnect; give it a moment after Close Game.
            for _ in range(20):
                if log_path.with_suffix(".json").is_file():
                    break
                time.sleep(0.5)
        receipt_path = log_path.with_suffix(".json")
        run_receipt = json.loads(receipt_path.read_text()) if receipt_path.is_file() else None
        log = log_path.read_bytes()
        receipt = receipt_for(parse_log(log.decode("utf-8", errors="replace")), artifact,
                              run_receipt, hashlib.sha256(log).hexdigest(), lifecycle_ok)
        receipt["source_log"] = str(log_path)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(receipt, indent=2) + "\n")
    brief = {key: receipt.get(key) for key in (
        "variant", "label", "run_id", "outcome", "last_stage", "first_refusal",
        "oracle", "crash", "gpu_hang_suspected", "lifecycle_ok", "identity_mismatches")}
    if receipt.get("fl_gate"):
        brief["fl_gate_failing"] = receipt["fl_gate"]["failing"]
    print(json.dumps(brief, indent=2))
    if not lifecycle_ok:
        raise RuntimeError("payload title did not stop")
    if args.require_vulkan13:
        return 0 if receipt.get("vulkan13_acceptance", {}).get("passed") else 1
    return 0 if log_path is not None and not receipt.get("identity_mismatches") else 1


if __name__ == "__main__":
    raise SystemExit(main())
