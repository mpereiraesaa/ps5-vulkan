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
    ("rgba8-unorm", "37", "float", "4", "4", "ffaa5511"),
    ("bgra8-unorm", "44", "float", "4", "4", "ffaa5511"),
    ("a2b10g10r10-unorm", "64", "float", "4", "4", "bffaa955"),
    ("r8-unorm", "9", "float", "1", "1", "00000040"),
    ("r8-snorm", "10", "float", "1", "1", "00000040"),
    ("r8-uint", "13", "uint", "1", "1", "00000011"),
    ("r8-sint", "14", "sint", "1", "1", "000000f1"),
    ("rg8-unorm", "16", "float", "2", "2", "0000c040"),
    ("rg8-snorm", "17", "float", "2", "2", "0000c040"),
    ("rg8-uint", "20", "uint", "2", "2", "0000c911"),
    ("rg8-sint", "21", "sint", "2", "2", "000051f1"),
    ("rgba8-snorm", "38", "float", "4", "4", "e020c040"),
    ("rgba8-uint", "41", "uint", "4", "4", "f1aa5511"),
    ("rgba8-sint", "42", "sint", "4", "4", "44b322f1"),
    ("a8b8g8r8-unorm", "51", "float", "4", "4", "ffaa5511"),
    ("a8b8g8r8-snorm", "52", "float", "4", "4", "e020c040"),
    ("a8b8g8r8-uint", "55", "uint", "4", "4", "f1aa5511"),
    ("a8b8g8r8-sint", "56", "sint", "4", "4", "44b322f1"),
    ("r16-unorm", "70", "float", "1", "2", "00004000"),
    ("r16-snorm", "71", "float", "1", "2", "00004000"),
    ("r16-uint", "74", "uint", "1", "2", "00001234"),
    ("r16-sint", "75", "sint", "1", "2", "0000ff85"),
    ("r16-sfloat", "76", "float", "1", "2", "00003800"),
    ("rg16-unorm", "77", "float", "2", "4", "c0004000"),
    ("rg16-snorm", "78", "float", "2", "4", "c0004000"),
    ("rg16-uint", "81", "uint", "2", "4", "cdef1234"),
    ("rg16-sint", "82", "sint", "2", "4", "04d2ff85"),
    ("rg16-sfloat", "83", "float", "2", "4", "bc003800"),
    ("rgba16-unorm", "91", "float", "4", "8", "40001000"),
    ("rgba16-snorm", "92", "float", "4", "8", "c0004000"),
    ("rgba16-uint", "95", "uint", "4", "8", "cdef1234"),
    ("rgba16-sint", "96", "sint", "4", "8", "04d2ff85"),
    ("rgba16-sfloat", "97", "float", "4", "8", "bc003800"),
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
    bounces = matching("PS5VK_VERTEX_BOUNCE")
    results = matching("PS5VK_VERTEX_FORMAT_READBACK")
    compute_results = matching("PS5VK_COMPUTE_RESULT")
    compute_ends = matching("PS5VK_COMPUTE_END")
    submits = matching("PS5VK_GRAPHICS_SUBMIT")
    completes = matching("PS5VK_GRAPHICS_COMPLETED")
    presents = matching("PS5VK_VIDEO_PRESENTED")
    ends = matching("PS5VK_GRAPHICS_REUSE_END")
    require(all(len(group) == len(CASES) for group in
                (inputs, bounces, results, submits, completes, presents, ends)), "all GPU cases")
    require(len(compute_results) == 12 * len(CASES) and
            len(compute_ends) == 2 * len(CASES) and
            all(fields.get("rounds") == "6" and fields.get("dispatches") == "12"
                for _, fields in compute_ends), "compute regression")
    last = -1
    for case, (name, format_number, numeric, components, byte_count, word) in enumerate(CASES):
        ordered = [group[case][0] for group in
                   (inputs, bounces, submits, completes, results, presents, ends)]
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
                source.get("bytes") == source.get("stride") == byte_count and
                source.get("binding_offset") == "25" and source.get("word") == word,
                "case identity")
        require(bounces[case][1].get("bytes") == "359" and
                bounces[case][1].get("alignment") == "4", "vertex bounce")
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
            "byte_granular_vertex_input": True,
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
