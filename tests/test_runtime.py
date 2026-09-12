import hashlib
import unittest
from tools.verify_runtime import validate, SHADER

class RuntimeTests(unittest.TestCase):
    def fixture(self, change=None):
        messages = ["agc_load=0", "agc_init=0",
            f"PS5VK_PREPARED shader={SHADER} dwords=82 submit_enabled=1 dma_only=0",
            "PS5VK_SUBMIT rc=0", "PS5VK_COMPLETION observed=13579bdf2468ace0",
            "PS5VK_VISIBILITY control_before_flush=00000000",
            "PS5VK_RESULT completion=token outputs=0 inputs=0 guards=0 first=18446744073709551615",
            "PS5VK_CLEANUP_BEGIN result=0", "PS5VK_CLEANUP_END result=0 allocations_live=0",
            "compute_run=0", "agc_unload=0", "PS5VK_NATIVE_END"]
        if change:
            change(messages)
        log = ("HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=test\n" +
               "".join(f"{i}\t{i}\tMARK\t{m}\n" for i, m in enumerate(messages, 1)) +
               f"BYE seq={len(messages)} reason=native-end\n").encode()
        manifest = dict(sha256=hashlib.sha256(log).hexdigest(), transport="tcp",
            protocol="ps5log/1", clean=True, bye=True, gaps=[], last_seq=len(messages),
            identity=dict(title="PPSA99994", app="ps5vk", boot="test"), run_id="synthetic")
        return log, manifest

    def test_pass(self):
        self.assertEqual(validate(*self.fixture())["results"], 1024)

    def test_suspend_protocol_and_fail_closed(self):
        def insert(messages):
            messages.insert(4,"PS5VK_SUSPEND_POINT rc=0")
        self.assertEqual(validate(*self.fixture(insert),require_suspend=True)["results"],1024)
        with self.assertRaises(ValueError):
            validate(*self.fixture(),require_suspend=True)
        with self.assertRaises(ValueError):
            validate(*self.fixture(insert))
        for index,rc in [(4,-1),(3,0),(5,0)]:
            with self.subTest(index=index,rc=rc),self.assertRaises(ValueError):
                validate(*self.fixture(lambda m:m.insert(index,f"PS5VK_SUSPEND_POINT rc={rc}")),
                         require_suspend=True)

    def test_each_required_record_is_required(self):
        for index in range(12):
            with self.subTest(index=index), self.assertRaises(ValueError):
                validate(*self.fixture(lambda m: m.pop(index)))

    def test_zero_fence_or_wrong_output_rejected(self):
        for index, value in [(4, "PS5VK_COMPLETION observed=0"),
                              (6, "PS5VK_RESULT completion=token outputs=1 inputs=0 guards=0 first=0")]:
            with self.assertRaises(ValueError):
                validate(*self.fixture(lambda m: m.__setitem__(index, value)))

    def test_delay_and_reordered_cleanup_rejected(self):
        for edit in [lambda m: m.append("PS5VK_VISIBILITY_SAMPLE sample=1"),
                     lambda m: m.insert(0, m.pop(8))]:
            with self.assertRaises(ValueError):
                validate(*self.fixture(edit))

    def test_tamper_and_gaps_rejected(self):
        log, manifest = self.fixture()
        with self.assertRaises(ValueError):
            validate(log + b"x", manifest)
        manifest["gaps"] = [1]
        with self.assertRaises(ValueError):
            validate(log, manifest)
