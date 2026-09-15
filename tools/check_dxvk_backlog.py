#!/usr/bin/env python3
"""Validate the ordered DXVK v2.6.2 implementation backlog fail-closed."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
BACKLOG = ROOT / "conformance_inventory/dxvk_v262_backlog.json"
MATRIX = ROOT / "conformance_inventory/dxvk_v262_matrix.json"
SCHEMA = "ps5vk-dxvk-v262-backlog/1"
FINAL_TRANCHE = "DXVK262-T15"
API_REQUIREMENT = "api-version:apiVersion"


def load(path: Path) -> dict:
    return json.loads(path.read_text())


def validate(document: dict, matrix: dict) -> dict:
    errors: list[str] = []
    if document.get("schema") != SCHEMA:
        errors.append("unexpected backlog schema")
    if document.get("profile_id") != matrix.get("profile", {}).get("id"):
        errors.append("backlog profile does not match the matrix")
    if document.get("source_matrix") != "conformance_inventory/dxvk_v262_matrix.json":
        errors.append("backlog source_matrix is not canonical")

    expected_gate = {
        "api": "satisfied",
        "implementation": "implemented",
        "cts": "cts-pass",
        "native": "native-evidence",
        "verdict": "satisfied",
    }
    if document.get("policy", {}).get("completion_gate") != expected_gate:
        errors.append("completion gate drift")

    tranches = document.get("tranches")
    if not isinstance(tranches, list) or not tranches:
        raise ValueError("backlog must contain tranches")
    ids = [item.get("id") for item in tranches]
    if any(not isinstance(identifier, str) or not identifier for identifier in ids):
        errors.append("every tranche needs a non-empty id")
    duplicates = sorted(name for name, count in Counter(ids).items() if count > 1)
    if duplicates:
        errors.append("duplicate tranche ids: " + ", ".join(duplicates))
    expected_orders = list(range(1, len(tranches) + 1))
    orders = [item.get("order") for item in tranches]
    if orders != expected_orders:
        errors.append("tranche order must be contiguous and match file order")

    order_by_id = {item.get("id"): item.get("order") for item in tranches}
    all_requirements: list[str] = []
    for item in tranches:
        tranche_id = item.get("id", "<missing>")
        if not item.get("title") or not item.get("acceptance"):
            errors.append(f"{tranche_id} lacks title or acceptance")
        requirements = item.get("requirements")
        if not isinstance(requirements, list) or not requirements:
            errors.append(f"{tranche_id} has no requirements")
            continue
        all_requirements.extend(requirements)
        dependencies = item.get("depends_on")
        if not isinstance(dependencies, list):
            errors.append(f"{tranche_id} depends_on is not a list")
            continue
        for dependency in dependencies:
            if dependency not in order_by_id:
                errors.append(f"{tranche_id} has unknown dependency {dependency}")
            elif order_by_id[dependency] >= item.get("order", 0):
                errors.append(f"{tranche_id} dependency {dependency} is not earlier")

    requirement_duplicates = sorted(
        name for name, count in Counter(all_requirements).items() if count > 1)
    if requirement_duplicates:
        errors.append("requirements assigned more than once: " +
                      ", ".join(requirement_duplicates))

    matrix_rows = matrix.get("requirements", [])
    rows_by_id = {row["id"]: row for row in matrix_rows}
    matrix_ids = set(rows_by_id)
    baseline = document.get("baseline", {})
    excluded = set(baseline.get("excluded_satisfied_requirement_ids", []))
    expected_assignment = matrix_ids - excluded
    assigned = set(all_requirements)
    missing = sorted(expected_assignment - assigned)
    unknown = sorted(assigned - matrix_ids)
    if missing:
        errors.append("unassigned baseline requirements: " + ", ".join(missing))
    if unknown:
        errors.append("unknown requirements: " + ", ".join(unknown))
    excluded_unknown = sorted(excluded - matrix_ids)
    if excluded_unknown:
        errors.append("unknown baseline exclusions: " + ", ".join(excluded_unknown))
    excluded_assigned = sorted(excluded & assigned)
    if excluded_assigned:
        errors.append("baseline-excluded requirements entered backlog: " +
                      ", ".join(excluded_assigned))
    regressed_exclusions = sorted(
        identifier for identifier in excluded
        if identifier in rows_by_id and rows_by_id[identifier].get("verdict") != "satisfied")
    if regressed_exclusions:
        errors.append("baseline satisfied requirements regressed: " +
                      ", ".join(regressed_exclusions))
    if baseline.get("requirements") != len(assigned):
        errors.append("baseline requirement count drift")

    final = next((item for item in tranches if item.get("id") == FINAL_TRANCHE), None)
    if final is None or final is not tranches[-1]:
        errors.append("API promotion tranche must be last")
    else:
        if final.get("requirements") != [API_REQUIREMENT]:
            errors.append("final tranche must contain only the API-version requirement")
        expected_dependencies = set(ids) - {FINAL_TRANCHE}
        if set(final.get("depends_on", [])) != expected_dependencies:
            errors.append("API promotion must depend on every implementation tranche")
    for item in tranches[:-1]:
        if API_REQUIREMENT in item.get("requirements", []):
            errors.append("API-version requirement appears before final promotion")

    invalid_verdicts = sorted(
        identifier for identifier in assigned
        if rows_by_id.get(identifier, {}).get("verdict") not in {"blocker", "satisfied"})
    if invalid_verdicts:
        errors.append("invalid matrix verdicts: " + ", ".join(invalid_verdicts))

    if errors:
        raise ValueError("DXVK backlog validation failed:\n- " + "\n- ".join(errors))

    kinds = Counter(row["kind"] for row in matrix_rows if row["id"] in assigned)
    completed = sum(rows_by_id[identifier]["verdict"] == "satisfied"
                    for identifier in assigned)
    return {
        "tranches": len(tranches),
        "requirements": len(assigned),
        "completed": completed,
        "remaining_blockers": len(assigned) - completed,
        "kinds": dict(sorted(kinds.items())),
        "first": tranches[0]["id"],
        "final": tranches[-1]["id"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="validate the checked-in backlog (default behavior)")
    args = parser.parse_args()
    del args
    try:
        summary = validate(load(BACKLOG), load(MATRIX))
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
