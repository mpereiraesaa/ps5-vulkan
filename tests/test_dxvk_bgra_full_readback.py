"""Strict receipt contract for the full-surface BGRA8 GPU-copy witness."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_bgra_full_readback import FRAMES, HEIGHT, PROFILE, WIDTH  # noqa: E402
from run_dxvk_bgra_full_readback import expected_digest, verify  # noqa: E402


def artifact():
    return {"profile": PROFILE, "width": WIDTH, "height": HEIGHT,
            "frames": FRAMES, "bytes_per_frame": WIDTH * HEIGHT * 4,
            "eboot_sha256": "0" * 64}


def log(*, bad_frame=None, bad_usage=False):
    lines = [f"DXVK_BGRA_FULL_READBACK_START width={WIDTH} height={HEIGHT}"
             f" bytes={WIDTH * HEIGHT * 4} usage={22 if bad_usage else 23} frames={FRAMES}"]
    for frame in range(FRAMES):
        lines.append(f"DXVK_BGRA_FULL_READBACK_FRAME frame={frame}"
                     f" mismatches={1 if frame == bad_frame else 0} guard=0"
                     f" digest={expected_digest(frame)} fence=complete")
    lines += [f"DXVK_BGRA_FULL_READBACK_RESULT frames={FRAMES} passed={FRAMES}",
              "DXVK_BGRA_FULL_READBACK_RETIRED resources=clean"]
    return ("\n".join(lines) + "\n").encode()


def receipt(payload):
    return {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
            "transport": "tcp", "clean": True, "bye": True, "gaps": [],
            "sha256": hashlib.sha256(payload).hexdigest(), "run_id": "fixture"}


class BgraFullReadback(unittest.TestCase):
    def test_two_distinct_full_surface_digests(self):
        self.assertNotEqual(expected_digest(0), expected_digest(1))
        payload = log()
        self.assertTrue(verify(payload, receipt(payload), artifact())["strict_verified"])

    def test_corrupt_pixel_or_receipt_does_not_pass(self):
        payload = log(bad_frame=1)
        self.assertFalse(verify(payload, receipt(payload), artifact())["strict_verified"])
        good = log()
        broken_receipt = receipt(good)
        broken_receipt["gaps"] = [1]
        with self.assertRaises(ValueError):
            verify(good, broken_receipt, artifact())

    def test_wrong_image_role_or_artifact_is_refused(self):
        payload = log(bad_usage=True)
        with self.assertRaises(ValueError):
            verify(payload, receipt(payload), artifact())
        good = log()
        wrong = artifact()
        wrong["width"] = 16
        with self.assertRaises(ValueError):
            verify(good, receipt(good), wrong)


if __name__ == "__main__":
    unittest.main()
