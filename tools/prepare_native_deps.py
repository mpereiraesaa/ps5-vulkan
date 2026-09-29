#!/usr/bin/env python3
"""Fetch pinned public PS5 graphics and logging support."""
import argparse
from pathlib import Path

from prepare_compiler_deps import prepare_dep

ROOT = Path(__file__).resolve().parents[1]
GEARS = {
    "name": "ps5-agc-gears",
    "dest": ROOT / "third_party/ps5-agc-gears",
    "url": "https://github.com/mpereiraesaa/ps5-agc-gears.git",
    "pin": "1ae1f9182abd2770c131b97419034fb85173c2dc",
}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify only")
    args = parser.parse_args()
    prepare_dep(GEARS, args.check)
