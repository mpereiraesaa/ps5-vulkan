#!/usr/bin/env python3
"""Join the pinned DXVK 2.6.2 profile to ps5vk's current evidence.

This is intentionally fail-closed.  A required value that cannot be obtained
from the public reporting matrix, reviewed implementation evidence, upstream
CTS mapping and native evidence remains a blocker.
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
DEVICE_SCOPE_ID = "feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModelDeviceScope"
DIAGNOSTIC_IMPLEMENTATIONS = {
    BDA_ID: (
        ("src/vk_memory.c", "VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressKHR"),
        ("src/vk_device.c", "VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME"),
        ("src/ps5vk_compiler.c", "opts.enable_physical_storage_buffer_addresses"),
    ),
    DEVICE_SCOPE_ID: (
        ("src/vk_device.c", "PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE"),
        ("src/vk_pipeline.c", "case 5346u: /* VulkanMemoryModelDeviceScope */"),
        ("src/ps5vk_compiler.c", "opts.enable_vulkan_memory_model_device_scope"),
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
                       "the independent API, original CTS and native axes still control promotion.")}


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
    return ({"state": "satisfied" if value and extension else "blocker",
             "observed": value and extension, "expected": row["expected"],
             "via": "VK_KHR_vulkan_memory_model" if extension else None,
             "detail": "KHR feature query on Vulkan 1.0; the Vulkan 1.2 aggregate is unadvertised."},
            {"state": "implemented" if implemented else "missing",
             "refs": ["src/vk_device.c", "src/vk_pipeline.c", "src/ps5vk_compiler.c",
                      "native/runtime_graphics_compiler.c",
                      "conformance_inventory/reporting_matrix.json"],
             "detail": "The base model and DeviceScope are independently gated; "
                       "compute and graphics compiler options follow each SPIR-V module's memory model."})


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
             "detail": "Reviewed multiview execution; the separate CTS and native axes must also pass."})


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
    # The extension is implemented only when the native platform advertises
    # the feature that makes its public query and device-create route usable.
    if "PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT" not in PLATFORM_SOURCE.read_text():
        tokens.discard("VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME")
    missing = sorted(token for token in tokens if token not in definitions)
    if missing:
        raise ValueError("unresolved device extension macros: " + ", ".join(missing))
    # Compiling a conditional KHR route does not mean the shipping platform
    # reports that extension. Count only the bits assigned to the native
    # platform's supported-features mask, keeping the capability probe aligned
    # until a measured promotion changes that mask.
    platform_source = (ROOT / "native/platform_ps5.c").read_text()
    # A default-off measurement build is not the shipping capability probe.
    # Strip only this explicitly named conditional block, and fail closed if
    # its preprocessor boundary is malformed rather than counting its bits.
    for name in ("PS5VK_MEMORY_MODEL_DIAGNOSTIC", "PS5VK_BDA_DIAGNOSTIC"):
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
    assignments = re.findall(r"platform->supported_features\s*(?:\|=|=)\s*(.*?);",
                             platform_source, re.DOTALL)
    shipping_bits = {bit for assignment in assignments
                     for bit in re.findall(r"PS5VK_FEATURE_[A-Z0-9_]+", assignment)}
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


def cts_join(rows: list[dict], override: dict | None,
             selected_cases: set[str], diagnostic_cases: set[str]) -> dict:
    cases = sorted({case for row in rows for case in row.get("cts", {}).get("cases", [])})
    requirement_ids = sorted({row["id"] for row in rows})
    if override:
        state = override.get("state")
        if state not in ("cts-pass", "cts-fail"):
            raise ValueError(f"unsupported CTS evidence state {state!r}")
        claimed = override.get("cases", [])
        allowed = selected_cases if state == "cts-pass" else diagnostic_cases
        if not claimed or not set(claimed).issubset(allowed):
            raise ValueError("CTS evidence names a case outside the current upstream "
                             f"{'selection' if state == 'cts-pass' else 'diagnostics'}")
        return {
            "state": state, "cases": claimed,
            "mapped_cases": cases, "related_requirement_ids": requirement_ids,
            "refs": override.get("refs", []), "note": override.get("note", ""),
        }
    return {
        "state": "mapped-not-run" if cases else "not-mapped",
        "cases": [], "mapped_cases": cases,
        "related_requirement_ids": requirement_ids,
        "refs": [],
    }


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
        diagnostic = diagnostic_implementation(identifier)
        if diagnostic is not None:
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
        if native.get("state") not in ("native-evidence", "witnessed-blocker",
                                       "reported-not-executed", "not-run"):
            raise ValueError(f"invalid native state for {identifier}")
        ready = (api["state"] == "satisfied" and
                 implementation["state"] == "implemented" and
                 cts["state"] == "cts-pass" and
                 native["state"] == "native-evidence")
        rows.append({
            **requirement,
            "api": api,
            "implementation": implementation,
            "cts": cts,
            "native": native,
            "verdict": "satisfied" if ready else "blocker",
        })

    if probe:
        api_satisfied = {row["id"] for row in rows
                         if row["api"]["state"] == "satisfied"}
        if api_satisfied != probe_satisfied:
            raise ValueError("DXVK native capability probe satisfied set drift")

    dimensions = {}
    for name, success in (("api", "satisfied"), ("implementation", "implemented"),
                          ("cts", "cts-pass"), ("native", "native-evidence")):
        satisfied = sum(row[name]["state"] == success for row in rows)
        dimensions[name] = {"satisfied": satisfied,
                            "blocker": len(rows) - satisfied}
    return {
        "schema": SCHEMA,
        "profile": profile["profile"],
        "source": profile["source"],
        "policy": {
            "ready_rule": "api=satisfied AND implementation=implemented AND cts=cts-pass AND native=native-evidence",
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
        ready = (row["api"]["state"] == "satisfied" and
                 row["implementation"]["state"] == "implemented" and
                 row["cts"]["state"] == "cts-pass" and
                 row["native"]["state"] == "native-evidence")
        if (row.get("verdict") == "satisfied") != ready:
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
