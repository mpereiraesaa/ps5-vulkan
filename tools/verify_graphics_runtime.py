"""Verify the bounded runtime triangle receipt, not Vulkan conformance or OS exit.

The artifact hash must independently match a verified deployment. A TCP log
does not attest the executable; Close Game and visual receipts are separate.
"""
import argparse
import hashlib
import json
from pathlib import Path


def validate(log, receipt, artifact):
    def require(value, label):
        if not value:
            raise ValueError(label)

    require(artifact.get("title") == "PPSA99994" and
            artifact.get("runtime_graphics") is True and artifact.get("submit_enabled") is True,
            "runtime graphics artifact")
    require(artifact.get("termination") == "shell-close-after-cleanup", "termination mode")
    digest = artifact.get("files", {}).get("eboot.bin", "")
    require(len(digest) == 64 and all(c in "0123456789abcdef" for c in digest), "SELF identity")
    require(hashlib.sha256(log).hexdigest() == receipt.get("sha256"), "log hash")
    require(receipt.get("protocol") == "ps5log/1" and receipt.get("transport") == "tcp" and
            receipt.get("clean") is True and receipt.get("bye") is True and receipt.get("gaps") == [],
            "complete TCP receipt")
    lines = log.decode().splitlines()
    require(len(lines) > 2 and lines[0].startswith("HELLO ps5log/1 "), "hello")
    identity = dict(item.split("=", 1) for item in lines[0].split()[2:])
    require(identity.get("title") == "PPSA99994" and identity.get("app") == "ps5vk" and
            all(identity.get(k) == receipt.get("identity", {}).get(k) for k in ("title", "app", "boot")),
            "stream identity")
    messages, clock = [], -1
    for seq, line in enumerate(lines[1:-1], 1):
        parts = line.split("\t", 3)
        require(len(parts) == 4 and parts[0] == str(seq), "sequence")
        require(int(parts[1]) >= clock, "clock ordering")
        clock = int(parts[1])
        require(parts[2] in ("MARK", "INFO") and "FAIL" not in parts[3], "runtime failure")
        messages.append(parts[3])
    require(lines[-1] == f"BYE seq={len(messages)} reason=graphics-api-end" and
            receipt.get("last_seq") == len(messages), "complete BYE")

    def records(name):
        return [(i, dict(word.split("=", 1) for word in message.split()[1:]))
                for i, message in enumerate(messages) if message.split()[0] == name]

    require(messages.count("PS5VK_RUNTIME_GRAPHICS_INPUT absent_from_offline_library=1") == 1,
            "offline exclusion")
    cached = records("PS5VK_RUNTIME_GRAPHICS_CACHE")
    require(len(cached) == 3, "three graphics acquisitions")
    byte_sizes = []
    for i, (_, row) in enumerate(cached):
        require(all(row.get(k) == v for k, v in {
            "rc": "0", "hit": str(int(i > 0)), "compiled_pairs": "1",
            "hits": str(i), "misses": "1", "entries": "1"}.items()), "cold/warm cache")
        byte_sizes.append(int(row["bytes"]))
    require(0 < byte_sizes[0] <= 4*1024*1024 and len(set(byte_sizes)) == 1, "bounded cache")
    phases = [records("PS5VK_" + name) for name in (
        "GRAPHICS_SUBMIT", "GRAPHICS_SUSPEND_POINT", "GRAPHICS_COMPLETED",
        "GRAPHICS_API_READBACK", "VIDEO_SUBMIT", "VIDEO_SUSPEND_POINT", "VIDEO_PRESENTED")]
    require(all(len(phase) == 18 for phase in phases), "18 completed/presented frames")
    for frame in range(18):
        positions = [phase[frame][0] for phase in phases]
        require(positions == sorted(set(positions)), "frame phase ordering")
        require(cached[frame//6][0] < positions[0], "compile before draw")
        for phase in (0, 1, 4, 5):
            require(phases[phase][frame][1].get("rc") == "0", "submission/suspension status")
        require(len({phases[p][frame][1].get("serial") for p in (0, 1, 2)}) == 1, "GPU serial")
        require(len({phases[p][frame][1].get("token") for p in (4, 5, 6)}) == 1, "VideoOut token")
        require(phases[6][frame][1].get("matching_event") == "1", "presentation event")
        row = phases[3][frame][1]
        viewport, changed = (("1920x1080", 471744), ("1440x810", 265362), ("960x540", 117936))[frame//6]
        require(all(row.get(k) == v for k, v in {
            "viewport": viewport, "changed_words": str(changed), "total_words": "2228224",
            "bad_alpha": "0", "bad_sum": "0", "valid": "1"}.items()), "image readback")
    cleanup = ["PS5VK_RUNTIME_GRAPHICS_CACHE_DESTROYED",
               "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
               "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE",
               "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1"]
    require(messages[-4:] == cleanup, "resource cleanup")
    return {"run_id": receipt.get("run_id"), "self_sha256": digest,
            "log_sha256": receipt["sha256"], "compiled_pairs": 1, "cache_hits": 2,
            "readbacks": 18, "presented_frames": 18, "retained_cache_bytes": byte_sizes[0],
            "os_close": "requires independent lifecycle/visual evidence"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    args = parser.parse_args()
    result = validate(args.log.read_bytes(), json.loads(args.log.with_suffix(".json").read_text()),
                      json.loads(args.artifact.read_text()))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
