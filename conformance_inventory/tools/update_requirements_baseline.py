#!/usr/bin/env python3
"""Synchronize requirement-row entry-point facts with baseline_surface.json."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def update(requirements: dict, surface: dict) -> int:
    entries = {entry["name"]: entry for entry in surface["entry_points"]}
    changed = 0
    for row in requirements["requirements"]:
        commands = row.get("commands") or []
        if not commands:
            continue
        baseline = row["baseline"]
        present = [name for name in commands if name in entries]
        absent = [name for name in commands if name not in entries]
        dispatched = sum(bool(entries[name].get("dispatch_scope")) for name in present)
        public = sum(bool(entries[name].get("public_header")) for name in present)
        note = (
            "Baseline surface: %d of %d named entry points exist; %d are dispatched; "
            "%d are declared in the public header."
            % (len(present), len(commands), dispatched, public)
        )
        before = (baseline.get("entry_points"), baseline.get("absent_entry_points"),
                  baseline.get("note"), baseline.get("surface_state"))
        baseline["entry_points"] = present
        baseline["absent_entry_points"] = absent
        baseline["note"] = note
        if present:
            baseline["surface_state"] = "symbol-present"
        elif baseline.get("surface_state") == "symbol-present":
            baseline["surface_state"] = "none"
        after = (present, absent, note, baseline.get("surface_state"))
        changed += before != after
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requirements", type=Path,
                        default=ROOT / "requirements.json")
    parser.add_argument("--surface", type=Path,
                        default=ROOT / "baseline_surface.json")
    args = parser.parse_args()
    requirements = json.loads(args.requirements.read_text())
    surface = json.loads(args.surface.read_text())
    changed = update(requirements, surface)
    args.requirements.write_text(json.dumps(requirements, indent=2) + "\n")
    print(f"updated {changed} requirement baseline rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
