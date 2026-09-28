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

ROOT = Path(__file__).resolve().parents[1]
MATRIX = Path("conformance_inventory/dxvk_v262_matrix.json")
INLINE_PLAN = Path("build/offline-dxvk-profile/inline-v7/hardware-validation-plan-da16c00e.json")
SIZE_MEASUREMENT = Path("build/offline-dxvk-profile/subgroup-size-cts/measurement-current.json")
SIZE_PREVIOUS = Path("build/offline-dxvk-profile/subgroup-size-cts/candidate-7e69fbb1/PPSA99994/cases.txt")
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
    "maintenance4": "Prepare current-source numerical/interface delivery evidence and rebuild CTS candidate.",
    "robustImageAccess": "Admit the original CTS combined image usage and register its factory.",
    "shaderIntegerDotProduct": "Complete the integrated original-CTS and graphics numerical delivery candidate.",
}
REBUILD = {
    "pipelineCreationCacheControl": "Run the rebuilt SDK witness and eight original CTS leaves, then canonical acceptance.",
    "shaderZeroInitializeWorkgroupMemory": "Run the rebuilt SDK witness and 42 original CTS leaves, then canonical acceptance.",
}
REBUILT_WITNESSES = {
    "pipelineCreationCacheControl": Path("build/offline-dxvk-profile/cache-control/rebuild-a355320b.json"),
    "shaderZeroInitializeWorkgroupMemory": Path("build/offline-dxvk-profile/zero-initialize-witness/rebuild-a355320b.json"),
}
POLICY = {"apiVersion": "Keep the truthful reported version until the required patch-level conformance is proven."}
EXPECTED = INLINE | set(OFFLINE) | set(REBUILD) | set(POLICY)


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
    artifact_verified = (
        record.get("native_executed") is False and
        hashlib.sha256(artifact_path.read_bytes()).hexdigest() == record["artifact_sha256"] and
        hashlib.sha256(eboot_path.read_bytes()).hexdigest() == record["eboot_sha256"] and
        artifact.get("eboot_sha256") == record["eboot_sha256"])
    source_current = subprocess.run(
        ["git", "diff", "--quiet", record["source_commit"], "--", "src", "native", "include",
         "examples", "experiments", "tools/build_sdk.py", "tools/build_dxvk_render_witness.py",
         "tools/build_zero_initialize_witness.py", "tools/build_integer_dot_witness.py"],
        cwd=root, check=False, capture_output=True).returncode == 0
    cts = record.get("cts", {})
    cts_dist = root / cts.get("candidate", "") / "PPSA99994"
    cts_eboot = cts_dist / "eboot.bin"
    cts_manifest = cts_dist / "build_manifest.json"
    selection_path = root / cts.get("selection_manifest", "")
    cts_verified = False
    cts_source_current = False
    if cts_eboot.is_file() and cts_manifest.is_file() and selection_path.is_file():
        build = json.loads(cts_manifest.read_text())
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
            build["tessellation_build_profile"]["switches"].get(cts.get("diagnostic_switch")) == "1" and
            selection["cases"][:len(frozen["cases"])] == frozen["cases"] and
            selection["measurement"]["base_selection_hash"] == selection_hash(frozen["cases"]))
        cts_source_current = subprocess.run(
            ["git", "diff", "--quiet", cts["source_commit"], "--", "src", "native", "include",
             "cts", "tools/build_sdk.py", "tools/build_upstream_cts.py"],
            cwd=root, check=False, capture_output=True).returncode == 0
    return {"artifact_verified": artifact_verified, "source_current": source_current,
            "source_commit": record["source_commit"], "eboot_sha256": record["eboot_sha256"],
            "cts_verified": cts_verified, "cts_source_current": cts_source_current,
            "cts_eboot_sha256": cts.get("eboot_sha256"),
            "execution_prepared": artifact_verified and source_current and
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
    rebuilt = {name: audit_rebuilt_witness(root, path)
               for name, path in REBUILT_WITNESSES.items()}
    rows = []
    for row in blockers:
        name = row["name"]
        if name in INLINE:
            phase = "native_validation" if inline["execution_prepared"] else "offline_rebuild"
            next_action = "Execute the eight-gate inline validation plan, then promote only verified rows."
        elif name in OFFLINE:
            phase, next_action = "offline_work", OFFLINE[name]
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
            "inline_plan": inline, "subgroup_size_selection": size, "rebuilt_witnesses": rebuilt,
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
