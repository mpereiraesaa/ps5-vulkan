import hashlib
import unittest
from tools.verify_graphics_presentation import REFERENCE_SELF, expected_messages, validate


class GraphicsPresentationTests(unittest.TestCase):
    def fixture(self, edit=None):
        messages = [m.replace("HANDLE", "1234") for m in expected_messages()]
        if edit:
            edit(messages)
        log = ("HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=test\n" +
               "".join(f"{i}\t{i * 10_000_000_000}\tMARK\t{m}\n"
                       for i, m in enumerate(messages, 1)) +
               f"BYE seq={len(messages)} reason=graphics-api-end\n").encode()
        manifest = dict(sha256=hashlib.sha256(log).hexdigest(), transport="tcp", protocol="ps5log/1",
                        clean=True, bye=True, gaps=[], last_seq=len(messages), run_id="synthetic",
                        identity=dict(title="PPSA99994", app="ps5vk", boot="test"))
        artifact = dict(stage="graphics-graphics-api-native-presentation", submit_enabled=True,
                        files={"eboot.bin": REFERENCE_SELF})
        return log, manifest, artifact

    def test_reference(self):
        self.assertEqual(validate(*self.fixture())["presentations"], 3)

    def test_every_record_required(self):
        for i in range(len(expected_messages())):
            with self.subTest(i=i), self.assertRaises(ValueError):
                validate(*self.fixture(lambda m: m.pop(i)))

    def test_faults(self):
        for old, new in [("fence=0", "fence=1"), ("matching_event=1", "matching_event=0"),
                         ("token=1440", "token=1920"), ("valid=1", "valid=0"),
                         ("bad_sum=0", "bad_sum=1"), ("bad_alpha=0", "bad_alpha=1"),
                         ("allocations_bytes=0", "allocations_bytes=1"),
                         ("buffers=2", "buffers=1"), ("handle=1234", "handle=-1"),
                         ("serial=2", "serial=1")]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(lambda m: m.__setitem__(slice(None),
                    [s.replace(old, new) for s in m])))

    def test_release_before_close(self):
        def edit(messages):
            i = next(i for i, s in enumerate(messages) if s.startswith("PS5VK_VIDEO_CLOSED"))
            messages[i], messages[i+1] = messages[i+1], messages[i]
        with self.assertRaises(ValueError):
            validate(*self.fixture(edit))

    def test_wrong_artifact_transport_or_hash(self):
        for key, value in [("transport", "udp"), ("clean", False), ("gaps", [1]),
                           ("bye", False), ("sha256", "wrong")]:
            log, manifest, artifact = self.fixture(); manifest[key] = value
            with self.assertRaises(ValueError):
                validate(log, manifest, artifact)
        log, manifest, artifact = self.fixture(); artifact["files"]["eboot.bin"] = "unknown"
        with self.assertRaises(ValueError):
            validate(log, manifest, artifact)

    def test_short_hold(self):
        log, manifest, artifact = self.fixture()
        lines = log.decode().splitlines()
        i = next(i for i, s in enumerate(lines) if "PS5VK_VIDEO_CLOSED" in s)
        parts = lines[i].split("\t"); parts[1] = str((i-1)*10_000_000_000+1)
        lines[i] = "\t".join(parts)
        log = ("\n".join(lines)+"\n").encode()
        manifest["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaises(ValueError):
            validate(log, manifest, artifact)

    def test_corrupt_sequence_clock_severity_and_hello(self):
        for column, value in [(0, "99"), (1, "-1"), (2, "ERR")]:
            log, manifest, artifact = self.fixture()
            lines = log.decode().splitlines()
            parts = lines[2].split("\t"); parts[column] = value
            lines[2] = "\t".join(parts)
            log = ("\n".join(lines)+"\n").encode()
            manifest["sha256"] = hashlib.sha256(log).hexdigest()
            with self.subTest(column=column), self.assertRaises(ValueError):
                validate(log, manifest, artifact)
        log, manifest, artifact = self.fixture()
        log = log.replace(b"boot=test", b"boot=other")
        manifest["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaises(ValueError):
            validate(log, manifest, artifact)
