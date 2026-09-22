import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

from test_upstream_run_orchestrator import build_capture, build_qpa

ROOT = Path(__file__).resolve().parents[1]
A = "dEQP-VK.a.one"
B = "dEQP-VK.a.two"
C = "dEQP-VK.a.three"


def load_decoder():
    spec = importlib.util.spec_from_file_location("decode_run", ROOT / "tools/decode_run.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


def mixed_qpa(statuses: dict) -> str:
    lines = ["#beginSession"]
    for path, status in statuses.items():
        lines += [f"#beginTestCaseResult {path}", f'<TestCaseResult CasePath="{path}">',
                  f'  <Result StatusCode="{status}">{status} detail for {path}</Result>',
                  "</TestCaseResult>", "#endTestCaseResult"]
    return "\n".join(lines + ["#endSession"]) + "\n"


def with_driver_lines(capture: str, per_case: dict) -> str:
    """Insert UPSTREAM_CTS_CASE marks and driver lines before the QPA chunks."""
    head, rest = capture.split("\n", 2)[:2], capture.split("\n", 2)[2]
    extra = []
    for path, lines in per_case.items():
        extra.append(f"0\t0\tMARK\tUPSTREAM_CTS_CASE name={path}")
        extra += [f"0\t0\tERR\t{line}" for line in lines]
    return "\n".join(head + extra) + "\n" + rest


class DecodeRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decoder = load_decoder()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name: str, text: str) -> Path:
        path = self.dir / name
        path.write_text(text)
        return path

    def test_qpa_counts_and_failing_details(self):
        report = self.decoder.decode(
            self.write("r.qpa", mixed_qpa({A: "Pass", B: "Fail", C: "NotSupported"})), None)
        self.assertEqual({"Fail": 1, "NotSupported": 1, "Pass": 1}, report["counts"])
        self.assertEqual(3, report["total"])
        self.assertTrue(report["complete"])
        self.assertEqual([B, C], [c["case_path"] for c in report["failing"]])
        self.assertEqual(f"Fail detail for {B}", report["failing"][0]["details"])

    def test_run_log_attributes_refusals_and_identity(self):
        capture = build_capture(mixed_qpa({A: "Pass", B: "Fail"}).encode(),
                                "run-1", "s" * 64, "e" * 64)
        log = with_driver_lines(capture, {
            A: ["PS5VK_PIPELINE_CREATE topology=3"],
            B: ["PS5VK_PIPELINE_CREATE topology=1",
                "PS5VK_GRAPHICS_PREPARE_FAILED serial=9 phase=draw site=34 rc=-8",
                "PS5VK_GRAPHICS_PREPARE_FAILED serial=10 phase=draw site=34 rc=-8",
                "PS5VK_MEMORY_RELEASE mapped=1"],
        })
        report = self.decoder.decode(self.write("run.log", log), None)
        self.assertEqual("e" * 64, report["identity"]["eboot_sha256"])
        failing = report["failing"][0]
        self.assertEqual(B, failing["case_path"])
        self.assertEqual("PS5VK_GRAPHICS_PREPARE_FAILED serial=9 phase=draw site=34 rc=-8",
                         failing["first_refusal"])
        self.assertEqual(2, failing["refusal_count"])
        self.assertEqual("PS5VK_GRAPHICS_PREPARE_FAILED serial=10 phase=draw site=34 rc=-8",
                         failing["last_driver_call"], "teardown lines are not the last call")
        self.assertEqual([("PS5VK_GRAPHICS_PREPARE_FAILED phase=draw site=34", 2)],
                         report["refusal_signatures"])

    def test_signature_keeps_name_and_classifying_fields_only(self):
        self.assertEqual("PS5VK_GRAPHICS_PREPARE_FAILED phase=draw site=34",
                         self.decoder.signature(
                             "PS5VK_GRAPHICS_PREPARE_FAILED serial=9 phase=draw site=34 rc=-8"))

    def test_unmarked_refusal_falls_back_to_the_last_driver_call(self):
        capture = build_capture(mixed_qpa({B: "Fail"}).encode(), "run-1", "s" * 64, "e" * 64)
        log = with_driver_lines(capture, {B: ["PS5VK_PIPELINE_CREATE topology=1",
                                              "PS5VK_MEMORY_RELEASE mapped=1"]})
        failing = self.decoder.decode(self.write("run.log", log), None)["failing"][0]
        self.assertIsNone(failing["first_refusal"])
        self.assertEqual("PS5VK_PIPELINE_CREATE topology=1", failing["last_driver_call"])

    def test_unfinalized_run_is_decoded_leniently(self):
        qpa = mixed_qpa({A: "Pass", B: "Fail"}).encode()
        truncated = qpa[:qpa.index(b"#endTestCaseResult", qpa.index(B.encode()))]
        capture = build_capture(truncated, "run-1", "s" * 64, "e" * 64)
        broken = "\n".join(l for l in capture.splitlines() if "UPSTREAM_CTS_END" not in l)
        report = self.decoder.decode(self.write("run.log", broken + "\n"), None)
        self.assertFalse(report["complete"])
        self.assertIn("decoded leniently", report["note"])
        self.assertEqual({"Incomplete": 1, "Pass": 1}, report["counts"])

    def test_malformed_case_is_reported_not_dropped(self):
        text = mixed_qpa({A: "Pass"}).replace("#endSession\n", "") + \
            f"#beginTestCaseResult {B}\n<TestCaseResult CasePath=\"{B}\">\n#endTestCaseResult\n"
        report = self.decoder.decode(self.write("r.qpa", text), None)
        self.assertEqual({"Malformed": 1, "Pass": 1}, report["counts"])

    def test_receipt_input(self):
        receipt = {"case_results": [{"case_path": A, "status": "Pass"},
                                    {"case_path": B, "status": "Fail", "details": "x"}],
                   "expected_identity": {"eboot_sha256": "f" * 64}}
        report = self.decoder.decode(self.write("r.json", json.dumps(receipt)), None)
        self.assertEqual({"Fail": 1, "Pass": 1}, report["counts"])
        self.assertEqual("f" * 64, report["identity"]["eboot_sha256"])

    def test_delta_classifies_every_change(self):
        before = self.write("before.qpa", mixed_qpa({A: "Fail", B: "Pass", C: "Fail",
                                                    "dEQP-VK.gone": "Pass"}))
        after = self.write("after.qpa", mixed_qpa({A: "Pass", B: "Fail", C: "NotSupported",
                                                   "dEQP-VK.new": "Pass"}))
        d = self.decoder.decode(after, before)["delta"]
        self.assertEqual([(A, "Fail", "Pass")], d["fixed"])
        self.assertEqual([(B, "Pass", "Fail")], d["broke"])
        self.assertEqual([(C, "Fail", "NotSupported")], d["moved"])
        self.assertEqual(["dEQP-VK.new"], d["added"])
        self.assertEqual(["dEQP-VK.gone"], d["removed"])

    def test_cli_text_and_json(self):
        path = self.write("r.qpa", build_qpa([A]).decode())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(0, self.decoder.main([str(path)]))
        self.assertIn("cases    1  Pass=1", out.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.decoder.main([str(path), "--json"])
        self.assertEqual({"Pass": 1}, json.loads(out.getvalue())["counts"])


if __name__ == "__main__":
    unittest.main()
