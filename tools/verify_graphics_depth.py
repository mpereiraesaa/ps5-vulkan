"""Audit the scoped 18-frame depth control, not complete graphics profile acceptance.

SELF identity comes from the locally retained deployment manifest, not TCP
attestation. Screenshots remain separate visual evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path

REFERENCE_SELF = "5abca5ad97ad317fff56bedaeac27ff36cf77afdfb3a170ce1dcf6c3fb5dba13"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(log, metadata, artifact):
    require(artifact.get("files", {}).get("eboot.bin") == REFERENCE_SELF, "artifact")
    require(hashlib.sha256(log).hexdigest() == metadata.get("sha256"), "hash")
    require(metadata.get("clean") is True and metadata.get("bye") is True and
            metadata.get("gaps") == [] and metadata.get("transport") == "tcp" and
            metadata.get("protocol") == "ps5log/1", "transport")
    identity = metadata.get("identity", {})
    require(identity.get("title") == "PPSA99994" and identity.get("app") == "ps5vk", "identity")
    lines = log.decode().splitlines()
    require(lines[0].startswith("HELLO ps5log/1 "), "hello")
    hello = dict(x.split("=", 1) for x in lines[0].split()[2:])
    require(all(hello.get(k) == identity.get(k) for k in ("title", "app", "boot")), "hello identity")
    records = []
    previous = -1
    for seq, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4, "record shape")
        n, stamp, level, message = fields
        require(int(n) == seq and int(stamp) >= previous and level != "ERR", "sequence/time/error")
        previous = int(stamp)
        words = message.split()
        records.append((words[0], dict(w.split("=", 1) for w in words[1:])))
    require(metadata.get("records") == len(records), "record count")
    require(lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end", "bye")

    def rows(name):
        return [(i, fields) for i, (kind, fields) in enumerate(records) if kind == name]

    names = ["PS5VK_DEPTH_INPUT", "PS5VK_GRAPHICS_SUBMIT", "PS5VK_GRAPHICS_COMPLETED",
             "PS5VK_GRAPHICS_API_READBACK", "PS5VK_TEXTURE_READBACK", "PS5VK_TEXTURE_COMPONENT_ORDER",
             "PS5VK_DEPTH_OCCLUSION", "PS5VK_VIDEO_SUBMIT", "PS5VK_VIDEO_PRESENTED", "PS5VK_GRAPHICS_REUSE_END"]
    groups = [rows(name) for name in names]
    require(all(len(group) == 18 for group in groups), "event counts")
    for index in range(18):
        positions = [g[index][0] for g in groups]
        require(positions == sorted(positions), "frame ordering")
        if index:
            require(groups[0][index][0] > groups[-1][index-1][0], "frame retirement")
        inp, submit, done, scan, texture, component, depth, video, presented, end = [g[index][1] for g in groups]
        frame, iteration = index % 6, index // 6
        enabled = int(iteration != 1)
        far = int(not enabled and frame % 2 == 0)
        require(inp == dict(frame=str(frame), near_z="0.4", far_z="0.8", near_first=str(int(frame % 2 == 0)), enabled=str(enabled)), "depth input")
        require(submit.get("serial") == done.get("serial") == str(index+1) and submit.get("rc") == "0", "GPU completion")
        require(scan.get("valid") == "1" and scan.get("bad_alpha") == scan.get("bad_sum") == "0", "readback")
        counts = [int(texture[k]) for k in ("red", "green", "blue")]
        require(texture.get("frame") == str(frame) and texture.get("unexpected") == "0" and
                sum(counts) == int(scan["changed_words"]), "texture coverage")
        a, b, c = (counts[(frame+n) % 3] for n in range(3))
        require((a > 0 and b == c == 0) if far else (a > b > c > 0), "occlusion colors")
        require(component == dict(frame=str(frame), dominant=str(frame % 3), valid="1"), "components")
        require(depth == dict(frame=str(frame), enabled=str(enabled), far_visible=str(far), valid="1"), "occlusion")
        require(video.get("token") == presented.get("token") == str(frame+1) and video.get("rc") == "0" and
                presented.get("matching_event") == "1", "presentation")
        require(end.get("frame") == str(frame) and end.get("slot") == end.get("displayed") == str(frame % 2), "reuse")
    require(records[-2:] == [("PS5VK_PLATFORM_CLOSE", {"rc": "0", "allocations_bytes": "0"}),
                            ("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE", {})], "cleanup")
    return {"frames": 18, "depth_enabled": 12, "depth_disabled": 6, "far_visible": 3}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(), json.loads(args.metadata.read_text()),
                              json.loads(args.artifact.read_text())), sort_keys=True))
