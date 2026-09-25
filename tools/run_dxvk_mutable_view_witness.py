#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK mutable-format view witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_mutable_view_witness import (  # noqa: E402
    EXTENT, PIXELS, PROFILE, SWITCH, min_differing, srgb_exact, texels)
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"DXVK_MUTABLE_VIEW_WITNESS_START extent=(\d+) flags2_spec=(\d+) "
                   r"list_spec=(\d+) unorm32=([0-9a-f]{8}) unorm64=([0-9a-f]{16}) "
                   r"srgb32=([0-9a-f]{8}) srgb64=([0-9a-f]{16}) rt_query=(-?\d+) "
                   r"rt_max=(\d+)x(\d+)")
REFUSAL = re.compile(r"DXVK_MUTABLE_VIEW_WITNESS_REFUSAL view=target_srgb result=(-?\d+)")
RESULT = re.compile(r"DXVK_MUTABLE_VIEW_WITNESS_RESULT bytes=(\d+) unorm_mismatches=(\d+) "
                    r"srgb_vs_native=(\d+) srgb_over_one=(\d+) srgb_exact=(\d+) "
                    r"alpha_mismatches=(\d+) srgb_vs_unorm_differing=(\d+) "
                    r"digest_unorm=([0-9a-f]{8}) digest_srgb=([0-9a-f]{8}) "
                    r"digest_native=([0-9a-f]{8})")
RETIRED = re.compile(r"DXVK_MUTABLE_VIEW_WITNESS_RETIRED resources=(\w+)")
COLOR_ATTACHMENT = 0x80
SAMPLED_IMAGE = 0x1


def fnv1a(payload: bytes) -> int:
    digest = 2166136261
    for value in payload:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return digest


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("extent") != EXTENT or
            artifact.get("diagnostic_switch") != SWITCH or
            artifact.get("min_differing") != min_differing() or
            artifact.get("texels_sha256") != hashlib.sha256(texels()).hexdigest() or
            artifact.get("srgb_exact_sha256") != hashlib.sha256(srgb_exact()).hexdigest()):
        raise ValueError("unexpected mutable-view witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start, refusal = START.findall(text), REFUSAL.findall(text)
    results, retired = RESULT.findall(text), RETIRED.findall(text)
    if (len(start) != 1 or len(refusal) != 1 or len(results) != 1 or len(retired) != 1 or
            "DXVK_MUTABLE_VIEW_WITNESS_FAILURE" in text):
        raise ValueError("missing, repeated or failed witness phase")
    (extent, flags2_spec, list_spec, unorm32, unorm64, srgb32, srgb64, rt_query,
     rt_width, rt_height) = start[0]
    if (int(extent) != EXTENT or int(flags2_spec) < 1 or int(list_spec) < 1 or
            int(unorm32, 16) != int(unorm64, 16) or int(srgb32, 16) != int(srgb64, 16) or
            not int(unorm32, 16) & COLOR_ATTACHMENT or int(srgb32, 16) & COLOR_ATTACHMENT or
            not int(srgb32, 16) & SAMPLED_IMAGE or int(rt_query) != 0 or
            int(rt_width) < EXTENT or int(rt_height) < EXTENT):
        raise ValueError("format reporting does not match the witness contract")
    if int(refusal[0]) == 0:
        raise ValueError("an SRGB colour-attachment view was created")
    (count, unorm_mismatches, native, over_one, exact, alpha, differing,
     digest_unorm, digest_srgb, digest_native) = results[0]
    if (int(count) != PIXELS * 4 or int(unorm_mismatches) or int(native) or
            int(over_one) or int(alpha) or int(differing) < min_differing() or
            int(digest_unorm, 16) != fnv1a(texels()) or digest_srgb != digest_native):
        raise ValueError("mutable-view readback failed its oracle")
    if retired[0] != "clean":
        raise ValueError("witness resources were not retired")
    order = [text.index(marker) for marker in (
        "DXVK_MUTABLE_VIEW_WITNESS_START", "DXVK_MUTABLE_VIEW_WITNESS_REFUSAL",
        "DXVK_MUTABLE_VIEW_WITNESS_RESULT", "DXVK_MUTABLE_VIEW_WITNESS_RETIRED")]
    if order != sorted(order):
        raise ValueError("witness phases are out of order")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "format_feature_flags2_spec": int(flags2_spec),
        "image_format_list_spec": int(list_spec),
        "srgb_exact_bytes": int(exact),
        "srgb_vs_unorm_differing": int(differing),
        "digests": {"unorm": digest_unorm, "srgb_view": digest_srgb,
                    "srgb_image": digest_native},
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
