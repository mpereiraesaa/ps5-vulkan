import hashlib
import unittest
from tools.verify_graphics_runtime import validate


def workload():
    messages=["PS5VK_RUNTIME_GRAPHICS_INPUT absent_from_offline_library=1"]
    for group,(viewport,changed) in enumerate((("1920x1080",471744),("1440x810",265362),("960x540",117936))):
        messages.append(f"PS5VK_RUNTIME_GRAPHICS_CACHE rc=0 hit={int(group>0)} compiled_pairs=1 hits={group} misses=1 entries=1 bytes=9316")
        for frame in range(6):
            serial=group*6+frame+1
            messages += [f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
                f"PS5VK_GRAPHICS_SUSPEND_POINT serial={serial} rc=0",
                f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=8912896",
                f"PS5VK_GRAPHICS_API_READBACK changed_words={changed} total_words=2228224 bad_alpha=0 bad_sum=0 viewport={viewport} valid=1",
                f"PS5VK_VIDEO_SUBMIT token={serial} rc=0",
                f"PS5VK_VIDEO_SUSPEND_POINT token={serial} rc=0",
                f"PS5VK_VIDEO_PRESENTED token={serial} matching_event=1"]
    return messages+["PS5VK_RUNTIME_GRAPHICS_CACHE_DESTROYED",
        "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
        "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE",
        "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1"]


def verify(messages):
    log=("HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=test\n"+
         "".join(f"{i}\t{i}\tMARK\t{m}\n" for i,m in enumerate(messages,1))+
         f"BYE seq={len(messages)} reason=graphics-api-end\n").encode()
    receipt={"sha256":hashlib.sha256(log).hexdigest(),"protocol":"ps5log/1","transport":"tcp",
             "clean":True,"bye":True,"gaps":[],"last_seq":len(messages),
             "identity":{"title":"PPSA99994","app":"ps5vk","boot":"test"}}
    artifact={"title":"PPSA99994","runtime_graphics":True,"submit_enabled":True,
              "termination":"shell-close-after-cleanup","files":{"eboot.bin":"0"*64}}
    return validate(log,receipt,artifact)


class RuntimeGraphicsEvidence(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(verify(workload())["cache_hits"],2)

    def test_rejects_false_success(self):
        for before,after in (("bad_sum=0","bad_sum=1"),
                             ("matching_event=1","matching_event=0"),
                             ("hits=2","hits=1"),
                             ("allocations_bytes=0","allocations_bytes=1"),
                             ("GRAPHICS_COMPLETED serial=1 ","GRAPHICS_COMPLETED serial=999 "),
                             ("absent_from_offline_library=1","absent_from_offline_library=0")):
            with self.subTest(before=before),self.assertRaises(ValueError):
                verify([m.replace(before,after) for m in workload()])

    def test_missing_frame_with_valid_sequence_still_fails(self):
        messages=workload()
        del messages[2:9]
        with self.assertRaises(ValueError):verify(messages)
