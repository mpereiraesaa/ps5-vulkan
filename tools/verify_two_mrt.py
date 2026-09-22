"""Strict artifact-bound verifier for the T06 two-colour-target (independentBlend) witness."""
import argparse
import hashlib
import json
from pathlib import Path

try:
    from tools.verify_fragment_store import exactly, parse, require
except ModuleNotFoundError:
    from verify_fragment_store import exactly, parse, require

DEFAULT_MANIFEST = Path("build/native-graphics-api/manifest.json")
EXPECTED_WITNESS = {
    "extent": [64, 64],
    "draws": 1,
    "colour_attachments": 2,
    "blending": "disabled-on-both-attachments",
    "expected_target0_rgba": [64, 128, 191, 255],
    "expected_target1_rgba": [204, 102, 51, 128],
    "tolerance_lsb": 1,
    "strict_readback": True,
}


def channels(field):
    """Four bytes printed as one %02x%02x%02x%02x word."""
    require(len(field) == 8, f"channel width {field!r}")
    return [int(field[2 * index:2 * index + 2], 16) for index in range(4)]


def within(observed, expected, tolerance):
    return observed + tolerance >= expected and observed <= expected + tolerance


def validate_artifact(manifest_path, artifact_path):
    manifest = json.loads(Path(manifest_path).read_text())
    require(manifest.get("stage") == "graphics-api-offscreen-draw",
            "manifest stage")
    require(manifest.get("submit_enabled") is True and
            manifest.get("runtime_graphics") is True, "manifest runtime draw")
    require(manifest.get("two_mrt_probe") == 1 and
            manifest.get("two_mrt_measurement") is True,
            "manifest measurement gate")
    require(manifest.get("graphics_shader_source") == "owned-runtime-two-mrt",
            "manifest shader source")
    require(manifest.get("two_mrt_witness") == EXPECTED_WITNESS,
            "manifest witness")
    require(manifest.get("termination") == "shell-close-after-cleanup",
            "manifest termination")
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("files", {}).get("eboot.bin") == digest,
            "artifact does not match manifest")
    return digest


def validate(path, manifest_path=DEFAULT_MANIFEST, artifact_path=None):
    require(artifact_path is not None, "an artifact path is required")
    digest = validate_artifact(manifest_path, artifact_path)
    records = parse(path)
    created = exactly(records, "PS5VK_GRAPHICS_API_DEVICE_CREATED")
    readback = exactly(records, "PS5VK_TWO_MRT_READBACK")
    close = exactly(records, "PS5VK_PLATFORM_CLOSE")
    cleanup = exactly(records, "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")

    r = readback[1]
    require(r.get("extent") == "64x64" and int(r["draws"]) == 1, "witness shape")
    tolerance = int(r["tolerance"])
    require(tolerance == EXPECTED_WITNESS["tolerance_lsb"], "tolerance")
    target0 = channels(r["target0"])
    target1 = channels(r["target1"])
    wanted0 = EXPECTED_WITNESS["expected_target0_rgba"]
    wanted1 = EXPECTED_WITNESS["expected_target1_rgba"]
    # Attachment zero carries the first export, whose channels are not exactly
    # representable in UNORM8, so they are judged inside one LSB. Attachment one
    # carries the second export: its colour channels are exact, its alpha is a
    # half and keeps the same one-LSB allowance the oracle uses.
    require(all(within(target0[index], wanted0[index], tolerance)
                for index in range(3)) and target0[3] == wanted0[3],
            f"attachment zero readback {target0} != {wanted0}")
    require(target1[:3] == wanted1[:3] and
            within(target1[3], wanted1[3], tolerance),
            f"attachment one readback {target1} != {wanted1}")
    require(int(r["distinct"]) == 1,
            "the two attachments did not carry two distinct exports")
    require(r.get("fence") == "success" and int(r["strict_verified"]) == 1,
            "strict verdict")
    require(close[1].get("rc") == "0" and
            close[1].get("allocations_bytes") == "0", "clean platform close")
    require(created[0] < readback[0] < close[0] < cleanup[0],
            "lifecycle ordering")
    return {"ok": True, "artifact_eboot_sha256": digest,
            "target0_rgba": target0, "target1_rgba": target1,
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
