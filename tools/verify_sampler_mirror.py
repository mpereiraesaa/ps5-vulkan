"""Verify one SDK-linked mirror-clamp sampling draw against a CPU reference."""
import argparse
import hashlib
import json
from pathlib import Path

try:
    from tools.verify_occlusion_probe import parse, require, rows
except ModuleNotFoundError:
    from verify_occlusion_probe import parse, require, rows


CASES = (
    ("u", "nearest", -0.25, 0.25),
    ("u", "nearest", 0.25, 0.25),
    ("u", "nearest", 1.25, 0.25),
    ("u", "linear", -0.375, 0.25),
    ("u", "linear", 0.375, 0.25),
    ("u", "linear", 1.25, 0.25),
    ("v", "nearest", 0.25, -0.25),
    ("v", "nearest", 0.25, 0.25),
    ("v", "nearest", 0.25, 1.25),
    ("v", "linear", 0.25, -0.375),
    ("v", "linear", 0.25, 0.375),
    ("v", "linear", 0.25, 1.25),
    ("w", "nearest", -0.25, 0.25),
    ("w", "nearest", 0.25, 0.25),
    ("w", "nearest", 1.25, 0.25),
    ("w", "linear", -0.375, 0.25),
    ("w", "linear", 0.375, 0.25),
    ("w", "linear", 1.25, 0.25),
)


def expected_pixel(axis, filtering, u, v):
    """Vulkan mirror-once coordinates over the bounded 2D/3D RGBA8 source."""
    if axis == "w":
        z = min(1.0, abs(u))
        weight = float(z >= 0.5) if filtering == "nearest" else min(1.0, max(0.0, 2*z-0.5))
        return (255 << 24) | (int(255 * (1-weight) + 0.5) << 16) | (int(255 * weight + 0.5) << 8)
    if axis == "u":
        u = min(1.0, abs(u))
    else:
        v = min(1.0, abs(v))

    def weight(coord):
        if filtering == "nearest":
            return float(coord >= 0.5)
        return min(1.0, max(0.0, coord * 2.0 - 0.5))

    x, y = weight(u), weight(v)
    # Source RGBA8 red/green/blue/red becomes BGRA8 in the readback target.
    texels = ((0, 0, 255, 255), (0, 255, 0, 255),
              (255, 0, 0, 255), (0, 0, 255, 255))
    rgba = [int((texels[0][channel] * (1-x) * (1-y) +
                 texels[1][channel] * x * (1-y) +
                 texels[2][channel] * (1-x) * y +
                 texels[3][channel] * x * y) + 0.5)
            for channel in range(4)]
    return sum(value << (8 * channel) for channel, value in enumerate(rgba))


def color_near(actual, expected, tolerance=1):
    return all(abs(((actual >> shift) & 255) - ((expected >> shift) & 255)) <= tolerance
               for shift in range(0, 32, 8))


def validate(run, manifest_path, artifact_path):
    manifest = json.loads(Path(manifest_path).read_text())
    case = manifest.get("sampler_mirror_case")
    require(type(case) is int and 8 <= case <= 25, "mirror case in manifest")
    is_w = case >= 20
    require(manifest.get("stage") == "graphics-api-offscreen-draw" and
            manifest.get("runtime_sdk") is True and
            manifest.get("submit_enabled") is True and
            manifest.get("scissor_probe") == 6 and
            manifest.get("geometry_fixture") == (
                "sampler-mirror-w-3d" if is_w else "sampler-core-addressing") and
            (not is_w or manifest.get("image_target") == "3d") and
            manifest.get("termination") == "shell-close-after-cleanup" and
            manifest.get("graphics", {}).get("source") == (
                "experiments/graphics/scene3d-mirror-w.pipe" if is_w else
                "experiments/graphics/scene3d.pipe"), "SDK offscreen artifact")
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("files", {}).get("eboot.bin") == digest, "signed eboot hash")

    receipt, records, hello, _ = parse(run)
    require(hello.get("title") == "PPSA99994" and hello.get("app") == "ps5vk",
            "project run identity")
    axis, filtering, u, v = CASES[case - 8]
    name = f"mirror-{axis}-{filtering}-" + (
        "negative" if (u if axis in ("u", "w") else v) < 0 else
        "positive" if (u if axis in ("u", "w") else v) > 1 else "inside")
    expected = expected_pixel(axis, filtering, u, v)
    inputs = rows(records, "PS5VK_SAMPLER_CORE_INPUT")
    uploads = rows(records, "PS5VK_TEXTURE_UPLOAD")
    layered = rows(records, "PS5VK_LAYERED_INPUT")
    outputs = rows(records, "PS5VK_SAMPLER_CORE_READBACK")
    submits = rows(records, "PS5VK_GRAPHICS_SUBMIT")
    completes = rows(records, "PS5VK_GRAPHICS_COMPLETED")
    closes = rows(records, "PS5VK_PLATFORM_CLOSE")
    require(all(len(group) == 1 for group in
                (inputs, uploads, outputs, submits, completes, closes)), "one completed draw")
    if is_w:
        require(len(layered) == 1 and layered[0][1] ==
                {"target": "3d", "slices": "2", "width": "2", "height": "2"},
                "3D source fixture")
    else:
        require(not layered, "2D source fixture")
    require(not rows(records, "PS5VK_VIDEO_PRESENTED"), "offscreen measurement")
    source, output = inputs[0][1], outputs[0][1]
    require(inputs[0][0] < uploads[0][0] < submits[0][0] <
            completes[0][0] < outputs[0][0] <
            closes[0][0], "draw/readback/close order")
    if is_w:
        require(inputs[0][0] < layered[0][0] < uploads[0][0], "3D upload order")
    require(uploads[0][1] == {"frame": "0", "pattern": "layered-rgb" if is_w else "rgb-cycle", "width": "2",
                               "height": "2", "slices": "2" if is_w else "1", "levels": "1",
                               "format": "37"}, "four-texel source fixture")
    require(source.get("case") == output.get("case") == str(case) and
            source.get("name") == output.get("name") == name and
            source.get("uv_milli") == str(int(u * 1000)) and
            source.get("uv_v_milli") == str(int(v * 1000)) and
            source.get("mirror_axis") == {"u": "1", "v": "2", "w": "3"}[axis] and
            source.get("filter") == filtering and
            source.get("minification") == "0" and
            source.get("expected_bgra") == output.get("expected_bgra") ==
            f"{expected:08x}", "CPU reference input")
    require(color_near(int(output.get("actual_bgra", "0"), 16), expected) and
            output.get("expected") == "373248" and
            output.get("other") == "0" and output.get("valid") == "1" and
            submits[0][1].get("rc") == "0" and
            submits[0][1].get("serial") == completes[0][1].get("serial"),
            "GPU sampler readback")
    require(closes[0][1] == {"rc": "0", "allocations_bytes": "0"} and
            len(rows(records, "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")) == 1,
            "resource retirement")
    return {"strict_verified": True, "case": case, "name": name,
            "expected_bgra": f"{expected:08x}", "eboot_sha256": digest,
            "log_sha256": receipt["sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.run, args.manifest, args.artifact), indent=2))


if __name__ == "__main__":
    main()
