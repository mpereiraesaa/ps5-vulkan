#!/usr/bin/env python3
"""Run and strictly verify the bounded compute subgroup BASIC witness."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

GROUPS, INVOCATIONS, FIELDS = 2, 128, 7
SALTS = (0x1000, 0x2000)
MARK = "T08_SUBGROUP_BASIC"


def expected_words() -> list[int]:
    """SubgroupSize, InvocationID, SubgroupID (16x4 workgroup), NumSubgroups,
    uniform Elect, odd-lane Elect (2 outside the branch), barrier exchange."""
    words = []
    for invocation in range(INVOCATIONS):
        group, local = divmod(invocation, 64)
        lane = local % 32
        words += [32, lane, local // 32, 2, int(lane == 0),
                  int(lane == 1) if lane & 1 else 2, (local ^ 1) * 3 + SALTS[group]]
    return words


def expected_digest() -> int:
    digest = 2166136261
    for value in expected_words():
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return digest


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != "t08-subgroup-basic-diagnostic-witness" or
            artifact.get("groups") != GROUPS or artifact.get("fields") != FIELDS or
            artifact.get("public_profile") != "vulkan-1.0-subgroup-disabled"):
        raise ValueError("unexpected subgroup witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt witness receipt")
    text = log.decode("utf-8", errors="replace")
    starts = re.findall(MARK + r"_START groups=(\d+) local=(\d+x\d+) subgroups=(\d+) "
                        r"fields=(\d+) api=([\d.]+)", text)
    results = re.findall(MARK + r"_RESULT outputs=(\d+) mismatches=(\d+) guards=(\d+) "
                         r"digest=([0-9a-f]{8}) fence=(\w+)", text)
    retired = re.findall(MARK + r"_RETIRED resources=(\w+)", text)
    if (starts != [("2", "16x4", "4", "7", "1.0")] or
            results != [(str(INVOCATIONS * FIELDS), "0", "0",
                         f"{expected_digest():08x}", "complete")] or
            retired != ["clean"] or MARK + "_FAILURE" in text or
            "first_mismatch" in text or
            not text.index(MARK + "_START") < text.index(MARK + "_RESULT") <
            text.index(MARK + "_RETIRED")):
        raise ValueError("subgroup data, guard, fence or cleanup failed")
    return {"strict_verified": True, "run_id": receipt["run_id"],
            "outputs": INVOCATIONS * FIELDS, "mismatches": 0, "guards": 0,
            "digest": f"{expected_digest():08x}", "log_sha256": receipt["sha256"],
            "eboot_sha256": artifact["eboot_sha256"]}


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
    if (artifact.get("profile") != "t08-subgroup-basic-diagnostic-witness" or
            hashlib.sha256((args.dist / "eboot.bin").read_bytes()).hexdigest() !=
            artifact.get("eboot_sha256")):
        raise RuntimeError("artifact identity mismatch")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result, launched = {}, False
    try:
        reply = control("launch", args.host)
        if "Error spawning payload" in reply:
            raise RuntimeError("console rejected witness launch: " + reply.strip())
        launched = True
        log_path = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log_path.with_suffix(".json").read_text())
        result = verify(log_path.read_bytes(), receipt, artifact)
        result["source_log"] = str(log_path)
    finally:
        try:
            lifecycle_ok = close_and_confirm(args.host) if launched else running(args.host) == "none"
        except RuntimeError:
            lifecycle_ok = False
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not lifecycle_ok:
        raise RuntimeError("witness title did not stop")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
