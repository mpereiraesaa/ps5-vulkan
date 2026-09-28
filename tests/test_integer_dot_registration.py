# SPDX-License-Identifier: GPL-3.0-or-later
"""Trace the integer-dot measurement window to original, registered CTS factories."""
from collections import Counter
import contextlib
import io
import json
import tempfile
from unittest import mock
from tools import check_upstream_selection as selection
from pathlib import Path
import unittest

from tools.check_upstream_selection import _integer_dot_32_leaf_factories
from tools.check_dxvk_profile import implemented_device_extensions
from tools.make_measurement_manifest import build_measurement_manifest, selection_hash

ROOT = Path(__file__).resolve().parents[1]
MODULE = "vktSpvAsmIntegerDotProductTests"
SOURCE = ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/vulkan/spirv_assembly" / (MODULE + ".cpp")
FACTORIES = {f"createOp{op}ComputeGroup" for op in (
    "SDotKHR", "UDotKHR", "SUDotKHR", "SDotAccSatKHR", "UDotAccSatKHR", "SUDotAccSatKHR")}


class IntegerDotRegistration(unittest.TestCase):
    def test_original_factories_and_source_are_packaged(self):
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn(f'#include "{MODULE}.hpp"', package)
        for factory in FACTORIES:
            self.assertEqual(1, package.count(f"computeGroup->addChild(vkt::SpirVAssembly::{factory}(m_testCtx))"))
        self.assertIn(f'cts_root / "external/vulkancts/modules/vulkan/spirv_assembly/{MODULE}.cpp"', builder)
        self.assertNotIn(f'focused_sources / "{MODULE}.cpp"', builder)

    def test_diagnostic_does_not_promote_shipping_extension(self):
        self.assertNotIn("VK_KHR_shader_integer_dot_product", implemented_device_extensions())


@unittest.skipUnless(SOURCE.is_file(), "requires the pinned CTS checkout")
class IntegerDotSourceSelection(unittest.TestCase):
    def test_bounded_feature_eligible_selection(self):
        found = _integer_dot_32_leaf_factories(SOURCE.read_text())
        self.assertEqual(224, len(found))
        self.assertEqual(FACTORIES, set(found.values()))
        self.assertEqual(Counter({"createOpSDotKHRComputeGroup": 20, "createOpUDotKHRComputeGroup": 20,
                                 "createOpSUDotKHRComputeGroup": 20, "createOpSDotAccSatKHRComputeGroup": 56,
                                 "createOpUDotAccSatKHRComputeGroup": 52, "createOpSUDotAccSatKHRComputeGroup": 56}), Counter(found.values()))
        for path in found:
            self.assertTrue(path.endswith("_out32"))
            self.assertTrue("_v4i8_" in path if "_packed_" in path else "i32_" in path)
            self.assertNotIn("i16_", path)
        prefix = "dEQP-VK.spirv_assembly.instruction.compute."
        for leaf in ("opsdotkhr.all_ss_v5i32_out32", "opsdotkhr.all_ss_v4i16_out32",
                     "opsdotkhr.all_packed_ss_v4i8_out8", "opudotkhr.limits_uu_v4i32_out32"):
            self.assertNotIn(prefix + leaf, found)

    def test_frozen_diagnostics_derive_a_224_case_measurement(self):
        found = _integer_dot_32_leaf_factories(SOURCE.read_text())
        frozen = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        pending = [d for d in frozen["diagnostics"]
                   if d["category"] == "integer-dot-product-pending"]
        self.assertEqual(set(found), {d["path"] for d in pending})
        self.assertEqual(224, len(pending))
        self.assertTrue(all(d["expected_status"] == "Pass" and
                            set(d["features_required"]) ==
                            {"VK_KHR_shader_integer_dot_product", "shaderIntegerDotProduct"}
                            for d in pending))
        derived = build_measurement_manifest(frozen, {"integer-dot-product-pending"})
        self.assertEqual(224, derived["measurement"]["moved"])
        self.assertEqual(1103, len(derived["cases"]))
        self.assertEqual(frozen["cases"], derived["cases"][:879])
        self.assertEqual(selection_hash(frozen["cases"]),
                         derived["measurement"]["base_selection_hash"])

    def test_compute_only_window_and_registration_failures(self):
        source = SOURCE.read_text()
        entries = [{"path": path, "source": str(SOURCE.relative_to(ROOT / "third_party/vk-gl-cts")),
                    "category": "integer-dot-product", "expected_status": "Pass",
                    "features_required": ["VK_KHR_shader_integer_dot_product", "shaderIntegerDotProduct"]}
                   for path in _integer_dot_32_leaf_factories(source)]
        manifest = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        manifest.update(cases=entries, diagnostics=[], resource_contracts={})
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (ROOT / "tools/build_upstream_cts.py").read_text()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mp, pp, bp = root / "manifest.json", root / "package.cpp", root / "builder.py"
            def check():
                mp.write_text(json.dumps(manifest))
                pp.write_text(package); bp.write_text(builder)
                with mock.patch.multiple(selection, MANIFEST=mp, INTEGRATION_SOURCE=pp, BDA_BUILD_SOURCE=bp), \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    return selection.main()
            self.assertEqual(0, check())
            entries[0]["features_required"] = []
            self.assertEqual(1, check())
            entries[0]["features_required"] = ["VK_KHR_shader_integer_dot_product", "shaderIntegerDotProduct"]
            package = package.replace("vkt::SpirVAssembly::createOpSDotKHRComputeGroup(", "missingFactory(")
            self.assertEqual(1, check())
            package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
            builder = builder.replace(MODULE + ".cpp", "missing.cpp")
            self.assertEqual(1, check())

    def test_source_changes_change_or_refuse_selection(self):
        source = SOURCE.read_text()
        self.assertFalse(_integer_dot_32_leaf_factories(source.replace('"_out"', '"_result"')))
        without_vec3 = _integer_dot_32_leaf_factories(source.replace("{32, 3},", ""))
        self.assertTrue(without_vec3)
        self.assertFalse(any("_v3i32_" in path for path in without_vec3))
        fewer = _integer_dot_32_leaf_factories(source.replace('string("small-nosat")', 'string("replacement")'))
        self.assertFalse(any("small-nosat" in path for path in fewer))
        self.assertTrue(any("replacement" in path for path in fewer))
