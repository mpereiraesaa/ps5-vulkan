"""Compile actual shaders; check emitted ACO IO, not source strings or code size.

This is a pinned-compiler diagnostic contract, not native tessellation evidence.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def check_io(hull, domain):
    stores = [s for s in hull.splitlines()
              if "buffer_store_dwordx4" in s and "storage:vmem_output" in s]
    if not stores:
        raise AssertionError("Hull lost all offchip attribute stores")
    if not any("offset:256 " in s for s in stores):
        raise AssertionError("Hull compact color slot absent")
    if not any("offset:" not in s or "offset:0 " in s for s in stores):
        raise AssertionError("Hull position slot absent")
    offsets = {int(x) for x in re.findall(r"buffer_load_dwordx3[^\n]*offset:(\d+) ", domain)}
    if offsets != {256, 272, 288}:
        raise AssertionError(f"TES color layout disagrees: {offsets}")


class TessDeliveryCompilerTests(unittest.TestCase):
    def test_rejects_missing_stores(self):
        with self.assertRaisesRegex(AssertionError, "lost"):
            check_io("buffer_store_dwordx4 factors", "")

    def test_rejects_old_unlinked_domain_layout(self):
        with self.assertRaisesRegex(AssertionError, "disagrees"):
            check_io("buffer_store_dwordx4 x storage:vmem_output\n"
                     "buffer_store_dwordx4 x offset:256 storage:vmem_output",
                     "buffer_load_dwordx3 x offset:1024 offen")

    def test_rejects_missing_position_store(self):
        with self.assertRaisesRegex(AssertionError, "position"):
            check_io("buffer_store_dwordx4 x offset:256 storage:vmem_output", "")

    def test_actual_compiler_preserves_matching_interface(self):
        self._compile_fixture(False)

    def test_actual_compiler_preserves_patch_region_base(self):
        self._compile_fixture(True)

    def test_isoline_metadata_selects_line_output(self):
        self._compile_fixture("isolines")

    def test_patch32_cross_invocation_delivery_compiles(self):
        self._compile_fixture("patch32")

    def _compile_fixture(self, patch):
        archive = ROOT / "build/libpsbc.host.a"
        if not archive.exists() or not shutil.which("glslangValidator"):
            self.skipTest("requires built host PSBC and glslangValidator")
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            modules = []
            for stage in ("vert", "tesc", "tese"):
                target = tmp / (stage + ".spv")
                name = ("runtime_tess_two_patch" if stage == "vert" else
                        "runtime_tess_patch_data") if patch else "runtime_tess_delivery"
                if patch == "isolines":
                    name = "runtime_tess_coord" if stage == "vert" else "runtime_tess_isoline"
                if patch == "patch32":
                    name = "runtime_tess_patch32"
                subprocess.run(["glslangValidator", "-V", "-o", str(target),
                    str(ROOT / f"experiments/graphics/{name}.{stage}")],
                    check=True, capture_output=True)
                modules.append(str(target))
            exe = tmp / "inspect"
            fragment = tmp / "fragment.spv"
            subprocess.run(["glslangValidator", "-V", "-o", str(fragment),
                str(ROOT / "experiments/graphics/runtime_tess_coord.frag")],
                check=True, capture_output=True)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "third_party/vulkan-headers/include"),
                "-I" + str(ROOT / "third_party/psbc-reference"),
                str(ROOT / "tests/tess_delivery_inspect.c"),
                str(ROOT / "src/spirv_graphics_interface.c"),
                str(ROOT / "src/texture_format.c"), str(archive),
                "-lstdc++", "-lm", "-lpthread", "-o", str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe), *modules,
                "32" if patch == "patch32" else "3", str(fragment)], check=True, capture_output=True,
                text=True, env={**os.environ, "PSBC_DEBUG_DISASM": "1"})
            hull, domain = result.stderr.split("DELIVERY_DOMAIN_BEGIN", 1)
            self.assertIn("DELIVERY_END", domain)
            if patch == "patch32":
                # The native pixel oracle checks all32 identities after a TCS
                # barrier. Here require that the linked pair really compiles
                # and preserves the patch output/load; no hardware claim.
                self.assertRegex(hull, r"buffer_store_dword")
                self.assertRegex(domain, r"buffer_load_dword")
                patch_stores = [s for s in hull.splitlines()
                                if "buffer_store_dword" in s and "storage:vmem_output" in s]
                self.assertTrue(patch_stores)
                self.assertTrue(all("offset:" not in s or "offset:0 " in s for s in patch_stores),
                                "TCS-only scratch outputs must not pad the TES patch region")
                patch_loads = [s for s in domain.splitlines() if "buffer_load_dword " in s]
                self.assertTrue(patch_loads)
                self.assertTrue(all("offset:" not in s or "offset:0 " in s for s in patch_loads),
                                "TES patch region must match the compact hull layout")
                return
            if patch == "isolines":
                tf = re.findall(r"DELIVERY_TF=([0-9a-f]+)", hull)
                self.assertEqual(len(tf), 1)
                self.assertEqual(int(tf[0], 16) & 0xff, 0x20,
                                 "isoline domain requires line topology, not triangles")
                return
            check_io(hull, domain)
            if patch:
                self.assertRegex(hull, r"buffer_store_dwordx4[^\n]*offset:512 [^\n]*storage:vmem_output")
                self.assertRegex(domain, r"buffer_load_dword [^\n]*offset:512 ")
                self.assertNotRegex(hull, r"buffer_store_dwordx4[^\n]*offset:1280 ")
