import hashlib
import unittest

from tools.verify_consumer_resource_abi import APP, TITLE, validate


MESSAGES = [
    "PS5VK_CONSUMER_BOOT mode=finite sdk_version=0.1",
    "PS5VK_CONSUMER_PHYSICAL_DEVICE api=00400000 vendor=1002 device=0000 "
    "heap=268435456 heap_flags=00000001 type_flags=00000003 "
    "queue_flags=00000003 storage=268435456 uniform=65536 texel=65536 "
    "push=256 allocations=2048 granularity=131072 map_align=64 "
    "texel_align=4 ubo_align=256 ssbo_align=256 atom=64 shared=65536 "
    "invocations=1024 hash=be169e1b",
    "PS5VK_CONSUMER_PHYSICAL_QUERIES devices=1 queues=1 two_call=1 "
    "tail_preserved=1 pnext_preserved=1 formats=5 image_supported=1 "
    "image_rejected=1",
    "PS5VK_CONSUMER_STORAGE_WIDTH_NEGOTIATED instance_ext=1 device_exts=3 "
    "storageBuffer8BitAccess=1 storageBuffer16BitAccess=1 narrow_arithmetic=0",
    "PS5VK_CONSUMER_BUFFER_TRANSFER_START",
    "PS5VK_CONSUMER_BUFFER_TRANSFER_SUCCESS copy_bytes=7 update_bytes=8 "
    "fill_bytes=20 whole_tail_bytes=3 guard_mismatches=0 hash=9a158222",
    "PS5VK_CONSUMER_BUFFER_TRANSFER_RETIRED",
    "PS5VK_CONSUMER_COMPUTE_START",
    "PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED",
    "PS5VK_QUEUE_PREPARED serial=5 dispatches=1",
    "PS5VK_QUEUE_SUBMIT serial=5 index=0 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=5 index=0 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=5 index=0 token=500000001 gcr=0070f528",
    "PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS sets=3 storage=2 uniform=1 texel=1 "
    "push_bytes=4 spec_constants=2 multiplier=5 extra_bias=11 addend=19 "
    "elements=64 mismatches=0 guard_words=128 guard_mismatches=0",
    "PS5VK_CONSUMER_STORAGE_WIDTH_START",
    "PS5VK_CONSUMER_STORAGE_WIDTH_PIPELINES_CREATED count=2",
    "PS5VK_QUEUE_PREPARED serial=6 dispatches=2",
    "PS5VK_QUEUE_SUBMIT serial=6 index=0 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=6 index=0 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=6 index=0 token=100000002 gcr=0070f528",
    "PS5VK_QUEUE_SUBMIT serial=6 index=1 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=6 index=1 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=6 index=1 token=100000003 gcr=0070f528",
    "PS5VK_CONSUMER_STORAGE_WIDTH_SUCCESS storage8=1 storage16=1 "
    "elements8=64 elements16=64 checksum8=9575e8c5 checksum16=603ddade "
    "mismatches8=0 mismatches16=0 guard_bytes8=4032 guard_bytes16=3968 "
    "guard_mismatches8=0 guard_mismatches16=0",
    "PS5VK_CONSUMER_STORAGE_WIDTH_RETIRED",
    "PS5VK_CONSUMER_SYNC_START",
    "PS5VK_QUEUE_PREPARED serial=8 dispatches=3",
    "PS5VK_QUEUE_PREPARED serial=10 dispatches=0",
    "PS5VK_QUEUE_SUBMIT serial=8 index=0 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=8 index=0 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=8 index=0 token=400000001 gcr=0070f528",
    "PS5VK_QUEUE_SUBMIT serial=8 index=1 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=8 index=1 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=8 index=1 token=400000002 gcr=0070f528",
    "PS5VK_QUEUE_SUBMIT serial=8 index=2 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=8 index=2 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=8 index=2 token=400000003 gcr=0070f528",
    "PS5VK_CONSUMER_SYNC_OBJECTS_SUCCESS host_set_reset=1 "
    "device_set_wait_reset=1 binary_signal_wait=1 semaphore_consumed=1",
    "PS5VK_CONSUMER_SYNC_SUCCESS producer_consumer=1 host_compute_host=1 "
    "local_size=128 waves32=4 lds_atomic=1 permutation=1 counter=128 "
    "sync_hash=467e2acd atomic_hash=1234abcd mismatches=0 guard_mismatches=0",
    "PS5VK_CONSUMER_SYNC_RETIRED",
    "PS5VK_CONSUMER_TEST_SUCCESS",
    "PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1",
    "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1",
]

FIXED_FUNCTION_MESSAGES = [
    "PS5VK_CONSUMER_GRAPHICS_START mode=finite",
    "PS5VK_CONSUMER_GRAPHICS_PIPELINE_COLD_CREATED",
    "PS5VK_CONSUMER_GRAPHICS_PIPELINE_WARM_CREATED",
    "PS5VK_CONSUMER_GRAPHICS_PIPELINE_DESTROYED refcount_verified=1",
    "PS5VK_CONSUMER_DYNAMIC_PIPELINE_CREATED viewport=1 scissor=1",
    "PS5VK_CONSUMER_PRESENT_SURFACE_CREATED buffers=2",
]
for frame in range(18):
    for phase in range(2):
        serial = 13 + frame * 2 + phase
        FIXED_FUNCTION_MESSAGES.extend([
            f"PS5VK_GRAPHICS_PREPARED serial={serial} draws=1 words=256",
            f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
            f"PS5VK_GRAPHICS_SUSPEND_POINT serial={serial} rc=0",
            f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=8388608",
        ])
        if phase == 0:
            FIXED_FUNCTION_MESSAGES.append(
                f"PS5VK_CONSUMER_DEPTH_REJECT frame={frame} nonblack=0 valid=1")
        else:
            FIXED_FUNCTION_MESSAGES.append(
                f"PS5VK_CONSUMER_READBACK frame={frame} slot={frame & 1} "
                "changed=471744 bad_alpha=0 bad_sum=0 valid=1")
FIXED_FUNCTION_MESSAGES.append("PS5VK_CONSUMER_PRESENT_SURFACE_DESTROYED")
MESSAGES[-3:-3] = FIXED_FUNCTION_MESSAGES


class ConsumerResourceAbiTests(unittest.TestCase):
    def fixture(self, edit=None):
        messages = list(MESSAGES)
        if edit:
            edit(messages)
        log = (f"HELLO ps5log/1 title={TITLE} app={APP} boot=test\n" +
               "".join(f"{i}\t{i}\tMARK\t{message}\n"
                       for i, message in enumerate(messages, 1)) +
               f"BYE seq={len(messages)} reason=consumer-finite-end\n").encode()
        receipt = {
            "sha256": hashlib.sha256(log).hexdigest(), "protocol": "ps5log/1",
            "transport": "tcp", "clean": True, "bye": True, "gaps": [],
            "raw_lines": 0, "last_seq": len(messages), "run_id": "synthetic",
            "identity": {"title": TITLE, "app": APP, "boot": "test"},
        }
        artifact = {
            "title": TITLE, "profile": "public-consumer-resource-abi",
            "submit_enabled": True, "files": {"eboot.bin": "a" * 64},
            "buffer_transfer": {
                "api": "Vulkan 1.0", "copy_bytes": 7,
                "update_bytes": 8, "fill_bytes": 20,
                "whole_tail_bytes": 3,
            },
            "storage_width": {
                "storageBuffer8BitAccess": True,
                "storageBuffer16BitAccess": True,
                "shaderInt8": False,
                "shaderInt16": False,
                "storage8_spirv_sha256": "b" * 64,
                "storage16_spirv_sha256": "c" * 64,
            },
            "synchronization": {
                "api": "Vulkan 1.0", "local_size": 128, "wave_size": 32,
                "binary_semaphore": True, "host_event": True,
                "device_event": True,
                "sync_producer_spirv_sha256": "d" * 64,
                "sync_consumer_spirv_sha256": "e" * 64,
                "shared_atomic_multiwave_spirv_sha256": "f" * 64,
            },
            "fixed_function": {
                "api": "Vulkan 1.0", "width": 1920, "height": 1080,
                "frames": 18, "color_format": "VK_FORMAT_B8G8R8A8_UNORM",
                "depth_format": "VK_FORMAT_D32_SFLOAT", "samples": 1,
                "load_preservation": True, "dynamic_viewport": True,
                "dynamic_scissor": True,
            },
        }
        return log, receipt, artifact

    def test_reference(self):
        result = validate(*self.fixture())
        self.assertEqual(result["descriptor_sets"], 3)
        self.assertEqual(result["guard_words_checked"], 128)
        self.assertEqual(result["push_constant_bytes"], 4)
        self.assertEqual(result["specialization_constants"], 2)
        self.assertEqual(result["storage8_elements_checked"], 64)
        self.assertEqual(result["storage16_elements_checked"], 64)
        self.assertEqual(result["narrow_guard_bytes_checked"], 8000)
        self.assertEqual(result["physical_device_report_fnv1a32"], "be169e1b")
        self.assertFalse(result["reported_host_coherent"])
        self.assertEqual(result["multiwave_atomic_lanes_checked"], 128)
        self.assertEqual(result["wave32_count"], 4)

    def test_every_witness_is_required(self):
        for index in range(len(MESSAGES)):
            with self.subTest(index=index), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.pop(index)))

    def test_corrupt_or_false_success_is_rejected(self):
        replacements = (("mismatches=0", "mismatches=1"),
                        ("guard_mismatches=0", "guard_mismatches=1"),
                        ("sets=3", "sets=1"), ("rc=0", "rc=-1"),
                        ("serial=5 dispatches=1", "serial=5 dispatches=3"),
                        ("hash=9a158222", "hash=00000000"),
                        ("checksum8=9575e8c5", "checksum8=00000000"),
                        ("checksum16=603ddade", "checksum16=00000000"),
                        ("sync_hash=467e2acd", "sync_hash=00000000"),
                        ("permutation=1", "permutation=0"),
                        ("heap=268435456", "heap=268435455"),
                        ("hash=be169e1b", "hash=00000000"),
                        ("pnext_preserved=1", "pnext_preserved=0"),
                        ("allocations=1", "allocations=0"))
        for old, new in replacements:
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.__setitem__(
                    slice(None), [message.replace(old, new) for message in messages])))

    def test_transport_identity_and_bye_are_fail_closed(self):
        for mutate in (
            lambda log, receipt, artifact: receipt.update(gaps=[{"expected": 2, "got": 3}]),
            lambda log, receipt, artifact: receipt.update(clean=False),
            lambda log, receipt, artifact: artifact.update(profile="other"),
            lambda log, receipt, artifact: artifact["files"].update({"eboot.bin": "bad"}),
            lambda log, receipt, artifact: artifact["storage_width"].update(shaderInt8=True),
            lambda log, receipt, artifact: artifact["storage_width"].update(storage8_spirv_sha256="bad"),
        ):
            args = list(self.fixture())
            mutate(*args)
            with self.assertRaises(ValueError):
                validate(*args)


if __name__ == "__main__":
    unittest.main()
