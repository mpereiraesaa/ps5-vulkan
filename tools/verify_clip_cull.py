#!/usr/bin/env python3
"""Strict evidence for the native packed clip/cull distance coverage witness.

The oracle that decides the verdict runs on the console
(src/clip_cull_witness.c, the same classifier the host regression drives), so
this parser does not re-derive pixels. It does require the exact case set, the
exact coverage counts the witness geometry can produce, the linked state each
case must have carried, and the digest relations only a real readback satisfies:
two cases that must render identically have to hash identically, and the four
structurally different images have to hash differently.

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
# case, specialization mode (-1 is the control module with no distances), and
# the number of pixels the case's distance predicate keeps
CASES = (
    (0, -1, PIXELS),
    (1, 0, PIXELS),
    (2, 1, PIXELS // 2),
    (3, 2, PIXELS // 4),
    # A cull distance negative at one vertex is not a discard: the rule needs
    # one half-space negative for every vertex of the primitive.
    (4, 3, PIXELS),
    (5, 4, 0),
    (6, 5, PIXELS // 4),
    (7, 6, 0),
)
POS_FORMAT = {-1: "00000004", 0: "00000044"}
VS_OUT_CONFIG = {-1: "00000000", 0: "00000002"}
VS_OUT_CNTL = {-1: "00000000", 0: "01400f03"}
DIGEST_EQUAL = ((0, 1), (0, 4), (3, 6), (5, 7))
DIGEST_DISTINCT = (0, 2, 3, 5)
DIGEST_NAMES = {0: "digest_plain", 1: "digest_positive", 2: "digest_clip_half",
                3: "digest_clip_quadrant", 4: "digest_cull_half",
                5: "digest_cull_negative", 6: "digest_mixed",
                7: "digest_cull_index"}


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
            artifact.get("graphics_shader_source") == "owned-runtime-clip-cull-distances" and
            artifact.get("geometry_fixture") == "clip-cull-distance-coverage" and
            artifact.get("clip_cull_probe") == 1 and
            artifact.get("clip_cull_extent") == EXTENT and
            artifact.get("clip_cull_cases") == len(CASES) and
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
    draws = matching(parsed, "PS5VK_CLIP_CULL_DRAW")
    cases = matching(parsed, "PS5VK_CLIP_CULL_CASE")
    summaries = matching(parsed, "PS5VK_CLIP_CULL_PROBE")
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
        control = -1 if mode < 0 else 0
        require(draw.get("vs_out_config") == VS_OUT_CONFIG[control] and
                draw.get("pos_format") == POS_FORMAT[control] and
                draw.get("vs_out_cntl") == VS_OUT_CNTL[control],
                f"case {case} pre-raster state")
        require(fields.get("pixels") == str(PIXELS) and
                fields.get("expected") == str(expected) and
                fields.get("covered") == str(expected) and
                fields.get("missing") == "0" and fields.get("foreign") == "0" and
                fields.get("wrong_color") == "0" and fields.get("verified") == "1",
                f"case {case} coverage")
        require(draw_sequence < case_sequence, f"case {case} ordering")
        if index:
            require(cases[index - 1][0] < draw_sequence, f"case {case} draw ordering")
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
            summary.get("clip_mask") == "03" and summary.get("cull_mask") == "0c" and
            summary.get("control_mask") == "000000" and
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
