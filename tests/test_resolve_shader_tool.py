"""The driver resolve-shader generator finds its compiler through the PATH.

The tool used to default to ~/.local/bin/glslangValidator, so a machine whose
glslang comes from the distribution (the CI image installs glslang-tools) failed
the compiler-contracts job at `make test-compiler`. The default now follows the
same order every other shader tool on this line uses - PS5VK_GLSLANG, then PATH -
and the Makefile passes its own $(GLSLANG) explicitly.
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/build_resolve_shaders.py"


class ResolveShaderToolTests(unittest.TestCase):
    def _stub(self, directory: Path) -> Path:
        stub = directory / "glslangValidator"
        # Writes the file the tool names with -o, and records that it ran.
        stub.write_text(
            "#!/bin/sh\n"
            "printf '%s\\n' \"$@\" >> \"$STUB_LOG\"\n"
            "while [ $# -gt 0 ]; do\n"
            "  if [ \"$1\" = \"-o\" ]; then printf '\\002\\043\\007\\000' > \"$2\"; fi\n"
            "  shift\n"
            "done\n"
            "exit 0\n")
        stub.chmod(0o755)
        return stub

    def run_tool(self, compiler, directory: Path) -> subprocess.CompletedProcess:
        log = directory / "invocations.txt"
        env = dict(os.environ, STUB_LOG=str(log))
        env.pop("PS5VK_GLSLANG", None)
        if compiler:
            env["PS5VK_GLSLANG"] = compiler
        else:
            env["PATH"] = str(directory) + os.pathsep + env.get("PATH", "")
        return subprocess.run([sys.executable, str(TOOL), "--out", str(directory / "resolve_spirv.h")],
                              env=env, capture_output=True, text=True)

    def test_environment_names_the_compiler(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            stub = self._stub(directory)
            result = self.run_tool(str(stub), directory)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue((directory / "invocations.txt").is_file())
            self.assertIn("-V", (directory / "invocations.txt").read_text())

    def test_default_falls_back_to_the_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self._stub(directory)
            result = self.run_tool(None, directory)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue((directory / "invocations.txt").is_file())

    def test_missing_compiler_is_named_not_a_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            result = self.run_tool(str(directory / "nowhere" / "glslangValidator"), directory)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("glslangValidator not found", result.stderr)

    def test_bare_name_resolves_through_the_path(self):
        """The Makefile's $(GLSLANG) may be a bare name, as in CI."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self._stub(directory)
            log = directory / "invocations.txt"
            env = dict(os.environ, STUB_LOG=str(log),
                       PATH=str(directory) + os.pathsep + os.environ.get("PATH", ""))
            env.pop("PS5VK_GLSLANG", None)
            result = subprocess.run([sys.executable, str(TOOL), "--out", str(directory / "resolve_spirv.h"),
                                     "--compiler", "glslangValidator"], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue(log.is_file())

    def test_makefile_passes_its_own_toolchain(self):
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn("--compiler $(GLSLANG)", makefile)


if __name__ == "__main__":
    unittest.main()
