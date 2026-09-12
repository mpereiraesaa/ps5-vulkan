#!/usr/bin/env python3
"""Rewrite the generated stats block in README.md from the inventory data.

README numbers are derived, never hand-maintained: this tool rewrites the JSON
block between the ``<!-- stats:begin -->`` and ``<!-- stats:end -->`` markers,
and ``validate.py`` fails (T018) when the block disagrees with the data.

Usage
-----
    python3 conformance_inventory/tools/update_readme_stats.py
    python3 conformance_inventory/tools/update_readme_stats.py --check
"""

from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INVENTORY_DIR = os.path.dirname(HERE)
if INVENTORY_DIR not in sys.path:
    sys.path.insert(0, INVENTORY_DIR)

import validate  # noqa: E402

README = os.path.join(INVENTORY_DIR, "README.md")


def render(stats: dict) -> str:
    body = json.dumps(stats, indent=2, sort_keys=True)
    return "%s\n```json\n%s\n```\n%s" % (validate.README_BEGIN, body, validate.README_END)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if README is out of date instead of rewriting it")
    args = parser.parse_args(argv)

    bundle = validate.Bundle.load(INVENTORY_DIR)
    rows = (bundle.requirements or {}).get("requirements", [])
    stats = validate.readme_stats(bundle, rows)
    with open(README, "r", encoding="utf-8") as handle:
        text = handle.read()
    expected_block = render(stats)
    start = text.find(validate.README_BEGIN)
    end = text.find(validate.README_END)
    if start < 0 or end < 0:
        raise SystemExit("README.md has no stats block; add the markers first")
    current = text[start: end + len(validate.README_END)]
    if current.strip() == expected_block.strip():
        print("README stats are current")
        return 0
    if args.check:
        print("README stats are stale; run tools/update_readme_stats.py", file=sys.stderr)
        return 1
    with open(README, "w", encoding="utf-8") as handle:
        handle.write(text[:start] + expected_block + text[end + len(validate.README_END):])
    print("README stats updated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
