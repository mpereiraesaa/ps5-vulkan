#!/usr/bin/env python3
"""Verify the compact UBO public-SDK GPU witness and ps5log receipt."""

import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def validate(log: bytes, receipt: dict, artifact: dict, eboot: Path) -> dict:
    def require(condition: bool, label: str) -> None:
        if not condition:
            raise ValueError(label)

    contract = artifact.get("ubo_standard_layout", {})
    digest = artifact.get("files", {}).get("eboot.bin")
    require(artifact.get("title") == "PPSA99994" and
            artifact.get("profile") == "ubo-standard-layout-witness" and
            artifact.get("submit_enabled") is True and
            contract.get("api") == "Vulkan 1.0 KHR extension" and
            contract.get("diagnostic_features") is True and
            contract.get("workgroups") == 2 and
            contract.get("values") == 64 and
            contract.get("ubo_bytes") == 136 and
            contract.get("guarded_output") is True,
            "diagnostic UBO artifact contract")
    require(isinstance(digest, str) and len(digest) == 64 and
            hashlib.sha256(eboot.read_bytes()).hexdigest() == digest,
            "eboot artifact identity")
    shader = ROOT / "examples/native_consumer/build/ubo_standard_layout_shader.spv"
    require(shader.is_file() and
            hashlib.sha256(shader.read_bytes()).hexdigest() ==
                contract.get("shader_spirv_sha256"),
            "shader artifact identity")
    require(hashlib.sha256(log).hexdigest() == receipt.get("sha256") and
            receipt.get("protocol") == "ps5log/1" and
            receipt.get("transport") == "tcp" and
            receipt.get("clean") is True and
            receipt.get("bye") is True and
            receipt.get("gaps") == [] and
            receipt.get("raw_lines") == 0,
            "complete TCP receipt")

    lines = log.decode().splitlines()
    require(len(lines) > 2 and lines[0].startswith("HELLO ps5log/1 "), "hello")
    identity = dict(item.split("=", 1) for item in lines[0].split()[2:])
    recorded = receipt.get("identity", {})
    require(identity.get("title") == "PPSA99994" and
            identity.get("app") == "ps5vk-ubo" and
            all(identity.get(key) == recorded.get(key)
                for key in ("title", "app", "boot")),
            "stream identity")
    messages = []
    previous_time = -1
    for seq, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4 and fields[0] == str(seq), "sequence")
        timestamp = int(fields[1])
        require(timestamp >= previous_time, "clock ordering")
        previous_time = timestamp
        require(fields[2] in ("MARK", "INFO") and
                "CHECK failed" not in fields[3] and
                "REQUIRE failed" not in fields[3],
                "runtime failure")
        messages.append(fields[3])
    require(receipt.get("last_seq") == len(messages) and
            lines[-1] ==
                f"BYE seq={len(messages)} reason=ubo-standard-layout-complete",
            "complete BYE")

    def one(prefix: str) -> tuple[int, str]:
        found = [(i, message) for i, message in enumerate(messages)
                 if message.startswith(prefix)]
        require(len(found) == 1, prefix)
        return found[0]

    query = one("PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_QUERY ")
    start = one("PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_START")
    result = one("PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_RESULT ")
    retired = one("PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_RETIRED")
    success = one("PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_SUCCESS")
    require(query[0] < start[0] < result[0] < retired[0] < success[0],
            "witness lifecycle order")
    require(query[1].split()[1:] == ["supported=1", "robust=1"],
            "enabled public feature query")
    require(result[1].split()[1:] == [
        "workgroups=2", "values=64", "ubo_bytes=136", "mismatches=0",
        "guard_mismatches=0"], "exact GPU data and guard result")
    return {
        "strict_verified": True,
        "eboot_sha256": digest,
        "shader_spirv_sha256": contract["shader_spirv_sha256"],
        "workgroups": 2,
        "values": 64,
        "mismatches": 0,
        "guard_mismatches": 0,
        "stream_identity": identity,
    }
