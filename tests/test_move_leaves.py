import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_tool():
    spec = importlib.util.spec_from_file_location("move_leaves", ROOT / "tools/move_leaves.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


def entry(path, category, status="Pass", rationale="why"):
    return {"category": category, "expected_status": status, "features_required": [],
            "path": path, "rationale": rationale, "source": "x.cpp:1"}


class MoveLeavesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.manifest = self.dir / "manifest.json"
        self.manifest.write_text(json.dumps({
            "manifest_version": "1.0",
            "cases": [entry("dEQP-VK.b", "draw"), entry("dEQP-VK.d", "draw")],
            "diagnostics": [entry("dEQP-VK.c", "pending", "Fail", "old reason"),
                            entry("dEQP-VK.a", "pending", "NotSupported"),
                            entry("dEQP-VK.z", "other", "Fail")],
        }, indent=2) + "\n")

    def tearDown(self):
        self.tmp.cleanup()

    def receipt(self, statuses):
        path = self.dir / "receipt.json"
        path.write_text(json.dumps({
            "case_results": [{"case_path": p, "status": s} for p, s in statuses.items()],
            "expected_identity": {"eboot_sha256": "ab" * 32},
            "source_log": "/runs/20260922T000000000Z_PPSA99994_upstream-cts_0x1.log"}))
        return path

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = self.tool.main([*args, "--manifest", str(self.manifest),
                                   "--date", "2026-09-22"])
        return code, out.getvalue(), err.getvalue()

    def load(self):
        return json.loads(self.manifest.read_text())

    def test_promote_moves_measured_leaves_sorted_with_provenance(self):
        receipt = self.receipt({"dEQP-VK.a": "Pass", "dEQP-VK.c": "Pass"})
        code, out, _ = self.run_tool("promote", "--category", "pending",
                                     "--to-category", "draw", "--receipt", str(receipt))
        self.assertEqual(0, code)
        manifest = self.load()
        self.assertEqual(["dEQP-VK.a", "dEQP-VK.b", "dEQP-VK.c", "dEQP-VK.d"],
                         [c["path"] for c in manifest["cases"]])
        self.assertEqual(["dEQP-VK.z"], [c["path"] for c in manifest["diagnostics"]])
        promoted = next(c for c in manifest["cases"] if c["path"] == "dEQP-VK.c")
        self.assertEqual(("draw", "Pass"), (promoted["category"], promoted["expected_status"]))
        self.assertEqual("PROMOTED 2026-09-22: measured Pass on the physical console in run "
                         "20260922T000000000Z_PPSA99994_upstream-cts_0x1 on payload eboot "
                         "abababababababab. old reason", promoted["rationale"])
        self.assertIn("acceptance  2 -> 4", out)
        self.assertIn("diagnostics 3 -> 1", out)

    def test_promote_refuses_any_leaf_the_receipt_does_not_show_passing(self):
        before = self.manifest.read_text()
        receipt = self.receipt({"dEQP-VK.a": "Pass", "dEQP-VK.c": "Fail"})
        code, _, err = self.run_tool("promote", "--category", "pending",
                                     "--to-category", "draw", "--receipt", str(receipt))
        self.assertEqual(2, code)
        self.assertIn("dEQP-VK.c (Fail)", err)
        self.assertEqual(before, self.manifest.read_text(), "nothing is written on refusal")
        code, _, err = self.run_tool("promote", "--path", "dEQP-VK.z",
                                     "--to-category", "draw", "--receipt", str(receipt))
        self.assertIn("dEQP-VK.z (not in the run)", err)

    def test_promote_requires_a_readable_receipt_with_an_identity(self):
        code, _, err = self.run_tool("promote", "--category", "pending", "--to-category", "d")
        self.assertEqual(2, code)
        self.assertIn("needs --receipt", err)
        bad = self.dir / "bad.json"
        bad.write_text("not json")
        code, _, err = self.run_tool("promote", "--category", "pending", "--to-category", "d",
                                     "--receipt", str(bad))
        self.assertIn("cannot read receipt", err)
        bad.write_text(json.dumps({"case_results": [{"case_path": "dEQP-VK.a",
                                                     "status": "Pass"}]}))
        code, _, err = self.run_tool("promote", "--path", "dEQP-VK.a", "--to-category", "d",
                                     "--receipt", str(bad))
        self.assertIn("does not name the payload eboot", err)

    def test_demote_records_status_and_reason(self):
        code, _, _ = self.run_tool("demote", "--path", "dEQP-VK.d", "--to-category", "gap",
                                   "--status", "Fail", "--reason", "hangs the GPU.")
        self.assertEqual(0, code)
        manifest = self.load()
        self.assertEqual(["dEQP-VK.b"], [c["path"] for c in manifest["cases"]])
        demoted = manifest["diagnostics"][-1]
        self.assertEqual(("dEQP-VK.d", "gap", "Fail"),
                         (demoted["path"], demoted["category"], demoted["expected_status"]))
        self.assertTrue(demoted["rationale"].startswith("DEMOTED 2026-09-22: hangs the GPU. "))
        code, _, err = self.run_tool("demote", "--path", "dEQP-VK.b", "--to-category", "gap")
        self.assertIn("needs --status and --reason", err)

    def test_unknown_names_and_empty_selections_fail_closed(self):
        receipt = self.receipt({"dEQP-VK.a": "Pass"})
        for args, message in ((["--path", "dEQP-VK.nope"], "not found: dEQP-VK.nope"),
                              (["--category", "nope"], "no entries in category: nope"),
                              ([], "nothing selected")):
            code, _, err = self.run_tool("promote", *args, "--to-category", "d",
                                         "--receipt", str(receipt))
            self.assertEqual(2, code)
            self.assertIn(message, err)

    def test_dry_run_writes_nothing_and_reports_the_hash_change(self):
        before = self.manifest.read_text()
        code, out, _ = self.run_tool("demote", "--path", "dEQP-VK.d", "--to-category", "g",
                                     "--status", "Fail", "--reason", "r", "--dry-run")
        self.assertEqual(0, code)
        self.assertEqual(before, self.manifest.read_text())
        old = self.tool.selection_hash([{"path": "dEQP-VK.b"}, {"path": "dEQP-VK.d"}])
        new = self.tool.selection_hash([{"path": "dEQP-VK.b"}])
        self.assertIn(f"selection   {old[:16]} -> {new[:16]}", out)

    def test_written_manifest_keeps_the_frozen_file_format(self):
        frozen = ROOT / "cts/upstream/manifest.json"
        text = frozen.read_text()
        self.assertEqual(text, json.dumps(json.loads(text), indent=2) + "\n",
                         "the frozen manifest must round-trip through the tool's writer")

    def test_pinned_count_hints_find_literal_counts(self):
        manifest = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        hints = self.tool.pinned_count_hints(len(manifest["cases"]), len(manifest["diagnostics"]))
        self.assertTrue(any("test_upstream_selection.py" in h for h in hints), hints)


if __name__ == "__main__":
    unittest.main()
