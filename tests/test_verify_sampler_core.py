import hashlib
import json
import unittest

from tools.verify_sampler_core import CASES, validate


class SamplerCoreVerifier(unittest.TestCase):
    def fixture(self):
        messages=[]
        for case,(name,uv,expected) in enumerate(CASES):
            messages += [
                f"PS5VK_SAMPLER_CORE_INPUT case={case} name={name} uv_milli={uv} expected_bgra={expected}",
                f"PS5VK_GRAPHICS_SUBMIT serial={case+1} rc=0",
                f"PS5VK_GRAPHICS_COMPLETED serial={case+1} image_bytes=8912896",
                f"PS5VK_SAMPLER_CORE_READBACK case={case} name={name} expected_bgra={expected} expected=12000 other=0 valid=1",
                f"PS5VK_VIDEO_PRESENTED token={case+1} fence=0 matching_event=1",
                f"PS5VK_GRAPHICS_REUSE_END frame=0 slot=0 displayed=0",
            ]
        messages += ["PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
                     "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE"]
        rows=["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=abc"]
        rows += [f"{i}\t{i}\tMARK\t{message}" for i,message in enumerate(messages,1)]
        rows.append(f"BYE seq={len(messages)} reason=graphics-api-end")
        log=("\n".join(rows)+"\n").encode()
        meta={"sha256":hashlib.sha256(log).hexdigest(),"clean":True,"bye":True,
              "gaps":[],"transport":"tcp","protocol":"ps5log/1","records":len(messages),
              "identity":{"title":"PPSA99994","app":"ps5vk","boot":"abc"}}
        artifact={"stage":"graphics-api-native-presentation-reuse","scissor_probe":6,
                  "geometry_fixture":"sampler-core-addressing","termination":"shell-close-after-cleanup",
                  "files":{"eboot.bin":"a"*64}}
        return log,meta,artifact

    def test_accepts_complete_gpu_witness(self):
        result=validate(*self.fixture())
        self.assertEqual(result["cases"],4)
        self.assertEqual(result["fixed_border_colors"],3)

    def test_rejects_wrong_border_output(self):
        log,meta,artifact=self.fixture()
        log=log.replace(b"expected=12000 other=0 valid=1",b"expected=12000 other=1 valid=1",1)
        meta["sha256"]=hashlib.sha256(log).hexdigest()
        with self.assertRaisesRegex(ValueError,"GPU sampler oracle"):
            validate(log,meta,artifact)

    def test_rejects_incomplete_transport(self):
        log,meta,artifact=self.fixture();meta["clean"]=False
        with self.assertRaisesRegex(ValueError,"transport"):
            validate(log,meta,artifact)


if __name__=="__main__":
    unittest.main()
