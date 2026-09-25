#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK VK_EXT_robustness2 witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t13_robustness2_witness import (  # noqa: E402
    PROFILE, SDK_SWITCHES, STORAGE_RANGE, UNIFORM_RANGE)
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

ACCESS_WORDS = 26
NULL_IMAGE_ALPHA_WORD = 25
# The robustness2 oracle, word by word (see examples/t13_robustness2_witness).
EXPECTED = {0: 0x1000, 1: 0x1008, 2: 0x1009, 5: 0x2008, 6: 0x2009, 13: 0x2000}

START = re.compile(r"T13_ROBUSTNESS2_WITNESS_START spec=(\d+) storage_alignment=(\d+) "
                   r"uniform_alignment=(\d+) storage_range=(\d+) uniform_range=(\d+)")
FENCE = re.compile(r"T13_ROBUSTNESS2_WITNESS_FENCE result=(-?\d+) slices=(\d+) "
                   r"query_pipeline=(-?\d+)")
WORD = re.compile(r"T13_ROBUSTNESS2_WITNESS_WORD index=(\d+) value=([0-9a-f]{8}) "
                  r"expected=([0-9a-f]{8})")
ACCESS = re.compile(r"T13_ROBUSTNESS2_WITNESS_ACCESS marker=([0-9a-f]{8}) words=(\d+) "
                    r"mismatches=(\d+) first=(-?\d+) guard_mismatches=(\d+) "
                    r"store_mismatches=(\d+) in_range_store=([0-9a-f]{8}) "
                    r"dropped_store_12=([0-9a-f]{8}) dropped_store_60=([0-9a-f]{8})")
QUERY = re.compile(r"T13_ROBUSTNESS2_WITNESS_QUERY pipeline=(created|refused)(.*)")
QUERY_CREATED = re.compile(r" marker=([0-9a-f]{8}) image_width=(\d+) image_height=(\d+) "
                           r"texel_size=(\d+)$")
RETIRED = re.compile(r"T13_ROBUSTNESS2_WITNESS_RETIRED resources=(\w+)")


def expected_word(index: int) -> int:
    return EXPECTED.get(index, 0)


def word_matches(index: int, value: int) -> bool:
    # A null storage image read through a format qualifier without alpha may
    # return alpha 1: the pinned CTS robustness2 oracle accepts zzzo there.
    return value == expected_word(index) or (index == NULL_IMAGE_ALPHA_WORD and value == 1)


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or
            artifact.get("storage_range") != STORAGE_RANGE or
            artifact.get("uniform_range") != UNIFORM_RANGE or
            artifact.get("sdk_switches") != SDK_SWITCHES):
        raise ValueError("unexpected robustness2 witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start, fence = START.findall(text), FENCE.findall(text)
    words, access = WORD.findall(text), ACCESS.findall(text)
    query, retired = QUERY.findall(text), RETIRED.findall(text)
    if (len(start) != 1 or len(fence) != 1 or len(words) != ACCESS_WORDS or
            len(access) != 1 or len(query) != 1 or len(retired) != 1 or
            "T13_ROBUSTNESS2_WITNESS_FAILURE" in text):
        raise ValueError("missing, repeated or failed witness phase")
    spec, storage_alignment, uniform_alignment, storage_range, uniform_range = (
        int(field) for field in start[0])
    if (spec < 1 or (storage_alignment, uniform_alignment) != (4, 4) or
            (storage_range, uniform_range) != (STORAGE_RANGE, UNIFORM_RANGE)):
        raise ValueError("robustness2 exposure does not match the witness contract")
    if int(fence[0][0]) != 0:
        raise ValueError("the bounded fence did not signal")
    for position, (index, value, expected) in enumerate(words):
        if (int(index) != position or int(expected, 16) != expected_word(position) or
                not word_matches(position, int(value, 16))):
            raise ValueError(f"access word {position} differs from the oracle")
    marker, count, mismatches, first, guards, stores, kept, dropped12, dropped60 = access[0]
    if (marker != "c0de0013" or int(count) != ACCESS_WORDS or int(mismatches) or
            int(first) != -1 or int(guards) or int(stores) or kept != "5a5a0001" or
            dropped12 != "0000100c" or dropped60 != "0000103c"):
        raise ValueError("access summary, guards or discarded stores failed")
    query_state, query_tail = query[0]
    query_result = {"pipeline": query_state}
    if query_state == "created":
        created = QUERY_CREATED.match(query_tail)
        if not created or created.groups() != ("c0de0014", "0", "0", "0"):
            raise ValueError("null size queries did not return zero")
        query_result.update(image_size=[0, 0], texel_size=0)
    if retired[0] != "clean":
        raise ValueError("witness resources were not retired")
    order = [text.index(marker) for marker in (
        "T13_ROBUSTNESS2_WITNESS_START", "T13_ROBUSTNESS2_WITNESS_FENCE",
        "T13_ROBUSTNESS2_WITNESS_WORD", "T13_ROBUSTNESS2_WITNESS_ACCESS",
        "T13_ROBUSTNESS2_WITNESS_QUERY", "T13_ROBUSTNESS2_WITNESS_RETIRED")]
    if order != sorted(order):
        raise ValueError("witness phases are out of order")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "spec_version": spec,
        "robust_access_size_alignment": {"storage": 4, "uniform": 4},
        "fence_slices": int(fence[0][1]),
        "access_words": ACCESS_WORDS,
        "query": query_result,
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
