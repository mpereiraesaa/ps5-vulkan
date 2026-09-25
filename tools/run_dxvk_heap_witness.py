#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK 1 GiB device-memory witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_heap_witness import (  # noqa: E402
    GIB, HEADROOM_CHUNK_BYTES, ITERATIONS, PROFILE, REGION_BYTES, REGION_OFFSETS,
    expected_digest, seed_for)
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

# The graphics profile budget (src/device_profile_report.h): one 1 GiB
# allocation plus 256 MiB of headroom for internal arenas and presentation.
HEAP_BYTES = GIB + (256 << 20)
STAGES = ("boot", "device", "held", "full", "released", "released", "released",
          "closed")
DIRECT = re.compile(r"DXVK_HEAP_WITNESS_DIRECT_MEMORY stage=(\w+) capacity=(\d+) "
                    r"rc=(-?\d+) block_start=(-?\d+) block_bytes=(\d+)")
START = re.compile(r"DXVK_HEAP_WITNESS_START heap=(\d+) heaps=(\d+) types=(\d+) "
                   r"storage_range=(\d+) allocations=(\d+) ssbo_align=(\d+) "
                   r"atom=(\d+)")
BOUNDARY = re.compile(r"DXVK_HEAP_WITNESS_BOUNDARY bytes=(\d+) result=(-?\d+)")
FENCE = re.compile(r"DXVK_HEAP_WITNESS_FENCE iteration=(\d+) result=(-?\d+)")
RESULT = re.compile(r"DXVK_HEAP_WITNESS_RESULT iteration=(\d+) region=(\d+) "
                    r"offset=(\d+) mismatches=(\d+) guard_mismatches=(\d+) "
                    r"digest=([0-9a-f]{8})")
HEADROOM = re.compile(r"DXVK_HEAP_WITNESS_HEADROOM chunk=(\d+) chunks=(\d+) "
                      r"bytes=(\d+) refusal=(-?\d+)")
RETIRED = re.compile(r"DXVK_HEAP_WITNESS_RETIRED resources=(\w+)")
OUT_OF_DEVICE_MEMORY = -2


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or
            artifact.get("iterations") != ITERATIONS or
            artifact.get("region_bytes") != REGION_BYTES or
            artifact.get("region_offsets") != list(REGION_OFFSETS) or
            artifact.get("headroom_chunk_bytes") != HEADROOM_CHUNK_BYTES):
        raise ValueError("unexpected heap witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    if "DXVK_HEAP_WITNESS_FAILURE" in text:
        raise ValueError("witness reported a failure")
    # The budget, not the kernel, must end every refusal: a kernel allocation
    # failure would mean the budget is not backed by available direct memory.
    if "PS5VK_MEMORY_ALLOC_FAILED" in text:
        raise ValueError("a direct-memory allocation failed inside the budget")
    direct = DIRECT.findall(text)
    if tuple(row[0] for row in direct) != STAGES:
        raise ValueError("missing or repeated direct-memory report")
    start = START.findall(text)
    if len(start) != 1:
        raise ValueError("missing witness start")
    heap, heaps, types, storage_range, allocations, _, _ = (int(v) for v in start[0])
    if heap != HEAP_BYTES or heaps != 1 or types != 1 or storage_range != 256 << 20:
        raise ValueError("physical-device memory report does not match the budget")
    if BOUNDARY.findall(text) != [(str(GIB + 131072), str(OUT_OF_DEVICE_MEMORY))]:
        raise ValueError("allocation above 1 GiB was not refused")
    if FENCE.findall(text) != [(str(n), "0") for n in range(ITERATIONS)]:
        raise ValueError("a bounded fence did not signal")
    expected = [(str(n), str(r), str(offset), "0", "0",
                 f"{expected_digest(seed_for(n, r)):08x}")
                for n in range(ITERATIONS) for r, offset in enumerate(REGION_OFFSETS)]
    if RESULT.findall(text) != expected:
        raise ValueError("1 GiB buffer windows, guards or digests failed")
    headroom = HEADROOM.findall(text)
    if len(headroom) != 1:
        raise ValueError("missing headroom phase")
    chunk, chunks, headroom_bytes, refusal = (int(v) for v in headroom[0])
    # 128 MiB of the 256 MiB headroom must stay reachable with the witness's
    # own pipeline and command arenas live.
    if (chunk != HEADROOM_CHUNK_BYTES or not 4 <= chunks <= 8 or
            headroom_bytes != chunks * chunk or refusal != OUT_OF_DEVICE_MEMORY):
        raise ValueError("heap headroom does not match the budget")
    if RETIRED.findall(text) != ["clean"]:
        raise ValueError("witness resources were not retired")
    by_stage = {}
    for stage, capacity, rc, block_start, block_bytes in direct:
        by_stage.setdefault(stage, []).append({
            "capacity": int(capacity), "rc": int(rc),
            "block_start": int(block_start), "block_bytes": int(block_bytes)})
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "heap_bytes": heap,
        "max_allocation_bytes": GIB,
        "iterations": ITERATIONS,
        "headroom_bytes": headroom_bytes,
        "direct_memory": by_stage,
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
            hashlib.sha256(eboot.read_bytes()).hexdigest() !=
            artifact.get("eboot_sha256")):
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
