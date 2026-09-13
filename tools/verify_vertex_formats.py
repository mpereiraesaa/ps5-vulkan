#!/usr/bin/env python3
"""Verify bounded GPU fetch, conversion and component order for vertex formats."""
import argparse
import hashlib
import json
from pathlib import Path


CASES = (
    ("r32-sint", "99", "sint", "1", "4", "ffffffff"),
    ("rg32-sint", "102", "sint", "2", "8", "ffffffff"),
    ("rgb32-sint", "105", "sint", "3", "12", "ffffffff"),
    ("rgba32-sint", "108", "sint", "4", "16", "ffffffff"),
    ("r32-uint", "98", "uint", "1", "4", "ffffffff"),
    ("rg32-uint", "101", "uint", "2", "8", "ffffffff"),
    ("rgb32-uint", "104", "uint", "3", "12", "ffffffff"),
    ("rgba32-uint", "107", "uint", "4", "16", "ffffffff"),
    ("rgba8-unorm", "37", "unorm", "4", "4", "ffaa5511"),
    ("bgra8-unorm", "44", "unorm", "4", "4", "ffaa5511"),
)


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(log, metadata, artifact):
    identity = artifact.get("files", {}).get("eboot.bin", "")
    require(len(identity) == 64 and all(c in "0123456789abcdef" for c in identity),
            "artifact identity")
    require(artifact.get("stage") == "graphics-api-native-presentation-reuse" and
            artifact.get("scissor_probe") == 8 and
            artifact.get("geometry_fixture") == "vertex-format-cases" and
            artifact.get("compiler") == "runtime-psbc-aco" and
            artifact.get("graphics_shader_source") == "owned-runtime-vertex-formats" and
            set(artifact.get("runtime_graphics_inputs", {})) ==
            {"vertex_sint", "vertex_uint", "vertex_unorm", "fragment"} and
            artifact.get("termination") == "shell-close-after-cleanup", "artifact profile")
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

    inputs = matching("PS5VK_VERTEX_FORMAT_INPUT")
    results = matching("PS5VK_VERTEX_FORMAT_READBACK")
    compute_results = matching("PS5VK_COMPUTE_RESULT")
    compute_ends = matching("PS5VK_COMPUTE_END")
    submits = matching("PS5VK_GRAPHICS_SUBMIT")
    completes = matching("PS5VK_GRAPHICS_COMPLETED")
    presents = matching("PS5VK_VIDEO_PRESENTED")
    ends = matching("PS5VK_GRAPHICS_REUSE_END")
    require(all(len(group) == len(CASES) for group in
                (inputs, results, submits, completes, presents, ends)), "all GPU cases")
    require(len(compute_results) == 12 * len(CASES) and
            len(compute_ends) == 2 * len(CASES) and
            all(fields.get("rounds") == "6" and fields.get("dispatches") == "12"
                for _, fields in compute_ends), "compute regression")
    last = -1
    for case, (name, format_number, numeric, components, stride, word) in enumerate(CASES):
        ordered = [group[case][0] for group in
                   (inputs, submits, completes, results, presents, ends)]
        pre_compute = compute_ends[2 * case][0]
        post_compute = compute_ends[2 * case + 1][0]
        require(last < pre_compute < ordered[0] and
                ordered == sorted(set(ordered)) and ordered[-1] < post_compute,
                "case order")
        last = post_compute
        source, result = inputs[case][1], results[case][1]
        require(source.get("case") == result.get("case") == str(case) and
                source.get("name") == result.get("name") == name and
                source.get("format") == result.get("format") == format_number and
                source.get("numeric") == result.get("numeric") == numeric and
                source.get("components") == result.get("components") == components and
                source.get("stride") == stride and source.get("word") == word,
                "case identity")
        require(result.get("expected_white") == "471744" and
                result.get("other") == "0" and result.get("first_other") == "00000000" and
                result.get("valid") == "1" and submits[case][1].get("rc") == "0",
                "GPU vertex-format oracle")
    require(any(index > last and name == "PS5VK_PLATFORM_CLOSE" and
                fields.get("rc") == "0" and fields.get("allocations_bytes") == "0"
                for index, (name, fields) in enumerate(records)), "resource cleanup")
    require(any(name == "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE" for name, _ in records),
            "API cleanup")
    return {"self_sha256": identity, "cases": len(CASES),
            "formats": [case[0] for case in CASES], "gpu_readback": True,
            "component_completion": True, "normalized_channel_order": True,
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
