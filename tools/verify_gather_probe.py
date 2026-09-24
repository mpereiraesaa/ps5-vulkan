"""Strict artifact-bound verifier for the single-form runtime gather witness."""
import argparse
import hashlib
import json
from pathlib import Path

try:
    from tools.verify_occlusion_probe import parse, require
except ModuleNotFoundError:
    from verify_occlusion_probe import parse, require

ROOT = Path(__file__).resolve().parents[1]
SOURCE_BY_FORM = {
    1: "experiments/graphics/runtime_gather_core.frag",
    2: "experiments/graphics/runtime_gather_const_offset.frag",
    3: "experiments/graphics/runtime_gather_dynamic_offset.frag",
    4: "experiments/graphics/runtime_gather_four_offsets.frag",
    5: "experiments/graphics/runtime_gather_component_0.frag",
    6: "experiments/graphics/runtime_gather_component_1.frag",
    7: "experiments/graphics/runtime_gather_component_2.frag",
    8: "experiments/graphics/runtime_gather_component_3.frag",
}


def expected_pixel(form):
    """CPU oracle for the 64x64 RGBA8 grid and CTS gather component order."""
    require(form in SOURCE_BY_FORM, "gather form")
    corner_x = (0, 1, 1, 0)
    corner_y = (1, 1, 0, 0)
    offset_x = (-8, 7, -8, 7)
    offset_y = (-8, -8, 7, 7)
    component = 0 if form in (1, 5) else (form - 5 if form >= 6 else 2)
    values = []
    for index in range(4):
        if form == 2:
            ox, oy = -8, 7
        elif form == 3:
            ox, oy = -1, 0  # gl_FragCoord.x is 960.5; int(x) is even.
        elif form == 4:
            ox, oy = offset_x[index], offset_y[index]
        else:
            ox, oy = 0, 0
        corner = 3 if form == 4 else index
        x = 31 + corner_x[corner] + ox
        y = 31 + corner_y[corner] + oy
        if component == 0:
            values.append(x & 0xff)
        elif component == 1:
            values.append(y & 0xff)
        elif component == 2:
            values.append((3 * x + 5 * y) & 0xff)
        else:
            values.append(255)
    # BGRA8 mapped storage is AARRGGBB in host integer order.
    return (values[3] << 24) | (values[0] << 16) | (values[1] << 8) | values[2]


def validate_artifact(manifest_path, artifact_path):
    manifest = json.loads(Path(manifest_path).read_text())
    witness = manifest.get("gather_probe")
    require(manifest.get("stage") == "graphics-api-offscreen-draw" and
            manifest.get("runtime_sdk") is True and
            manifest.get("submit_enabled") is True, "SDK-linked draw manifest")
    require(manifest.get("scissor_probe") == 15, "bounded probe-15 draw")
    require(isinstance(witness, dict) and witness.get("gpu_readback") is True,
            "gather witness manifest")
    form = witness.get("form")
    require(form in SOURCE_BY_FORM, "gather form in manifest")
    require(witness.get("texture_extent") == [64, 64] and
            witness.get("sample_coordinate") == [0.5, 0.5] and
            witness.get("texel_pattern") == "rgba8-x-y-3x-plus-5y",
            "deterministic texture contract")
    requires_extended = form in (2, 3, 4)
    require(witness.get("diagnostic_feature") is requires_extended and
            witness.get("offset_limits") == ({"min": -8, "max": 7}
                if requires_extended else {"min": 0, "max": 0}),
            "feature and gather-limit profile matches the shader operands")
    require(witness.get("source") == SOURCE_BY_FORM[form] and
            manifest.get("graphics_shader_source") ==
                f"owned-runtime-image-gather-{form}",
            "artifact names the matching gather source")
    source_digest = hashlib.sha256((ROOT / SOURCE_BY_FORM[form]).read_bytes()).hexdigest()
    require(manifest.get("runtime_graphics_inputs", {}).get("fragment", {}).get(
            "glsl_sha256") == source_digest,
            "runtime fragment source hash")
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("files", {}).get("eboot.bin") == digest,
            "artifact does not match manifest")
    return manifest, digest


def validate(path, manifest_path, artifact_path):
    manifest, digest = validate_artifact(manifest_path, artifact_path)
    receipt, records, hello, lines = parse(path)
    names = [(index, kind, fields) for index, (kind, fields) in enumerate(records)]
    created = [i for i, kind, _ in names if kind == "PS5VK_GRAPHICS_API_DEVICE_CREATED"]
    completed = [i for i, kind, _ in names if kind == "PS5VK_GRAPHICS_COMPLETED"]
    profiles = [fields for _, kind, fields in names if kind == "PS5VK_GATHER_PROFILE"]
    inputs = [fields for _, kind, fields in names if kind == "PS5VK_GATHER_INPUT"]
    results = [fields for _, kind, fields in names if kind == "PS5VK_GATHER_READBACK"]
    close = [(i, fields) for i, kind, fields in names if kind == "PS5VK_PLATFORM_CLOSE"]
    require(len(created) == 1 and len(completed) == 1, "one created device and completed draw")
    if manifest["gather_probe"].get("profile_query_logged"):
        require(len(profiles) == 1, "one queried gather feature/limits profile")
        expected_extended = manifest["gather_probe"]["diagnostic_feature"]
        expected_limits = manifest["gather_probe"]["offset_limits"]
        require(profiles[0].get("extended") == ("1" if expected_extended else "0") and
                profiles[0].get("min_offset") == str(expected_limits["min"]) and
                profiles[0].get("max_offset") == str(expected_limits["max"]),
                "queried feature and gather limits match the build profile")
    else:
        require(not profiles, "legacy manifest cannot claim an unbound profile query")
    require(len(inputs) == 1 and len(results) == 1, "one deterministic input and readback")
    require(len(close) == 1 and close[0][1].get("rc") == "0" and
            close[0][1].get("allocations_bytes") == "0", "clean platform close")
    form = manifest["gather_probe"]["form"]
    expected = expected_pixel(form)
    require(inputs[0].get("extent") == "64x64" and
            inputs[0].get("channels") == "R:x,G:y,B:3x+5y,A:255",
            "logged texture input matches manifest")
    require(results[0].get("form") == str(form), "logged gather form matches manifest")
    require(int(results[0].get("expected_bgra", "-1"), 16) == expected,
            "payload expected value matches independent CPU oracle")
    require(int(results[0].get("actual_bgra", "-1"), 16) == expected and
            results[0].get("changed_words") == "1" and
            0 <= int(results[0].get("first_word_index", "-1")) < 1920 * 1080 and
            results[0].get("valid") == "1", "single GPU readback word matches gather oracle")
    require(created[0] < completed[0] < next(i for i, kind, _ in names
            if kind == "PS5VK_GATHER_READBACK") < close[0][0],
            "result follows completed GPU draw and precedes close")
    require(lines[-1] == f"BYE seq={receipt['records']} reason=graphics-api-end",
            "clean ps5log end")
    return {"ok": True, "form": form, "expected_bgra": f"0x{expected:08x}",
            "actual_bgra": f"0x{expected:08x}", "artifact_eboot_sha256": digest,
            "log": str(path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.run, args.manifest, args.artifact), indent=2))


if __name__ == "__main__":
    main()
