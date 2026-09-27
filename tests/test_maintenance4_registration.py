# SPDX-License-Identifier: GPL-3.0-or-later
"""Original maintenance4 CTS selection, registration and drift rejection."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools import check_upstream_selection as selection
from tools.maintenance4_cts import entries, local_size_paths, multiple_shader_paths, LOCAL_SOURCE, MULTIPLE_SOURCE

ROOT = Path(__file__).resolve().parents[1]
CTS = ROOT/'third_party/vk-gl-cts'


@unittest.skipUnless((CTS/LOCAL_SOURCE).is_file(), 'requires pinned CTS checkout')
class Maintenance4Registration(unittest.TestCase):
    def test_original_shapes_and_support_requirements(self):
        found = entries(CTS)
        self.assertEqual(33, len(found))
        self.assertEqual(33, len({e['path'] for e in found}))
        local = local_size_paths((CTS/LOCAL_SOURCE).read_text())
        self.assertEqual(32, len(local))
        for suffix in ('', '_x', '_y', '_z'):
            self.assertIn('dEQP-VK.spirv_assembly.instruction.compute.localsize_id.specid_wgsize_none'+suffix, local)
            self.assertIn('dEQP-VK.spirv_assembly.instruction.compute.localsize_id.none_wgsize_literal'+suffix, local)
        self.assertFalse(any('none_wgsize_none' in p for p in local))
        self.assertEqual({'dEQP-VK.spirv_assembly.instruction.compute.multiple_shaders_extended.two_entry_points_execution_mode_id'},
                         multiple_shader_paths((CTS/MULTIPLE_SOURCE).read_text()))
        for e in found:
            self.assertEqual(['VK_KHR_maintenance4','maintenance4'], e['features_required'])

    def test_source_drift_changes_or_refuses_selection(self):
        source = (CTS/LOCAL_SOURCE).read_text()
        for old, new in [('j < 3', 'j < 4'), ('i < DE_LENGTH_OF_ARRAY(cases)', 'i < 1'),
                         ('groupName[useLocalSizeId]', 'groupName[0]'), ('_wgsize_', '_workgroup_'),
                         ('VK_KHR_maintenance4', 'VK_KHR_wrong'),
                         ('LSV_NONE && wgSizeType == LSV_NONE', 'LSV_LITERAL && wgSizeType == LSV_NONE')]:
            self.assertNotEqual(source, source.replace(old,new))
            self.assertFalse(local_size_paths(source.replace(old,new)))
        renamed = local_size_paths(source.replace('return "specid";', 'return "specialized";'))
        self.assertEqual(32, len(renamed))
        self.assertFalse(any('specid' in p for p in renamed))
        source = (CTS/MULTIPLE_SOURCE).read_text()
        self.assertFalse(multiple_shader_paths(source.replace('VK_KHR_maintenance4','VK_KHR_wrong')))
        self.assertEqual({'dEQP-VK.spirv_assembly.instruction.compute.multiple_shaders_extended.replacement'},
                         multiple_shader_paths(source.replace('"two_entry_points_execution_mode_id"','"replacement"')))

    def test_measurement_and_missing_registration_fail_closed(self):
        manifest = json.loads((ROOT/'cts/upstream/manifest.json').read_text())
        manifest.update(cases=entries(CTS), diagnostics=[], resource_contracts={})
        original_package = (ROOT/'cts/upstream/package_ps5.cpp').read_text()
        original_builder = (ROOT/'tools/build_upstream_cts.py').read_text()
        original_wrapper = (ROOT/'cts/upstream/volatile_atomic_focus.cpp').read_text()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            mp, pp, bp, wp = [root/name for name in ('manifest.json','package.cpp','builder.py','wrapper.cpp')]
            def check(package=original_package, builder=original_builder, wrapper=original_wrapper):
                mp.write_text(json.dumps(manifest)); pp.write_text(package); bp.write_text(builder); wp.write_text(wrapper)
                with mock.patch.multiple(selection, MANIFEST=mp, INTEGRATION_SOURCE=pp,
                                         BDA_BUILD_SOURCE=bp, LOCAL_SIZE_WRAPPER=wp), \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    return selection.main()
            self.assertEqual(0, check())
            for factory in ('createFocusedLocalSizeIdGroup','createMultipleShaderExtendedGroup'):
                self.assertEqual(1, check(package=original_package.replace(factory+'(', 'missingFactory(')))
            for file in ('volatile_atomic_focus.cpp','vktSpvAsmMultipleShadersTests.cpp'):
                self.assertEqual(1, check(builder=original_builder.replace(file, 'missing.cpp')))
            self.assertEqual(1, check(wrapper=original_wrapper.replace('testCtx, true', 'testCtx, false')))
            manifest['cases'][0]['features_required'] = []
            self.assertEqual(1, check())
            manifest['cases'][0]['features_required'] = ['VK_KHR_maintenance4','maintenance4']
            manifest['cases'][0]['path'] += '_invented'
            self.assertEqual(1, check())
