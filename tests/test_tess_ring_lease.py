"""Host lifecycle fault injection; does not claim native driver behavior."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TessRingLeaseTests(unittest.TestCase):
    def test_transitions_and_driver_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = str(Path(directory) / "ring-lease")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "native"),
                str(ROOT / "native/tess_ring_lease.c"),
                str(ROOT / "tests/test_tess_ring_lease.c"), "-o", exe], check=True)
            subprocess.run([exe], check=True)
