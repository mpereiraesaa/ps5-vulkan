"""Shared storage host lifetime tests; no native GPU behavior asserted."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TessSharedStorageTests(unittest.TestCase):
    def test_lifetime_rollback_and_concurrent_references(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = str(Path(directory) / "shared-ring")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                "-I" + str(ROOT / "native"), "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "third_party/vulkan-headers/include"),
                str(ROOT / "native/tess_shared_storage.c"),
                str(ROOT / "tests/test_tess_shared_storage.c"), "-o", exe], check=True)
            subprocess.run([exe], check=True)
