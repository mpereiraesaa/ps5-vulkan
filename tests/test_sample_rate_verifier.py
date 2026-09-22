"""The sample-rate verifier must accept the measured shape and nothing weaker."""
import hashlib
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.verify_sample_rate import validate  # noqa: E402


HELLO = "HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=0x1 tag=gate1"


def record(seq, level, text):
    return f"{seq}\t{seq * 1000}\t{level}\t{text}"


def stream(clear=None, shaded=None, close=None, bye="BYE reason=graphics-api-end"):
    clear = clear if clear is not None else (
        "PS5VK_SAMPLE_RATE_CLEAR extent=64x64 samples=4 bytes=262144 words=65536 "
        "expected=ff4080bf first=ff4080bf last=ff4080bf distinct=1 correct=65536 verdict=1")
    shaded = shaded if shaded is not None else (
        "PS5VK_SAMPLE_RATE_SHADED extent=64x64 samples=4 words=65536 shaded_values=4 "
        "expected_values=4 matched=4 covered_words=16384 "
        "values=ff000000,ff010000,ff020000,ff030000 verdict=1")
    close = close if close is not None else (
        "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0")
    lines = [HELLO,
             record(1, "MARK", "PS5VK_GRAPHICS_API_DEVICE_CREATED"),
             record(2, "MARK", "PS5VK_MULTISAMPLE_CLEAR_PREPARED serial=1 samples=4 bytes=262144 word=ff4080bf"),
             record(3, "MARK", clear),
             record(4, "MARK", shaded),
             record(5, "MARK", close),
             record(6, "MARK", "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE"),
             bye]
    return "\n".join(lines) + "\n"


class SampleRateVerifier(unittest.TestCase):
    def setUp(self):
        self.dir = Path(self._testMethodName)
        self.dir.mkdir(exist_ok=True)
        self.artifact = self.dir / "eboot.bin"
        self.artifact.write_bytes(b"sample-rate-witness-artifact")
        self.manifest = self.dir / "manifest.json"
        self.manifest.write_text(json.dumps({
            "stage": "graphics-api-offscreen-draw", "submit_enabled": True,
            "runtime_graphics": True, "sample_rate_probe": 1,
            "sample_rate_measurement": True,
            "graphics_shader_source": "owned-runtime-sample-id",
            "termination": "shell-close-after-cleanup",
            "sample_rate_witness": {
                "extent": [64, 64], "samples": 4,
                "clear_rgba": [0.25, 0.5, 0.75, 1.0],
                "clear_word": "ff4080bf",
                "shaded_values": ["ff000000", "ff010000", "ff020000", "ff030000"],
                "phases": ["clear", "shaded"], "strict_readback": True},
            "files": {"eboot.bin": hashlib.sha256(self.artifact.read_bytes()).hexdigest()},
        }))

    def tearDown(self):
        for path in sorted(self.dir.rglob("*"), reverse=True):
            path.unlink() if path.is_file() else path.rmdir()
        self.dir.rmdir()

    def check(self, text, message=None):
        log = self.dir / "run.log"
        log.write_text(text)
        if message is None:
            result = validate(log, self.manifest, self.artifact)
            self.assertTrue(result["ok"])
        else:
            with self.assertRaises(ValueError) as error:
                validate(log, self.manifest, self.artifact)
            self.assertIn(message, str(error.exception))

    def test_accepts_the_measured_run(self):
        self.check(stream())

    def test_refuses_a_false_clear_green(self):
        # The payload claims success while its own numbers show an uncovered
        # word: the verifier must derive the verdict, not trust the field.
        self.check(stream(clear=(
            "PS5VK_SAMPLE_RATE_CLEAR extent=64x64 samples=4 bytes=262144 words=65536 "
            "expected=ff4080bf first=ff4080bf last=ff4080bf distinct=1 correct=65535 verdict=1")),
            "clear left uncovered words")

    def test_refuses_a_once_per_pixel_shading_claim(self):
        self.check(stream(shaded=(
            "PS5VK_SAMPLE_RATE_SHADED extent=64x64 samples=4 words=65536 shaded_values=1 "
            "expected_values=4 matched=1 covered_words=16384 "
            "values=ff000000,ff000000,ff000000,ff000000 verdict=1")),
            "shaded value count")

    def test_refuses_the_wrong_shaded_values(self):
        self.check(stream(shaded=(
            "PS5VK_SAMPLE_RATE_SHADED extent=64x64 samples=4 words=65536 shaded_values=4 "
            "expected_values=4 matched=4 covered_words=16384 "
            "values=ff000000,ff010000,ff020000,ff040000 verdict=1")),
            "shaded values")

    def test_refuses_an_unreleased_close(self):
        self.check(stream(close="PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=4096"),
                   "close did not release everything")

    def test_refuses_an_unclean_stream(self):
        self.check(stream(bye="BYE reason=graphics-api-failed"), "unclean close")


if __name__ == "__main__":
    unittest.main()
