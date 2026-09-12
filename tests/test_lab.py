import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("lab", ROOT / "tools/lab.py")
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)


class LabTests(unittest.TestCase):
    def test_worktree_resolves_sibling_canonical_lab(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lab_root = root / "engineering/homebrew_ps5"
            script = root / "engineering/worktrees/ps5vk-topic/tools/lab.py"
            script.parent.mkdir(parents=True)
            script.touch()
            for relative in lab.REQUIRED:
                path = lab_root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            self.assertEqual(lab.resolve_lab_root(script), lab_root)

    def test_native_packagers_use_shared_lab_resolver(self):
        for relative in ("tools/build_sdk.py", "tools/build_consumer.py",
                         "tools/build_cts_native.py"):
            source = (ROOT / relative).read_text()
            self.assertIn("lab_root()", source)
            self.assertNotIn("ROOT.parents[1]", source)

    def test_remoteplay_preserves_arguments_without_shell(self):
        args = ["remoteplay", "record-demo", "--name", "ps5vk demo ; literal"]
        result = lab.command(Path("/tmp/lab with spaces"), args)
        self.assertEqual(result[1], "/tmp/lab with spaces/tools/ps5_remoteplay.py")
        self.assertEqual(result[2:], args[1:])

    def test_logs_reuses_server(self):
        self.assertTrue(lab.command(Path("/lab"), ["logs", "--help"])[1]
                        .endswith("logging_server/server/ps5logd.py"))

    def test_unknown_action_rejected(self):
        for args in ([], ["deploy"], ["kill"]):
            with self.assertRaises(ValueError):
                lab.command(Path("/lab"), args)

    def test_missing_lab_fails_without_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(lab.os.environ, {"PS5VK_LAB_ROOT": directory}), \
                 patch.object(lab.subprocess, "run") as run:
                self.assertEqual(lab.main(["doctor"]), 2)
                run.assert_not_called()

    def test_doctor_never_launches(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in lab.REQUIRED:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            with patch.dict(lab.os.environ, {"PS5VK_LAB_ROOT": directory}), \
                 patch.object(lab.subprocess, "run") as run:
                self.assertEqual(lab.main(["doctor"]), 0)
                run.assert_not_called()
