#!/usr/bin/env python3
"""Strictly verify one DXVK262-T06 sample-rate witness run.

The verifier derives every expected value from the witness the build recorded in
its manifest and from the profile's own storage arithmetic. The payload's own
`verdict=` fields are data to check, never an oracle: a run whose reported
verdict disagrees with what the observed numbers imply is refused.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = Path("build/native-graphics-api/manifest.json")
EXPECTED_WITNESS = {
    "extent": [64, 64],
    "samples": 4,
    "clear_rgba": [0.25, 0.5, 0.75, 1.0],
    "clear_word": "ff4080bf",
    "shaded_values": ["ff000000", "ff010000", "ff020000", "ff030000"],
    "phases": ["clear", "shaded"],
    "strict_readback": True,
}
# The profile's single-sample colour footprint: the 64KB_R_X layout pads both
# dimensions to 128 texels, so a 64x64 BGRA8 surface is 128*128*4 bytes before
# the sample count applies (src/depth_layout.c, ps5vk_depth_layout).
def single_sample_bytes(extent: list[int]) -> int:
    pitch = (extent[0] + 127) // 128 * 128
    padded = (extent[1] + 127) // 128 * 128
    return pitch * padded * 4


def require(condition: object, message: str) -> None:
    if not condition:
        raise ValueError(message)


def parse(path: Path) -> list[tuple[int, str, str]]:
    """HELLO, then one ordered stream of seq/clock/level/text records, then BYE."""
    records: list[tuple[int, str, str]] = []
    if not path.is_file() and path.with_suffix(".log").is_file():
        path = path.with_suffix(".log")
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    require(lines and lines[0].startswith("HELLO ps5log/1 "), "missing HELLO")
    expect = 1
    for line in lines[1:]:
        if line.startswith("BYE "):
            records.append((-1, "BYE", line))
            continue
        parts = line.split("\t", 3)
        require(len(parts) == 4, f"malformed record {line!r}")
        seq, _clock, level, text = parts
        require(int(seq) == expect, f"sequence gap at {seq}")
        expect += 1
        records.append((int(seq), level, text))
    require(records and records[-1][1] == "BYE", "stream did not end with BYE")
    # The transport prints the closing sequence number before the reason:
    # "BYE seq=NN reason=graphics-api-end". The reason is what says the title
    # ran its own teardown instead of failing.
    require("reason=graphics-api-end" in records[-1][2], "unclean close")
    return records


def fields(text: str) -> dict[str, str]:
    return {word.split("=", 1)[0]: word.split("=", 1)[1]
            for word in text.split()[1:] if "=" in word}


def exactly(records, tag):
    found = [(seq, fields(text)) for seq, _level, text in records
             if text.split(" ", 1)[0] == tag]
    require(len(found) == 1, f"expected exactly one {tag}, found {len(found)}")
    return found[0]


def validate_artifact(manifest_path: Path, artifact_path: Path) -> str:
    manifest = json.loads(Path(manifest_path).read_text())
    require(manifest.get("stage") == "graphics-api-offscreen-draw", "manifest stage")
    require(manifest.get("submit_enabled") is True and
            manifest.get("runtime_graphics") is True, "manifest runtime draw")
    require(manifest.get("sample_rate_probe") == 1 and
            manifest.get("sample_rate_measurement") is True,
            "manifest measurement gate")
    require(manifest.get("graphics_shader_source") == "owned-runtime-sample-id",
            "manifest shader source")
    require(manifest.get("sample_rate_witness") == EXPECTED_WITNESS,
            "manifest witness")
    require(manifest.get("termination") == "shell-close-after-cleanup",
            "manifest termination")
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("files", {}).get("eboot.bin") == digest,
            "artifact does not match manifest")
    return digest


def int_of(row: dict, key: str) -> int:
    require(key in row, f"missing field {key}")
    try:
        return int(row[key], 0)
    except ValueError as error:
        raise ValueError(f"field {key} is not an integer: {row[key]!r}") from error


def hex_of(row: dict, key: str) -> int:
    """The register-shaped fields print as bare hexadecimal words."""
    require(key in row, f"missing field {key}")
    try:
        return int(row[key], 16)
    except ValueError as error:
        raise ValueError(f"field {key} is not a hex word: {row[key]!r}") from error


def validate(path: Path, manifest_path: Path = DEFAULT_MANIFEST,
             artifact_path: Path | None = None) -> dict:
    require(artifact_path is not None, "an artifact path is required")
    digest = validate_artifact(manifest_path, artifact_path)
    records = parse(path)
    exactly(records, "PS5VK_GRAPHICS_API_DEVICE_CREATED")
    exactly(records, "PS5VK_MULTISAMPLE_CLEAR_PREPARED")
    clear_seq, clear = exactly(records, "PS5VK_SAMPLE_RATE_CLEAR")
    shaded_seq, shaded = exactly(records, "PS5VK_SAMPLE_RATE_SHADED")
    close_seq, close = exactly(records, "PS5VK_PLATFORM_CLOSE")
    exactly(records, "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    require(clear_seq < shaded_seq < close_seq, "measurement ordering")

    extent = EXPECTED_WITNESS["extent"]
    samples = EXPECTED_WITNESS["samples"]
    span_bytes = single_sample_bytes(extent) * samples

    # Clear phase: the span the profile claims, one distinct word, every word.
    require(clear.get("extent") == f"{extent[0]}x{extent[1]}", "clear extent")
    require(int_of(clear, "samples") == samples, "clear sample count")
    require(int_of(clear, "bytes") == span_bytes,
            f"clear span {clear.get('bytes')} != {span_bytes}")
    words = int_of(clear, "words")
    require(words == span_bytes // 4, "clear word count")
    require(hex_of(clear, "expected") == int(EXPECTED_WITNESS["clear_word"], 16),
            "clear word")
    require(int_of(clear, "distinct") == 1, "clear left more than one value")
    require(int_of(clear, "correct") == words, "clear left uncovered words")
    require(hex_of(clear, "first") == hex_of(clear, "expected") and
            hex_of(clear, "last") == hex_of(clear, "expected"), "clear edges")
    require(int_of(clear, "verdict") == 1, "payload reported a failed clear")

    # Shaded phase: one distinct value per sample, all of them the values the
    # module writes for those sample indices, over a covered region.
    require(shaded.get("extent") == f"{extent[0]}x{extent[1]}", "shaded extent")
    require(int_of(shaded, "samples") == samples, "shaded sample count")
    require(int_of(shaded, "words") == words, "shaded word count")
    require(int_of(shaded, "shaded_values") == samples, "shaded value count")
    require(int_of(shaded, "expected_values") == samples, "expected value count")
    require(int_of(shaded, "matched") == samples, "shaded value set")
    require(int_of(shaded, "covered_words") > 0, "shaded coverage")
    observed = shaded.get("values", "").split(",")
    require(observed == EXPECTED_WITNESS["shaded_values"],
            f"shaded values {observed} != {EXPECTED_WITNESS['shaded_values']}")
    require(int_of(shaded, "verdict") == 1, "payload reported a failed shading")

    require(int_of(close, "rc") == 0 and int_of(close, "allocations_bytes") == 0,
            "close did not release everything")
    return {"ok": True, "artifact_sha256": digest, "clear_words": words,
            "span_bytes": span_bytes, "shaded_values": observed,
            "samples": samples}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = validate(args.log, args.manifest, args.artifact)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
