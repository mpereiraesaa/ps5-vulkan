"""Verify the bounded D16 depth attachment witness against its signed eboot."""
import hashlib
import json
from pathlib import Path

try:
    from tools.verify_occlusion_probe import parse, require, rows
except ModuleNotFoundError:
    from verify_occlusion_probe import parse, require, rows


def validate(log_path, manifest_path, artifact_path):
    manifest = json.loads(Path(manifest_path).read_text())
    digest = hashlib.sha256(Path(artifact_path).read_bytes()).hexdigest()
    require(manifest.get("stage") == "graphics-api-offscreen-draw" and
            manifest.get("runtime_sdk") is True and
            manifest.get("submit_enabled") is True and
            manifest.get("scissor_probe") == 14 and
            manifest.get("d16_depth_witness") == 1 and
            (manifest.get("d16_depth_attachment_supported") == 1 or
             manifest.get("d16_depth_attachment_diagnostic") == 1),
            "D16 SDK-linked artifact")
    require(manifest.get("files", {}).get("eboot.bin") == digest,
            "signed eboot hash")
    require(manifest.get("graphics", {}).get("source") ==
            "experiments/graphics/scene3d.pipe", "witness shader source")
    receipt, records, hello, _ = parse(log_path)
    require(hello.get("title") == "PPSA99994" and hello.get("app") == "ps5vk",
            "project run identity")
    require(len(rows(records, "PS5VK_GRAPHICS_API_DEVICE_CREATED")) == 1,
            "one created device")
    queries = rows(records, "PS5VK_D16_IMAGE_QUERY")
    require(len(queries) == 1 and queries[0][1] == {
        "usage": "depth-attachment", "max_extent": "128x128",
        "mip_levels": "1", "layers": "1", "samples": "1"},
        "public D16 image query")
    loads = rows(records, "PS5VK_D16_DEPTH_LOAD_CLEAR")
    witnesses = rows(records, "PS5VK_DEPTH_CLEAR_WITNESS")
    readbacks = rows(records, "PS5VK_GRAPHICS_API_READBACK")
    submits = rows(records, "PS5VK_GRAPHICS_SUBMIT")
    completions = rows(records, "PS5VK_GRAPHICS_COMPLETED")
    require(all(len(group) == 2 for group in
                (loads, witnesses, readbacks, submits, completions)),
            "two independent depth draws")
    for frame, (load, witness, readback, submit, complete) in enumerate(
            zip(loads, witnesses, readbacks, submits, completions)):
        expected_word = "ffffffff" if frame == 0 else "00000000"
        expected_depth = "1.0" if frame == 0 else "0.0"
        require(load[0] < witness[0] < readback[0] and
                submit[0] < complete[0] < readback[0], "frame order")
        require(load[1].get("frame") == str(frame) and
                load[1].get("extent") == "128x128" and
                load[1].get("usage") == "depth-attachment" and
                load[1].get("clear_word") == expected_word and
                load[1].get("depth") == expected_depth, "D16 load clear")
        require(witness[1].get("frame") == str(frame) and
                witness[1].get("format") == "124" and
                witness[1].get("clear_depth_word") == expected_word and
                witness[1].get("expect_visible") == str(1-frame) and
                witness[1].get("valid") == "1" and
                witness[1].get("bad_alpha") == "0", "depth draw oracle")
        changed = int(witness[1].get("changed", "-1"))
        total = int(witness[1].get("total", "-1"))
        require(total == 32768 and
                (1000 < changed < total if frame == 0 else changed == 0),
                "visible and occluded control")
        require(readback[1].get("changed_words") == str(changed) and
                readback[1].get("total_words") == str(total) and
                readback[1].get("bad_alpha") == "0" and
                readback[1].get("bad_sum") == "0" and
                readback[1].get("viewport") == "128x128" and
                readback[1].get("valid") == "1", "GPU color readback")
        require(submit[1].get("rc") == "0" and
                submit[1].get("serial") == complete[1].get("serial"),
                "GPU completion")
    require(len(rows(records, "PS5VK_PLATFORM_CLOSE")) == 1 and
            rows(records, "PS5VK_PLATFORM_CLOSE")[0][1] ==
            {"rc": "0", "allocations_bytes": "0"} and
            len(rows(records, "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")) == 1,
            "resource retirement")
    return {"strict_verified": True, "eboot_sha256": digest,
            "log_sha256": receipt["sha256"], "frames": 2,
            "visible_pixels": int(witnesses[0][1]["changed"]),
            "occluded_pixels": int(witnesses[1][1]["changed"])}
