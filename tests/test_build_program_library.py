import inspect
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_program_library


class OfflineLibraryAbiTests(unittest.TestCase):
    def test_pinned_amdllpc_library_uses_its_measured_s1_table_abi(self):
        source = inspect.getsource(build_program_library.emit_program)
        self.assertIn('".descriptor_set_sgpr={1}"', source)
        self.assertNotIn('".descriptor_set_sgpr={2}"', source)


if __name__ == "__main__":
    unittest.main()
