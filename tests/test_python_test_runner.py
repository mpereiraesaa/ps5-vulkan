import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "run_python_tests", ROOT / "tools/run_python_tests.py")
runner = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(runner)


class PythonTestSelectionTests(unittest.TestCase):
    def test_default_excludes_only_ledger_snapshots_when_requested(self):
        with tempfile.TemporaryDirectory() as directory:
            tests = Path(directory)
            for name in (*runner.LEDGER_ONLY, "test_dxvk_probe", "test_vk_device"):
                (tests / f"{name}.py").touch()
            selected = runner.select_modules(tests, [], exclude_ledger=True)
            self.assertEqual(["test_dxvk_probe", "test_vk_device"], selected)
            self.assertEqual(len(runner.LEDGER_ONLY) + 2,
                             len(runner.select_modules(tests, [], exclude_ledger=False)))

    def test_explicit_ledger_audit_remains_runnable(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(["test_dxvk_matrix"], runner.select_modules(
                Path(directory), ["test_dxvk_matrix"], exclude_ledger=True))


if __name__ == "__main__":
    unittest.main()
