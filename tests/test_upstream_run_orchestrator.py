"""End-to-end (offline) test of the native run orchestrator.

Builds a synthetic ps5log/1 capture that follows the real wire format produced
by the payload, then drives tools/run_upstream_cts.py in verify-only mode. This
covers the whole host-side acceptance path except the console itself.
"""
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "cts/upstream/manifest.json"
ORCHESTRATOR = ROOT / "tools/run_upstream_cts.py"
TITLE = "PPSA99994"


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


if __name__ == "__main__":
    unittest.main()
