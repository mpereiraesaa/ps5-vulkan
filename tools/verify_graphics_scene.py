"""Audit the 180-frame black-background scene evidence, not clean process exit.

Artifact identity is a deployment record, not runtime attestation. Aggregate
readback is not a spatial oracle, and Remote Play must be inspected separately.
"""
import argparse
import hashlib
import json
from pathlib import Path

REFERENCE_SELF = "37361c9acff7e99eaa87af78661db96253a2d1e0541a38f727dee6c3429ffe36"


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(log, meta, artifact):
    require(artifact.get("files", {}).get("eboot.bin") == REFERENCE_SELF, "artifact")
    require(artifact.get("scene") == "two-cubes" and not artifact.get("exit_control")
            and not artifact.get("keep_agc_module"), "diagnostic artifact")
    require(hashlib.sha256(log).hexdigest() == meta.get("sha256"), "hash")
    require(meta.get("clean") is True and meta.get("bye") is True and
            meta.get("gaps") == [] and meta.get("transport") == "tcp" and
            meta.get("protocol") == "ps5log/1", "transport")
    lines = log.decode().splitlines()
    require(lines and lines[0].startswith("HELLO ps5log/1 "), "hello")
    identity = dict(w.split("=", 1) for w in lines[0].split()[2:])
    require(identity.get("title") == "PPSA99994" and identity.get("app") == "ps5vk"
            and all(meta.get("identity", {}).get(k) == identity.get(k)
                    for k in ("title", "app", "boot")), "identity")
    records = []
    previous = -1
    for i, line in enumerate(lines[1:-1], 1):
        seq, stamp, level, message = line.split("\t", 3)
        require(int(seq) == i and int(stamp) >= previous and level != "ERR", "record")
        previous = int(stamp)
        words = message.split()
        records.append((words[0], dict(w.split("=", 1) for w in words[1:])))
    require(meta.get("records") == len(records) and
            lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end", "bye/count")
    names = ("PS5VK_COLOR_CLEAR_PREPARED", "PS5VK_GRAPHICS_SUBMIT",
             "PS5VK_GRAPHICS_COMPLETED", "PS5VK_GRAPHICS_API_READBACK",
             "PS5VK_TEXTURE_READBACK", "PS5VK_VIDEO_SUBMIT",
             "PS5VK_VIDEO_PRESENTED", "PS5VK_GRAPHICS_REUSE_END")
    groups = [[(i, f) for i, (n, f) in enumerate(records) if n == name] for name in names]
    require(all(len(g) == 180 for g in groups), "180 frames")
    last = -1
    for frame in range(180):
        indices = [g[frame][0] for g in groups]
        require(last < indices[0] and indices == sorted(set(indices)), "frame order")
        last = indices[-1]
        clear, submit, done, scan, texture, video, present, end = [g[frame][1] for g in groups]
        require(clear.get("serial") == submit.get("serial") == done.get("serial") == str(frame+1)
                and submit.get("rc") == "0", "completion")
        require(clear.get("bgra") == "ff000000" and clear.get("bytes") == "8912896", "clear")
        require(scan.get("valid") == "1" and scan.get("bad_alpha") == scan.get("bad_sum") == "0"
                and 1000 < int(scan["changed_words"]) < int(scan["total_words"]), "readback")
        require(texture.get("frame") == str(frame) and texture.get("unexpected") == "0"
                and all(int(texture[c]) > 0 for c in ("red", "green", "blue"))
                and sum(int(texture[c]) for c in ("red", "green", "blue")) == int(scan["changed_words"]), "texture")
        require(video.get("token") == present.get("token") == str(frame+1) and video.get("rc") == "0"
                and present.get("fence") == "0" and present.get("matching_event") == "1", "present")
        require(end.get("frame") == str(frame) and end.get("slot") == end.get("displayed") == str(frame % 2), "reuse")
    require(any(i > last and n == "PS5VK_PLATFORM_CLOSE" and f.get("rc") == "0"
                and f.get("allocations_bytes") == "0" for i, (n, f) in enumerate(records)), "cleanup")
    return dict(frames=180, aggregate_readback=True, process_exit_verified=False,
                spatial_correctness_verified=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("log", type=Path)
    p.add_argument("metadata", type=Path)
    p.add_argument("artifact", type=Path)
    a = p.parse_args()
    print(json.dumps(validate(a.log.read_bytes(), json.loads(a.metadata.read_text()),
                              json.loads(a.artifact.read_text()))))


if __name__ == "__main__":
    main()
