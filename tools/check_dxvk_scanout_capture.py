#!/usr/bin/env python3
"""Check the visible 1080p frame of a DXVK quadrant scanout control.

This is a Remote Play frame oracle, not a physical-panel oracle. It consumes
local screenshots and never copies them into the repository. Pillow is needed
only when running this optional visual check.
"""

import argparse
import json
import statistics
import sys
from pathlib import Path


EXPECTED = {
    "top_left": ((0.25, 0.25), (255, 0, 0)),
    "top_right": ((0.75, 0.25), (0, 255, 0)),
    "bottom_left": ((0.25, 0.75), (0, 0, 255)),
    "bottom_right": ((0.75, 0.75), (255, 255, 0)),
}
QUARTER_PATTERN = {
    "top_left": ((0.125, 0.125), (255, 0, 0)),
    "top_right": ((0.375, 0.125), (0, 255, 0)),
    "bottom_left": ((0.125, 0.375), (0, 0, 255)),
    "bottom_right": ((0.375, 0.375), (255, 255, 0)),
    "unused": ((0.75, 0.75), (0, 0, 0)),
}


def sample_median(image, xy, radius=5):
    width, height = image.size
    x = min(width - 1, max(0, int(xy[0] * width)))
    y = min(height - 1, max(0, int(xy[1] * height)))
    pixels = list(image.crop((x - radius, y - radius, x + radius + 1, y + radius + 1)).getdata())
    return tuple(int(statistics.median(pixel[channel] for pixel in pixels)) for channel in range(3))


def matches(actual, expected, tolerance):
    return all(abs(actual[channel] - expected[channel]) <= tolerance for channel in range(3))


def measure(image, pattern, tolerance):
    samples = {}
    for name, (xy, expected) in pattern.items():
        actual = sample_median(image, xy)
        samples[name] = {"rgb": actual, "expected": expected, "pass": matches(actual, expected, tolerance)}
    return samples


def check(path, tolerance):
    from PIL import Image

    with Image.open(path) as source:
        image = source.convert("RGB")
    full = measure(image, EXPECTED, tolerance)
    quarter = measure(image, QUARTER_PATTERN, tolerance)
    return {
        "capture": str(path),
        "size": list(image.size),
        "full_frame_pass": image.size == (1920, 1080) and all(sample["pass"] for sample in full.values()),
        "top_left_quarter_pattern": image.size == (1920, 1080)
        and all(sample["pass"] for sample in quarter.values()),
        "full_frame_samples": full,
        "remote_play_only": True,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="+", type=Path, help="local 1920x1080 screenshot(s)")
    parser.add_argument("--tolerance", type=int, default=80, help="per-channel RGB tolerance (default: 80)")
    args = parser.parse_args()
    if not 0 <= args.tolerance <= 255:
        parser.error("--tolerance must be between 0 and 255")
    try:
        results = [check(path, args.tolerance) for path in args.capture]
    except (OSError, ImportError) as error:
        print(f"scanout capture check: {error}", file=sys.stderr)
        return 2
    print(json.dumps(results, indent=2))
    return 0 if all(result["full_frame_pass"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
