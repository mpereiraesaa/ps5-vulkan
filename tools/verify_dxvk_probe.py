#!/usr/bin/env python3
"""Strictly verify one public-ABI DXVK 2.6.2 capability-probe run.

The verifier derives every expected status and aggregate from the pinned
profile.  Payload-reported verdicts are data to check, never an oracle.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "conformance_inventory/dxvk_v262_profile.json"
EXPECTED_PROFILE = "VP_DXVK_d3d11_level_11_0_baseline"
EXPECTED_ARTIFACT_PROFILE = "dxvk-v262-capability-probe"


def require(condition: object, message: str) -> None:
    if not condition:
        raise ValueError(message)


def fields(text: str) -> dict[str, str]:
    return {word.split("=", 1)[0]: word.split("=", 1)[1]
            for word in text.split()[1:] if "=" in word}


def wire_expected(row: dict) -> int:
    value = row["expected"]
    if row["kind"] == "api-version":
        major, minor, patch = (int(part) for part in value.split("."))
        return (major << 22) | (minor << 12) | patch
    if isinstance(value, bool):
        return int(value)
    require(isinstance(value, int) and value >= 0,
            f"unsupported expected value for {row['id']}")
    return value


def validate(run: Path, artifact_manifest: Path, artifact_path: Path,
             matrix_snapshot: Path | None = None) -> dict:
    profile_bytes = PROFILE.read_bytes()
    profile = json.loads(profile_bytes)
    expected_rows = profile["requirements"]
    expected_ids = [row["id"] for row in expected_rows]
    require(profile["profile"]["id"] == EXPECTED_PROFILE, "profile identity")
    require(len(expected_ids) == len(set(expected_ids)), "profile duplicate ids")

    artifact = json.loads(artifact_manifest.read_text())
    digest = hashlib.sha256(artifact_path.read_bytes()).hexdigest()
    require(artifact.get("title") == "PPSA99994" and
            artifact.get("profile") == EXPECTED_ARTIFACT_PROFILE and
            artifact.get("submit_enabled") is False, "artifact profile")
    require(artifact.get("files", {}).get("eboot.bin") == digest,
            "artifact identity")
    dxvk = artifact.get("dxvk", {})
    require(dxvk == {
        "version": "2.6.2",
        "profile_id": EXPECTED_PROFILE,
        "target_api": profile["profile"]["api_version"],
        "requirements": len(expected_rows),
        "profile_sha256": hashlib.sha256(profile_bytes).hexdigest(),
        "matrix_sha256": hashlib.sha256(
            (matrix_snapshot or ROOT / "conformance_inventory/dxvk_v262_matrix.json").read_bytes()
        ).hexdigest(),
    }, "artifact DXVK contract")

    base = Path(run)
    log_path = base if base.suffix == ".log" else base.with_suffix(".log")
    receipt_path = base.with_suffix(".json")
    data = log_path.read_bytes()
    receipt = json.loads(receipt_path.read_text())
    require(hashlib.sha256(data).hexdigest() == receipt.get("sha256"), "log hash")
    require(receipt.get("clean") is True and receipt.get("bye") is True and
            receipt.get("gaps") == [] and receipt.get("transport") == "tcp" and
            receipt.get("protocol") == "ps5log/1", "transport")
    lines = data.decode().splitlines()
    require(len(lines) >= 3 and lines[0].startswith(
        "HELLO ps5log/1 title=PPSA99994 app=ps5vk "), "hello")
    hello = fields("HELLO " + lines[0].split(" ", 2)[2])
    identity = receipt.get("identity", {})
    require(all(hello.get(key) == identity.get(key)
                for key in ("title", "app", "boot")), "runtime identity")

    messages: list[tuple[str, dict[str, str]]] = []
    previous_clock = -1
    for sequence, line in enumerate(lines[1:-1], 1):
        parts = line.split("\t", 3)
        require(len(parts) == 4, "record shape")
        seq, clock, level, message = parts
        require(int(seq) == sequence and int(clock) >= previous_clock,
                "record sequence")
        require(level != "ERR" and "CHECK failed" not in message and
                "REQUIRE failed" not in message, "runtime failure")
        previous_clock = int(clock)
        words = message.split()
        require(bool(words), "empty message")
        messages.append((words[0], fields(message)))
    require(receipt.get("records") == len(messages), "record count")
    require(lines[-1] ==
            f"BYE seq={len(messages)} reason=dxvk262-capability-probe", "bye")

    begins = [row for kind, row in messages if kind == "DXVK262_PROBE_BEGIN"]
    ends = [row for kind, row in messages if kind == "DXVK262_PROBE_END"]
    results = [row for kind, row in messages if kind == "DXVK262_REQUIREMENT"]
    require(len(begins) == 1 and len(ends) == 1, "one probe envelope")
    begin, end = begins[0], ends[0]
    require(begin.get("schema") == "1" and
            begin.get("profile") == EXPECTED_PROFILE and
            begin.get("target_api") == profile["profile"]["api_version"] and
            re.fullmatch(r"\d+\.\d+\.\d+", begin.get("device_api", "")),
            "probe begin")
    actual_ids = [row.get("id") for row in results]
    require(actual_ids == expected_ids, "missing, duplicate or reordered requirements")

    satisfied = 0
    observed: dict[str, int] = {}
    for expected_row, result in zip(expected_rows, results):
        identifier = expected_row["id"]
        expected = wire_expected(expected_row)
        require(result.get("expected", "").isdigit() and
                result.get("observed", "").isdigit(),
                f"non-numeric value for {identifier}")
        require(int(result["expected"]) == expected,
                f"payload expectation drift for {identifier}")
        value = int(result["observed"])
        verdict = "satisfied" if value >= expected else "blocker"
        require(result.get("status") == verdict,
                f"payload verdict mismatch for {identifier}")
        satisfied += verdict == "satisfied"
        observed[identifier] = value

    multiview_ids = {
        "multiview": "feature:VkPhysicalDeviceVulkan11Features:multiview",
        "maxMultiviewViewCount": "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewViewCount",
        "maxMultiviewInstanceIndex": "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewInstanceIndex",
    }
    routes = [row for kind, row in messages if kind == "DXVK262_MULTIVIEW_QUERY"]
    version = tuple(int(part) for part in begin["device_api"].split("."))
    if routes or (version < (1, 2, 0) and any(observed[i] for i in multiview_ids.values())):
        require(len(routes) == 1 and routes[0].get("route") == "VK_KHR_multiview",
                "explicit multiview query route")
        for field, identifier in multiview_ids.items():
            require(routes[0].get(field) == str(observed[identifier]),
                    "multiview route value mismatch")
    standard_ubo_id = "feature:VkPhysicalDeviceVulkan12Features:uniformBufferStandardLayout"
    standard_ubo_routes = [row for kind, row in messages if kind == "DXVK262_STANDARD_UBO_QUERY"]
    if standard_ubo_routes or (version < (1, 2, 0) and observed[standard_ubo_id]):
        require(len(standard_ubo_routes) == 1 and
                standard_ubo_routes[0].get("route") == "VK_KHR_uniform_buffer_standard_layout" and
                standard_ubo_routes[0].get("uniformBufferStandardLayout") ==
                    str(observed[standard_ubo_id]),
                "explicit standard UBO query route")

    total = len(expected_rows)
    blockers = total - satisfied
    compatible = int(satisfied == total)
    require(end.get("valid") == "1" and end.get("compatible") == str(compatible),
            "probe end validity")
    require(end.get("total") == str(total) and
            end.get("satisfied") == str(satisfied) and
            end.get("blockers") == str(blockers), "probe aggregate")
    require(end.get("device_extensions", "").isdigit(), "extension count")
    return {
        "strict_verified": True,
        "compatible": bool(compatible),
        "requirements": total,
        "satisfied": satisfied,
        "blockers": blockers,
        "device_api": begin["device_api"],
        "device_extensions": int(end["device_extensions"]),
        "artifact_eboot_sha256": digest,
        "observed": observed,
        "source_log": str(log_path),
        "query_routes": routes + standard_ubo_routes,
        "source_matrix_sha256": dxvk["matrix_sha256"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--manifest", type=Path,
                        default=ROOT / "dist-consumer/artifact.json")
    parser.add_argument("--artifact", type=Path,
                        default=ROOT / "dist-consumer/PPSA99994/eboot.bin")
    parser.add_argument("--matrix-snapshot", type=Path,
                        help="Immutable build-time matrix; permits verifying historical runs after promotion")
    args = parser.parse_args()
    print(json.dumps(validate(args.run, args.manifest, args.artifact, args.matrix_snapshot), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
