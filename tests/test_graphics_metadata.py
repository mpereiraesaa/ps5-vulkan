import copy
import unittest
import yaml
from tools.graphics_metadata import decode_graphics_metadata


def fixture():
    stages = {}
    for name, wave in ((".gs", 32), (".ps", 64)):
        stages[name] = {".wavefront_size": wave, ".vgpr_count": 9, ".sgpr_count": 12,
            ".user_sgprs": 1, ".user_data_reg_map": [0x10000000] + [0xffffffff] * 31,
            ".entry_point_symbol": f"_amdgpu_{name[1:]}_main", ".float_mode": 192,
            ".ieee_mode": False, ".mem_ordered": True}
    return {"amdpal.pipelines": [{".type": "Ngg", ".hardware_stages": stages,
        ".graphics_registers": {".gs_vgpr_comp_cnt": 1, ".es_vgpr_comp_cnt": 0,
            ".vgt_shader_stages_en": {".gs_w32_en": True},
            ".spi_ps_in_control": {".ps_w32_en": False}}}]}


def decode(metadata):
    return decode_graphics_metadata("amdgcn--amdpal--gfx1013\nAMDGPU Metadata: ---\n" +
                                    yaml.safe_dump(metadata) + "\n...\n")


class GraphicsMetadataTests(unittest.TestCase):
    def test_separate_wave_allocations(self):
        data = fixture(); out = decode(data)
        self.assertEqual(out["stages"]["gs"]["rsrc1"] & 63, 1)
        self.assertEqual(out["stages"]["ps"]["rsrc1"] & 63, 2)
        data["amdpal.pipelines"][0][".hardware_stages"][".ps"][".wavefront_size"] = 32
        data["amdpal.pipelines"][0][".graphics_registers"][".spi_ps_in_control"][".ps_w32_en"] = True
        self.assertEqual(decode(data)["stages"]["ps"]["rsrc1"] & 63, 1)

    def test_execution_requirements_rejected(self):
        for key, value in ((".scratch_en", True), (".lds_size", 256), (".trap_present", 1),
                           (".float_mode", 256), (".vgpr_count", 257), (".wavefront_size", 16)):
            data = fixture(); data["amdpal.pipelines"][0][".hardware_stages"][".gs"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): decode(data)

    def test_stage_and_map_mismatch(self):
        data = fixture(); data["amdpal.pipelines"][0][".hardware_stages"][".ps"][".user_data_reg_map"][4] = 0
        with self.assertRaises(ValueError): decode(data)
        data = fixture(); data["amdpal.pipelines"][0][".graphics_registers"][".spi_ps_in_control"][".ps_w32_en"] = True
        with self.assertRaises(ValueError): decode(data)
