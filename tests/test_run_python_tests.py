import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_runner():
    spec = importlib.util.spec_from_file_location(
        "run_python_tests", ROOT / "tools/run_python_tests.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


class RunPythonTestsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner()

    def run_fixture(self, modules: dict[str, str], *names: str) -> tuple[int, str, dict]:
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name, body in modules.items():
                (folder / f"{name}.py").write_text(textwrap.dedent(body))
            times = folder / "times.json"
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = self.runner.main(["-j", "4", "--start-dir", str(folder),
                                         "--times", str(times), *names])
            return code, out.getvalue(), json.loads(times.read_text())

    PASSING = """
        import unittest
        class T(unittest.TestCase):
            def test_one(self): self.assertEqual(2, 1 + 1)
            def test_two(self): self.assertTrue(True)
            @unittest.skip("fixture skip")
            def test_skipped(self): pass
    """
    FAILING = """
        import unittest
        class T(unittest.TestCase):
            def test_wrong(self): self.assertEqual(3, 1 + 1)
    """

    def test_all_passing_modules_exit_zero_with_exact_counts(self):
        code, out, times = self.run_fixture({"test_a": self.PASSING, "test_b": self.PASSING})
        self.assertEqual(0, code)
        self.assertIn("Ran 6 tests in 2 modules", out)
        self.assertIn("skipped=2", out)
        self.assertEqual({"test_a", "test_b"}, set(times))

    def test_one_failing_module_fails_the_run_and_prints_its_output(self):
        code, out, _ = self.run_fixture({"test_a": self.PASSING, "test_b": self.FAILING})
        self.assertEqual(1, code)
        self.assertIn("FAILED modules: test_b", out)
        self.assertIn("AssertionError: 3 != 2", out)

    def test_import_error_counts_as_a_failure(self):
        code, out, _ = self.run_fixture({"test_a": "import no_such_module_anywhere\n"})
        self.assertEqual(1, code)
        self.assertIn("FAILED modules: test_a", out)

    def test_named_modules_restrict_the_run(self):
        code, out, times = self.run_fixture(
            {"test_a": self.PASSING, "test_b": self.FAILING}, "test_a")
        self.assertEqual(0, code)
        self.assertEqual({"test_a"}, set(times))

    def test_every_serial_module_exists(self):
        """A renamed module would silently leave the serial lane and race."""
        missing = [name for name in self.runner.SERIAL
                   if not (ROOT / "tests" / f"{name}.py").is_file()]
        self.assertEqual([], missing)


if __name__ == "__main__":
    unittest.main()
