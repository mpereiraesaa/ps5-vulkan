"""Check exact coverage of the planar diagnostic, not artifact/lifecycle identity."""
import argparse
import json
from pathlib import Path


def check(text):
    # Triangle screen vertices: (384,216), (1536,216), (960,864).
    # The horizontal split bisects height, leaving 1/4 area below it.
    total = (1536 - 384) * (864 - 216) // 2
    regions = {(0, 0, 960, 540): total * 3 // 8,
               (960, 0, 960, 540): total * 3 // 8,
               (0, 540, 960, 540): total // 8,
               (960, 540, 960, 540): total // 8,
               (0, 0, 1920, 1080): total}
    expected = {(depth, *region): count for depth in (0, 1)
                for region, count in regions.items()}
    seen = {}
    for line in text.splitlines():
        fields = line.split("\t", 3)
        if len(fields) != 4 or not fields[3].startswith("PS5VK_SCISSOR_PROBE "):
            continue
        values = dict(item.split("=", 1) for item in fields[3].split()[1:])
        if int(values["mode"]) != 3:
            raise ValueError("requires planar diagnostic mode 3")
        key = tuple(int(values[name]) for name in ("depth", "x", "y", "width", "height"))
        if key not in expected or key in seen:
            raise ValueError("unexpected or duplicate scissor case")
        seen[key] = int(values["total_changed"])
        if seen[key] != expected[key]:
            raise ValueError(f"coverage mismatch for {key}: {seen[key]} != {expected[key]}")
    if set(seen) != set(expected):
        raise ValueError("missing planar scissor cases")
    return {"cases": len(seen), "exact_coverage": True,
            "full_screen_pixels": total, "scope": "planar coverage only; not graphics profile acceptance"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.log.read_text()), indent=2))
