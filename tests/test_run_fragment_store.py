"""Lifecycle tests for the fragment-storage hardware runner."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import run_fragment_store as runner


class FragmentStoreRunnerTests(unittest.TestCase):
    def run_case(self, validation_error=False, close_ok=True):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "run.log"
            log.write_text("synthetic")
            args = argparse.Namespace(host="mock", runs_dir=root,
                                      artifact=root / "eboot.bin",
                                      manifest=root / "manifest.json",
                                      out=root / "result.json", timeout=1)
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(patch.object(argparse.ArgumentParser,
                                                  "parse_args", return_value=args))
                stack.enter_context(patch.object(runner, "running",
                                                  return_value="none"))
                control = stack.enter_context(patch.object(runner, "control"))
                stack.enter_context(patch.object(runner, "wait_for_log",
                                                  return_value=log))
                close = stack.enter_context(patch.object(
                    runner, "close_and_confirm", return_value=close_ok))
                stack.enter_context(patch.object(
                    runner, "validate", return_value={"ok": True},
                    side_effect=ValueError("bad witness")
                    if validation_error else None))
                if validation_error:
                    with self.assertRaisesRegex(ValueError, "bad witness"):
                        runner.main()
                else:
                    self.assertEqual(runner.main(), 0 if close_ok else 3)
                control.assert_called_once_with("launch", "mock")
                close.assert_called_once_with("mock")
            result = json.loads(args.out.read_text())
            self.assertEqual(result["strict_verified"], not validation_error)
            self.assertEqual(result["process_exit_verified"], close_ok)

    def test_success_requires_verification_and_process_exit(self):
        self.run_case()
        self.run_case(close_ok=False)

    def test_verification_failure_still_closes_and_records_failure(self):
        self.run_case(validation_error=True)

    def test_active_title_refuses_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            args = argparse.Namespace(host="mock", runs_dir=root,
                                      artifact=root / "eboot.bin",
                                      manifest=root / "manifest.json",
                                      out=root / "result.json", timeout=1)
            with patch.object(argparse.ArgumentParser, "parse_args",
                              return_value=args), \
                    patch.object(runner, "running", return_value="OTHER"), \
                    patch.object(runner, "control") as control:
                with self.assertRaisesRegex(RuntimeError, "active title"):
                    runner.main()
                control.assert_not_called()


if __name__ == "__main__":
    unittest.main()
