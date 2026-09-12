"""SDK archives cannot retain source members removed from a later build."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.build_sdk import archive


class SdkArchiveTests(unittest.TestCase):
    def test_rebuild_replaces_members(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            old, new = root / "old.o", root / "new.o"
            old.write_bytes(b"old fixture")
            new.write_bytes(b"new fixture")
            output = root / "libfixture.a"
            archive("ar", output, [str(old)])
            archive("ar", output, [str(new)])
            members = subprocess.check_output(["ar", "t", str(output)], text=True).splitlines()
            self.assertEqual(members, ["new.o"])

    def test_failed_rebuild_preserves_previous_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "good.o"
            source.write_bytes(b"fixture")
            output = root / "libfixture.a"
            archive("ar", output, [str(source)])
            previous = output.read_bytes()
            with self.assertRaises(subprocess.CalledProcessError):
                archive("false", output, [str(source)])
            self.assertEqual(output.read_bytes(), previous)
