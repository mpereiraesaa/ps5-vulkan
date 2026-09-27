#!/usr/bin/env python3
"""Verify saved subgroup-size witness evidence offline; never launch or deploy."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

# name: (XYZ, stage flags, required size 32)
CASES = {
    "32x3x1": ([32, 3, 1], 3, False),
    "64x2x1": ([64, 2, 1], 2, True),
    "32x2x2": ([32, 2, 2], 3, False),
    "1024x1x1": ([1024, 1, 1], 2, True),
    "33x1x1": ([33, 1, 1], 0, False),
    "1x1x1": ([1, 1, 1], 0, True),
}
PROFILE = "subgroup-size-public-sdk-witness"


def expected_words(case: str) -> list[int]:
    total = math.prod(CASES[case][0])
    words = []
    for _ in range(2):
        for first in range(0, total, 32):
            active = min(32, total-first)
            for lane in range(active):
                words.extend((32, lane, first//32, (total+31)//32,
                              active, (1 << active)-1, 1, first+lane))
    return words


def expected_digest(case: str) -> int:
    digest = 2166136261
    for word in expected_words(case):
        digest = ((digest ^ word)*16777619) & 0xffffffff
    return digest


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    case = artifact.get("case")
    if not isinstance(case, str) or case not in CASES:
        raise ValueError("unknown subgroup case")
    dimensions, flags, required = CASES[case]
    if (artifact.get("profile") != PROFILE or artifact.get("contract_version") != 1 or
            artifact.get("dimensions") != dimensions or artifact.get("flags") != flags or
            artifact.get("required_size") is not required or artifact.get("groups") != 2 or
            artifact.get("fields") != 8 or
            not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get("eboot_sha256", "")))):
        raise ValueError("unexpected subgroup artifact contract")
    digest = hashlib.sha256(log).hexdigest()
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            receipt.get("clean") is not True or receipt.get("bye") is not True or
            receipt.get("gaps") != 0 or receipt.get("sha256") != digest or
            not receipt.get("run_id")):
        raise ValueError("incomplete or corrupt receipt")
    text = log.decode("utf-8", errors="strict")
    # Check every marker-bearing line, including duplicates and malformed markers.
    markers = []
    for line in text.splitlines():
        if "SUBGROUP_SIZE_" in line:
            markers.append(line[line.index("SUBGROUP_SIZE_"):])
    outputs = len(expected_words(case))
    expected = [
        f"SUBGROUP_SIZE_START case={case} local={'x'.join(map(str, dimensions))} "
        f"flags={flags} required={int(required)} groups=2 fields=8",
        f"SUBGROUP_SIZE_RESULT outputs={outputs} mismatches=0 guards=0 "
        f"digest={expected_digest(case):08x} fence=complete",
        "SUBGROUP_SIZE_RETIRED resources=clean",
    ]
    if markers != expected:
        raise ValueError("subgroup outputs, guards, fence or retirement failed")
    return {"strict_verified": True, "verification_scope": "log_contents_only",
            "deployment_identity_verified": False, "run_id": receipt["run_id"], "case": case,
            "outputs": outputs, "digest": f"{expected_digest(case):08x}",
            "log_sha256": digest, "eboot_sha256": artifact["eboot_sha256"],
            "native_identity_note": "Requires separate exact deployed-artifact identity evidence"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                    json.loads(args.artifact.read_text()))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
