import hashlib
import unittest
from tools.verify_compute_runtime import REFERENCE_SELF, SUSPEND_SELF, CURRENT_SELF, expected_messages, validate


class ComputeRuntimeTests(unittest.TestCase):
    def fixture(self, edit=None, suspend=False):
        messages = expected_messages(suspend)
        if edit:
            edit(messages)
        log = ("HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=test\n" +
               "".join(f"{i}\t{i}\tMARK\t{message}\n" for i, message in enumerate(messages, 1)) +
               f"BYE seq={len(messages)} reason=compute-end\n").encode()
        manifest = dict(sha256=hashlib.sha256(log).hexdigest(), transport="tcp", protocol="ps5log/1",
                        clean=True, bye=True, gaps=[], last_seq=len(messages), run_id="synthetic",
                        identity=dict(title="PPSA99994", app="ps5vk", boot="test"))
        artifact = dict(stage="compute-compute-api", submit_enabled=True,
                        files={"eboot.bin": SUSPEND_SELF if suspend else REFERENCE_SELF})
        return log, manifest, artifact

    def test_reference_workload(self):
        result = validate(*self.fixture())
        self.assertEqual(result["dispatches"], 12)
        self.assertEqual(result["guard_words_checked"], 3 * (4096 - 1024) * 6)

    def test_current_build_requires_full_suspend_protocol(self):
        for suspend in (True, False):
            log, manifest, artifact = self.fixture(suspend=suspend)
            artifact["files"]["eboot.bin"] = CURRENT_SELF
            if suspend:
                self.assertEqual(validate(log, manifest, artifact)["dispatches"], 12)
            else:
                with self.assertRaises(ValueError):
                    validate(log, manifest, artifact)

    def test_suspend_workload_and_required_success_records(self):
        self.assertEqual(validate(*self.fixture(suspend=True))["dispatches"], 12)
        positions = [i for i, m in enumerate(expected_messages(True)) if "SUSPEND_POINT" in m]
        self.assertEqual(len(positions), 12)
        for index in positions:
            for edit in (lambda m: m.pop(index),
                         lambda m: m.__setitem__(index, m[index].replace("rc=0", "rc=-1"))):
                with self.subTest(index=index), self.assertRaises(ValueError):
                    validate(*self.fixture(edit, suspend=True))

    def test_artifact_selects_exact_protocol(self):
        for suspend in (False, True):
            log, manifest, artifact = self.fixture(suspend=suspend)
            artifact["files"]["eboot.bin"] = REFERENCE_SELF if suspend else SUSPEND_SELF
            with self.assertRaises(ValueError):
                validate(log, manifest, artifact)

    def test_every_record_required(self):
        for index in range(len(expected_messages())):
            with self.subTest(index=index), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.pop(index)))

    def test_changed_results_tokens_offsets_and_teardown(self):
        for old, new in [("token=100000001", "token=0"), ("outputs=0", "outputs=1"),
                         ("guards=0", "guards=1"), ("descriptor_offset=256", "descriptor_offset=0"),
                         ("binding_offset=256", "binding_offset=0"), ("gcr=0070f528", "gcr=06000528"),
                         ("allocations_bytes=0", "allocations_bytes=65536"),
                         ("first_program=1", "first_program=0")]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.__setitem__(slice(None),
                    [message.replace(old, new) for message in messages])))

    def test_reordered_duplicate_error_and_wrong_artifact(self):
        for edit in [lambda m: m.insert(0, m.pop(10)), lambda m: m.append(m[-1])]:
            with self.assertRaises(ValueError):
                validate(*self.fixture(edit))
        log, manifest, artifact = self.fixture()
        artifact["files"]["eboot.bin"] = "wrong"
        with self.assertRaises(ValueError):
            validate(log, manifest, artifact)

    def test_transport_and_corruption(self):
        for key, value in [("transport", "udp"), ("clean", False), ("gaps", [1]), ("bye", False)]:
            log, manifest, artifact = self.fixture(); manifest[key] = value
            with self.assertRaises(ValueError):
                validate(log, manifest, artifact)
        log, manifest, artifact = self.fixture()
        with self.assertRaises(ValueError):
            validate(log + b"x", manifest, artifact)
