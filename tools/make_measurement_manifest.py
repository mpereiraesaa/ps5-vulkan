#!/usr/bin/env python3
"""Derive a measurement selection from the frozen upstream manifest.

The frozen manifest in cts/upstream/manifest.json is the acceptance selection.
Some of its diagnostics are leaves whose upstream oracle is expected to pass
once a feature is advertised on a measurement build (PS5VK_RASTER_DIAGNOSTIC=1);
measuring them means packaging and running them beside the acceptance cases
without editing the frozen file by hand. This tool writes a separate manifest
that moves the diagnostics of the named categories into ``cases`` and records
where they came from, so tools/build_upstream_cts.py and
tools/run_upstream_cts.py can take it through ``--manifest`` and the receipt
carries the derivation.

Only diagnostics whose ``expected_status`` is ``Pass`` may move: a leaf that is
documented as NotSupported or Fail is a gap, and running it under acceptance
rules would only reproduce the gap.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FROZEN_MANIFEST = ROOT / "cts/upstream/manifest.json"


def selection_hash(cases: list[dict]) -> str:
    content = "\n".join(case["path"] for case in cases) + "\n"
    return hashlib.sha256(content.encode("utf-8")).hexdigest()


def build_measurement_manifest(manifest: dict, categories: set[str],
                               base_manifest: str = "cts/upstream/manifest.json") -> dict:
    """Return a copy of ``manifest`` with the diagnostics of ``categories``
    moved into ``cases`` and a ``measurement`` record describing the move."""
    if not categories:
        raise ValueError("at least one diagnostic category is required")
    derived = copy.deepcopy(manifest)
    known = {d.get("category") for d in manifest.get("diagnostics", [])}
    unknown = sorted(c for c in categories if c not in known)
    if unknown:
        raise ValueError(f"no diagnostic carries category {', '.join(unknown)}")
    moved: list[dict] = []
    kept: list[dict] = []
    for diagnostic in derived.get("diagnostics", []):
        if diagnostic.get("category") not in categories:
            kept.append(diagnostic)
            continue
        if diagnostic.get("expected_status") != "Pass":
            raise ValueError(
                f"{diagnostic['path']}: category {diagnostic.get('category')!r} is "
                f"expected {diagnostic.get('expected_status')!r}, not Pass; a measurement "
                f"selection may only move leaves whose oracle is expected to pass")
        entry = dict(diagnostic)
        entry["measurement_origin"] = f"diagnostic:{diagnostic['category']}"
        moved.append(entry)
    derived["cases"] = list(derived["cases"]) + moved
    derived["diagnostics"] = kept
    derived["measurement"] = {
        "base_manifest": base_manifest,
        "base_selection_hash": selection_hash(manifest["cases"]),
        "categories": sorted(categories),
        "moved": len(moved),
        "selection_hash": selection_hash(derived["cases"]),
        "note": ("Measurement selection: NOT the frozen acceptance selection. Its receipt "
                 "is evidence for the moved leaves only; promotion still edits the frozen "
                 "manifest in its own change."),
    }
    return derived


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--manifest", type=Path, default=FROZEN_MANIFEST,
                        help="frozen selection to derive from")
    parser.add_argument("--category", action="append", default=[], required=True,
                        help="diagnostic category to move into cases (repeatable)")
    parser.add_argument("-o", "--output", type=Path, required=True,
                        help="where to write the measurement manifest")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    try:
        base = str(args.manifest.resolve().relative_to(ROOT))
    except ValueError:
        base = str(args.manifest)
    derived = build_measurement_manifest(manifest, set(args.category), base)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(derived, indent=2) + "\n")
    record = derived["measurement"]
    print(f"[measurement] {record['moved']} leaves moved from {record['categories']}; "
          f"{len(derived['cases'])} cases; selection {record['selection_hash'][:16]}... "
          f"(frozen {record['base_selection_hash'][:16]}...) -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
