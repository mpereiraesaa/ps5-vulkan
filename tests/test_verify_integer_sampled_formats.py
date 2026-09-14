import copy
import hashlib
import unittest

from tools.verify_integer_sampled_formats import CASES, EXPECTED_PIXELS, validate


def fixture(sign="uint"):
    lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=b"]
    seq = 0
    def add(message):
        nonlocal seq
        seq += 1
        lines.append(f"{seq}\t{seq}\tMARK\t{message}")
    for case, (name, number, texel_bytes, components, expected) in enumerate(CASES[sign]):
        for _ in range(6):
            add("PS5VK_COMPUTE_RESULT round=0")
        add("PS5VK_COMPUTE_END rounds=6 dispatches=12")
        add(f"PS5VK_INTEGER_SAMPLED_INPUT case={case} name={name} sign={sign} format={number} components={components} bytes_per_texel={texel_bytes} expected_bgra={expected}")
        add("PS5VK_GRAPHICS_SUBMIT rc=0")
        add("PS5VK_GRAPHICS_COMPLETED")
        add(f"PS5VK_INTEGER_SAMPLED_READBACK case={case} name={name} sign={sign} format={number} expected_bgra={expected} expected={EXPECTED_PIXELS} other=0 first_other=00000000 valid=1")
        add("PS5VK_VIDEO_PRESENTED")
        add("PS5VK_GRAPHICS_REUSE_END")
        for _ in range(6):
            add("PS5VK_COMPUTE_RESULT round=0")
        add("PS5VK_COMPUTE_END rounds=6 dispatches=12")
    add("PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0")
    add("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    lines.append(f"BYE seq={seq} reason=graphics-api-end")
    log=("\n".join(lines)+"\n").encode()
    metadata={"sha256":hashlib.sha256(log).hexdigest(),"clean":True,"bye":True,
              "gaps":[],"transport":"tcp","protocol":"ps5log/1","records":seq,
              "identity":{"title":"PPSA99994","app":"ps5vk","boot":"b"}}
    artifact={"stage":"graphics-api-native-presentation-reuse","scissor_probe":10,
              "geometry_fixture":"integer-sampled-formats","integer_sampled_sign":sign,
              "termination":"shell-close-after-cleanup",
              "graphics":{"source":f"experiments/graphics/scene3d-{sign}.pipe"},
              "files":{"eboot.bin":"a"*64}}
    return log,metadata,artifact


class IntegerSampledVerifier(unittest.TestCase):
    def test_fixture_covers_exactly_half_of_1080p(self):
        self.assertEqual(EXPECTED_PIXELS, 1_036_800)

    def test_accepts_exact_uint_and_sint_streams(self):
        for sign in CASES:
            log,metadata,artifact=fixture(sign)
            self.assertTrue(validate(log,metadata,artifact)["typed_integer_sampling"])

    def test_rejects_sign_or_oracle_mismatch(self):
        log,metadata,artifact=fixture("uint")
        bad=copy.deepcopy(artifact);bad["integer_sampled_sign"]="sint"
        with self.assertRaises(ValueError): validate(log,metadata,bad)
        altered=log.replace(f"expected={EXPECTED_PIXELS}".encode(),b"expected=1",1)
        bad_meta=copy.deepcopy(metadata);bad_meta["sha256"]=hashlib.sha256(altered).hexdigest()
        with self.assertRaises(ValueError): validate(altered,bad_meta,artifact)

    def test_rejects_incomplete_or_error_stream(self):
        log,metadata,artifact=fixture("uint")
        altered=log.replace(b"\tMARK\tPS5VK_GRAPHICS_SUBMIT",b"\tERR\tPS5VK_GRAPHICS_SUBMIT",1)
        bad_meta=copy.deepcopy(metadata);bad_meta["sha256"]=hashlib.sha256(altered).hexdigest()
        with self.assertRaises(ValueError): validate(altered,bad_meta,artifact)


if __name__ == "__main__":
    unittest.main()
