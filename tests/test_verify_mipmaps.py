import hashlib
import unittest

from tools.verify_mipmaps import EXPECTED_DESCRIPTOR, EXPECTED_HISTOGRAM, EXPECTED_STORAGE, validate


class MipmapVerifier(unittest.TestCase):
    def fixture(self, bias=0):
        messages = [
            "PS5VK_IMAGE_QUERY format=37 usage=6 max_width=16384 max_height=16384 max_bytes=268435456",
            "PS5VK_COMPUTE_END rounds=6 dispatches=12",
            "PS5VK_RUNTIME_GRAPHICS_CACHE rc=0 hit=0 compiled_pairs=1 hits=0 misses=1 entries=1 bytes=19136",
            f"PS5VK_MIPMAP_INPUT levels=3 view_base=0 lod_bias={bias} width=64 height=64 colors=red,green,blue bytes=21504",
            "PS5VK_TEXTURE_UPLOAD frame=0 pattern=mipmap-rgb width=64 height=64 slices=1 levels=3 format=37",
        ]
        descriptor = {0: "02080400", **EXPECTED_DESCRIPTOR,
                      10: f"{0x04000000 | ((bias * 256) & 0x3fff):08x}"}
        messages += [f"PS5VK_TEXTURE_DESCRIPTOR serial=7 draw=0 word={word} value={descriptor[word]}"
                     for word in range(12)]
        messages += [
            "PS5VK_GRAPHICS_SUBMIT serial=7 rc=0",
            "PS5VK_GRAPHICS_COMPLETED serial=7 image_bytes=8912896",
        ]
        messages += ["PS5VK_MIPMAP_STORAGE level={} offset={} pitch={} first={}".format(
            level, fields["offset"], fields["pitch"], fields["first"])
            for level, fields in EXPECTED_STORAGE.items()]
        messages += [
            "PS5VK_MIPMAP_READBACK levels=3 lod_bias={} red={} green={} blue={} unexpected=0 valid=1".format(
                bias, EXPECTED_HISTOGRAM[bias]["red"], EXPECTED_HISTOGRAM[bias]["green"],
                EXPECTED_HISTOGRAM[bias]["blue"]),
            "PS5VK_VIDEO_PRESENTED token=1 fence=0 matching_event=1 hold_seconds=0",
            "PS5VK_GRAPHICS_REUSE_END frame=0 slot=0 displayed=0",
            "PS5VK_COMPUTE_END rounds=6 dispatches=12",
            "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
            "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE",
        ]
        rows = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=abc"]
        rows += [f"{i}\t{i}\tMARK\t{message}" for i, message in enumerate(messages, 1)]
        rows.append(f"BYE seq={len(messages)} reason=graphics-api-end")
        log = ("\n".join(rows) + "\n").encode()
        metadata = {"sha256": hashlib.sha256(log).hexdigest(), "clean": True,
                    "bye": True, "gaps": [], "transport": "tcp",
                    "protocol": "ps5log/1", "records": len(messages),
                    "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "abc"}}
        artifact = {"stage": "graphics-api-native-presentation-reuse",
                    "runtime_graphics": True, "runtime_sdk": True,
                    "scissor_probe": 12, "geometry_fixture": "sampled-image-mipmaps",
                    "mip_lod_bias": bias,
                    "termination": "shell-close-after-cleanup",
                    "files": {"eboot.bin": "a" * 64}}
        return log, metadata, artifact

    def test_accepts_exact_explicit_lod_witness(self):
        self.assertTrue(validate(*self.fixture())["explicit_lod"])

    def test_accepts_both_core_lod_bias_boundaries(self):
        for bias in (-2, 2):
            with self.subTest(bias=bias):
                self.assertEqual(validate(*self.fixture(bias))["mip_lod_bias"], bias)

    def test_public_sdk_run_does_not_require_private_descriptor_tracing(self):
        log, metadata, artifact = self.fixture()
        rows = [row for row in log.decode().splitlines()
                if "PS5VK_TEXTURE_DESCRIPTOR" not in row]
        messages = [row.split("\t", 3)[-1] for row in rows[1:-1]]
        rebuilt = [rows[0]] + [f"{i}\t{i}\tMARK\t{message}"
            for i, message in enumerate(messages, 1)]
        rebuilt.append(f"BYE seq={len(messages)} reason=graphics-api-end")
        log = ("\n".join(rebuilt) + "\n").encode()
        metadata.update(sha256=hashlib.sha256(log).hexdigest(), records=len(messages))
        self.assertTrue(validate(log, metadata, artifact)["explicit_lod"])

    def test_rejects_missing_color_or_false_success(self):
        for old, new in ((b"blue=20736", b"blue=0"), (b"valid=1", b"valid=0")):
            with self.subTest(old=old):
                log, metadata, artifact = self.fixture()
                log = log.replace(old, new)
                metadata["sha256"] = hashlib.sha256(log).hexdigest()
                with self.assertRaisesRegex(ValueError, "explicit mip readback"):
                    validate(log, metadata, artifact)

    def test_rejects_descriptor_or_storage_drift(self):
        for old, new, error in ((b"value=90020fac", b"value=90000fac", "mipmap descriptor"),
                                (b"level=2 offset=0", b"level=2 offset=256", "mipmap backing")):
            with self.subTest(old=old):
                log, metadata, artifact = self.fixture()
                log = log.replace(old, new)
                metadata["sha256"] = hashlib.sha256(log).hexdigest()
                with self.assertRaisesRegex(ValueError, error):
                    validate(log, metadata, artifact)

        log, metadata, artifact = self.fixture()
        line = next(row for row in log.splitlines() if b"word=11" in row)
        log = log.replace(line + b"\n", b"")
        rows = log.decode().splitlines()
        messages = [row.split("\t", 3)[-1] for row in rows[1:-1]]
        rebuilt = [rows[0]] + [f"{i}\t{i}\tMARK\t{message}"
            for i, message in enumerate(messages, 1)]
        rebuilt.append(f"BYE seq={len(messages)} reason=graphics-api-end")
        log = ("\n".join(rebuilt) + "\n").encode()
        metadata.update(sha256=hashlib.sha256(log).hexdigest(), records=len(messages))
        with self.assertRaisesRegex(ValueError, "complete descriptor"):
            validate(log, metadata, artifact)

    def test_rejects_wrong_profile(self):
        log, metadata, artifact = self.fixture()
        artifact["runtime_sdk"] = False
        with self.assertRaisesRegex(ValueError, "artifact profile"):
            validate(log, metadata, artifact)


if __name__ == "__main__":
    unittest.main()
