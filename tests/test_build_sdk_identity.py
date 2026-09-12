import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.build_sdk import validate_compiler_archive


class NativeCompilerIdentityTests(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory()
        archive = Path(temporary.name) / "libpsbc.ps5.a"
        archive.write_bytes(b"current-native-compiler")
        identity = {
            "schema": 1,
            "target": "ps5",
            "source_commit": "a" * 40,
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        }
        archive.with_suffix(".json").write_text(json.dumps(identity))
        return temporary, archive, identity

    def test_matching_archive_is_accepted(self):
        temporary, archive, _ = self.fixture()
        with temporary:
            validate_compiler_archive(archive, "a" * 40)

    def test_stale_revision_and_tampered_archive_are_rejected(self):
        temporary, archive, _ = self.fixture()
        with temporary:
            with self.assertRaises(RuntimeError):
                validate_compiler_archive(archive, "b" * 40)
            archive.write_bytes(b"stale-native-compiler")
            with self.assertRaises(RuntimeError):
                validate_compiler_archive(archive, "a" * 40)

    def test_missing_or_malformed_stamp_is_rejected(self):
        temporary, archive, _ = self.fixture()
        with temporary:
            archive.with_suffix(".json").unlink()
            with self.assertRaises(RuntimeError):
                validate_compiler_archive(archive, "a" * 40)
            archive.with_suffix(".json").write_text("not json")
            with self.assertRaises(json.JSONDecodeError):
                validate_compiler_archive(archive, "a" * 40)


if __name__ == "__main__":
    unittest.main()
