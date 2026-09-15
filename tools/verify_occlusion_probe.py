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
    require(manifest.get("stage") == EXPECTED_STAGE, "manifest stage")
    require(manifest.get("submit_enabled") is True, "manifest submit")
    require(manifest.get("scissor_probe") == EXPECTED_SCISSOR_PROBE,
            "manifest must build the occlusion probe scenario")
    require(manifest.get("termination") == EXPECTED_TERMINATION,
            "manifest termination")
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("files", {}).get("eboot.bin") == digest,
            "artifact does not match the manifest it was built from")
    return digest


def validate(path, manifest_path=DEFAULT_MANIFEST, artifact_path=None):
    require(artifact_path is not None, "an artifact path is required")
    artifact_digest = validate_artifact(manifest_path, artifact_path)
    receipt, records, hello, lines = parse(path)
    begin = rows(records, "PS5VK_OCCLUSION_PROBE_BEGIN")
    end = rows(records, "PS5VK_OCCLUSION_PROBE_END")
    pairs = rows(records, "PS5VK_OCCLUSION_PROBE_PAIR")
    slot = rows(records, "PS5VK_OCCLUSION_PROBE_SLOT")
    completed = rows(records, "PS5VK_GRAPHICS_COMPLETED")
    close = rows(records, "PS5VK_PLATFORM_CLOSE")
    require(len(begin) == 1 and len(end) == 1 and len(slot) == 1, "one probe round")
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
        "max_render_backends_lower_bound": first_pair + available,
        "artifact_eboot_sha256": artifact_digest,
    }
    return {"ok": True, "geometry": geometry, "log": str(path)}


def compare(first, second, manifest_path=DEFAULT_MANIFEST, artifact_path=None):
    a = validate(first, manifest_path, artifact_path)
    b = validate(second, manifest_path, artifact_path)
    require(a["geometry"] == b["geometry"],
            f"two runs disagree: {a['geometry']} vs {b['geometry']}")
    return {"ok": True, "identical": True, "geometry": a["geometry"]}


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
