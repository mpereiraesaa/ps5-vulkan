"""Recompute the colour target's sample fields from the pinned register table.

The multisampled colour target states its sample geometry in CB_COLOR0_ATTRIB
(AGC context offset 0x31d, the fourth word of the target block), whose
NUM_SAMPLES and NUM_FRAGMENTS fields hold log2 of a count. Both the field
positions and the offset are read from the pinned gfx103 register table here
instead of being trusted, so a mistyped shift would program the wrong bits of
the right register - the one mistake the target's own tests cannot see.
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTERS = ROOT / "third_party/psbc-reference/src/amd/registers/gfx103.json"
CONTRACT = ROOT / "src/sample_rate_contract.h"
OFFSETS = ROOT / "src/color_attachment_contract.c"
MM_BASE = 0x28000


def register_table():
    data = json.loads(REGISTERS.read_text())

    def walk(node):
        if isinstance(node, dict):
            name, mapping = node.get("name"), node.get("map")
            if (name and isinstance(mapping, dict) and mapping.get("to") == "mm"
                    and isinstance(mapping.get("at"), int)):
                yield name, mapping["at"]
            for value in node.values():
                yield from walk(value)
        elif isinstance(node, list):
            for value in node:
                yield from walk(value)

    offsets = {}
    for name, raw in walk(data["register_mappings"]):
        offsets.setdefault(name, (raw - MM_BASE) // 4)
    fields = {name: {field["name"]: field["bits"] for field in entry["fields"]}
              for name, entry in data["register_types"].items()}
    return offsets, fields


def contract_constants():
    text = CONTRACT.read_text()
    shift = {}
    for name in ("NUM_SAMPLES", "NUM_FRAGMENTS"):
        match = re.search(rf"PS5VK_COLOR_ATTRIB_{name}_SHIFT = (\d+)", text)
        if not match:
            raise AssertionError(f"{name} shift not found in the contract")
        shift[name] = int(match.group(1))
    mask = re.search(r"PS5VK_COLOR_ATTRIB_SAMPLE_FIELDS_MASK UINT32_C\((0x[0-9a-f]+)\)", text)
    if not mask:
        raise AssertionError("sample-field mask not found in the contract")
    return shift, int(mask.group(1), 16)


def target_offsets():
    text = OFFSETS.read_text()
    block = re.search(r"ps5vk_color_attachment_offsets\[2\]\[[^]]*\] = \{(.*?)\n\};",
                      text, re.DOTALL)
    if not block:
        raise AssertionError("colour-target offset table not found")
    rows = re.findall(r"\{([^{}]*)\}", block.group(1))
    return [[int(value, 16) for value in re.findall(r"0x[0-9a-f]+", row)]
            for row in rows]


class SampleCountRegisters(unittest.TestCase):
    def table(self):
        """The pinned register table, or a skip where it is not vendored."""
        if not REGISTERS.is_file():
            self.skipTest("pinned gfx103 register table not present")
        return register_table()

    def test_sample_fields_match_the_pinned_table(self):
        _, fields = self.table()
        shift, mask = contract_constants()
        attrib = fields["CB_COLOR0_ATTRIB"]
        covered = 0
        for name in ("NUM_SAMPLES", "NUM_FRAGMENTS"):
            low, high = attrib[name]
            self.assertEqual(shift[name], low, f"{name} shift")
            covered |= ((1 << (high - low + 1)) - 1) << low
        # The mask covers exactly the two fields and nothing else.
        self.assertEqual(mask, covered)

    def test_the_word_sits_where_the_target_writes_it(self):
        offsets, _ = self.table()
        self.assertEqual(offsets["CB_COLOR0_ATTRIB"], 0x31d)
        # native/targets_ps5.c patches registers[3] of the colour target block,
        # whose fourth entry is CB_COLOR0_ATTRIB.
        self.assertEqual(target_offsets()[0][3], offsets["CB_COLOR0_ATTRIB"])


if __name__ == "__main__":
    unittest.main()
