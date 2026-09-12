import hashlib
import unittest

from tools.verify_consumer_resource_abi import APP, TITLE, validate


MESSAGES = [
    "PS5VK_CONSUMER_BOOT mode=finite sdk_version=0.1",
    "PS5VK_CONSUMER_COMPUTE_START",
    "PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED",
    "PS5VK_QUEUE_PREPARED serial=1 dispatches=1",
    "PS5VK_QUEUE_SUBMIT serial=1 index=0 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=1 index=0 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=1 index=0 token=100000001 gcr=0070f528",
    "PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS sets=3 storage=2 uniform=1 texel=1 "
    "elements=64 mismatches=0 guard_words=128 guard_mismatches=0",
    "PS5VK_CONSUMER_TEST_SUCCESS",
    "PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1",
    "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1",
]


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
        }
        return log, receipt, artifact

    def test_reference(self):
        result = validate(*self.fixture())
        self.assertEqual(result["descriptor_sets"], 3)
        self.assertEqual(result["guard_words_checked"], 128)

    def test_every_witness_is_required(self):
        for index in range(len(MESSAGES)):
            with self.subTest(index=index), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.pop(index)))

    def test_corrupt_or_false_success_is_rejected(self):
        replacements = (("mismatches=0", "mismatches=1"),
                        ("guard_mismatches=0", "guard_mismatches=1"),
                        ("sets=3", "sets=1"), ("rc=0", "rc=-1"),
                        ("dispatches=1", "dispatches=2"),
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
        ):
            args = list(self.fixture())
            mutate(*args)
            with self.assertRaises(ValueError):
                validate(*args)


if __name__ == "__main__":
    unittest.main()
