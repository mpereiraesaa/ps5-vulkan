#!/usr/bin/env python3
"""Strict ps5log/1 verification of the diagnostic imageless framebuffer witness."""

from verify_cube_array_witness import validate_stream


def _require(condition, label):
    if not condition:
        raise ValueError(label)


def validate(log, receipt, artifact):
    _require(artifact.get("title") == "PPSA99994" and
             artifact.get("profile") == "imageless-framebuffer-witness" and
             artifact.get("submit_enabled") is True, "artifact profile")
    digest = artifact.get("files", {}).get("eboot.bin", "")
    _require(len(digest) == 64 and
             all(character in "0123456789abcdef" for character in digest.lower()),
             "artifact eboot identity")
    _require(artifact.get("imageless_framebuffer") == {
        "feature": "VkPhysicalDeviceImagelessFramebufferFeatures.imagelessFramebuffer",
        "diagnostic_only": True,
        "views": 2,
        "framebuffers": 1,
        "submissions": 2,
        "extent": [64, 64],
        "format": "VK_FORMAT_R8G8B8A8_UNORM",
        "pixels_per_view": 4096,
    }, "artifact contract")
    messages = validate_stream(log, receipt, "consumer-imageless-framebuffer-end")

    def exactly(marker):
        _require(messages.count(marker) == 1, marker)

    exactly("PS5VK_CONSUMER_IMAGELESS_RESULT views=2 same_framebuffer=1 pixels=4096 "
            "first_mismatches=0 second_mismatches=0 valid=1")
    exactly("PS5VK_CONSUMER_TEST_SUCCESS")
    exactly("PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1")
    exactly("PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1")
    return {
        "profile": "imageless-framebuffer-witness",
        "artifact_eboot_sha256": digest,
        "views": 2,
        "framebuffers": 1,
        "submissions": 2,
        "pixels_checked": 8192,
        "mismatches": 0,
    }
