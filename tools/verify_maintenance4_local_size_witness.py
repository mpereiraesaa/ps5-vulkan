#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify saved LocalSizeId logs offline; deployment identity is a separate gate."""
import argparse
import hashlib
import json
from pathlib import Path
import re


PROFILE = "maintenance4-local-size-id-public-sdk-witness"


def expected_digest(local_x):
    digest = 2166136261
    for index in range(2 * local_x):
        word = 0x62000000 | ((index // local_x) << 16) | (index % local_x)
        digest = ((digest ^ word) * 16777619) & 0xffffffff
    return f"{digest:08x}"


def fixture_contract(local_x):
    if local_x not in (32, 64):
        raise ValueError("unsupported local size")
    return {"profile": PROFILE, "version": 1, "local_x": local_x,
            "specialization": local_x == 32, "groups": 2,
            "outputs": 2 * local_x, "expected_digest": expected_digest(local_x)}


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    local_x = artifact.get("local_x")
    contract = fixture_contract(local_x)
    if any(artifact.get(key) != value for key, value in contract.items()):
        raise ValueError("unexpected LocalSizeId artifact contract")
    for key in ("eboot_sha256", "sdk_sha256", "source_sha256", "helper_sha256",
                "header_sha256", "spirv_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get(key, ""))):
            raise ValueError("missing artifact hash")
    digest = hashlib.sha256(log).hexdigest()
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            receipt.get("clean") is not True or receipt.get("bye") is not True or
            receipt.get("gaps") != 0 or receipt.get("sha256") != digest or
            not receipt.get("run_id")):
        raise ValueError("incomplete or corrupt receipt")
    markers = []
    for line in log.decode("utf-8", errors="strict").splitlines():
        if "MAINTENANCE4_LOCAL_SIZE_" in line:
            markers.append(line[line.index("MAINTENANCE4_LOCAL_SIZE_"):])
    expected = [
        f"MAINTENANCE4_LOCAL_SIZE_START x={local_x} specialization={int(local_x == 32)} groups=2",
        f"MAINTENANCE4_LOCAL_SIZE_RESULT x={local_x} outputs={2*local_x} "
        f"mismatches=0 guards=0 digest={contract['expected_digest']} fence=complete",
        "MAINTENANCE4_LOCAL_SIZE_RETIRED resources=clean",
    ]
    if markers != expected:
        raise ValueError("LocalSizeId output, guard, fence or retirement failed")
    return {"strict_verified": True, "verification_scope": "log_contents_only",
            "deployment_identity_verified": False, "run_id": receipt["run_id"],
            "local_x": local_x, "outputs": 2 * local_x,
            "digest": contract["expected_digest"], "log_sha256": digest,
            "eboot_sha256": artifact["eboot_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                    json.loads(args.artifact.read_text()))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
