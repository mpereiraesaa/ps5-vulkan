import copy
from pathlib import Path
import struct
import sys
import unittest

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compile_program import metadata, resources, module_contract


def notes(changes=None):
    cs = {".scratch_en": False, ".scratch_memory_size": 0, ".lds_size": 0,
          ".debug_mode": False, ".trap_present": False, ".excp_en": 0, ".wgp_mode": False,
          ".wavefront_size": 32, ".entry_point_symbol": "_amdgpu_cs_main",
          ".user_sgprs": 2, ".user_data_reg_map": [0x10000000, 0] + [0xffffffff] * 30,
          ".threadgroup_dimensions": [64, 1, 1], ".vgpr_count": 3, ".sgpr_count": 10,
          ".float_mode": 192, ".ieee_mode": False, ".mem_ordered": True}
    cs.update(changes or {})
    pipeline = {".type": "Cs", ".hardware_stages": {".cs": cs},
                ".compute_registers": {".tg_size_en": True, ".tgid_x_en": True,
                    ".tgid_y_en": True, ".tgid_z_en": True, ".tidig_comp_cnt": 0}}
    return "amdgcn--amdpal--gfx1013\nAMDGPU Metadata: ---\n" + yaml.safe_dump(
        {"amdpal.version": [3, 0], "amdpal.pipelines": [pipeline]}) + "...\n"


def mapping():
    root = "userDataNode[0]"
    lines = ["[ResourceMapping]", f"{root}.visibility = 128", f"{root}.type = DescriptorTableVaPtr",
             f"{root}.offsetInDwords = 0", f"{root}.sizeInDwords = 1"]
    for index, binding in enumerate((1, 0)):
        node = f"{root}.next[{index}]"
        for key, value in (("type", "DescriptorBuffer"), ("offsetInDwords", index * 4),
                           ("sizeInDwords", 4), ("set", 0), ("binding", binding), ("strideInDwords", 0)):
            lines.append(f"{node}.{key} = {value}")
    return "\n".join(lines) + "\n[ComputePipelineState]\n"


class CompilerContractTests(unittest.TestCase):
    def test_metadata_and_changed_register_count(self):
        self.assertEqual(metadata(notes())["vgprs"], 3)
        self.assertEqual(metadata(notes({".vgpr_count": 17}))["vgprs"], 17)

    def test_unsupported_abi(self):
        for field, value in ((".scratch_en", True), (".lds_size", 256), (".wavefront_size", 64),
                             (".user_sgprs", 3), (".vgpr_count", 257), (".threadgroup_dimensions", [1024, 2, 1])):
            with self.subTest(field=field), self.assertRaises(ValueError):
                metadata(notes({field: value}))
        with self.assertRaises(ValueError):
            metadata(notes().replace("gfx1013", "gfx1030"))

    def test_binding_order_is_compiler_owned(self):
        parsed = resources(mapping())
        self.assertEqual([(r["binding"], r["table_offset_dwords"]) for r in parsed], [(1, 0), (0, 4)])

    def test_resource_rejections(self):
        for before, after in (("DescriptorBuffer", "DescriptorImage"), ("strideInDwords = 0", "strideInDwords = 4"),
                              ("offsetInDwords = 4", "offsetInDwords = 0"), ("binding = 0", "binding = 1"),
                              (".set = 0", ".set = 1")):
            with self.subTest(after=after), self.assertRaises(ValueError):
                resources(mapping().replace(before, after))

    def test_module_structure(self):
        words = [0x07230203, 0x10000, 0, 2, 0, (5 << 16) | 15, 5, 1,
                 0x6e69616d, 0, (6 << 16) | 16, 1, 17, 64, 1, 1]
        encode = lambda w: struct.pack(f"<{len(w)}I", *w)
        self.assertEqual(module_contract(encode(words)), {"entry": "main", "local_size": [64, 1, 1]})
        for index, value in ((0, 0), (5, 15), (6, 4), (8, 0), (10, (99 << 16) | 16)):
            broken = copy.copy(words); broken[index] = value
            with self.subTest(index=index), self.assertRaises(ValueError):
                module_contract(encode(broken))


if __name__ == "__main__":
    unittest.main()
