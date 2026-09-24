"""Strict, artifact-bound audit of the GFX10 occlusion-counter probe.

The probe emits the ZPASS_DONE begin/end event pair around the bounded scene
draws and reports, per 16-byte render-backend pair, whether the hardware set
availability bit 63 and what the start/end counter words were. This verifier
binds the run to the build manifest and the deployed artifact, then derives
validity itself from the record invariants instead of trusting a flag the
payload printed about itself. It does not claim any public Vulkan behaviour.

Usage:
    verify_occlusion_probe.py RUN --artifact dist-graphics-api/PPSA99994/eboot.bin
    verify_occlusion_probe.py RUN1 RUN2 --artifact ...   # compare two runs
"""
import argparse
import hashlib
import json
from pathlib import Path

AVAILABILITY = 1 << 63
PAIRS = 64
EXPECTED_STAGE = "graphics-api-native-presentation-reuse"
EXPECTED_TERMINATION = "shell-close-after-cleanup"
EXPECTED_SCISSOR_PROBE = 15
DEFAULT_MANIFEST = Path("build/native-graphics-api/manifest.json")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def parse(path):
    """Parse one finalized ps5log/1 run into (receipt, records, hello, lines)."""
    log = Path(path).with_suffix(".log")
    receipt = json.loads(Path(path).with_suffix(".json").read_text())
    data = log.read_bytes()
    require(hashlib.sha256(data).hexdigest() == receipt.get("sha256"), "log hash")
    require(receipt.get("clean") is True and receipt.get("bye") is True and
            receipt.get("gaps") == [] and receipt.get("transport") == "tcp" and
            receipt.get("protocol") == "ps5log/1", "transport")
    lines = data.decode().splitlines()
    require(lines[0].startswith("HELLO ps5log/1 "), "hello")
    hello = {}
    for field in lines[0].split()[2:]:
        if "=" in field:
            key, value = field.split("=", 1)
            hello[key] = value
    identity = receipt.get("identity", {})
    for key in ("title", "app", "boot"):
        require(hello.get(key) == identity.get(key), f"hello {key}")
    records = []
    previous = -1
    for seq, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4, "record shape")
        index, stamp, level, message = fields
        require(int(index) == seq and int(stamp) >= previous, "sequence/clock")
        require(level != "ERR" and "CHECK failed" not in message, "runtime failure")
        previous = int(stamp)
        words = message.split()
        records.append((words[0], {w.split("=", 1)[0]: w.split("=", 1)[1]
                                   for w in words[1:] if "=" in w}))
    require(receipt.get("records") == len(records), "record count")
    require(lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end", "bye")
    return receipt, records, hello, lines


def rows(records, name):
    return [(index, fields) for index, (kind, fields) in enumerate(records)
            if kind == name]


def validate_artifact(manifest_path, artifact_path):
    manifest = json.loads(Path(manifest_path).read_text())
    require(manifest.get("host_query_reset_probe", 0) in (0, 1),
            "manifest host query reset mode")
    if manifest.get("host_query_reset_probe") == 1:
        require(manifest.get("occlusion_query_api_probe") == 1 and
                manifest.get("runtime_sdk") is True,
                "host query reset requires the SDK query API witness")
    if manifest.get("occlusion_query_api_probe") == 1:
        require(manifest.get("stage") == "graphics-api-offscreen-draw" and
                manifest.get("runtime_sdk") is True and
                manifest.get("occlusion_query_secondary") == 1,
                "query API witness must use the SDK-linked secondary-buffer path")
    else:
        require(manifest.get("stage") == EXPECTED_STAGE, "manifest stage")
    require(manifest.get("submit_enabled") is True, "manifest submit")
    require(manifest.get("scissor_probe") == EXPECTED_SCISSOR_PROBE,
            "manifest must build the occlusion probe scenario")
    require(manifest.get("termination") == EXPECTED_TERMINATION,
            "manifest termination")
    if "occlusion_precise_probe" in manifest:
        require(manifest["occlusion_precise_probe"] in (0, 1),
                "manifest precise-counter mode")
    if "occlusion_depth_probe" in manifest:
        require(manifest["occlusion_depth_probe"] in (0, 1),
                "manifest depth-counter mode")
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("files", {}).get("eboot.bin") == digest,
            "artifact does not match the manifest it was built from")
    return digest


def validate(path, manifest_path=DEFAULT_MANIFEST, artifact_path=None):
    require(artifact_path is not None, "an artifact path is required")
    artifact_digest = validate_artifact(manifest_path, artifact_path)
    receipt, records, hello, lines = parse(path)
    manifest = json.loads(Path(manifest_path).read_text())
    if manifest.get("occlusion_query_api_probe", 0):
        return validate_query_api(path, records, manifest, artifact_digest)
    begin = rows(records, "PS5VK_OCCLUSION_PROBE_BEGIN")
    end = rows(records, "PS5VK_OCCLUSION_PROBE_END")
    pairs = rows(records, "PS5VK_OCCLUSION_PROBE_PAIR")
    slot = rows(records, "PS5VK_OCCLUSION_PROBE_SLOT")
    completed = rows(records, "PS5VK_GRAPHICS_COMPLETED")
    close = rows(records, "PS5VK_PLATFORM_CLOSE")
    require(len(begin) == 1 and len(end) == 1 and len(slot) == 1, "one probe round")
    precise_mode = manifest.get("occlusion_precise_probe")
    if precise_mode is not None:
        require(begin[0][1].get("precise") == str(precise_mode),
                "logged precise-counter mode must match the build manifest")
    depth_mode = manifest.get("occlusion_depth_probe")
    if depth_mode is not None:
        require(begin[0][1].get("depth") == str(depth_mode),
                "logged depth-counter mode must match the build manifest")
    require(len(close) == 1, "exactly one platform close")
    require(close[0][1].get("rc") == "0" and
            close[0][1].get("allocations_bytes") == "0", "clean platform close")
    serial = begin[0][1].get("serial")
    require(serial is not None, "begin serial")
    for label, group in (("end", end), ("slot", slot), ("pairs", pairs)):
        for _, fields in group:
            require(fields.get("serial") == serial, f"{label} serial must match")
    finished = [row for row in completed if row[1].get("serial") == serial]
    require(len(finished) == 1, "one completion for the probe serial")
    # Ordering: begin, end, completion, per-pair rows, slot summary, close.
    require(begin[0][0] < end[0][0] < finished[0][0], "probe ordering")
    require(all(finished[0][0] <= index < slot[0][0] for index, _ in pairs),
            "pair rows must follow completion and precede the slot summary")
    require(slot[0][0] < close[0][0], "slot summary must precede the close")
    base = int(begin[0][1]["base"], 16)
    require(base % 64 == 0, "cache-line aligned slot")
    require(int(end[0][1]["base_plus_8"], 16) == base + 8, "end at base+8")
    fields = slot[0][1]
    require(int(fields["pairs"]) == PAIRS, "generous pair count")
    available = int(fields["available"])
    require(0 < available <= PAIRS, "at least one render backend wrote its pair")
    first_pair = int(fields["first_pair"])
    highest_pair = int(fields["highest_pair"])
    counter = int(fields["counter"])
    mask = int(fields["mask_hi"], 16) << 32 | int(fields["mask_lo"], 16)
    indices = [int(f["index"]) for _, f in pairs]
    require(len(pairs) == available, "one detail row per available backend")
    require(len(set(indices)) == len(indices), "render-backend indices unique")
    require(indices == sorted(indices), "render-backend rows ordered")
    require(indices == list(range(first_pair, first_pair + available)),
            "enabled render backends must be a contiguous range from first_pair")
    require(highest_pair == indices[-1], "highest_pair matches the last index")
    require(mask == sum(1 << i for i in indices), "mask matches the indices")
    total = 0
    for _, row in pairs:
        begin_word, end_word = int(row["begin"], 16), int(row["end"], 16)
        require(bool(begin_word & AVAILABILITY) and bool(end_word & AVAILABILITY),
                "availability bit 63 set in both words")
        delta = (end_word & ~AVAILABILITY) - (begin_word & ~AVAILABILITY)
        require(delta == int(row["delta"]), "per-pair delta")
        total += delta
    require(total == counter, "slot total equals the per-pair sum")
    require(counter > 0, "measured nonzero occlusion counter")
    geometry = {
        "enabled_render_backends": available,
        "first_pair": first_pair,
        "highest_pair": highest_pair,
        "mask": f"0x{mask:016x}",
        "counter": counter,
        "precise_counter_mode": bool(precise_mode) if precise_mode is not None else None,
        "max_render_backends_lower_bound": first_pair + available,
        "artifact_eboot_sha256": artifact_digest,
    }
    return {"ok": True, "geometry": geometry, "log": str(path)}


def validate_query_api(path, records, manifest, artifact_digest):
    """Validate the Vulkan query API witness instead of the raw counter probe."""
    require(manifest.get("occlusion_query_api_probe") == 1,
            "manifest query API mode")
    require(manifest.get("occlusion_precise_probe") == 1 and
            manifest.get("occlusion_depth_probe") == 1,
            "query API witness requires the measured precise depth mode")
    result = rows(records, "PS5VK_OCCLUSION_QUERY_API_RESULT")
    created = rows(records, "PS5VK_GRAPHICS_API_DEVICE_CREATED")
    completed = rows(records, "PS5VK_GRAPHICS_COMPLETED")
    close = rows(records, "PS5VK_PLATFORM_CLOSE")
    require(len(result) == 1, "exactly one query API result")
    require(len(created) == 1, "exactly one graphics API device creation")
    require(len(completed) >= 2, "original and same-pool repeat submissions completed")
    require(len(close) == 1 and close[0][1].get("rc") == "0" and
            close[0][1].get("allocations_bytes") == "0", "clean platform close")
    if manifest.get("host_query_reset_probe") == 1:
        host = rows(records, "PS5VK_HOST_QUERY_RESET")
        require(len(host) == 2, "host reset and reused query records")
        require(host[0][1] == {
            "phase": "after_reset", "old": "1,0,3",
            "availability": "0,0,0", "status": "not_ready",
        }, "host reset must invalidate all results after the first execution")
        require(host[1][1] == {
            "phase": "after_reuse", "values": "1,0,3",
            "availability": "1,1,1", "completed": "1",
        }, "same query slots must produce known values after reset")
        require(created[0][0] < completed[0][0] < host[0][0] <
                completed[-1][0] < host[1][0] < result[0][0] < close[0][0],
                "host reset must fall between two completed executions")
    fields = result[0][1]
    expected = {
        "passed_samples": "1",
        "availability": "1",
        "copied_samples": "1",
        "copied_availability": "1",
        "zero_samples": "0",
        "zero_availability": "1",
        "zero_copied_samples": "0",
        "zero_copied_availability": "1",
        "three_samples": "3",
        "three_availability": "1",
        "three_copied_samples": "3",
        "three_copied_availability": "1",
        "samples32": "1",
        "availability32": "1",
        "copied_samples32": "1",
        "copied_availability32": "1",
        "zero_samples32": "0",
        "zero_availability32": "1",
        "zero_copied_samples32": "0",
        "zero_copied_availability32": "1",
        "three_samples32": "3",
        "three_availability32": "1",
        "three_copied_samples32": "3",
        "three_copied_availability32": "1",
        "precise_enabled": "1",
        "secondary": "1",
        "get_wait": "1",
        "copy_wait": "1",
        "partial": "1",
        "same_pool_reset_repeat": "1",
        "valid": "1",
    }
    for key, value in expected.items():
        require(fields.get(key) == value, f"query API {key} must equal {value}")
    require(created[0][0] < completed[-1][0] < result[0][0] < close[0][0],
            "query result follows completed submission and precedes clean close")
    return {
        "ok": True,
        "query_api": {
            "passed_samples": 1,
            "availability": 1,
            "copied_samples": 1,
            "copied_availability": 1,
            "zero_samples": 0,
            "zero_availability": 1,
            "zero_copied_samples": 0,
            "zero_copied_availability": 1,
            "three_samples": 3,
            "three_availability": 1,
            "three_copied_samples": 3,
            "three_copied_availability": 1,
            "samples32": 1,
            "availability32": 1,
            "copied_samples32": 1,
            "copied_availability32": 1,
            "zero_samples32": 0,
            "zero_availability32": 1,
            "zero_copied_samples32": 0,
            "zero_copied_availability32": 1,
            "three_samples32": 3,
            "three_availability32": 1,
            "three_copied_samples32": 3,
            "three_copied_availability32": 1,
            "precise_enabled": True,
            "secondary_command_buffer": True,
            "get_wait": True,
            "copy_wait": True,
            "partial": True,
            "same_pool_reset_repeat": True,
            "host_query_reset": manifest.get("host_query_reset_probe") == 1,
            "artifact_eboot_sha256": artifact_digest,
        },
        "log": str(path),
    }


def compare(first, second, manifest_path=DEFAULT_MANIFEST, artifact_path=None):
    a = validate(first, manifest_path, artifact_path)
    b = validate(second, manifest_path, artifact_path)
    key = "query_api" if "query_api" in a else "geometry"
    require(key in b and a[key] == b[key],
            f"two runs disagree: {a.get(key)} vs {b.get(key)}")
    return {"ok": True, "identical": True, key: a[key]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    if len(args.runs) == 1:
        print(json.dumps(validate(args.runs[0], args.manifest, args.artifact),
                         indent=2))
    elif len(args.runs) == 2:
        print(json.dumps(compare(*args.runs, manifest_path=args.manifest,
                                 artifact_path=args.artifact), indent=2))
    else:
        raise SystemExit("one run to validate, or two runs to compare")


if __name__ == "__main__":
    main()
