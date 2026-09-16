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
VULKAN_HEADER = ROOT / "third_party/vulkan-headers/include/vulkan/vulkan_core.h"
SCHEMA = "ps5vk-dxvk-matrix/1"


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
    missing = sorted(token for token in tokens if token not in definitions)
    if missing:
        raise ValueError("unresolved device extension macros: " + ", ".join(missing))
    return {definitions[token] for token in tokens}


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
             selected_cases: set[str]) -> dict:
    cases = sorted({case for row in rows for case in row.get("cts", {}).get("cases", [])})
    requirement_ids = sorted({row["id"] for row in rows})
    if override:
        state = override.get("state")
        if state != "cts-pass":
            raise ValueError(f"unsupported CTS evidence state {state!r}")
        claimed = override.get("cases", [])
        if not claimed or not set(claimed).issubset(selected_cases):
            raise ValueError("CTS evidence names a case outside the current upstream selection")
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
        cts = cts_join(related, override.get("cts"), selected_cases)
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
