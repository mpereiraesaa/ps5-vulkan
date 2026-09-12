"""End-to-end (offline) test of the native run orchestrator.

Builds a synthetic ps5log/1 capture that follows the real wire format produced
by the payload, then drives tools/run_upstream_cts.py in verify-only mode. This
covers the whole host-side acceptance path except the console itself.
"""
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "cts/upstream/manifest.json"
ORCHESTRATOR = ROOT / "tools/run_upstream_cts.py"
TITLE = "PPSA99994"


def load_orchestrator():
    spec = importlib.util.spec_from_file_location("run_upstream_cts", ORCHESTRATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeConsole:
    """Stands in for tools/control.py while exercising the lifecycle logic."""

    def __init__(self, runs_dir, capture_text, title=TITLE, stop_on_close=True):
        self.runs_dir = runs_dir
        self.capture_text = capture_text
        self.title = title
        self.running = "none"
        self.stop_on_close = stop_on_close
        self.launches = 0
        self.closes = 0

    def __call__(self, action, host, timeout=30.0):
        if action == "status":
            return (f"bigapp-control action=status target={self.title} "
                    f"app_id=-1 identify_rc=-1 running={self.running}")
        if action == "launch":
            self.launches += 1
            self.running = self.title
            (self.runs_dir / f"20260912T010101000Z_{self.title}_upstream-cts_0xabc.log"
             ).write_text(self.capture_text)
            return "launched"
        if action == "close":
            self.closes += 1
            if self.stop_on_close:
                self.running = "none"
            return "closed"
        raise AssertionError(f"unexpected control action {action!r}")


def build_qpa(paths, status="Pass"):
    lines = ["#sessionInfo releaseName vk-gl-cts-1.3.8.4", "#beginSession"]
    for path in paths:
        lines += [
            f"#beginTestCaseResult {path}",
            f'<TestCaseResult CasePath="{path}">',
            f'  <Result StatusCode="{status}">upstream oracle satisfied</Result>',
            "</TestCaseResult>",
            "#endTestCaseResult",
        ]
    lines.append("#endSession")
    return ("\n".join(lines) + "\n").encode("utf-8")


def build_capture(qpa_bytes, run_id, selection_hash, eboot_sha256, status=0,
                  chunk_size=384):
    chunks = [qpa_bytes[i:i + chunk_size] for i in range(0, len(qpa_bytes), chunk_size)]
    lines = [f"HELLO ps5log/1 title={TITLE} app=upstream-cts boot=0xdeadbeef"]
    seq = 1

    def record(level, text):
        nonlocal seq
        lines.append(f"{seq}\t{1000 + seq}\t{level}\t{text}")
        seq += 1

    record("MARK", f"UPSTREAM_CTS_START run_id={run_id} selection_hash={selection_hash} "
                   f"eboot_sha256={eboot_sha256}")
    for index, chunk in enumerate(chunks):
        data = base64.b64encode(chunk).decode("ascii")
        record("QPA", f"CHUNK seq={index} size={len(chunk)} data={data}")
    digest = hashlib.sha256(qpa_bytes).hexdigest()
    record("MARK", f"UPSTREAM_CTS_END chunks={len(chunks)} total_bytes={len(qpa_bytes)} "
                   f"sha256={digest}")
    record("INFO", f"UPSTREAM_CTS_COMPLETE status={status}")
    lines.append(f"BYE seq={seq - 1} reason=complete-success")
    return "\n".join(lines) + "\n"


class TestRunOrchestrator(unittest.TestCase):
    def setUp(self):
        self.cases = [case["path"] for case in json.loads(MANIFEST.read_text())["cases"]]
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.runs = Path(self.tmp.name) / "runs"
        self.runs.mkdir()
        self.dist = Path(self.tmp.name) / "dist"
        self.dist.mkdir()
        self.selection_hash = hashlib.sha256(
            ("\n".join(self.cases) + "\n").encode()).hexdigest()
        self.eboot_hash = "a" * 64
        (self.dist / "selection_hash.txt").write_text(self.selection_hash + "\n")
        (self.dist / "eboot_sha256.txt").write_text(self.eboot_hash + "\n")
        self.out_json = Path(self.tmp.name) / "receipt.json"

    def write_run(self, text, name="20260912T000000000Z_PPSA99994_upstream-cts_0xdead.log"):
        path = self.runs / name
        path.write_text(text)
        return path

    def invoke(self):
        return subprocess.run(
            [sys.executable, str(ORCHESTRATOR), "--no-launch",
             "--host", "192.0.2.1", "--runs-dir", str(self.runs),
             "--dist", str(self.dist), "--out-json", str(self.out_json),
             "--timeout", "5"],
            capture_output=True, text=True, cwd=ROOT)

    def test_all_pass_capture_is_accepted(self):
        self.write_run(build_capture(build_qpa(self.cases), "run-1",
                                     self.selection_hash, self.eboot_hash))
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        receipt = json.loads(self.out_json.read_text())
        self.assertTrue(receipt["strict_verified"])
        self.assertEqual(receipt["pass_count"], len(self.cases))

    def test_stale_executable_identity_is_rejected(self):
        self.write_run(build_capture(build_qpa(self.cases), "run-1",
                                     self.selection_hash, "b" * 64))
        result = self.invoke()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("Run identity mismatch", result.stderr)

    def test_unsupported_case_is_not_acceptance(self):
        qpa = build_qpa(self.cases)
        qpa = qpa.replace(b'StatusCode="Pass"', b'StatusCode="NotSupported"',
                          1)
        self.write_run(build_capture(qpa, "run-1", self.selection_hash, self.eboot_hash))
        result = self.invoke()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        receipt = json.loads(self.out_json.read_text())
        self.assertFalse(receipt["strict_verified"])
        self.assertEqual(receipt["not_supported_count"], 1)

    def test_truncated_capture_is_rejected(self):
        capture = build_capture(build_qpa(self.cases), "run-1",
                                self.selection_hash, self.eboot_hash)
        # Drop the final END/COMPLETE/BYE records: an interrupted run.
        self.write_run("\n".join(capture.splitlines()[:6]) + "\n")
        result = self.invoke()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_missing_case_is_rejected(self):
        self.write_run(build_capture(build_qpa(self.cases[:-1]), "run-1",
                                     self.selection_hash, self.eboot_hash))
        result = self.invoke()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        receipt = json.loads(self.out_json.read_text())
        self.assertIn(self.cases[-1], receipt["missing"])


class TestLifecycleIsPartOfAcceptance(unittest.TestCase):
    """A failed Close Game must fail the run even when every case passed."""

    def setUp(self):
        self.module = load_orchestrator()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.runs = Path(self.tmp.name) / "runs"
        self.runs.mkdir()
        self.dist = Path(self.tmp.name) / "dist"
        self.dist.mkdir()

        cases = [case["path"] for case in json.loads(MANIFEST.read_text())["cases"]]
        self.capture = build_capture(build_qpa(cases), "run-1",
                                     hashlib.sha256(("\n".join(cases) + "\n").encode()).hexdigest(),
                                     "a" * 64)
        self.selection_hash = hashlib.sha256(("\n".join(cases) + "\n").encode()).hexdigest()
        (self.dist / "selection_hash.txt").write_text(self.selection_hash + "\n")
        (self.dist / "eboot_sha256.txt").write_text("a" * 64 + "\n")
        self.out_json = Path(self.tmp.name) / "receipt.json"

    def invoke(self, console, extra=()):
        argv = [
            "run_upstream_cts", "--host", "192.0.2.1", "--runs-dir", str(self.runs),
            "--dist", str(self.dist), "--out-json", str(self.out_json),
            "--timeout", "10", "--close-attempts", "2", "--close-settle", "0.2",
            *extra,
        ]
        stdout = io.StringIO()
        with mock.patch.object(self.module, "control", console), \
                mock.patch.object(sys, "argv", argv), \
                mock.patch("sys.stdout", stdout):
            code = self.module.main()
        return code, stdout.getvalue()

    def test_successful_run_and_close_passes(self):
        console = FakeConsole(self.runs, self.capture)
        code, output = self.invoke(console)
        self.assertEqual(code, 0, output)
        self.assertIn("UPSTREAM ACCEPTANCE PASSED", output)
        self.assertEqual(console.launches, 1)
        receipt = json.loads(self.out_json.read_text())
        self.assertTrue(receipt["cts_verified"])
        self.assertTrue(receipt["lifecycle_ok"])

    def test_failed_close_fails_the_run_despite_passing_cases(self):
        console = FakeConsole(self.runs, self.capture, stop_on_close=False)
        code, output = self.invoke(console)
        self.assertEqual(code, self.module.EXIT_LIFECYCLE)
        self.assertNotIn("UPSTREAM ACCEPTANCE PASSED", output)
        receipt = json.loads(self.out_json.read_text())
        self.assertTrue(receipt["cts_verified"])
        self.assertFalse(receipt["lifecycle_ok"])
        self.assertGreaterEqual(console.closes, 2)

    def test_launch_is_refused_while_title_runs(self):
        console = FakeConsole(self.runs, self.capture)
        console.running = TITLE
        with self.assertRaises(self.module.RunIncomplete):
            self.invoke(console)
        self.assertEqual(console.launches, 0)

    def test_no_close_skips_lifecycle_requirement(self):
        console = FakeConsole(self.runs, self.capture, stop_on_close=False)
        code, output = self.invoke(console, extra=["--no-close"])
        self.assertEqual(code, 0, output)
        receipt = json.loads(self.out_json.read_text())
        self.assertIsNone(receipt["lifecycle_ok"])


if __name__ == "__main__":
    unittest.main()
