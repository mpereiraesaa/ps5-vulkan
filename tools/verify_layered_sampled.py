#!/usr/bin/env python3
"""Strictly verify one layered sampled-image hardware run."""
import argparse
import hashlib
import json
from pathlib import Path


TARGETS = {
    "2d-array": {"fixture": "sampled-image-array", "dimension": "16384",
                 "depth": "1", "layers": "256", "slices": "3"},
    "cube": {"fixture": "sampled-image-cube", "dimension": "4096",
             "depth": "1", "layers": "6", "slices": "6"},
    "3d": {"fixture": "sampled-image-3d", "dimension": "512",
           "depth": "512", "layers": "1", "slices": "3"},
}


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(log, metadata, artifact, target):
    expected = TARGETS[target]
    identity = artifact.get("files", {}).get("eboot.bin", "")
    require(len(identity) == 64 and all(c in "0123456789abcdef" for c in identity),
            "artifact identity")
    require(artifact.get("stage") == "graphics-api-native-presentation-reuse" and
            artifact.get("scissor_probe") == 11 and
            artifact.get("image_target") == target and
            artifact.get("geometry_fixture") == expected["fixture"] and
            artifact.get("termination") == "return-main", "artifact profile")
    require(hashlib.sha256(log).hexdigest() == metadata.get("sha256"), "log hash")
    require(metadata.get("clean") is True and metadata.get("bye") is True and
            metadata.get("gaps") == [] and metadata.get("transport") == "tcp" and
            metadata.get("protocol") == "ps5log/1", "transport")
    lines = log.decode().splitlines()
    require(lines and lines[0].startswith("HELLO ps5log/1 "), "hello")
    hello = dict(word.split("=", 1) for word in lines[0].split()[2:])
    require(hello.get("title") == "PPSA99994" and hello.get("app") == "ps5vk" and
            all(metadata.get("identity", {}).get(key) == hello.get(key)
                for key in ("title", "app", "boot")), "runtime identity")
    records = []
    previous = -1
    for sequence, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4, "record shape")
        seq, stamp, level, message = fields
        require(int(seq) == sequence and int(stamp) >= previous and level != "ERR",
                "record integrity")
        previous = int(stamp)
        words = message.split()
        records.append((words[0], dict(word.split("=", 1) for word in words[1:])))
    require(metadata.get("records") == len(records) and
            lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end", "bye/count")

    def one(name):
        found = [(index, fields) for index, (record, fields) in enumerate(records)
                 if record == name]
        require(len(found) == 1, f"exactly one {name}")
        return found[0]

    query = one("PS5VK_LAYERED_QUERY")
    source = one("PS5VK_LAYERED_INPUT")
    submit = one("PS5VK_GRAPHICS_SUBMIT")
    complete = one("PS5VK_GRAPHICS_COMPLETED")
    readback = one("PS5VK_LAYERED_READBACK")
    present = one("PS5VK_VIDEO_PRESENTED")
    end = one("PS5VK_GRAPHICS_REUSE_END")
    cleanup = one("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    platform = one("PS5VK_PLATFORM_CLOSE")
    require(query[1] == {"target": target, "dimension": expected["dimension"],
                         "depth": expected["depth"], "layers": expected["layers"]},
            "image query")
    require(source[1] == {"target": target, "slices": expected["slices"],
                          "width": "64", "height": "64"}, "upload fixture")
    require(readback[1].get("target") == target and
            all(int(readback[1].get(color, "0")) > 0 for color in ("red", "green", "blue")) and
            readback[1].get("unexpected") == "0" and readback[1].get("valid") == "1",
            "GPU layered readback")
    require(submit[1].get("rc") == "0" and complete[1].get("image_bytes") and
            present[1].get("matching_event") == "1" and end[1].get("displayed") == "0",
            "submit/presentation")
    compute_ends = [index for index, (name, fields) in enumerate(records)
                    if name == "PS5VK_COMPUTE_END" and fields.get("rounds") == "6" and
                    fields.get("dispatches") == "12"]
    require(len(compute_ends) == 2 and
            query[0] < compute_ends[0] < source[0] < submit[0] < complete[0] < readback[0] <
            present[0] < end[0] < compute_ends[1] < platform[0] < cleanup[0], "record order")
    require(platform[1].get("rc") == "0" and
            platform[1].get("allocations_bytes") == "0", "resource cleanup")
    return {"target": target, "self_sha256": identity, "gpu_readback": True,
            "process_exit_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=TARGETS)
    parser.add_argument("log", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(), json.loads(args.metadata.read_text()),
                              json.loads(args.artifact.read_text()), args.target), indent=2))


if __name__ == "__main__":
    main()
