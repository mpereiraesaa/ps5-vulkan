import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools import run_format_diagnostic as runner


class FormatDiagnosticLifecycle(unittest.TestCase):
    def test_verification_failure_still_closes_and_records_failure(self):
        self.run_case(validation_error=True)

    def test_failed_close_is_a_failed_command_even_with_good_pixels(self):
        self.run_case(close_ok=False)

    def test_success_requires_validation_and_process_exit(self):
        self.run_case()

    def run_case(self, validation_error=False, close_ok=True):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifact = root / "artifact.json"
            artifact.write_text(json.dumps({"scissor_probe": 7}))
            log = root / "run.log"
            log.write_text("synthetic fixture")
            log.with_suffix(".json").write_text("{}")
            args = argparse.Namespace(host="mock", artifact=artifact, runs_dir=root,
                                      out=root / "result.json", timeout=1)
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(patch.object(argparse.ArgumentParser, "parse_args", return_value=args))
                stack.enter_context(patch.object(runner, "running", return_value="none"))
                control = stack.enter_context(patch.object(runner, "control"))
                stack.enter_context(patch.object(runner, "wait_for_log", return_value=log))
                close = stack.enter_context(patch.object(runner, "close_and_confirm", return_value=close_ok))
                stack.enter_context(patch.object(runner, "sampled", return_value={"gpu_readback": True},
                                                side_effect=ValueError("bad pixels") if validation_error else None))
                if validation_error:
                    with self.assertRaisesRegex(ValueError, "bad pixels"):
                        runner.main()
                else:
                    self.assertEqual(runner.main(), 0 if close_ok else 3)
                control.assert_called_once_with("launch", "mock")
                close.assert_called_once_with("mock")
            result = json.loads(args.out.read_text())
            self.assertEqual(result["strict_verified"], not validation_error)
            self.assertEqual(result["process_exit_verified"], close_ok)
            self.assertFalse(result["deployment_identity_verified"])

    def test_unknown_diagnostic_never_touches_console(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / "artifact.json"
            artifact.write_text(json.dumps({"scissor_probe": 13}))
            with patch.object(argparse.ArgumentParser, "parse_args",
                              return_value=argparse.Namespace(artifact=artifact)), \
                    patch.object(runner, "running") as running:
                with self.assertRaisesRegex(ValueError, "not a format diagnostic"):
                    runner.main()
                running.assert_not_called()
