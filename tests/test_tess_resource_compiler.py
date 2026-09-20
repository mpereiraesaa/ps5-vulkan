"""Actual PSBC regression; not hardware acceptance or a synthetic compiler."""
import pathlib
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class TessResourceCompilerTests(unittest.TestCase):
    def test_merged_ls_hs_binding_union(self):
        archive = ROOT / "build/libpsbc.host.a"
        compiler = shutil.which("glslangValidator")
        if not archive.exists() or not compiler:
            self.skipTest("requires host PSBC archive and glslangValidator")
        sibling_roots = [pathlib.Path(os.environ["LAB_SIBLINGS"])] if "LAB_SIBLINGS" in os.environ else []
        sibling_roots += [ROOT.parent, ROOT.parents[1] / "projects"]
        gears = next((p / "ps5-agc-gears/src" for p in sibling_roots
                      if (p / "ps5-agc-gears/src/ps5_shader_header.h").exists()), None)
        self.assertIsNotNone(gears, "host compiler tests require the AGC header dependency")
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            def run(args):
                result = subprocess.run(args, cwd=ROOT, text=True,
                                        capture_output=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout
            for suffix, define in (("vert", "USE_VS"), ("tesc", "USE_HS")):
                for active in (0, 1):
                    run([compiler, "-V", *([f"-D{define}=1"] if active else []),
                         f"experiments/graphics/tess_resources.{suffix}",
                         "-o", str(path / f"{active}.{suffix}.spv")])
            run([compiler, "-V", "experiments/graphics/runtime_tess_delivery.tese",
                 "-o", str(path / "eval.spv")])
            run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Ithird_party/psbc-reference/libpsbc",
                 "-Ithird_party/psbc-reference", "-Inative", "-Isrc",
                 "-I" + str(gears),
                 "tests/test_tess_resource_metadata.c", "native/runtime_shader.c", str(archive),
                 "-lstdc++", "-lm", "-lpthread", "-o", str(path / "probe")])
            for vertex, hull in ((0, 0), (0, 1), (1, 0), (1, 1)):
                with self.subTest(vertex=vertex, hull=hull):
                    run([str(path / "probe"), str(path / f"{vertex}.vert.spv"),
                         str(path / f"{hull}.tesc.spv"), str(path / "eval.spv"),
                         str(vertex | (hull << 1)),
                         str(28 if hull else 4 if vertex else 0)])
            # A merged hull must not reserve the separately-compiled LS/HS
            # argument ABI: two live descriptor sets formerly exceeded16 SGPRs.
            run([compiler, "-V", "-DUSE_HS=1", "-DTWO_SETS=1",
                 "experiments/graphics/tess_resources.tesc",
                 "-o", str(path / "two_sets.tesc.spv")])
            run(["env", "TESS_TEST_TWO_SETS=1", str(path / "probe"),
                 str(path / "0.vert.spv"), str(path / "two_sets.tesc.spv"),
                 str(path / "eval.spv"), "2", "28"])
            for suffix, define in (("vert", "USE_VS"), ("tesc", "USE_HS")):
                run([compiler, "-V", f"-D{define}=1", "-DUSE_SPEC=1",
                     f"experiments/graphics/tess_resources.{suffix}",
                     "-o", str(path / f"spec.{suffix}.spv")])
            # Both modules use SpecId0: opposite values must not bleed across
            # merged stages. Explicit empty VS map must retain source defaults.
            for vertex, hull in ((0, 0), (0, 1), (1, 0), (1, 1), (2, 1)):
                with self.subTest(specialized_vertex=vertex, specialized_hull=hull):
                    active_vertex = vertex == 1
                    run([str(path / "probe"), str(path / "spec.vert.spv"),
                         str(path / "spec.tesc.spv"), str(path / "eval.spv"),
                         str(int(active_vertex) | (hull << 1)),
                         str(28 if hull else 4 if active_vertex else 0), str(vertex), str(hull)])
            run([compiler, "-V", "-DUSE_HS=1", "-DDYNAMIC_PUSH=1",
                 "experiments/graphics/tess_resources.tesc", "-o", str(path / "dynamic.tesc.spv")])
            run([str(path / "probe"), str(path / "0.vert.spv"),
                 str(path / "dynamic.tesc.spv"), str(path / "eval.spv"),
                 # The dereference member range excludes the unrelated prefix.
                 "2", "80", "0xffff0"])
            # Unknown runtime indices still access the same declared member;
            # use dereference provenance, not arithmetic lower-bound guessing.
            run([compiler, "-V", "-DUSE_HS=1", "-DDYNAMIC_PUSH=1",
                 "-DDYNAMIC_UNBOUNDED=1", "experiments/graphics/tess_resources.tesc",
                 "-o", str(path / "unbounded.tesc.spv")])
            run([str(path / "probe"), str(path / "0.vert.spv"),
                 str(path / "unbounded.tesc.spv"), str(path / "eval.spv"),
                 "2", "80", "0xffff0"])
