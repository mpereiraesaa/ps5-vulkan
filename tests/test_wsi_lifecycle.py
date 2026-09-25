"""Compile and run the public WSI lifecycle contract without shared build output."""

from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
import unittest
from tools.run_wsi_native_witness import verify


ROOT = Path(__file__).resolve().parents[1]


class WsiLifecycleTests(unittest.TestCase):
    def test_public_acquire_present_and_teardown(self):
        with tempfile.TemporaryDirectory(prefix="ps5vk-wsi-") as directory:
            temporary = Path(directory)
            output = temporary / "test_vk_wsi_lifecycle"
            makefile = temporary / "wsi.mk"
            makefile.write_text(
                "wsi-lifecycle-test:\n"
                "\t$(CC) -std=c11 -g -Wall -Wextra -Werror "
                "$(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) "
                "src/image_layout_state.c "
                "tests/test_vk_wsi_lifecycle.c "
                f"-o {output}\n"
                f"\t{output}\n"
            )
            environment = os.environ.copy()
            environment.setdefault("LAB_SIBLINGS", str(ROOT.parent))
            command = ["make", "-f", "Makefile", "-f", str(makefile),
                       "wsi-lifecycle-test"]
            completed = subprocess.run(
                command, cwd=ROOT, env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=120)
            self.assertEqual(completed.returncode, 0, completed.stdout)

    def test_native_receipt_requires_three_completed_frames_and_clean_close(self):
        events = [
            "WSI_WITNESS_ADAPTER surface=created",
            "WSI_WITNESS_START display=1920x1080 refresh=60000 image_count=2 usage=18",
            "PS5VK_VIDEO_REGISTER handle=5 buffers=2 image_bytes=8912896 format_word=0000000000000000",
            "PS5VK_VIDEO_PRESENTED token=1 fence=0 matching_event=1 hold_seconds=0",
            "WSI_WITNESS_FRAME frame=0 slot=0 color=0 fence=complete present=complete",
            "PS5VK_VIDEO_PRESENTED token=2 fence=0 matching_event=1 hold_seconds=0",
            "WSI_WITNESS_FRAME frame=1 slot=1 color=1 fence=complete present=complete",
            "PS5VK_VIDEO_PRESENTED token=3 fence=0 matching_event=1 hold_seconds=0",
            "WSI_WITNESS_FRAME frame=2 slot=0 color=2 fence=complete present=complete",
            "PS5VK_VIDEO_CLOSED token=3 deferred=0",
            "WSI_WITNESS_RETIRED frames=3 resources=clean",
        ]
        log = ("\n".join(events) + "\n").encode()
        receipt = {"protocol": "ps5log/1", "title": "PPSA99994",
                   "app": "ps5vk", "transport": "tcp", "clean": True,
                   "bye": True, "gaps": [], "run_id": "fixture",
                   "sha256": hashlib.sha256(log).hexdigest()}
        artifact = {"profile": "vulkan-wsi-native-lifecycle",
                    "eboot_sha256": "a" * 64}
        immediate = verify(log, receipt, artifact)
        self.assertTrue(immediate["strict_verified"])
        self.assertFalse(immediate["unregister_deferred_to_close"])
        deferred = log.replace(b"deferred=0", b"deferred=1")
        deferred_receipt = {**receipt, "sha256": hashlib.sha256(deferred).hexdigest()}
        clean_deferred = verify(deferred, deferred_receipt, artifact)
        self.assertTrue(clean_deferred["strict_verified"])
        self.assertTrue(clean_deferred["unregister_deferred_to_close"])
        for altered in (log.replace(b"slot=1", b"slot=0"),
                        log.replace(b"WSI_WITNESS_ADAPTER", b"WSI_WITNESS_DIRECT"),
                        log.replace(b"fence=complete", b"fence=timeout", 1),
                        log.replace(b"deferred=0", b"deferred=2"),
                        log.replace(b"PS5VK_VIDEO_CLOSED", b"PS5VK_VIDEO_RETAIN"),
                        log + b"PS5VK_VIDEO_SETUP_FAILED\n",
                        log.replace(b"resources=clean", b"resources=leaked")):
            with self.subTest(altered=altered), self.assertRaises(ValueError):
                verify(altered, {**receipt, "sha256": hashlib.sha256(altered).hexdigest()},
                       artifact)
        with self.assertRaises(ValueError):
            verify(log, {**receipt, "bye": False}, artifact)


if __name__ == "__main__":
    unittest.main()
