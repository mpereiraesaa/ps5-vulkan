#!/usr/bin/env python3
"""Strict evidence for the native geometry-stage coverage witness.

The oracle that decides each case runs on the console (src/geometry_witness.c,
the same classifier the host regression drives), so this parser does not
re-derive pixels. It requires the exact case set, the coverage counts the
witness geometry can produce, the pipeline state a geometry case must have
carried (output topology, maximum vertices, subgroup/on-chip/ring registers) and
the digest relations only a real readback satisfies: passthrough must reproduce
the two-stage control image, and the shrunk, suppressed and rewritten images
must differ from it.

Artifact hashes must additionally be tied to a verified deployment by the run
operator. This parser does not prove which executable the OS launched, nor OS
process exit, and never promotes generic Vulkan conformance.
"""
import argparse
import hashlib
import json
from pathlib import Path

EXTENT = 64
PIXELS = EXTENT * EXTENT
# case, geometry mode (-1 is the control with no geometry stage), and the pixels
# the case's coverage predicate keeps
SHRINK_PIXELS = int(EXTENT * 0.6) ** 2
# In the order the probe logs them: the input-independent case runs second so a
# stage that never emits is separable from a broken input path.
CASES = (
    (0, -1, PIXELS),
    # The fixed centred quad covers half of each axis.
    (6, 5, (EXTENT // 2) ** 2),
    (1, 0, PIXELS),
    (2, 1, SHRINK_PIXELS),
    (3, 2, 0),
    (4, 3, PIXELS),
    # Three sub-triangles tile the input triangle, so the amplified image must
    # be the control image again.
    (5, 4, PIXELS),
    # The input positions with a constant colour.
    (7, 6, PIXELS),
)
# The amplified image must hash equal to the control, and the four structurally
# different images must all differ.
DIGEST_EQUAL = ((0, 1), (0, 5))
DIGEST_DISTINCT = (0, 2, 3, 4, 6, 7)
DIGEST_NAMES = {0: "digest_control", 1: "digest_passthrough", 2: "digest_shrink",
                3: "digest_suppress", 4: "digest_recolor", 5: "digest_amplify",
                6: "digest_constant", 7: "digest_positions"}
GEOMETRY_REGISTERS = ("1ff", "291", "2ab", "2ce", "2d3")


def require(ok, label):
    if not ok:
        raise ValueError(label)


def mark_records(log):
    """The console's MARK records as (sequence, name, fields), in order."""
    parsed = []
    for line in log.decode().splitlines():
        parts = line.split("\t")
        if len(parts) != 4 or parts[2] != "MARK":
            continue
        words = parts[3].split()
        if not words:
            continue
        fields = {}
        for word in words[1:]:
            if "=" in word:
                key, value = word.split("=", 1)
                fields[key] = value
        parsed.append((int(parts[0]), words[0], fields))
    return parsed


def matching(parsed, name):
    return [(sequence, fields) for sequence, record, fields in parsed if record == name]


def validate(log, receipt, artifact):
    digest = artifact.get("files", {}).get("eboot.bin", "")
    require(len(digest) == 64 and all(c in "0123456789abcdef" for c in digest),
            "artifact identity")
    require(artifact.get("title") == "PPSA99994" and
            artifact.get("stage") == "graphics-api-offscreen-draw" and
            artifact.get("compiler") == "runtime-psbc-aco" and
            artifact.get("target_gfx") == 1013 and
            artifact.get("graphics_shader_source") == "owned-runtime-geometry-stage" and
            artifact.get("geometry_fixture") == "geometry-stage-coverage" and
            artifact.get("geometry_probe") == 1 and
            artifact.get("geometry_extent") == EXTENT and
            artifact.get("geometry_cases") == len(CASES) and
            artifact.get("sample_count") == 1 and
            artifact.get("scene") is None and
            artifact.get("termination") == "shell-close-after-cleanup" and
            artifact.get("submit_enabled") is True,
            "artifact profile")
    require(hashlib.sha256(log).hexdigest() == receipt.get("sha256"), "log hash")
    require(receipt.get("clean") is True and receipt.get("bye") is True and
            receipt.get("gaps") == [] and receipt.get("raw_lines") == 0 and
            receipt.get("transport") == "tcp" and receipt.get("protocol") == "ps5log/1",
            "complete TCP receipt")
    lines = log.decode().splitlines()
    require(len(lines) > 2 and lines[0].startswith("HELLO ps5log/1 "), "hello")
    hello = dict(word.split("=", 1) for word in lines[0].split()[2:])
    require(hello.get("title") == "PPSA99994" and hello.get("app") == "ps5vk", "hello identity")
    require(lines[-1].startswith("BYE "), "bye line")
    parsed = mark_records(log)
    draws = matching(parsed, "PS5VK_GEOMETRY_DRAW")
    cases = matching(parsed, "PS5VK_GEOMETRY_CASE")
    summaries = matching(parsed, "PS5VK_GEOMETRY_PROBE")
    cleanup = matching(parsed, "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    require(len(draws) == len(cases) == len(CASES) and len(summaries) == 1 and
            len(cleanup) == 1, "witness record counts")
    digests = {}
    for index, (case, mode, expected) in enumerate(CASES):
        draw_sequence, draw = draws[index]
        case_sequence, fields = cases[index]
        require(draw.get("case") == str(case) and fields.get("case") == str(case),
                f"case {case} identity")
        require(draw.get("mode") == str(mode) and fields.get("mode") == str(mode),
                f"case {case} mode")
        require(draw.get("vertices") == "6" and draw.get("instances") == "1",
                f"case {case} draw")
        if mode < 0:
            require(draw.get("stages") == "2", f"case {case} control stages")
        else:
            require(draw.get("stages") == "3" and
                    draw.get("out_prim_type") == "2" and
                    draw.get("max_vertices") == "9", f"case {case} geometry state")
        require(fields.get("pixels") == str(PIXELS) and
                fields.get("expected") == str(expected) and
                fields.get("covered") == str(expected) and
                fields.get("missing") == "0" and fields.get("foreign") == "0" and
                fields.get("wrong_color") == "0" and fields.get("verified") == "1",
                f"case {case} coverage")
        require(draw_sequence < case_sequence, f"case {case} ordering")
        require(case_sequence < draws[index + 1][0] if index + 1 < len(draws)
                else case_sequence < summaries[0][0], f"case {case} sequence")
        digests[case] = fields.get("digest", "")
        require(len(digests[case]) == 16 and
                all(c in "0123456789abcdef" for c in digests[case]),
                f"case {case} digest")
    require(cases[-1][0] < summaries[0][0] < cleanup[0][0], "summary ordering")
    for left, right in DIGEST_EQUAL:
        require(digests[left] == digests[right], f"digest equality {left}/{right}")
    for index, left in enumerate(DIGEST_DISTINCT):
        for right in DIGEST_DISTINCT[:index]:
            require(digests[left] != digests[right], f"digest collision {left}/{right}")
    summary = summaries[0][1]
    require(summary.get("cases") == str(len(CASES)) and
            summary.get("extent") == str(EXTENT) and
            summary.get("clear") == "000000ff" and
            summary.get("out_prim_type") == "2" and
            summary.get("max_vertices") == "9" and
            summary.get("strict_verified") == "1", "summary")
    for case, name in DIGEST_NAMES.items():
        require(summary.get(name) == digests[case], f"summary digest {name}")
    return {"self_sha256": digest, "cases": len(CASES), "extent": EXTENT,
            "gpu_readback": True, "strict_verified": True,
            "process_exit_verified": False, "deployment_identity_verified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                              json.loads(args.artifact.read_text())), indent=2))
