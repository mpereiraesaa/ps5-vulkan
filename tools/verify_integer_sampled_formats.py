#!/usr/bin/env python3
"""Verify typed integer sampled-image execution on GFX1013."""
import argparse
import hashlib
import json
from pathlib import Path

try:
    from tools.verify_graphics_regression import validate_regression
except ModuleNotFoundError:
    from verify_graphics_regression import validate_regression


CASES = {
    "uint": (
        ("r8-uint", "13", "1", "1", "ff330000"),
        ("rg8-uint", "20", "2", "2", "ff336600"),
        ("rgba8-uint", "41", "4", "4", "ff336699"),
        ("r16-uint", "74", "2", "1", "ff330000"),
        ("rg16-uint", "81", "4", "2", "ff336600"),
        ("rgba16-uint", "95", "8", "4", "ff336699"),
        ("r32-uint", "98", "4", "1", "ff330000"),
        ("rg32-uint", "101", "8", "2", "ff336600"),
        ("rgba32-uint", "107", "16", "4", "ff336699"),
        ("abgr8-uint-packed", "55", "4", "4", "ff336699"),
    ),
    "sint": (
        ("r8-sint", "14", "1", "1", "ff408080"),
        ("rg8-sint", "21", "2", "2", "ff40a080"),
        ("rgba8-sint", "42", "4", "4", "ff40a0e0"),
        ("r16-sint", "75", "2", "1", "ff408080"),
        ("rg16-sint", "82", "4", "2", "ff40a080"),
        ("rgba16-sint", "96", "8", "4", "ff40a0e0"),
        ("r32-sint", "99", "4", "1", "ff408080"),
        ("rg32-sint", "102", "8", "2", "ff40a080"),
        ("rgba32-sint", "108", "16", "4", "ff40a0e0"),
        ("abgr8-sint-packed", "56", "4", "4", "ff40a0e0"),
    ),
}
EXPECTED_PIXELS = 1920 * 1080 // 2


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(log, metadata, artifact):
    sign = artifact.get("integer_sampled_sign")
    require(sign in CASES, "artifact integer sign")
    cases = CASES[sign]
    identity = artifact.get("files", {}).get("eboot.bin", "")
    require(len(identity) == 64 and all(c in "0123456789abcdef" for c in identity),
            "artifact identity")
    require(artifact.get("stage") == "graphics-api-native-presentation-reuse" and
            artifact.get("scissor_probe") == 10 and
            artifact.get("geometry_fixture") == "integer-sampled-formats" and
            artifact.get("termination") == "shell-close-after-cleanup" and
            artifact.get("graphics", {}).get("source") ==
            f"experiments/graphics/scene3d-{sign}.pipe", "artifact profile")
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
        require(int(seq) == sequence and int(stamp) >= previous and level in ("INFO", "MARK"),
                "record integrity")
        previous = int(stamp)
        words = message.split()
        records.append((words[0], dict(word.split("=", 1) for word in words[1:])))
    require(metadata.get("records") == len(records) and
            lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end", "bye/count")
    validate_regression(records, len(cases))

    def matching(name):
        return [(index, fields) for index, (record, fields) in enumerate(records)
                if record == name]

    inputs = matching("PS5VK_INTEGER_SAMPLED_INPUT")
    results = matching("PS5VK_INTEGER_SAMPLED_READBACK")
    submits = matching("PS5VK_GRAPHICS_SUBMIT")
    completes = matching("PS5VK_GRAPHICS_COMPLETED")
    presents = matching("PS5VK_VIDEO_PRESENTED")
    ends = matching("PS5VK_GRAPHICS_REUSE_END")
    compute_results = matching("PS5VK_COMPUTE_RESULT")
    compute_ends = matching("PS5VK_COMPUTE_END")
    require(all(len(group) == len(cases) for group in
                (inputs, results, submits, completes, presents, ends)), "all integer cases")
    require(len(compute_results) == 12 * len(cases) and
            len(compute_ends) == 2 * len(cases) and
            all(fields.get("rounds") == "6" and fields.get("dispatches") == "12"
                for _, fields in compute_ends), "compute regression")
    last = -1
    for case, (name, number, texel_bytes, components, expected_bgra) in enumerate(cases):
        ordered = [group[case][0] for group in
                   (inputs, submits, completes, results, presents, ends)]
        pre_compute = compute_ends[2 * case][0]
        post_compute = compute_ends[2 * case + 1][0]
        require(last < pre_compute < ordered[0] and ordered == sorted(set(ordered)) and
                ordered[-1] < post_compute, "case order")
        last = post_compute
        source, result = inputs[case][1], results[case][1]
        expected = {"case": str(case), "name": name, "sign": sign,
                    "format": number, "expected_bgra": expected_bgra}
        require(all(source.get(key) == value and result.get(key) == value
                    for key, value in expected.items()), "case identity")
        require(source.get("bytes_per_texel") == texel_bytes and
                source.get("components") == components and
                result.get("expected") == str(EXPECTED_PIXELS) and
                result.get("other") == "0" and
                result.get("first_other") == "00000000" and result.get("valid") == "1" and
                submits[case][1].get("rc") == "0", "GPU integer sampling oracle")
    require(any(index > last and name == "PS5VK_PLATFORM_CLOSE" and
                fields.get("rc") == "0" and fields.get("allocations_bytes") == "0"
                for index, (name, fields) in enumerate(records)), "resource cleanup")
    require(any(name == "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE" for name, _ in records),
            "API cleanup")
    return {"self_sha256": identity, "sign": sign, "cases": len(cases),
            "typed_integer_sampling": True, "process_exit_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    try:
        result = validate(args.log.read_bytes(), json.loads(args.metadata.read_text()),
                          json.loads(args.artifact.read_text()))
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise SystemExit(f"integer sampled-format verification failed: {exc}")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
