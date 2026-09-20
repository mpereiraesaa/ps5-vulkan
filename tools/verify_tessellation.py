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
    10: ("J-patch32-barrier", 32, 1378),
    11: ("K-expand3-to32", 3, 1378),
    12: ("L-unused-vs-output", 3, 1378),
    13: ("M-quad-points", 3, 9),
    14: ("N-stage-specialization", 3, 9),
    15: ("O-cache-reuse", 3, 9),
    16: ("P-domain-geometry", 3, 9),
    17: ("Q-blend-overlap", 3, 9),
    18: ("R-dense-color", 3, 2916),
    19: ("S-dense-blend", 3, 2916),
    20: ("T-source-blend", 3, 2916),
    21: ("U-constant-blend", 3, 2916),
    22: ("V-indexed-delivery", 3, 1378),
    23: ("W-indexed32-delivery", 3, 1378),
    24: ("X-instance-delivery", 3, 956),
    25: ("Y-indirect-instance", 3, 956),
    26: ("Z-component-envelope", 3, 1378),
    27: ("AA-patch-envelope", 3, 1378),
    28: ("AB-joint-envelopes", 3, 1378),
    29: ("AC-level64-points", 3, 65),
    30: ("AD-total-components", 3, 1378),
    31: ("AE-cull-only", 3, 1378),
    32: ("AF-mixed-distance", 3, 1378),
    33: ("AG-dynamic-distance", 3, 1378),
    34: ("AH-evaluation-outputs", 3, 1378),
    35: ("AI-point-matrix", 3, 7),
    36: ("AI-point-matrix", 3, 7),
    37: ("AI-point-matrix", 3, 12),
    38: ("AI-point-matrix", 3, 9),
    39: ("AI-point-matrix", 3, 9),
    40: ("AI-point-matrix", 3, 16),
    41: ("AI-point-matrix", 3, 6),
    42: ("AI-point-matrix", 3, 6),
    43: ("AI-point-matrix", 3, 12),
    44: ("AJ-discard-matrix", 51, 8),
    45: ("AJ-discard-matrix", 51, 8),
    46: ("AJ-discard-matrix", 51, 8),
    47: ("AJ-discard-matrix", 51, 5),
    48: ("AJ-discard-matrix", 51, 5),
    49: ("AJ-discard-matrix", 51, 5),
    50: ("AJ-discard-matrix", 51, 11),
    51: ("AJ-discard-matrix", 51, 11),
    52: ("AJ-discard-matrix", 51, 11),
    53: ("AK-swapped-modes", 3, 1378),
    54: ("AL-indexed-instance", 3, 956),
    55: ("AM-indexed-indirect-instance", 3, 956),
    56: ("AN-push-member", 3, 1378),
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
    if config["variant"] in (11, 12, 26, 28):
        launch = re.fullmatch(r"([0-9a-f]{8})@2d6", identity.get("ls_hs_config", ""))
        require(launch is not None, "asymmetric launch register missing")
        value = int(launch.group(1), 16)
        output_vertices = 31 if config["variant"] in (26, 28) else 32
        require((value >> 8) & 63 == 3 and (value >> 14) & 63 == output_vertices and
                value & 255 > 0, "asymmetric launch counts")
    bound_seq, bound = one("PS5VK_TESS_QUEUE_RING_BOUND")
    if config["variant"] == 56:
        seq, member = one("PS5VK_TESS_PUSH_MEMBER")
        require(seq < bound_seq and member.get("vertex") == "0:16" and
                member.get("control") == "16:68" and member.get("selected") == "1",
                "tessellation push member contract")
    if config["variant"] in (54, 55):
        seq, indexed = one("PS5VK_TESS_INDEXED_INSTANCE")
        require(seq < bound_seq and all(indexed.get(k) == v for k,v in {
            "indirect":str(int(config["variant"] == 55)), "binding_offset":"4",
            "first_index":"1", "vertex_offset":"-65536", "instances":"2",
            "first_instance":"3"
        }.items()), "tessellation indexed instance contract")
    if config["variant"] == 25:
        seq, indirect = one("PS5VK_TESS_INDIRECT")
        require(seq < bound_seq and all(indirect.get(k) == v for k,v in {
            "offset":"16", "count":"1", "vertices":"3", "instances":"2",
            "first_vertex":"0", "first_instance":"3", "resolver":"host_submit"
        }.items()), "tessellation indirect contract")
    if config["variant"] == 24:
        seq, instance = one("PS5VK_TESS_INSTANCE")
        require(seq < bound_seq and instance.get("count") == "2" and
                instance.get("first") == "3" and instance.get("expected_ids") == "3,4",
                "tessellation instance contract")
    if config["variant"] in (22, 23):
        seq, indexed = one("PS5VK_TESS_INDEXED")
        wide = config["variant"] == 23
        require(seq < bound_seq and indexed.get("type") == ("uint32" if wide else "uint16") and
                indexed.get("binding_offset") == ("4" if wide else "2") and indexed.get("first_index") == "1" and
                indexed.get("vertex_offset") == ("-65536" if wide else "1") and
                indexed.get("indices") == ("65541,65538,65543" if wide else "4,1,6"),
                "tessellation indexed contract")
    if config["variant"] in (17, 19, 20, 21):
        blend_seq, blend_fields = one("PS5VK_TESS_BLEND")
        require(blend_seq < bound_seq and blend_fields.get("draws") == "3" and
                blend_fields.get("alpha") == "0.25"
                and blend_fields.get("src") == ("CONSTANT_ALPHA" if config["variant"]==21 else "SRC_ALPHA")
                and blend_fields.get("dst") == ("ZERO" if config["variant"] in (20,21) else "ONE")
                and blend_fields.get("op") == "ADD",
                "tessellation blending contract")
    if config["variant"] == 16:
        geometry_seq, geometry_fields = one("PS5VK_TESS_GEOMETRY")
        require(geometry_seq < bound_seq and geometry_fields.get("stages") == "5" and
                geometry_fields.get("shift_x") == "4" and geometry_fields.get("shift_y") == "-4"
                and geometry_fields.get("color") == "gbr", "tessellation geometry contract")
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
    shared_count = config.get("shared_pipelines", 1)
    require(shared_count in (1, 2), "shared pipeline profile")
    if shared_count == 2:
        shared_seq, shared = one("PS5VK_TESS_SHARED_STORAGE")
        prepared_seq, prepared = job("PS5VK_GRAPHICS_PREPARED")
        require(shared.get("pipelines") == "2" and shared.get("same_storage") == "1"
                and shared.get("split_scissors") == "1"
                and prepared.get("draws") == ("6" if config["variant"] in (17,19,20,21) else "2") and
                shared_seq < prepared_seq < bound_seq, "shared pipeline execution")
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
    if config["variant"] == 18:
        _, alpha = one("PS5VK_TESS_ALPHA")
        require(alpha.get("expected") == "64" and alpha.get("tolerance") == "3"
                and alpha.get("samples") == str(expected) and alpha.get("wrong") == "0"
                and alpha.get("min", "").isdecimal() and alpha.get("max", "").isdecimal()
                and 61 <= int(alpha["min"]) <= int(alpha["max"]) <= 67,
                "tessellation alpha oracle")
    require(ink.isdecimal() and expected <= int(ink) <= 4096 and
            re.fullmatch(r"[0-9a-f]{16}", pixels.get("digest", "")), "pixel readback")
    if config["variant"] in (13, 14, 15, 16, 17, 29):
        require(int(ink) == expected, "point-mode exact coverage")
    if config["variant"] in (14, 15):
        require(shared_count == 2, "specialization needs both pipeline maps")
    if config["variant"] == 15:
        cache_seq, cache = one("PS5VK_TESS_CACHE_REUSE")
        require(cache.get("hits_delta") == "1" and
                cache.get("compiles_delta") == "0" and
                cache.get("original_destroyed") == "1" and
                cache_seq < shared_seq < bound_seq,
                "tessellation cache reuse")
    close_seq, close = one("PS5VK_PLATFORM_CLOSE")
    cleanup_seq, _ = one("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    require(close.get("rc") == "0" and close.get("allocations_bytes") == "0",
            "allocation cleanup")
    require(identity_seq < bound_seq < submit_seq < restored_seq < complete_seq <
            pixel_seq < close_seq < cleanup_seq, "tessellation ordering")
    result.update(tessellation_variant=name, tessellation_verified=True,
                  ring_restored=True, tessellation_build_id=build,
                  tessellation_digest=pixels["digest"])
    result["shared_pipelines_verified"] = shared_count == 2
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("log", "receipt", "artifact"):
        parser.add_argument(argument, type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                              json.loads(args.artifact.read_text())), indent=2))
