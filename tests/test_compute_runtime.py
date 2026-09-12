import hashlib
import unittest
from tools.verify_compute_runtime import PROFILE_STAGE, PROFILE_TITLE, PROFILE_APP, expected_messages, validate

VALID_EBOOT_SHA256 = "1b11840180edf74795e8cdf9d0bfa76bfb91967c743c4b46a0d205e5dbace281"


class ComputeRuntimeTests(unittest.TestCase):
    def fixture(self, edit=None, suspend=True):
        messages = expected_messages(suspend)
        if edit:
            edit(messages)
        log = (f"HELLO ps5log/1 title={PROFILE_TITLE} app={PROFILE_APP} boot=test\n" +
               "".join(f"{i}\t{i}\tMARK\t{message}\n" for i, message in enumerate(messages, 1)) +
               f"BYE seq={len(messages)} reason=compute-end\n").encode()
        manifest = dict(sha256=hashlib.sha256(log).hexdigest(), transport="tcp", protocol="ps5log/1",
                        clean=True, bye=True, gaps=[], last_seq=len(messages), run_id="synthetic",
                        identity=dict(title=PROFILE_TITLE, app=PROFILE_APP, boot="test"))
        artifact = dict(title=PROFILE_TITLE, stage=PROFILE_STAGE, submit_enabled=True,
                        files={"eboot.bin": VALID_EBOOT_SHA256})
        return log, manifest, artifact

    def test_builder_verifier_agreement(self):
        result = validate(*self.fixture())
        self.assertEqual(result["dispatches"], 12)
        self.assertEqual(result["rounds"], 6)
        self.assertEqual(result["deployment_self_sha256"], VALID_EBOOT_SHA256)
        self.assertTrue(result["clean_tcp"])

    def test_suspend_workload_and_required_success_records(self):
        positions = [i for i, m in enumerate(expected_messages(True)) if "SUSPEND_POINT" in m]
        self.assertEqual(len(positions), 12)
        for index in positions:
            for edit in (lambda m: m.pop(index),
                         lambda m: m.__setitem__(index, m[index].replace("rc=0", "rc=-1"))):
                with self.subTest(index=index), self.assertRaises(ValueError):
                    validate(*self.fixture(edit, suspend=True))

    def test_unknown_profiles_and_mismatches(self):
        for stage in ("compute-compute-api", "graphics-api", "m3-compute-api", "unknown", ""):
            log, manifest, artifact = self.fixture()
            artifact["stage"] = stage
            with self.subTest(stage=stage), self.assertRaisesRegex(ValueError, "profile mismatch"):
                validate(log, manifest, artifact)

        log, manifest, artifact = self.fixture()
        artifact["submit_enabled"] = False
        with self.assertRaisesRegex(ValueError, "submit must be enabled"):
            validate(log, manifest, artifact)

    def test_wrong_identities(self):
        # Artifact title mismatch
        log, manifest, artifact = self.fixture()
        artifact["title"] = "PPSA99999"
        with self.assertRaisesRegex(ValueError, "artifact title"):
            validate(log, manifest, artifact)

        # Artifact eboot missing or invalid
        for bad_eboot in ("", "not-a-hash", "1234", None):
            log, manifest, artifact = self.fixture()
            artifact["files"]["eboot.bin"] = bad_eboot
            with self.subTest(bad_eboot=bad_eboot), self.assertRaisesRegex(ValueError, "artifact identity"):
                validate(log, manifest, artifact)

        # Telemetry title or app mismatch
        for key, val in (("title", "WRONG"), ("app", "wrong_app")):
            log, manifest, artifact = self.fixture()
            manifest["identity"][key] = val
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "identity"):
                validate(log, manifest, artifact)

        # Hello boot token mismatch
        log, manifest, artifact = self.fixture()
        manifest["identity"]["boot"] = "other_boot"
        with self.assertRaisesRegex(ValueError, "hello identity"):
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
                         ("first_program=1", "first_program=0"),
                         ("dispatches=12", "dispatches=10")]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.__setitem__(slice(None),
                    [message.replace(old, new) for message in messages])))

    def test_sequence_gaps_and_clock(self):
        log, manifest, artifact = self.fixture()
        lines = log.decode().splitlines()

        # Reordered sequence
        reordered = lines[:1] + [lines[2], lines[1]] + lines[3:]
        reordered_bytes = "\n".join(reordered).encode() + b"\n"
        reordered_manifest = dict(manifest, sha256=hashlib.sha256(reordered_bytes).hexdigest())
        with self.assertRaisesRegex(ValueError, "sequence|ordering"):
            validate(reordered_bytes, reordered_manifest, artifact)

        # Timestamp inversion
        lines_inverted = list(lines)
        parts = lines_inverted[2].split("\t")
        parts[1] = "0"  # timestamp earlier than previous
        lines_inverted[2] = "\t".join(parts)
        tampered_log = "\n".join(lines_inverted).encode() + b"\n"
        tampered_manifest = dict(manifest, sha256=hashlib.sha256(tampered_log).hexdigest())
        with self.assertRaisesRegex(ValueError, "clock ordering"):
            validate(tampered_log, tampered_manifest, artifact)

    def test_incomplete_tails(self):
        # Missing BYE
        log, manifest, artifact = self.fixture()
        no_bye = "\n".join(log.decode().splitlines()[:-1]) + "\n"
        no_bye_bytes = no_bye.encode()
        m = dict(manifest, sha256=hashlib.sha256(no_bye_bytes).hexdigest())
        with self.assertRaises(ValueError):
            validate(no_bye_bytes, m, artifact)

        # Wrong BYE reason
        log, manifest, artifact = self.fixture()
        wrong_reason = log.decode().replace("reason=compute-end", "reason=m3-end").encode()
        m = dict(manifest, sha256=hashlib.sha256(wrong_reason).hexdigest())
        with self.assertRaisesRegex(ValueError, "bye"):
            validate(wrong_reason, m, artifact)

        # Truncated tail before compute-end
        log, manifest, artifact = self.fixture()
        lines = log.decode().splitlines()
        truncated = lines[:-3] + [f"BYE seq={len(lines)-3} reason=compute-end"]
        t_bytes = ("\n".join(truncated) + "\n").encode()
        m = dict(manifest, sha256=hashlib.sha256(t_bytes).hexdigest(), last_seq=len(lines)-3)
        with self.assertRaisesRegex(ValueError, "workload/lifecycle mismatch"):
            validate(t_bytes, m, artifact)

    def test_transport_and_corruption(self):
        for key, value in [("transport", "udp"), ("clean", False), ("gaps", [1]), ("bye", False)]:
            log, manifest, artifact = self.fixture()
            manifest[key] = value
            with self.assertRaises(ValueError):
                validate(log, manifest, artifact)
        log, manifest, artifact = self.fixture()
        with self.assertRaises(ValueError):
            validate(log + b"x", manifest, artifact)
