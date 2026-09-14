#!/usr/bin/env python3
"""Strictly verify the bounded GFX1013 explicit-mip hardware witness."""
import argparse
import hashlib
import json
from pathlib import Path


EXPECTED_STORAGE = {
    0: {"offset": "12288", "pitch": "256", "first": "ff0000ff"},
    1: {"offset": "4096", "pitch": "256", "first": "ff00ff00"},
    2: {"offset": "0", "pitch": "256", "first": "ffff0000"},
}
EXPECTED_DESCRIPTOR = {
    1: "c3800000", 2: "800fc00f", 3: "90020fac", 4: "00000000",
    5: "00400020", 6: "00000000", 7: "00000000", 8: "00000092",
    9: "00200000", 10: "04000000", 11: "00000000",
}
EXPECTED_HISTOGRAM = {
    -2: {"red": "186624", "green": "0", "blue": "0"},
    0: {"red": "103680", "green": "62208", "blue": "20736"},
    2: {"red": "0", "green": "0", "blue": "186624"},
}


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(log, metadata, artifact):
    identity = artifact.get("files", {}).get("eboot.bin", "")
    require(len(identity) == 64 and all(c in "0123456789abcdef" for c in identity),
            "artifact identity")
    require(artifact.get("stage") == "graphics-api-native-presentation-reuse" and
            artifact.get("runtime_graphics") is True and
            artifact.get("runtime_sdk") is True and
            artifact.get("scissor_probe") == 12 and
            artifact.get("geometry_fixture") == "sampled-image-mipmaps" and
            artifact.get("termination") == "shell-close-after-cleanup",
            "artifact profile")
    lod_bias = artifact.get("mip_lod_bias", 0)
    require(lod_bias in EXPECTED_HISTOGRAM, "mipmap LOD bias profile")
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

    def matching(name):
        return [(index, fields) for index, (record, fields) in enumerate(records)
                if record == name]

    def one(name):
        found = matching(name)
        require(len(found) == 1, f"exactly one {name}")
        return found[0]

    query = [item for item in matching("PS5VK_IMAGE_QUERY")
             if item[1].get("format") == "37" and item[1].get("usage") == "6"]
    require(len(query) == 1 and query[0][1].get("max_width") == "16384" and
            query[0][1].get("max_height") == "16384", "mipmap image query")
    runtime = one("PS5VK_RUNTIME_GRAPHICS_CACHE")
    source = one("PS5VK_MIPMAP_INPUT")
    upload = [item for item in matching("PS5VK_TEXTURE_UPLOAD")
              if item[1].get("pattern") == "mipmap-rgb"]
    require(len(upload) == 1, "mipmap upload")
    descriptors = matching("PS5VK_TEXTURE_DESCRIPTOR")
    if descriptors:
        require(len(descriptors) == 12 and
                [int(fields.get("word", "-1")) for _, fields in descriptors] == list(range(12)),
                "complete descriptor")
        values = {int(fields["word"]): fields.get("value", "") for _, fields in descriptors}
        expected_descriptor = dict(EXPECTED_DESCRIPTOR)
        expected_descriptor[10] = f"{0x04000000 | ((lod_bias * 256) & 0x3fff):08x}"
        require(int(values[0], 16) != 0 and (int(values[0], 16) & 0xff) == 0 and
                all(values.get(word) == value for word, value in expected_descriptor.items()),
                "mipmap descriptor")
    submit = one("PS5VK_GRAPHICS_SUBMIT")
    complete = one("PS5VK_GRAPHICS_COMPLETED")
    storage = matching("PS5VK_MIPMAP_STORAGE")
    require(len(storage) == 3 and
            {int(fields.get("level", "-1")): {key: fields.get(key) for key in
             ("offset", "pitch", "first")} for _, fields in storage} == EXPECTED_STORAGE,
            "mipmap backing storage")
    readback = one("PS5VK_MIPMAP_READBACK")
    present = one("PS5VK_VIDEO_PRESENTED")
    end = one("PS5VK_GRAPHICS_REUSE_END")
    platform = one("PS5VK_PLATFORM_CLOSE")
    cleanup = one("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    source_fields = dict(source[1])
    source_bias = int(source_fields.pop("lod_bias", "0"))
    require(source_bias == lod_bias and source_fields ==
            {"levels": "3", "view_base": "0", "width": "64", "height": "64",
             "colors": "red,green,blue", "bytes": "21504"},
            "mipmap source")
    require(upload[0][1].get("levels") == "3" and upload[0][1].get("format") == "37",
            "mipmap upload profile")
    require(int(readback[1].get("lod_bias", "0")) == lod_bias and
            readback[1].get("levels") == "3" and
            all(readback[1].get(key) == value for key, value in EXPECTED_HISTOGRAM[lod_bias].items()) and
            readback[1].get("unexpected") == "0" and readback[1].get("valid") == "1",
            "explicit mip readback")
    require(runtime[1].get("rc") == "0" and runtime[1].get("compiled_pairs") == "1" and
            submit[1].get("rc") == "0" and complete[1].get("image_bytes") == "8912896" and
            present[1].get("matching_event") == "1" and end[1].get("displayed") == "0",
            "compile/submit/presentation")
    compute_ends = matching("PS5VK_COMPUTE_END")
    require(len(compute_ends) == 2 and all(fields.get("rounds") == "6" and
            fields.get("dispatches") == "12" for _, fields in compute_ends),
            "compute regression")
    descriptor_index=descriptors[0][0] if descriptors else submit[0]
    require(query[0][0] < compute_ends[0][0] < runtime[0] < source[0] < upload[0][0] <=
            descriptor_index <= submit[0] < complete[0] < storage[0][0] < readback[0] <
            present[0] < end[0] < compute_ends[1][0] < platform[0] < cleanup[0],
            "record order")
    require(platform[1].get("rc") == "0" and
            platform[1].get("allocations_bytes") == "0", "resource cleanup")
    return {"self_sha256": identity, "levels": 3, "explicit_lod": True,
            "mip_lod_bias": lod_bias, "histogram": EXPECTED_HISTOGRAM[lod_bias], "gpu_readback": True,
            "process_exit_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(), json.loads(args.metadata.read_text()),
                              json.loads(args.artifact.read_text())), indent=2))


if __name__ == "__main__":
    main()
