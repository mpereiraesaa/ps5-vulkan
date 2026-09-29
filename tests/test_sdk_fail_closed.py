"""The PS5 SDK build must never replace its native archive with a host mock."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_sdk  # noqa: E402


class NativeSdkBuildGateTest(unittest.TestCase):
    def test_public_payload_toolchain_detection(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary)
            for name in ("bin/prospero-clang", "bin/prospero-lld",
                         "target/lib/crt1.o"):
                path = sdk / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            with patch.dict(os.environ, PS5_PAYLOAD_SDK=temporary):
                self.assertEqual(build_sdk.get_public_ps5_toolchain(),
                                 (sdk, sdk / "bin/prospero-clang"))
                (sdk / "target/lib/crt1.o").unlink()
                self.assertEqual(build_sdk.get_public_ps5_toolchain(),
                                 (None, None))

    def test_missing_native_dependencies_fail_before_staging(self):
        with tempfile.TemporaryDirectory() as missing:
            destination = Path(missing) / "dist-sdk"
            with patch.dict(os.environ, PS5_PAYLOAD_SDK=missing,
                            PS5VK_LAB_ROOT=missing):
                with patch.object(build_sdk, "DIST_SDK", destination):
                    with self.assertRaisesRegex(SystemExit,
                                                "Native PS5 SDK dependencies missing"):
                        build_sdk.main()
            self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
