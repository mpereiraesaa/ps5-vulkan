#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK DXVK render witness (DXVK262-T10).

The expected image is derived here, independently of the payload, from the
Vulkan rules the witness exercises: DXVK's y-flipped viewport
{x 0, y 64, width 64, height -64}, the signed-area facing rule with
FRONT_FACE_CLOCKWISE and CULL_MODE_BACK for the two marker quads, the exact
gradient R = 4x, G = 4y, B = 64 over columns 0..47, and the red clear.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

EXTENT = 64
PROFILE = "dxvk-render-public-sdk-witness"
VIEWPORT = (0.0, 64.0, 64.0, -64.0)
SAMPLE_POINTS = ((0, 0), (47, 63), (48, 0), (63, 40), (63, 63))


def fnv(data: bytes) -> int:
    digest = 2166136261
    for byte in data:
        digest = ((digest ^ byte) * 16777619) & 0xffffffff
    return digest


def rgba(r: int, g: int, b: int, a: int) -> int:
    return r | (g << 8) | (b << 16) | (a << 24)


def corner(index: int) -> tuple[float, float]:
    """The NDC corner the witness vertex shader emits for gl_VertexIndex."""
    q, k = divmod(index, 6)
    if q == 2:
        k = (k // 3) * 3 + 2 - k % 3
    cx = 1.0 if k in (1, 4, 5) else 0.0
    cy = 1.0 if k in (2, 3, 5) else 0.0
    lo_x, hi_x = (-1.0, 0.5) if q == 0 else (0.5, 1.0)
    lo_y = 0.0 if q == 2 else -1.0
    hi_y = 0.0 if q == 1 else 1.0
    return lo_x + (hi_x - lo_x) * cx, lo_y + (hi_y - lo_y) * cy


def front_facing_clockwise(first: int, viewport=VIEWPORT) -> bool:
    vx, vy, vw, vh = viewport
    points = []
    for i in range(3):
        xn, yn = corner(first + i)
        points.append((vx + vw / 2 + vw / 2 * xn, vy + vh / 2 + vh / 2 * yn))
    area = -0.5 * sum(points[i][0] * points[(i + 1) % 3][1] -
                      points[(i + 1) % 3][0] * points[i][1] for i in range(3))
    return area < 0.0


def expected_image(viewport=VIEWPORT) -> tuple[list[int], int, int]:
    image = [rgba(4 * x, 4 * y, 64, 255) if x < 48 else rgba(255, 0, 0, 255)
             for y in range(EXTENT) for x in range(EXTENT)]
    visible, top_row = 0, None
    vx, vy, vw, vh = viewport
    for q, color in ((1, rgba(0, 255, 0, 255)), (2, rgba(0, 0, 255, 255))):
        if not front_facing_clockwise(q * 6, viewport):
            continue
        y0n, y1n = (-1.0, 0.0) if q == 1 else (0.0, 1.0)
        rows = sorted((vy + vh / 2 + vh / 2 * y0n, vy + vh / 2 + vh / 2 * y1n))
        for y in range(int(rows[0]), int(rows[1])):
            for x in range(48, EXTENT):
                image[y * EXTENT + x] = color
        visible |= 1 << q
        top_row = int(rows[0]) if top_row is None else min(top_row, int(rows[0]))
    return image, visible, top_row if top_row is not None else 0xffffffff


def expected_digest() -> str:
    image, _, _ = expected_image()
    return f"{fnv(b''.join(v.to_bytes(4, 'little') for v in image)):08x}"


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("extent") != EXTENT or
            artifact.get("format") != "R8G8B8A8_UNORM" or
            artifact.get("diagnostic_switch") != "PS5VK_DXVK_RENDER_DIAGNOSTIC"):
        raise ValueError("unexpected DXVK render witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start = re.findall(r"DXVK_RENDER_WITNESS_START extent=(\d+) dynamicRendering=(\d+) "
                       r"extendedDynamicState=(\d+)", text)
    steps = re.findall(r"DXVK_RENDER_WITNESS_STEP index=(\d+) fence=complete", text)
    samples = re.findall(r"DXVK_RENDER_WITNESS_SAMPLES p0_0=([0-9a-f]{8}) p47_63=([0-9a-f]{8}) "
                         r"p48_0=([0-9a-f]{8}) p63_40=([0-9a-f]{8}) p63_63=([0-9a-f]{8})", text)
    result = re.findall(r"DXVK_RENDER_WITNESS_RESULT extent=(\d+) full_mismatches=(\d+) "
                        r"sub_mismatches=(\d+) sentinel_mismatches=(\d+) "
                        r"visible_markers=([0-9a-f]+) marker_top=(\d+) digest=([0-9a-f]{8}) "
                        r"submissions=(\d+) fence=(\w+)", text)
    retired = re.findall(r"DXVK_RENDER_WITNESS_RETIRED resources=(\w+)", text)
    image, visible, top_row = expected_image()
    if (len(start) != 1 or len(result) != 1 or len(samples) != 1 or len(retired) != 1 or
            start[0] != (str(EXTENT), "1", "1") or steps != ["0"] or
            [int(s, 16) for s in samples[0]] != [image[y * EXTENT + x] for x, y in SAMPLE_POINTS] or
            result[0] != (str(EXTENT), "0", "0", "0", f"{visible:x}", str(top_row),
                          expected_digest(), "1", "complete") or
            retired[0] != "clean" or
            text.index("DXVK_RENDER_WITNESS_START") >= text.index("DXVK_RENDER_WITNESS_RESULT") or
            text.index("DXVK_RENDER_WITNESS_RESULT") >= text.index("DXVK_RENDER_WITNESS_RETIRED") or
            "DXVK_RENDER_WITNESS_FAILURE" in text):
        raise ValueError("render, readback, fence or cleanup failed")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "extent": EXTENT,
        "visible_markers": visible,
        "marker_top": top_row,
        "digest": result[0][6],
        "mismatches": [0, 0, 0],
        "log_sha256": receipt["sha256"],
        "eboot_sha256": artifact["eboot_sha256"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch while a title is active")
    artifact = json.loads(args.artifact.read_text())
    eboot = args.dist / "eboot.bin"
    if (artifact.get("profile") != PROFILE or
            hashlib.sha256(eboot.read_bytes()).hexdigest() != artifact.get("eboot_sha256")):
        raise RuntimeError("artifact identity mismatch")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result = {}
    lifecycle_ok = False
    launched = False
    try:
        control("launch", args.host)
        launched = True
        log_path = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log_path.with_suffix(".json").read_text())
        result = verify(log_path.read_bytes(), receipt, artifact)
        result["source_log"] = str(log_path)
    finally:
        lifecycle_ok = (close_and_confirm(args.host) if launched else
                        running(args.host) == "none")
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not lifecycle_ok:
        raise RuntimeError("witness title did not stop")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
