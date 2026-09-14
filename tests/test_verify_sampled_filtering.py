import hashlib
import unittest

from tools.verify_sampled_filtering import TRIALS, validate


class SampledFilteringVerifier(unittest.TestCase):
    def fixture(self):
        records = []
        for trial, (case, name, number, texel_bytes, filtering, expected) in enumerate(TRIALS):
            records.append(f"PS5VK_COMPUTE_END rounds=6 dispatches=12")
            records.extend("PS5VK_COMPUTE_RESULT valid=1" for _ in range(6))
            records.append(
                f"PS5VK_SAMPLED_FILTER_INPUT trial={trial} case={case} name={name} "
                f"format={number} filter={filtering} bytes_per_texel={texel_bytes} "
                f"expected_bgra={expected}")
            records.append("PS5VK_GRAPHICS_SUBMIT rc=0")
            records.append("PS5VK_GRAPHICS_COMPLETED serial=1")
            records.append(
                f"PS5VK_SAMPLED_FILTER_READBACK trial={trial} case={case} name={name} "
                f"format={number} filter={filtering} expected_bgra={expected} "
                "expected=373248 other=0 first_other=00000000 valid=1")
            records.append("PS5VK_VIDEO_PRESENTED slot=0")
            records.append("PS5VK_GRAPHICS_REUSE_END frame=0")
            records.append("PS5VK_COMPUTE_END rounds=6 dispatches=12")
            records.extend("PS5VK_COMPUTE_RESULT valid=1" for _ in range(6))
        records += ["PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
                    "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE"]
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=b"]
        lines += [f"{index}\t{index}\tMARK\t{record}"
                  for index, record in enumerate(records, 1)]
        lines.append(f"BYE seq={len(records)} reason=graphics-api-end")
        log = ("\n".join(lines) + "\n").encode()
        metadata = {"sha256": hashlib.sha256(log).hexdigest(), "clean": True,
                    "bye": True, "gaps": [], "transport": "tcp",
                    "protocol": "ps5log/1", "records": len(records),
                    "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "b"}}
        artifact = {"files": {"eboot.bin": "a" * 64},
                    "stage": "graphics-api-native-presentation-reuse",
                    "scissor_probe": 9, "geometry_fixture": "sampled-format-filtering",
                    "termination": "shell-close-after-cleanup"}
        return log, metadata, artifact

    def test_accepts_complete_nearest_linear_matrix(self):
        result = validate(*self.fixture())
        self.assertEqual(result["formats"], 20)
        self.assertEqual(result["trials"], 40)
        self.assertTrue(result["nearest_linear_discriminated"])

    def test_nearest_oracle_tracks_dword_aligned_r8_fixture_width(self):
        self.assertEqual(TRIALS[0][5], "ffff0000")
        self.assertEqual(TRIALS[2][5], "ff000000")
        self.assertEqual(TRIALS[6][5], "ffff0000")
        self.assertEqual(TRIALS[8][5], "ff000000")

    def test_rejects_missing_or_false_trial(self):
        log, metadata, artifact = self.fixture()
        for altered in (log.replace(b"PS5VK_SAMPLED_FILTER_READBACK trial=3", b"REMOVED trial=3", 1),
                        log.replace(b"expected=373248 other=0", b"expected=0 other=373248", 1)):
            metadata["sha256"] = hashlib.sha256(altered).hexdigest()
            with self.assertRaises(ValueError):
                validate(altered, metadata, artifact)

    def test_rejects_wrong_artifact_mode(self):
        log, metadata, artifact = self.fixture()
        artifact["scissor_probe"] = 7
        with self.assertRaises(ValueError):
            validate(log, metadata, artifact)


if __name__ == "__main__":
    unittest.main()
