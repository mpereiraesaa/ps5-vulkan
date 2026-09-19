"""Compile actual shaders; check emitted ACO IO, not source strings or code size.

This is a pinned-compiler diagnostic contract, not native tessellation evidence.
"""
import os
from pathlib import Path
import re
import shutil
import struct
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

    def test_three_input_thirty_two_output_points_compile(self):
        self._compile_fixture("expand32")

    def test_unused_vs_output_does_not_change_lshs_stride(self):
        self._compile_fixture("unused_vs")

    def test_single_point_empty_vertex_shader_compiles(self):
        self._compile_fixture("single")

    def test_hull_vertex_fetch_has_an_independent_argument_block(self):
        self._compile_fixture("vertex_input")

    def test_tessellation_builtin_inputs_compile_through_interface(self):
        self._compile_fixture("builtins")

    def test_aggregate_interfaces_compile_through_hull_and_domain(self):
        for scope in ("patch", "vertex"):
            for value_type in ("int", "uint", "float", "vec3", "vec4", "mat4x3"):
                with self.subTest(scope=scope, value_type=value_type):
                    self._compile_fixture((scope, value_type))

    def test_aggregate_interfaces_reject_invalid_spans_and_types(self):
        for defect in ("overlap", "overflow", "mismatch", "matrix_columns",
                       "zero_array", "cyclic_array"):
            with self.subTest(defect=defect):
                self._compile_fixture(("patch", "mat4x3"), defect)

    def _compile_fixture(self, patch, reject=None):
        archive = ROOT / "build/libpsbc.host.a"
        if not archive.exists() or not shutil.which("glslangValidator"):
            self.skipTest("requires built host PSBC and glslangValidator")
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            modules = []
            for stage in ("vert", "tesc", "tese"):
                defines = []
                target = tmp / (stage + ".spv")
                name = ("runtime_tess_two_patch" if stage == "vert" else
                        "runtime_tess_patch_data") if patch else "runtime_tess_delivery"
                if patch == "isolines":
                    name = "runtime_tess_coord" if stage == "vert" else "runtime_tess_isoline"
                if patch in ("patch32", "expand32", "unused_vs"):
                    name = "runtime_tess_patch32"
                if patch == "single":
                    name = "runtime_tess_single"
                if patch == "vertex_input":
                    name = "runtime_tess_delivery"
                if patch == "builtins":
                    name = "runtime_tess_delivery" if stage == "vert" else "runtime_tess_builtins"
                if isinstance(patch, tuple):
                    name = "runtime_tess_single" if stage == "vert" else "runtime_tess_aggregate"
                    if stage != "vert":
                        scope, value_type = patch
                        span = 4 if value_type == "mat4x3" else 1
                        defines = [f"-DVALUE_TYPE={value_type}",
                                   f"-DSECOND_LOCATION={span * (3 if scope == 'patch' else 1)}"]
                        if scope == "vertex":
                            defines.append("-DPER_VERTEX=1")
                source = ROOT / f"experiments/graphics/{name}.{stage}"
                if patch in ("expand32", "unused_vs") and stage == "tesc":
                    source = ROOT / "experiments/graphics/runtime_tess_expand32.tesc"
                if patch == "unused_vs" and stage == "vert":
                    source = ROOT / "experiments/graphics/runtime_tess_unused_output.vert"
                if patch == "vertex_input" and stage == "vert":
                    source = ROOT / "experiments/graphics/runtime_tess_vertex_input.vert"
                subprocess.run(["glslangValidator", "-V", *defines, "-o", str(target),
                    str(source)],
                    check=True, capture_output=True)
                modules.append(str(target))
            if reject:
                target = Path(modules[2 if reject == "mismatch" else 1])
                words = list(struct.unpack(f"<{target.stat().st_size // 4}I", target.read_bytes()))
                at = 5
                changed = False
                while at < len(words):
                    size, op = words[at] >> 16, words[at] & 65535
                    if reject in ("overlap", "overflow", "mismatch") and op == 71 and size == 4 and words[at+2:at+4] == [30, 12]:
                        words[at+3] = {"overlap": 0, "overflow": 25, "mismatch": 13}[reject]
                        changed = True
                    elif reject == "matrix_columns" and op == 24:
                        words[at+3] = 5
                        changed = True
                    elif reject == "zero_array" and op == 43 and size == 4 and words[at+3] == 3:
                        words[at+3] = 0
                        changed = True
                    elif reject == "cyclic_array" and op == 28:
                        words[at+2] = words[at+1]
                        changed = True
                    at += size
                self.assertTrue(changed, reject)
                target.write_bytes(struct.pack(f"<{len(words)}I", *words))
            exe = tmp / "inspect"
            fragment = tmp / "fragment.spv"
            subprocess.run(["glslangValidator", "-V", "-o", str(fragment),
                str(ROOT / ("experiments/graphics/runtime_tess_single.frag"
                    if patch == "single" else "experiments/graphics/runtime_tess_coord.frag"))],
                check=True, capture_output=True)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "native"),
                "-I" + str(ROOT.parent / "ps5-agc-gears/src"),
                "-I" + str(ROOT.parent / "ps5-agc-gears/include"),
                "-I" + str(ROOT / "third_party/vulkan-headers/include"),
                "-I" + str(ROOT / "third_party/psbc-reference"),
                str(ROOT / "tests/tess_delivery_inspect.c"),
                str(ROOT / "native/runtime_shader.c"),
                str(ROOT / "src/spirv_graphics_interface.c"),
                str(ROOT / "src/texture_format.c"), str(archive),
                "-lstdc++", "-lm", "-lpthread", "-o", str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe), *modules,
                "32" if patch == "patch32" else "1" if patch == "single" else "3", str(fragment)], capture_output=True,
                text=True, env={**os.environ, "PSBC_DEBUG_DISASM": "1",
                    **({"TESS_EXPECT_INVALID_INTERFACE": "1"} if reject else {}),
                    **({"TESS_VERTEX_INPUT": "1"} if patch == "vertex_input" else {})})
            self.assertEqual(result.returncode, 0, result.stderr)
            if reject:
                return
            hull, domain = result.stderr.split("DELIVERY_DOMAIN_BEGIN", 1)
            self.assertIn("DELIVERY_END", domain)
            if isinstance(patch, tuple):
                self.assertRegex(hull, r"ds_read")
                self.assertRegex(hull, r"buffer_store")
                self.assertRegex(domain, r"buffer_load")
                self.assertRegex(domain, r"exp .*pos0")
                return
            if patch == "builtins":
                self.assertRegex(domain, r"buffer_load")
                self.assertRegex(domain, r"exp .*pos0")
                return
            if patch == "vertex_input":
                self.assertIn("HULL_FETCH valid=1 mask=1", hull)
                self.assertRegex(hull, r"buffer_load")
                check_io(hull, domain)
                return
            if patch == "single":
                # No varying IO is required. Compilation must still produce
                # factor writes and a domain position export; this is not CTS
                # execution and says nothing about culling/rasterization.
                self.assertRegex(hull, r"buffer_store_dword")
                self.assertRegex(domain, r"exp .*pos0")
                return
            if patch in ("patch32", "expand32", "unused_vs"):
                if patch == "unused_vs":
                    self.assertNotRegex(hull, r"v_mul_u32_u24 0xc4,",
                                        "unused location7 must not enlarge the LS stride")
                    self.assertRegex(hull, r"v_mul_u32_u24 0x54,")
                if patch in ("expand32", "unused_vs"):
                    # TCS_OFFCHIP_LAYOUT_NUM_LS_OUTPUTS is bits17..22.
                    # Native PS5 supplies no dynamic layout SGPR: ACO must not
                    # extract this field (BFE width6/offset17 = 0x60011).
                    self.assertNotRegex(hull, r"s_bfe_u32[^\n]*0x60011")
                    self.assertRegex(hull, r"ds_write_b32[^\n]*storage:shared")
                    self.assertRegex(hull, r"ds_read_b32[^\n]*storage:shared")
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
