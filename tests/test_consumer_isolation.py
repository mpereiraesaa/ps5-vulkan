import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
DIST_SDK = ROOT / "dist-sdk"
CONSUMER_DIR = ROOT / "examples/native_consumer"
BUILD_DIR = CONSUMER_DIR / "build"


class TestConsumerIsolation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Build consumer using the dedicated build tool
        cmd = ["python3", str(ROOT / "tools/build_consumer.py")]
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"build_consumer.py failed:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")

    def test_dependency_isolation(self):
        """Inspect compiler dependency (.d) files to verify no private headers are included."""
        dep_file = BUILD_DIR / "main.d"
        self.assertTrue(dep_file.is_file(), f"Expected dependency file {dep_file} to exist")
        content = dep_file.read_text()
        headers = re.findall(r'(\S+\.h|\S+\.hpp)', content)
        self.assertGreater(len(headers), 0, "Expected dependency list to contain headers")

        for h in headers:
            hp = Path(h).resolve()
            is_sdk = str(hp).startswith(str(DIST_SDK / "include"))
            is_local = str(hp).startswith(str(CONSUMER_DIR))
            is_crt = "ps5-native-app-boilerplate" in str(hp) or str(hp).startswith("/usr/")
            self.assertTrue(
                is_sdk or is_local or is_crt,
                f"Isolation violation: consumer depends on unauthorized header {hp}"
            )
            # Explicit negative checks
            self.assertNotIn(str(ROOT / "src"), str(hp), f"Consumer leaked src/ header: {hp}")
            self.assertNotIn(str(ROOT / "native"), str(hp), f"Consumer leaked native/ header: {hp}")

    def test_symbol_isolation(self):
        """Inspect consumer object and ensure no unexported/private ps5vk symbols are referenced."""
        obj_file = BUILD_DIR / "main.o"
        self.assertTrue(obj_file.is_file(), f"Expected object file {obj_file} to exist")
        nm_out = subprocess.check_output(["nm", "-u", str(obj_file)], text=True)

        for line in nm_out.strip().splitlines():
            parts = line.strip().split()
            if len(parts) >= 2 and parts[0] == "U":
                sym = parts[1]
                self.assertFalse(
                    sym.startswith("ps5vk_"),
                    f"Consumer references internal ps5vk symbol: {sym}"
                )
                self.assertFalse(
                    sym.startswith("ps5_"),
                    f"Consumer references internal ps5 backend symbol: {sym}"
                )

    def test_map_file_generated(self):
        """Verify linker map file was generated for memory layout audit."""
        map_file = BUILD_DIR / "consumer.map"
        if not map_file.is_file():
            self.skipTest("Linker map file not available (native PS5 toolchain absent on host)")
        self.assertTrue(map_file.is_file(), f"Expected map file {map_file} to exist")
        self.assertGreater(map_file.stat().st_size, 1024, "Map file is suspiciously small")


if __name__ == "__main__":
    unittest.main()
