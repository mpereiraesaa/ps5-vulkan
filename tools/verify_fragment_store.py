"""Strict artifact-bound verifier for the T06 fragment-storage witness."""
import argparse
import hashlib
import json
from pathlib import Path

DEFAULT_MANIFEST = Path("build/native-graphics-api/manifest.json")
EXPECTED_WITNESS = {
    "extent": [64, 64],
    "expected_fragments": 4096,
    "descriptor_set": 0,
    "binding": 0,
    "record_bytes": 16,
    "control_compile_define": "CONTROL=1",
    "control_and_candidate_same_submit": True,
    "strict_readback": True,
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def parse(path):
    path = Path(path)
    log = path.with_suffix(".log")
    receipt = json.loads(path.with_suffix(".json").read_text())
    data = log.read_bytes()
    require(hashlib.sha256(data).hexdigest() == receipt.get("sha256"), "log hash")
    require(receipt.get("clean") is True and receipt.get("bye") is True and
            receipt.get("gaps") == [] and receipt.get("transport") == "tcp" and
            receipt.get("protocol") == "ps5log/1", "transport")
    lines = data.decode().splitlines()
    require(lines and lines[0].startswith("HELLO ps5log/1 "), "hello")
    hello = dict(field.split("=", 1) for field in lines[0].split()[2:]
                 if "=" in field)
    for key in ("title", "app", "boot"):
        require(hello.get(key) == receipt.get("identity", {}).get(key),
                f"hello {key}")
    records = []
    previous = -1
    for sequence, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4, "record shape")
        index, stamp, level, message = fields
        require(int(index) == sequence and int(stamp) >= previous,
                "sequence/clock")
        require(level != "ERR" and "CHECK failed" not in message,
                "runtime failure")
        previous = int(stamp)
        words = message.split()
        records.append((words[0], dict(word.split("=", 1)
                                      for word in words[1:] if "=" in word)))
    require(receipt.get("records") == len(records), "record count")
    require(lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end", "bye")
    return records


def rows(records, name):
    return [(index, fields) for index, (kind, fields) in enumerate(records)
            if kind == name]


def exactly(records, name):
    found = rows(records, name)
    require(len(found) == 1, f"exactly one {name}")
    return found[0]


def validate_artifact(manifest_path, artifact_path):
    manifest = json.loads(Path(manifest_path).read_text())
    require(manifest.get("stage") == "graphics-api-offscreen-draw",
            "manifest stage")
    require(manifest.get("submit_enabled") is True and
            manifest.get("runtime_graphics") is True, "manifest runtime draw")
    require(manifest.get("fragment_store_probe") == 1 and
            manifest.get("t06_diagnostic_features") is True,
            "manifest diagnostic gate")
    require(manifest.get("graphics_shader_source") ==
            "owned-runtime-fragment-storage-atomic", "manifest shader source")
    require(manifest.get("fragment_store_witness") == EXPECTED_WITNESS,
            "manifest witness")
    require(manifest.get("termination") == "return-main", "manifest termination")
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("files", {}).get("eboot.bin") == digest,
            "artifact does not match manifest")
    return digest


def validate(path, manifest_path=DEFAULT_MANIFEST, artifact_path=None):
    require(artifact_path is not None, "an artifact path is required")
    digest = validate_artifact(manifest_path, artifact_path)
    records = parse(path)
    created = exactly(records, "PS5VK_GRAPHICS_API_DEVICE_CREATED")
    abi = exactly(records, "PS5VK_FRAGMENT_STORE_ABI")
    readback = exactly(records, "PS5VK_FRAGMENT_STORE_READBACK")
    close = exactly(records, "PS5VK_PLATFORM_CLOSE")
    cleanup = exactly(records, "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")

    a = abi[1]
    require(int(a["set"]) == 0 and int(a["binding"]) == 0 and
            int(a["record_bytes"]) == 16 and int(a["control_table"]) == 0 and
            int(a["candidate_fragment_table"]) == 1 and
            int(a["used_bindings"], 16) == 1, "fragment descriptor ABI")
    r = readback[1]
    require(r.get("extent") == "64x64" and int(r["draws"]) == 2 and
            int(r["expected"]) == 4096 and int(r["control_counter"]) == 0 and
            int(r["candidate_counter"]) == 4096 and
            int(r["guard_words"]) == 30 and int(r["guard_mismatches"]) == 0 and
            r.get("fence") == "success" and int(r["strict_verified"]) == 1,
            "deterministic fragment-storage readback")
    require(close[1].get("rc") == "0" and
            close[1].get("allocations_bytes") == "0", "clean platform close")
    require(created[0] < abi[0] < readback[0] < close[0] < cleanup[0],
            "lifecycle ordering")
    return {"ok": True, "artifact_eboot_sha256": digest,
            "control_counter": 0, "candidate_counter": 4096,
            "run": str(path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    print(json.dumps(validate(args.run, args.manifest, args.artifact), indent=2))


if __name__ == "__main__":
    main()
