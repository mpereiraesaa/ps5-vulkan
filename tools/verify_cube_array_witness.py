#!/usr/bin/env python3
"""Strictly verify the public SDK's two-cube/six-face ps5log/1 witness."""
import hashlib

TITLE = "PPSA99994"
APP = "ps5vk"
PROFILE = "image-cube-array-witness"
FACE_ORDER = "+x,-x,+y,-y,+z,-z"
TARGET_PIXELS = 192 * 64


def _require(condition, label):
    if not condition:
        raise ValueError(label)


def validate(log, receipt, artifact):
    _require(artifact.get("title") == TITLE and
             artifact.get("profile") == PROFILE and
             artifact.get("submit_enabled") is True,
             "cube-array artifact profile")
    eboot_digest = artifact.get("files", {}).get("eboot.bin", "")
    _require(len(eboot_digest) == 64 and
             all(c in "0123456789abcdef" for c in eboot_digest.lower()),
             "cube-array artifact eboot identity")
    contract = artifact.get("cube_array", {})
    _require(contract.get("feature") == "VkPhysicalDeviceFeatures.imageCubeArray" and
             contract.get("cubes") == 2 and contract.get("faces_per_cube") == 6 and
             contract.get("layers") == 12 and contract.get("face_extent") == [4, 4] and
             contract.get("target_extent") == [192, 64] and
             contract.get("format") == "VK_FORMAT_R8G8B8A8_UNORM",
             "cube-array artifact contract")
    shader_hashes = (contract.get("vertex_spirv_sha256", ""),
                     contract.get("fragment_spirv_sha256", ""))
    _require(all(len(value) == 64 and
                 all(c in "0123456789abcdef" for c in value.lower())
                 for value in shader_hashes),
             "cube-array shader identities")

    _require(hashlib.sha256(log).hexdigest() == receipt.get("sha256"), "log hash")
    _require(receipt.get("protocol") == "ps5log/1" and
             receipt.get("transport") == "tcp" and
             receipt.get("clean") is True and receipt.get("bye") is True and
             receipt.get("gaps") == [] and receipt.get("raw_lines") == 0,
             "complete TCP receipt")
    lines = log.decode("utf-8", errors="strict").splitlines()
    _require(len(lines) > 2 and lines[0].startswith("HELLO ps5log/1 "), "hello")
    identity = dict(item.split("=", 1) for item in lines[0].split()[2:])
    manifest_identity = receipt.get("identity", {})
    _require(identity.get("title") == TITLE and identity.get("app") == APP and
             all(identity.get(key) == manifest_identity.get(key)
                 for key in ("title", "app", "boot")), "stream identity")
    _require(lines[-1].startswith("BYE seq=") and
             lines[-1].endswith(" reason=consumer-cube-array-end"), "complete BYE")

    messages = []
    previous_time = -1
    for sequence, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        _require(len(fields) == 4 and fields[0] == str(sequence), "sequence")
        timestamp = int(fields[1])
        _require(timestamp >= previous_time, "clock ordering")
        previous_time = timestamp
        _require(fields[2] in ("MARK", "INFO") and
                 "CHECK failed" not in fields[3] and
                 "REQUIRE failed" not in fields[3], "runtime failure")
        messages.append(fields[3])
    _require(receipt.get("last_seq") == len(messages), "manifest sequence")

    def one(prefix):
        found = [message for message in messages if message.startswith(prefix)]
        _require(len(found) == 1, prefix)
        return found[0]

    feature = one("PS5VK_CONSUMER_CUBE_ARRAY_FEATURE ")
    _require(feature ==
             "PS5VK_CONSUMER_CUBE_ARRAY_FEATURE imageCubeArray=1 enabled_by_features2=1",
             "feature negotiation")
    start = one("PS5VK_CONSUMER_CUBE_ARRAY_START ")
    _require(f"cubes=2 faces=6 layers=12 image=4x4 target=192x64 " in start and
             f"vertex_sha256={shader_hashes[0]} fragment_sha256={shader_hashes[1]}" in start,
             "witness identity and shape")
    result = one("PS5VK_CONSUMER_CUBE_ARRAY_RESULT ")
    _require(result ==
             f"PS5VK_CONSUMER_CUBE_ARRAY_RESULT cells=12 pixels={TARGET_PIXELS} "
             f"mismatches=0 face_order={FACE_ORDER} cube_count=2",
             "all cube-array face/cube samples")
    _require(one("PS5VK_CONSUMER_CUBE_ARRAY_RETIRED ") ==
             "PS5VK_CONSUMER_CUBE_ARRAY_RETIRED fence_complete=1 allocations=0",
             "witness resource retirement")
    _require(one("PS5VK_CONSUMER_TEST_SUCCESS") == "PS5VK_CONSUMER_TEST_SUCCESS" and
             one("PS5VK_CONSUMER_RESOURCES_RETIRED ") ==
             "PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1" and
             one("PS5VK_READY_FOR_SHELL_CLOSE ") ==
             "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1",
             "consumer clean shutdown markers")

    return {
        "profile": PROFILE,
        "artifact_eboot_sha256": eboot_digest,
        "vertex_spirv_sha256": shader_hashes[0],
        "fragment_spirv_sha256": shader_hashes[1],
        "cubes": 2,
        "faces_per_cube": 6,
        "pixels_checked": TARGET_PIXELS,
        "mismatches": 0,
        "fence_complete": True,
    }
