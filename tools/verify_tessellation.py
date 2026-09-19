"""Strict diagnostic tessellation evidence, not feature/conformance promotion.

Reuses bundled geometry controls and transport receipt validation. Deployment
and OS exit still need independent evidence. Old manifests without tessellation
candidate metadata are intentionally not certified.
"""
import argparse
import json
import re
from pathlib import Path

from verify_geometry import mark_records, matching, require, validate as geometry

# Fixed fixture expectations, not counts accepted from the log.
CASES = {
    1: ("A-tesscoord-nonzero", 3, 1378),
    5: ("E-offchip-linear", 3, 1378),
    6: ("F-two-patch-linear", 6, 956),
    7: ("G-patch-data-linear", 6, 956),
    8: ("H-quad-linear", 3, 2916),
    9: ("I-isoline-linear", 3, 90),
}


def validate(log, receipt, artifact):
    result = geometry(log, receipt, artifact)
    config = artifact.get("tessellation_witness", {})
    require(config.get("variant") in CASES and config.get("ring_mode") == 4 and
            config.get("no_draw") == 0, "tessellation artifact profile")
    build = config.get("build_id", "")
    require(isinstance(build, str) and re.fullmatch(r"[0-9a-f]{16}", build),
            "tessellation build identity")
    name, vertices, expected = CASES[config["variant"]]
    lines = log.decode().splitlines()
    for sequence, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t")
        require(len(fields) == 4 and fields[0] == str(sequence), "stream sequence")
        require(fields[2] not in ("ERR", "ERROR", "FATAL"), "runtime error")
    require(lines[-1] == f"BYE seq={len(lines)-2} reason=graphics-api-end",
            "successful termination")
    parsed = mark_records(log)
    require(not matching(parsed, "PS5VK_TESS_STALL"), "tessellation timeout")

    def one(marker):
        records = matching(parsed, marker)
        require(len(records) == 1, marker + " count")
        return records[0]

    identity_seq, identity = one("PS5VK_TESS_RECEIPT")
    require(identity.get("build") == build and identity.get("variant") == name and
            identity.get("vertices") == str(vertices) and identity.get("no_draw") == "0"
            and identity.get("tessellation") == "1", "tessellation runtime identity")
    bound_seq, bound = one("PS5VK_TESS_QUEUE_RING_BOUND")
    restored_seq, restored = one("PS5VK_TESS_QUEUE_RING_RESTORED")
    serial = bound.get("serial", "")
    require(serial.isdecimal() and int(serial) > 0 and bound.get("rc") == "0" and
            bound.get("state") == "1" and restored.get("serial") == serial and
            restored.get("rc") == "0" and restored.get("state") == "0",
            "ring lifecycle")

    def job(marker):
        records = [(seq, f) for seq, f in matching(parsed, marker)
                   if f.get("serial") == serial]
        require(len(records) == 1, marker + " serial")
        return records[0]

    submit_seq, submit = job("PS5VK_GRAPHICS_SUBMIT")
    complete_seq, _ = job("PS5VK_GRAPHICS_COMPLETED")
    require(submit.get("rc") == "0", "tessellation submission")
    pixel_seq, pixels = one("PS5VK_TESS_CONTROL")
    require(pixels.get("variant") == name and pixels.get("rc") == "0" and
            pixels.get("created") == "1" and pixels.get("vertices") == str(vertices),
            "tessellation pixel identity")
    require(pixels.get("expected") == str(expected) and
            pixels.get("covered") == str(expected) and
            all(pixels.get(k) == "0" for k in ("missing", "foreign", "wrong_color"))
            and pixels.get("verified") == "1", "tessellation pixel oracle")
    ink = pixels.get("ink", "")
    require(ink.isdecimal() and expected <= int(ink) <= 4096 and
            re.fullmatch(r"[0-9a-f]{16}", pixels.get("digest", "")), "pixel readback")
    close_seq, close = one("PS5VK_PLATFORM_CLOSE")
    cleanup_seq, _ = one("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    require(close.get("rc") == "0" and close.get("allocations_bytes") == "0",
            "allocation cleanup")
    require(identity_seq < bound_seq < submit_seq < restored_seq < complete_seq <
            pixel_seq < close_seq < cleanup_seq, "tessellation ordering")
    result.update(tessellation_variant=name, tessellation_verified=True,
                  ring_restored=True, tessellation_build_id=build,
                  tessellation_digest=pixels["digest"])
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("log", "receipt", "artifact"):
        parser.add_argument(argument, type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                              json.loads(args.artifact.read_text())), indent=2))
