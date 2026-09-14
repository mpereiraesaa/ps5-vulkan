#!/usr/bin/env python3
"""Verify per-format nearest/linear discrimination on the GFX1013 sampler."""
import argparse
import hashlib
import json
from pathlib import Path

try:
    from tools.verify_sampled_formats import CASES as FORMAT_CASES
    from tools.verify_graphics_regression import validate_regression
except ModuleNotFoundError:
    from verify_sampled_formats import CASES as FORMAT_CASES
    from verify_graphics_regression import validate_regression


LINEAR = (
    "ff800000", "ff808000", "ff808080", "ff800000", "ff808000",
    "ff808080", "ff808080", "ff808080", "ff808080", "ff800000",
    "ff800000", "ff800000", "ff808000", "ff808000", "ff808000",
    "ff808080", "ff808080", "ff800000", "ff808000", "ff808080",
    "ff808080", "ff808080", "ff808080",
)
NEAREST = (
    "ffff0000", "ff000000", "ff000000", "ffff0000", "ff000000",
    "ff000000", "ff000000", "ff000000", "ff000000", "ff000000",
    "ff000000", "ff000000", "ff000000",
    "ff000000", "ff000000", "ff000000", "ff000000", "ff000000",
    "ff000000", "ff000000", "ff000000", "ff000000", "ff000000",
)
TRIALS = tuple(
    (case, name, number, texel_bytes, filtering,
     NEAREST[case] if filtering == "nearest" else LINEAR[case])
    for case, (name, number, texel_bytes, _) in enumerate(FORMAT_CASES)
    for filtering in ("nearest", "linear")
)


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(log, metadata, artifact):
    identity = artifact.get("files", {}).get("eboot.bin", "")
    require(len(identity) == 64 and all(c in "0123456789abcdef" for c in identity),
            "artifact identity")
    require(artifact.get("stage") == "graphics-api-native-presentation-reuse" and
            artifact.get("scissor_probe") == 9 and
            artifact.get("geometry_fixture") == "sampled-format-filtering" and
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
        require(int(seq) == sequence and int(stamp) >= previous and level in ("INFO", "MARK"),
                "record integrity")
        previous = int(stamp)
        words = message.split()
        records.append((words[0], dict(word.split("=", 1) for word in words[1:])))
    require(metadata.get("records") == len(records) and
            lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end", "bye/count")
    validate_regression(records, len(TRIALS))

    def matching(name):
        return [(index, fields) for index, (record, fields) in enumerate(records)
                if record == name]

    inputs = matching("PS5VK_SAMPLED_FILTER_INPUT")
    results = matching("PS5VK_SAMPLED_FILTER_READBACK")
    submits = matching("PS5VK_GRAPHICS_SUBMIT")
    completes = matching("PS5VK_GRAPHICS_COMPLETED")
    presents = matching("PS5VK_VIDEO_PRESENTED")
    ends = matching("PS5VK_GRAPHICS_REUSE_END")
    compute_results = matching("PS5VK_COMPUTE_RESULT")
    compute_ends = matching("PS5VK_COMPUTE_END")
    require(all(len(group) == len(TRIALS) for group in
                (inputs, results, submits, completes, presents, ends)), "all filter trials")
    require(len(compute_results) == 12 * len(TRIALS) and
            len(compute_ends) == 2 * len(TRIALS) and
            all(fields.get("rounds") == "6" and fields.get("dispatches") == "12"
                for _, fields in compute_ends), "compute regression")
    last = -1
    for trial, expected in enumerate(TRIALS):
        case, name, number, texel_bytes, filtering, expected_bgra = expected
        ordered = [group[trial][0] for group in
                   (inputs, submits, completes, results, presents, ends)]
        pre_compute = compute_ends[2 * trial][0]
        post_compute = compute_ends[2 * trial + 1][0]
        require(last < pre_compute < ordered[0] and ordered == sorted(set(ordered)) and
                ordered[-1] < post_compute, "trial order")
        last = post_compute
        source, result = inputs[trial][1], results[trial][1]
        required = {"trial": str(trial), "case": str(case), "name": name,
                    "format": number, "filter": filtering,
                    "expected_bgra": expected_bgra}
        require(all(source.get(key) == value and result.get(key) == value
                    for key, value in required.items()), "trial identity")
        require(source.get("bytes_per_texel") == texel_bytes and
                result.get("expected") == "373248" and result.get("other") == "0" and
                result.get("first_other") == "00000000" and result.get("valid") == "1" and
                submits[trial][1].get("rc") == "0", "GPU filtering oracle")
    require(all(TRIALS[2 * case][5] != TRIALS[2 * case + 1][5]
                for case in range(len(FORMAT_CASES))), "nearest/linear discriminator")
    require(any(index > last and name == "PS5VK_PLATFORM_CLOSE" and
                fields.get("rc") == "0" and fields.get("allocations_bytes") == "0"
                for index, (name, fields) in enumerate(records)), "resource cleanup")
    require(any(name == "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE" for name, _ in records),
            "API cleanup")
    return {"self_sha256": identity, "formats": len(FORMAT_CASES),
            "trials": len(TRIALS), "nearest_linear_discriminated": True,
            "process_exit_verified": False}


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
        raise SystemExit(f"sampled filtering verification failed: {exc}")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
