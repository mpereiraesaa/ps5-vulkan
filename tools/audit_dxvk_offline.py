#!/usr/bin/env python3
"""Audit every current DXVK profile blocker against local preparation.

The output is a work queue, not native execution or Vulkan conformance evidence.
It reads only this repository and ignored local build artifacts.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
MATRIX = Path("conformance_inventory/dxvk_v262_matrix.json")
INLINE_PLAN = Path("build/offline-dxvk-profile/inline-v7/hardware-validation-plan-da16c00e.json")
SIZE_MEASUREMENT = Path("build/offline-dxvk-profile/subgroup-size-cts/measurement-current.json")
SIZE_PREVIOUS = Path("build/offline-dxvk-profile/subgroup-size-cts/candidate-7e69fbb1/PPSA99994/cases.txt")
SIZE_PACKAGE = Path("build/offline-dxvk-profile/subgroup-size-cts/rebuild-cb372781.json")
DOT_PACKAGE = Path("build/offline-dxvk-profile/integer-dot/current-c78057db/package.json")
T08_PACKAGE = Path("build/offline-dxvk-profile/t08-current-468c730b/package.json")
ROBUST_IMAGE_PACKAGE = Path("build/offline-dxvk-profile/robust-image-current-3010012d/package.json")
DEFAULT_OUT = Path("build/offline-dxvk-profile/audit-17/current.json")

INLINE = {
    "inlineUniformBlock", "maxDescriptorSetInlineUniformBlocks",
    "maxDescriptorSetUpdateAfterBindInlineUniformBlocks", "maxInlineUniformBlockSize",
    "maxInlineUniformTotalSize", "maxPerStageDescriptorInlineUniformBlocks",
    "maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks",
}
OFFLINE = {
    "shaderSubgroupExtendedTypes": "Finish type/operation coverage and package unchanged subgroup CTS.",
    "subgroupBroadcastDynamicId": "Prove runtime-selected Broadcast ID and complete BALLOT reporting.",
    "subgroupSizeControl": "Package the flagged size-control CTS selection and establish stage bounds.",
    "computeFullSubgroups": "Complete truthful BALLOT reporting and package the flagged full-subgroup CTS case.",
    "robustImageAccess": "Admit the original CTS combined image usage and register its factory.",
    "shaderIntegerDotProduct": "Run 42 compute and 252 graphics numerical witnesses, then 224 original CTS leaves and review public scope.",
}
REBUILD = {
    "maintenance4": "Run three SDK witnesses and 33 original CTS leaves, then canonical acceptance.",
    "pipelineCreationCacheControl": "Run the rebuilt SDK witness and eight original CTS leaves, then canonical acceptance.",
    "shaderZeroInitializeWorkgroupMemory": "Run the rebuilt SDK witness and 42 original CTS leaves, then canonical acceptance.",
}
REBUILT_WITNESSES = {
    "maintenance4": Path("build/offline-dxvk-profile/maintenance4-cts/rebuild-e8318a25.json"),
    "pipelineCreationCacheControl": Path("build/offline-dxvk-profile/cache-control/rebuild-a355320b.json"),
    "shaderZeroInitializeWorkgroupMemory": Path("build/offline-dxvk-profile/zero-initialize-witness/rebuild-a355320b.json"),
}
POLICY = {"apiVersion": "Keep the truthful reported version until the required patch-level conformance is proven."}
EXPECTED = INLINE | set(OFFLINE) | set(REBUILD) | set(POLICY)
T08_COMBINED_SWITCHES = (
    "PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC",
    "PS5VK_SUBGROUP_IADD_DIAGNOSTIC",
    "PS5VK_SHADER_INT16_DIAGNOSTIC",
)


def combined_t08_is_off(profile: dict) -> bool:
    switches = profile["switches"]
    return not all(switches.get(name) == "1" for name in T08_COMBINED_SWITCHES)


def artifact_profile(artifact: dict) -> dict:
    if "build_profile" in artifact:
        return artifact["build_profile"]
    switch = artifact.get("diagnostic_switch")
    return {"switches": {switch: "1"} if switch else {}}


def source_unchanged(root: Path, commit: str, paths: list[str], profiles: list[dict]) -> bool:
    """Accept only the added default-off T08 block when every artifact excludes it."""
    result = subprocess.run(["git", "diff", "--name-only", commit, "--", *paths],
                            cwd=root, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        return False
    changed = set(result.stdout.splitlines())
    if not changed:
        return True
    if changed != {"native/platform_ps5.c"} or not all(map(combined_t08_is_off, profiles)):
        return False
    old = subprocess.run(["git", "show", f"{commit}:native/platform_ps5.c"],
                         cwd=root, check=False, capture_output=True, text=True)
    if old.returncode != 0:
        return False
    current = (root / "native/platform_ps5.c").read_text()
    start = current.find("#if defined(PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC) && "
                         "PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC && \\\n")
    if start < 0:
        return False
    end = current.find("#endif\n", start)
    if end < 0:
        return False
    end += len("#endif\n")
    block = current[start:end]
    if ("#else" in block or not all(name in block for name in T08_COMBINED_SWITCHES) or
            not all(name in block for name in (
                "PS5VK_V13_FEATURE_SUBGROUP_BALLOT_COMPUTE",
                "PS5VK_V13_FEATURE_SUBGROUP_ARITHMETIC_COMPUTE",
                "PS5VK_V13_FEATURE_SHADER_SUBGROUP_EXTENDED_TYPES",
                "PS5VK_V13_FEATURE_SUBGROUP_BROADCAST_DYNAMIC_ID"))):
        return False
    return current[:start] + current[end:] == old.stdout


def selection_hash(cases: list[dict]) -> str:
    return hashlib.sha256(("\n".join(c["path"] for c in cases) + "\n").encode()).hexdigest()


def candidate_file(root: Path, candidate: str, filename: str) -> Path | None:
    directory = root / candidate
    direct = directory / filename
    if direct.is_file():
        return direct
    matches = list(directory.rglob(filename)) if directory.is_dir() else []
    return matches[0] if len(matches) == 1 else None


def audit_inline(plan: dict, root: Path) -> dict:
    checks = []
    for gate in plan["order"]:
        candidates = gate.get("candidates", [gate] if "candidate" in gate else [])
        for entry in candidates:
            path = candidate_file(root, entry["candidate"], "eboot.bin")
            actual = hashlib.sha256(path.read_bytes()).hexdigest() if path else None
            checks.append({"gate": gate["gate"], "candidate": entry["candidate"],
                           "eboot_sha256": entry["eboot_sha256"],
                           "hash_matches": actual == entry["eboot_sha256"]})
    cts = next(g for g in plan["order"] if g["gate"] == "original_cts_limits")
    selection_file = candidate_file(root, cts["candidate"], "selection_hash.txt")
    selected = selection_file.read_text().strip() if selection_file else None
    return {"checks": checks, "checked_eboots": len(checks),
            "cts_selection_hash_matches": selected == cts["selection_hash"],
            "execution_prepared": (len(checks) == 8 and all(c["hash_matches"] for c in checks)
                                   and selected == cts["selection_hash"]
                                   and plan.get("native_executed") is False)}


def audit_size_selection(root: Path) -> dict:
    path, old = root / SIZE_MEASUREMENT, root / SIZE_PREVIOUS
    if not path.is_file() or not old.is_file():
        return {"selection_verified": False, "new_flagged_leaves": []}
    derived = json.loads(path.read_text())
    frozen = json.loads((root / "cts/upstream/manifest.json").read_text())
    baseline = frozen["cases"]
    moved = derived["cases"][len(baseline):]
    selected = {case["path"] for case in moved}
    previous = set(old.read_text().splitlines())
    new = sorted(path for path in selected - previous if path.endswith("_flags_spirv16"))
    record = derived["measurement"]
    valid = (derived["cases"][:len(baseline)] == baseline and
             record["base_selection_hash"] == selection_hash(baseline) and
             record["selection_hash"] == selection_hash(derived["cases"]) and
             record["moved"] == 6 and len(new) == 2)
    return {"selection_verified": valid, "measurement_hash": record["selection_hash"],
            "new_flagged_leaves": new, "previous_candidate_covers_new_flags": not new}


def audit_subgroup_size_package(root: Path) -> dict:
    from tools.verify_subgroup_size_witness import CASES

    path = root / SIZE_PACKAGE
    if not path.is_file():
        return {"package_verified": False}
    record = json.loads(path.read_text())
    variants = record["variants"]
    valid = (record.get("native_executed") is False and
             len(variants) == len(CASES) and
             {item["case"] for item in variants} == set(CASES))
    profiles = []
    for item in variants:
        if item["case"] not in CASES:
            valid = False
            break
        candidate = root / item["candidate"]
        artifact_path = candidate / "artifact.json"
        eboot_path = candidate / "PPSA99994/eboot.bin"
        if not artifact_path.is_file() or not eboot_path.is_file():
            valid = False
            break
        artifact = json.loads(artifact_path.read_text())
        profiles.append(artifact_profile(artifact))
        valid = valid and (
            hashlib.sha256(artifact_path.read_bytes()).hexdigest() == item["artifact_sha256"] and
            hashlib.sha256(eboot_path.read_bytes()).hexdigest() == item["eboot_sha256"] and
            artifact.get("eboot_sha256") == item["eboot_sha256"] and
            artifact.get("case") == item["case"] and
            artifact.get("native_executed") is False and
            artifact["build_profile"]["switches"].get("PS5VK_SUBGROUP_SIZE_DIAGNOSTIC") == "1")
    cts = record["cts"]
    cts_dist = root / cts["candidate"] / "PPSA99994"
    cts_eboot = cts_dist / "eboot.bin"
    cts_manifest = cts_dist / "build_manifest.json"
    selection_path = root / cts["selection_manifest"]
    if not cts_eboot.is_file() or not cts_manifest.is_file() or not selection_path.is_file():
        return {"package_verified": False}
    build = json.loads(cts_manifest.read_text())
    profiles.append(build["tessellation_build_profile"])
    selection = json.loads(selection_path.read_text())
    frozen = json.loads((root / "cts/upstream/manifest.json").read_text())
    paths = [case["path"] for case in selection["cases"]]
    moved = paths[len(frozen["cases"]):]
    valid = valid and (
        hashlib.sha256(cts_eboot.read_bytes()).hexdigest() == cts["eboot_sha256"] and
        hashlib.sha256(cts_manifest.read_bytes()).hexdigest() == cts["build_manifest_sha256"] and
        build["eboot_sha256"] == cts["eboot_sha256"] and
        build["selection_hash"] == cts["selection_hash"] == selection_hash(selection["cases"]) and
        build["selected_cases"] == paths and
        build["measurement"] == selection["measurement"] and
        build["measurement"]["moved"] == cts["moved"] == 6 and
        selection["cases"][:len(frozen["cases"])] == frozen["cases"] and
        sum(path.endswith("_flags_spirv16") for path in moved) == 2 and
        build["tessellation_build_profile"]["switches"].get(cts["diagnostic_switch"]) == "1")
    source_current = source_unchanged(root, record["source_commit"],
        ["src", "native", "include", "examples", "experiments", "cts", "tools/build_sdk.py",
         "tools/build_upstream_cts.py", "tools/build_subgroup_size_witness.py"], profiles)
    combined = record.get("combined_cts", {})
    combined_dist = root / combined.get("candidate", "") / "PPSA99994"
    combined_eboot = combined_dist / "eboot.bin"
    combined_manifest = combined_dist / "build_manifest.json"
    required_switches = {"PS5VK_SUBGROUP_SIZE_DIAGNOSTIC",
                         "PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC",
                         "PS5VK_SUBGROUP_IADD_DIAGNOSTIC",
                         "PS5VK_SHADER_INT16_DIAGNOSTIC"}
    combined_prepared = False
    if combined_eboot.is_file() and combined_manifest.is_file():
        packaged = json.loads(combined_manifest.read_text())
        profile = packaged["tessellation_build_profile"]
        combined_prepared = (
            hashlib.sha256(combined_eboot.read_bytes()).hexdigest() == combined.get("eboot_sha256") and
            hashlib.sha256(combined_manifest.read_bytes()).hexdigest() == combined.get("build_manifest_sha256") and
            packaged["eboot_sha256"] == combined.get("eboot_sha256") and
            packaged["selection_hash"] == combined.get("selection_hash") == selection_hash(selection["cases"]) and
            packaged["selected_cases"] == paths and
            packaged["measurement"] == selection["measurement"] and
            packaged["measurement"]["moved"] == combined.get("moved") == 6 and
            set(combined.get("required_switches", [])) == required_switches and
            all(profile["switches"].get(switch) == "1" for switch in required_switches) and
            source_unchanged(root, combined["source_commit"],
                ["src", "native", "include", "examples", "experiments", "cts", "tools/build_sdk.py",
                 "tools/build_upstream_cts.py"], [profile]))
    return {"package_verified": valid and source_current, "artifact_verified": valid,
            "source_current": source_current, "variant_count": len(variants),
            "cts_eboot_sha256": cts["eboot_sha256"],
            "combined_cts_prepared": combined_prepared,
            "combined_cts_eboot_sha256": combined.get("eboot_sha256")}


def audit_integer_dot_package(root: Path) -> dict:
    from tools.integer_dot_graphics_witness import GRAPHS
    from tools.verify_integer_dot_witness import CASES

    path = root / DOT_PACKAGE
    if not path.is_file():
        return {"package_verified": False}
    record = json.loads(path.read_text())
    valid = record.get("native_executed") is False
    totals = {}
    profiles = []
    for kind, expected in (("compute", {(case,) for case in CASES}),
                           ("graphics", {(case, graph) for case in CASES for graph in GRAPHS})):
        inventory_path = root / record[f"{kind}_inventory"]
        if not inventory_path.is_file():
            return {"package_verified": False}
        valid = valid and hashlib.sha256(inventory_path.read_bytes()).hexdigest() == record[f"{kind}_inventory_sha256"]
        inventory = json.loads(inventory_path.read_text())
        entries = inventory["cases"]
        observed = {(item["case"], item["graph"]) if kind == "graphics" else (item["case"],)
                    for item in entries}
        valid = valid and (inventory["source_commit"] == record["source_commit"] and
                           inventory["native_executed"] is False and
                           len(entries) == len(expected) and observed == expected)
        totals[kind] = len(entries)
        for item in entries:
            candidate = root / Path(record[f"{kind}_inventory"]).parent / item["case"]
            if kind == "graphics":
                candidate /= item["graph"]
            artifact_path = candidate / "artifact.json"
            eboot_path = candidate / "PPSA99994/eboot.bin"
            if not artifact_path.is_file() or not eboot_path.is_file():
                valid = False
                break
            artifact = json.loads(artifact_path.read_text())
            profiles.append(artifact_profile(artifact))
            valid = valid and (
                hashlib.sha256(artifact_path.read_bytes()).hexdigest() == item["artifact_sha256"] and
                hashlib.sha256(eboot_path.read_bytes()).hexdigest() == item["eboot_sha256"] and
                artifact.get("eboot_sha256") == item["eboot_sha256"] and
                artifact.get("case") == item["case"] and
                (kind != "graphics" or artifact.get("graph") == item["graph"]) and
                artifact.get("native_executed") is False and
                artifact["build_profile"]["switches"].get("PS5VK_INTEGER_DOT_DIAGNOSTIC") == "1")
        if not valid:
            break
    cts = record["cts"]
    cts_dist = root / cts["candidate"] / "PPSA99994"
    eboot = cts_dist / "eboot.bin"
    build_path = cts_dist / "build_manifest.json"
    selection_path = root / cts["selection_manifest"]
    if not eboot.is_file() or not build_path.is_file() or not selection_path.is_file():
        return {"package_verified": False}
    build = json.loads(build_path.read_text())
    profiles.append(build["tessellation_build_profile"])
    selection = json.loads(selection_path.read_text())
    frozen = json.loads((root / "cts/upstream/manifest.json").read_text())
    paths = [case["path"] for case in selection["cases"]]
    valid = valid and (
        hashlib.sha256(eboot.read_bytes()).hexdigest() == cts["eboot_sha256"] and
        hashlib.sha256(build_path.read_bytes()).hexdigest() == cts["build_manifest_sha256"] and
        build["eboot_sha256"] == cts["eboot_sha256"] and
        build["selection_hash"] == cts["selection_hash"] == selection_hash(selection["cases"]) and
        build["selected_cases"] == paths and
        build["measurement"] == selection["measurement"] and
        build["measurement"]["moved"] == cts["moved"] == 224 and
        build["measurement"]["categories"] == ["integer-dot-product-pending"] and
        selection["cases"][:len(frozen["cases"])] == frozen["cases"] and
        selection["measurement"]["base_selection_hash"] == selection_hash(frozen["cases"]) and
        build["tessellation_build_profile"]["switches"].get(cts["diagnostic_switch"]) == "1")
    source_current = source_unchanged(root, record["source_commit"],
        ["src", "native", "include", "examples", "experiments", "cts", "tools/build_sdk.py",
         "tools/build_upstream_cts.py", "tools/build_integer_dot_witness.py",
         "tools/build_integer_dot_graphics_witness.py", "tools/integer_dot_spirv.py",
         "tools/integer_dot_vectors.py"], profiles)
    return {"package_verified": valid and source_current, "artifact_verified": valid,
            "source_current": source_current, "variants": totals,
            "cts_eboot_sha256": cts["eboot_sha256"]}


def audit_robust_image_package(root: Path) -> dict:
    from tools.build_upstream_cts import tessellation_build_profile
    from tools.verify_robust_image_witness import CASES, fixture_contract

    path = root / ROBUST_IMAGE_PACKAGE
    if not path.is_file():
        return {"package_verified": False}
    record = json.loads(path.read_text())
    archive = root / record["sdk_archive"]
    if not archive.is_file():
        return {"package_verified": False}
    expected_profile = tessellation_build_profile({"PS5VK_IMAGE_ROBUSTNESS_DIAGNOSTIC": "1"})
    variants = record["variants"]
    valid = (record.get("native_executed") is False and
             len(variants) == len(CASES) == 11 and
             {item["case"] for item in variants} == set(CASES) and
             hashlib.sha256(archive.read_bytes()).hexdigest() == record["sdk_archive_sha256"])
    for item in variants:
        if item["case"] not in CASES:
            valid = False
            break
        candidate = root / item["candidate"]
        artifact_path = candidate / "artifact.json"
        eboot_path = candidate / "PPSA99994/eboot.bin"
        header_path = candidate / "robust_image_fixture.h"
        shader_path = candidate / "shader.spv"
        source_path = candidate / "shader.comp"
        if not all(path.is_file() for path in
                   (artifact_path, eboot_path, header_path, shader_path, source_path)):
            valid = False
            break
        artifact = json.loads(artifact_path.read_text())
        contract = fixture_contract(item["case"])
        valid = valid and (
            hashlib.sha256(artifact_path.read_bytes()).hexdigest() == item["artifact_sha256"] and
            hashlib.sha256(eboot_path.read_bytes()).hexdigest() == item["eboot_sha256"] and
            artifact.get("eboot_sha256") == item["eboot_sha256"] and
            artifact.get("sdk_sha256") == record["sdk_archive_sha256"] and
            artifact.get("source_sha256") == hashlib.sha256(
                (root / "examples/robust_image_witness/main.c").read_bytes()).hexdigest() and
            artifact.get("helper_sha256") == hashlib.sha256(
                (root / "examples/robust_image_witness/compute.h").read_bytes()).hexdigest() and
            artifact.get("header_sha256") == hashlib.sha256(header_path.read_bytes()).hexdigest() and
            artifact.get("shader_sha256") == hashlib.sha256(shader_path.read_bytes()).hexdigest() and
            artifact.get("fixture_sha256", {}).get("shader_source") == hashlib.sha256(
                source_path.read_bytes()).hexdigest() and
            artifact.get("build_profile") == expected_profile and
            artifact.get("native_executed") is False and
            all(artifact.get(key) == value for key, value in contract.items()))
    source_current = source_unchanged(root, record["source_commit"],
        ["src", "native", "include", "examples/robust_image_witness",
         "tools/build_sdk.py", "tools/build_robust_image_witness.py",
         "tools/build_integer_dot_witness.py", "tools/robust_image_witness.py",
         "tools/verify_robust_image_witness.py"], [expected_profile])
    return {"package_verified": valid and source_current,
            "artifact_verified": valid, "source_current": source_current,
            "variant_count": len(variants),
            "image_variants": sum(case.requirement == "robustImageAccess" for case in CASES.values()),
            "original_cts_prepared": False}


def audit_t08_package(root: Path) -> dict:
    path = root / T08_PACKAGE
    if not path.is_file():
        return {"witnesses_verified": False, "compiler_census_verified": False}
    record = json.loads(path.read_text())
    expected = {"ballot", "broadcast", "iadd", "iadd_int8", "iadd_int16",
                "iadd_int64", "fadd_float16", "arithmetic21"}
    witnesses = record["witnesses"]
    valid = (record.get("native_executed") is False and len(witnesses) == 8 and
             {item["operation"] for item in witnesses} == expected)
    profiles = []
    for item in witnesses:
        artifact_path = root / item["candidate"] / "artifact.json"
        eboot_path = root / item["candidate"] / "PPSA99994/eboot.bin"
        if not artifact_path.is_file() or not eboot_path.is_file():
            valid = False
            break
        artifact = json.loads(artifact_path.read_text())
        profiles.append(artifact_profile(artifact))
        operation = item["operation"]
        switches = artifact["build_profile"]["switches"]
        family = ("PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC" if operation in ("ballot", "broadcast")
                  else "PS5VK_SUBGROUP_IADD_DIAGNOSTIC")
        valid = valid and (
            hashlib.sha256(artifact_path.read_bytes()).hexdigest() == item["artifact_sha256"] and
            hashlib.sha256(eboot_path.read_bytes()).hexdigest() == item["eboot_sha256"] and
            artifact.get("eboot_sha256") == item["eboot_sha256"] and
            (operation == "arithmetic21" or artifact.get("operation") == operation) and
            (operation != "arithmetic21" or artifact.get("operations") == 21) and
            switches.get(family) == "1" and
            (operation != "iadd_int8" or switches.get("PS5VK_SHADER_INT8_DIAGNOSTIC") == "1") and
            (operation != "iadd_int16" or switches.get("PS5VK_SHADER_INT16_DIAGNOSTIC") == "1"))
    census_path = root / record["compiler_census"]
    if not census_path.is_file():
        return {"witnesses_verified": False, "compiler_census_verified": False}
    census = json.loads(census_path.read_text())
    arithmetic_source = (root / "third_party/vk-gl-cts/external/vulkancts/modules/"
                         "vulkan/subgroups/vktSubgroupsArithmeticTests.cpp")
    compiler_verified = (
        hashlib.sha256(census_path.read_bytes()).hexdigest() == record["compiler_census_sha256"] and
        census["cts_source_sha256"] == hashlib.sha256(arithmetic_source.read_bytes()).hexdigest() and
        census["probe_source_sha256"] == hashlib.sha256((root / "tests/t08_compile_probe.c").read_bytes()).hexdigest() and
        census["psbc_archive_sha256"] == hashlib.sha256((root / "build/libpsbc.host.a").read_bytes()).hexdigest() and
        len(census["operation_enum"]) == 21 and len(census["rows"]) == 118 and
        len([row for row in census["rows"] if row["name"].startswith("ballot_")]) == 10 and
        all(row["glslang_exit"] == row["psbc_exit"] == 0 and row["group_opcodes"]
            for row in census["rows"]))
    selection = json.loads((root / record["original_cts_selection"]).read_text())
    frozen = json.loads((root / "cts/upstream/manifest.json").read_text())
    selection_verified = (
        selection["cases"][:len(frozen["cases"])] == frozen["cases"] and
        selection["measurement"]["moved"] == 5 and
        selection["measurement"]["selection_hash"] == selection_hash(selection["cases"]) and
        record["original_cts_eligible_on_current_report"] is False)
    cts = record.get("diagnostic_cts", {})
    cts_dist = root / cts.get("candidate", "") / "PPSA99994"
    cts_eboot = cts_dist / "eboot.bin"
    cts_manifest = cts_dist / "build_manifest.json"
    diagnostic_cts_prepared = False
    if cts_eboot.is_file() and cts_manifest.is_file():
        build = json.loads(cts_manifest.read_text())
        switches = build["tessellation_build_profile"]["switches"]
        diagnostic_cts_prepared = (
            hashlib.sha256(cts_eboot.read_bytes()).hexdigest() == cts.get("eboot_sha256") and
            hashlib.sha256(cts_manifest.read_bytes()).hexdigest() == cts.get("build_manifest_sha256") and
            build["eboot_sha256"] == cts.get("eboot_sha256") and
            build["selection_hash"] == cts.get("selection_hash") == selection_hash(selection["cases"]) and
            build["selected_cases"] == [case["path"] for case in selection["cases"]] and
            build["measurement"] == selection["measurement"] and
            build["measurement"]["moved"] == cts.get("moved") == 5 and
            set(cts.get("required_switches", [])) == set(T08_COMBINED_SWITCHES) and
            {name for name, value in switches.items() if value == "1"} == set(T08_COMBINED_SWITCHES) and
            source_unchanged(root, cts["source_commit"],
                ["src", "native", "include", "cts", "tools/build_sdk.py",
                 "tools/build_upstream_cts.py"], [build["tessellation_build_profile"]]))
    source_current = source_unchanged(root, record["source_commit"],
        ["src", "native", "include", "examples", "experiments", "tools/build_sdk.py",
         "tools/build_t08_subgroup_broadcast_witness.py",
         "tools/build_t08_subgroup_arithmetic_witness.py", "tests/t08_compile_probe.c"], profiles)
    return {"witnesses_verified": valid and source_current,
            "compiler_census_verified": compiler_verified, "cts_selection_verified": selection_verified,
            "diagnostic_cts_prepared": diagnostic_cts_prepared,
            "diagnostic_cts_eboot_sha256": cts.get("eboot_sha256"),
            "source_current": source_current, "witness_count": len(witnesses),
            "original_cts_eligible": False}


def audit_rebuilt_witness(root: Path, record_path: Path) -> dict:
    path = root / record_path
    if not path.is_file():
        return {"artifact_verified": False, "source_current": False}
    record = json.loads(path.read_text())
    candidate = root / record["candidate"]
    artifact_path = candidate / "artifact.json"
    eboot_path = candidate / "PPSA99994/eboot.bin"
    if not artifact_path.is_file() or not eboot_path.is_file():
        return {"artifact_verified": False, "source_current": False}
    artifact = json.loads(artifact_path.read_text())
    profiles = [artifact_profile(artifact)]
    artifact_verified = (
        record.get("native_executed") is False and
        hashlib.sha256(artifact_path.read_bytes()).hexdigest() == record["artifact_sha256"] and
        hashlib.sha256(eboot_path.read_bytes()).hexdigest() == record["eboot_sha256"] and
        artifact.get("eboot_sha256") == record["eboot_sha256"])
    additional_verified = True
    for extra in record.get("additional_witnesses", []):
        extra_dist = root / extra["candidate"]
        extra_artifact = extra_dist / "artifact.json"
        extra_eboot = extra_dist / "PPSA99994/eboot.bin"
        if not extra_artifact.is_file() or not extra_eboot.is_file():
            additional_verified = False
            break
        data = json.loads(extra_artifact.read_text())
        profiles.append(artifact_profile(data))
        additional_verified = (
            hashlib.sha256(extra_artifact.read_bytes()).hexdigest() == extra["artifact_sha256"] and
            hashlib.sha256(extra_eboot.read_bytes()).hexdigest() == extra["eboot_sha256"] and
            data.get("eboot_sha256") == extra["eboot_sha256"] and
            all(data.get(key) == extra[key] for key in
                ("local_x", "specialization", "expected_digest", "spirv_sha256")))
        if not additional_verified:
            break
    source_current = source_unchanged(root, record["source_commit"],
        ["src", "native", "include", "examples", "experiments", "tools/build_sdk.py",
         "tools/build_dxvk_render_witness.py", "tools/build_zero_initialize_witness.py",
         "tools/build_integer_dot_witness.py"], profiles)
    cts = record.get("cts", {})
    cts_dist = root / cts.get("candidate", "") / "PPSA99994"
    cts_eboot = cts_dist / "eboot.bin"
    cts_manifest = cts_dist / "build_manifest.json"
    selection_path = root / cts.get("selection_manifest", "")
    cts_verified = False
    cts_source_current = False
    if cts_eboot.is_file() and cts_manifest.is_file() and selection_path.is_file():
        build = json.loads(cts_manifest.read_text())
        cts_profiles = [build["tessellation_build_profile"]]
        selection = json.loads(selection_path.read_text())
        frozen = json.loads((root / "cts/upstream/manifest.json").read_text())
        selected = [case["path"] for case in selection["cases"]]
        cts_verified = (
            hashlib.sha256(cts_eboot.read_bytes()).hexdigest() == cts.get("eboot_sha256") and
            hashlib.sha256(cts_manifest.read_bytes()).hexdigest() == cts.get("build_manifest_sha256") and
            build.get("eboot_sha256") == cts.get("eboot_sha256") and
            build.get("selection_hash") == cts.get("selection_hash") == selection_hash(selection["cases"]) and
            build.get("selected_cases") == selected and
            build.get("measurement") == selection.get("measurement") and
            build["measurement"]["moved"] == cts.get("moved") and
            (build["tessellation_build_profile"]["switches"].get(cts["diagnostic_switch"]) == "1"
             if cts.get("diagnostic_switch") else
             build["tessellation_build_profile"]["experimental"] is False) and
            selection["cases"][:len(frozen["cases"])] == frozen["cases"] and
            selection["measurement"]["base_selection_hash"] == selection_hash(frozen["cases"]))
        cts_source_current = source_unchanged(root, cts["source_commit"],
            ["src", "native", "include", "cts", "tools/build_sdk.py",
             "tools/build_upstream_cts.py"], cts_profiles)
    return {"artifact_verified": artifact_verified, "additional_witnesses_verified": additional_verified,
            "source_current": source_current,
            "source_commit": record["source_commit"], "eboot_sha256": record["eboot_sha256"],
            "cts_verified": cts_verified, "cts_source_current": cts_source_current,
            "cts_eboot_sha256": cts.get("eboot_sha256"),
            "execution_prepared": artifact_verified and additional_verified and source_current and
                                  cts_verified and cts_source_current}


def build_audit(root: Path = ROOT, matrix: dict | None = None,
                inline_plan: dict | None = None) -> dict:
    if matrix is None:
        matrix = json.loads((root / MATRIX).read_text())
    blockers = [row for row in matrix["requirements"] if row["verdict"] == "blocker"]
    names = [row["name"] for row in blockers]
    if (len(blockers) != 17 or len(set(names)) != 17 or set(names) != EXPECTED or
            matrix["summary"]["blocker"] != 17 or matrix["summary"]["satisfied"] != 45):
        raise ValueError("the frozen 17-blocker audit mapping no longer matches the profile matrix")
    if inline_plan is None:
        inline_plan = json.loads((root / INLINE_PLAN).read_text())
    inline = audit_inline(inline_plan, root)
    size = audit_size_selection(root)
    subgroup_package = audit_subgroup_size_package(root)
    size_execution_prepared = (size["selection_verified"] and
                               subgroup_package.get("package_verified", False) and
                               subgroup_package.get("combined_cts_prepared", False))
    integer_dot_package = audit_integer_dot_package(root)
    robust_image_package = audit_robust_image_package(root)
    t08_package = audit_t08_package(root)
    rebuilt = {name: audit_rebuilt_witness(root, path)
               for name, path in REBUILT_WITNESSES.items()}
    rows = []
    for row in blockers:
        name = row["name"]
        if name in INLINE:
            phase = "native_validation" if inline["execution_prepared"] else "offline_rebuild"
            next_action = "Execute the eight-gate inline validation plan, then promote only verified rows."
        elif name in OFFLINE:
            if name in {"subgroupSizeControl", "computeFullSubgroups"} and size_execution_prepared:
                phase = "native_validation"
                next_action = ("Run six occupancy witnesses and the combined six-leaf original CTS "
                               "selection, then canonical acceptance.")
            else:
                phase = ("native_validation" if name == "shaderIntegerDotProduct" and
                         integer_dot_package.get("package_verified") else "offline_work")
                next_action = OFFLINE[name]
        elif name in REBUILD:
            phase = "native_validation" if rebuilt[name].get("execution_prepared") else "offline_rebuild"
            next_action = REBUILD[name]
        else:
            phase, next_action = "version_policy", POLICY[name]
        rows.append({"id": row["id"], "name": name, "phase": phase,
                     "api": row["api"]["state"],
                     "implementation": row["implementation"]["state"],
                     "native": row["native"]["state"], "next_action": next_action,
                     "hardware_evidence_required": True})
    return {"schema": "ps5vk-dxvk-offline-audit/1", "matrix": str(MATRIX),
            "matrix_summary": matrix["summary"], "rows": rows,
            "inline_plan": inline, "subgroup_size_selection": size,
            "subgroup_size_package": subgroup_package, "integer_dot_package": integer_dot_package,
            "robust_image_package": robust_image_package,
            "t08_package": t08_package,
            "rebuilt_witnesses": rebuilt,
            "note": "Offline preparation only; no row is promoted by this audit."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    report = build_audit()
    out = args.out if args.out.is_absolute() else ROOT / args.out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n")
    counts = {phase: sum(r["phase"] == phase for r in report["rows"])
              for phase in sorted({r["phase"] for r in report["rows"]})}
    print(f"DXVK offline audit: {len(report['rows'])} blockers; {counts}; "
          f"inline eboots {report['inline_plan']['checked_eboots']}/8 checked, "
          f"prepared={report['inline_plan']['execution_prepared']} -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
