"""Verify the three-viewports graphics profile presentation reference, not graphics profile acceptance.

The deployment manifest supplies artifact identity; TCP does not attest SELF.
This checks observed telemetry ordering, not uninstrumented resource operations.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

REFERENCE_SELF = "4b1826fb6b4210f0a702abe061963844064976fe8b37cd94551093190199b49f"


def expected_messages():
    result = ["PS5VK_BOOT stage=graphics-api submit_enabled=1",
              "PS5VK_PLATFORM_LOAD rc=0", "PS5VK_PLATFORM_INIT rc=0",
              "PS5VK_GRAPHICS_API_DEVICE_CREATED"]
    for index, (width, height, changed) in enumerate(
            [(1920, 1080, 471744), (1440, 810, 265362), (960, 540, 117936)]):
        serial = index + 1
        result += [
            "PS5VK_MEMORY_ALLOC requested=1744 mapped=131072",
            f"PS5VK_GRAPHICS_API_PIPELINE_CREATED iteration={index}",
            "PS5VK_MEMORY_ALLOC requested=134217728 mapped=134217728",
            "PS5VK_MEMORY_ALLOC requested=1008 mapped=131072",
            f"PS5VK_GRAPHICS_PREPARED serial={serial} draws=1 words=44",
            f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
            f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=8912896",
            "PS5VK_MEMORY_RELEASE mapped=131072",
            f"PS5VK_GRAPHICS_API_READBACK changed_words={changed} total_words=2228224 "
            f"bad_alpha=0 bad_sum=0 viewport={width}x{height} valid=1",
            "PS5VK_VIDEO_REGISTER handle=HANDLE buffers=2 image_bytes=8912896",
            f"PS5VK_VIDEO_SUBMIT token={width} rc=0",
            f"PS5VK_VIDEO_PRESENTED token={width} fence=0 matching_event=1 hold_seconds=10",
            f"PS5VK_VIDEO_CLOSED token={width} deferred=1",
            "PS5VK_MEMORY_RELEASE mapped=134217728",
            "PS5VK_MEMORY_RELEASE mapped=131072",
            f"PS5VK_GRAPHICS_API_PIPELINE_DESTROYED iteration={index}",
        ]
    return result + ["PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
                     "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE"]


def validate(log, manifest, artifact):
    def require(condition, message):
        if not condition:
            raise ValueError(message)
    require(artifact.get("title") == "PPSA99994", "artifact title")
    require(artifact.get("stage") == "graphics-api-native-presentation-reuse", "profile mismatch")
    require(artifact.get("submit_enabled") is True, "submit must be enabled")
    eboot = artifact.get("files", {}).get("eboot.bin")
    require(isinstance(eboot, str) and len(eboot) == 64 and all(c in "0123456789abcdef" for c in eboot.lower()),
            "artifact identity")
    require(hashlib.sha256(log).hexdigest() == manifest.get("sha256"), "log hash")
    require(manifest.get("protocol") == "ps5log/1" and manifest.get("transport") == "tcp", "transport")
    require(manifest.get("clean") is True and manifest.get("bye") is True and
            manifest.get("gaps") == [], "unclean stream")
    identity = manifest.get("identity", {})
    require(identity.get("title") == "PPSA99994" and identity.get("app") == "ps5vk", "identity")
    lines = log.decode().splitlines()
    expected = expected_messages()
    require(len(lines) == len(expected) + 2, "record count")
    hello = lines[0].split()
    require(hello[:2] == ["HELLO", "ps5log/1"], "hello")
    fields = dict(item.split("=", 1) for item in hello[2:])
    require(all(fields.get(k) == identity.get(k) for k in ("title", "app", "boot")), "hello identity")
    previous_time = -1
    presented_at = None
    for seq, (line, reference) in enumerate(zip(lines[1:-1], expected), 1):
        parts = line.split("\t", 3)
        require(len(parts) == 4 and parts[0] == str(seq), "sequence")
        timestamp = int(parts[1])
        require(timestamp >= previous_time, "clock ordering")
        previous_time = timestamp
        require(parts[2] in ("MARK", "INFO"), "severity")
        pattern = re.escape(reference).replace("HANDLE", r"[0-9]+")
        require(re.fullmatch(pattern, parts[3]) is not None, f"record {seq}: lifecycle/readback")
        if parts[3].startswith("PS5VK_VIDEO_PRESENTED "):
            presented_at = timestamp
        if parts[3].startswith("PS5VK_VIDEO_CLOSED "):
            require(presented_at is not None and timestamp - presented_at >= 10_000_000_000,
                    "display hold duration")
            presented_at = None
    require(manifest.get("last_seq") == len(expected), "manifest sequence")
    require(lines[-1] == f"BYE seq={len(expected)} reason=graphics-api-end", "bye")
    return {"run_id": manifest["run_id"], "log_sha256": manifest["sha256"],
            "deployment_self_sha256": eboot, "presentations": 3,
            "aggregate_readbacks": 3, "clean_tcp": True,
            "scope": "three-viewports native interop; not swapchain or full graphics profile acceptance"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    print(json.dumps(validate((args.manifest.parent / manifest["log_path"]).read_bytes(),
                              manifest, json.loads(args.artifact.read_text())), indent=2))


if __name__ == "__main__":
    main()
