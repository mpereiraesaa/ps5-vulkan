"""Recompute the colour-target context offsets from the pinned register table.

The native path writes AGC context registers by offset, and the offset of a
context register is (raw address - 0x28000) / 4 in the pinned gfx103 table.
That rule reproduces every offset this driver already wrote before the second
target existed (CB_COLOR0_BASE -> 0x318, CB_TARGET_MASK -> 0x08e,
CB_BLEND0_CONTROL -> 0x1e0), and it is what makes CB_COLOR1 derivable rather
than guessed. A mistyped table would program the wrong register, so it is
checked against the pinned source instead of trusted.
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTERS = ROOT / "third_party/psbc-reference/src/amd/registers/gfx103.json"
CONTRACT = ROOT / "src/color_attachment_contract.c"
GEARS_TARGET = (ROOT / ".." / ".." / "projects/ps5-agc-gears/src/ps5_color_target.c")
MM_BASE = 0x28000
TARGET0 = [
    "CB_COLOR0_BASE", "CB_COLOR0_VIEW", "CB_COLOR0_INFO", "CB_COLOR0_ATTRIB",
    "CB_COLOR0_DCC_CONTROL", "CB_COLOR0_CMASK", "CB_COLOR0_FMASK",
    "CB_COLOR0_CLEAR_WORD0", "CB_COLOR0_CLEAR_WORD1", "CB_COLOR0_DCC_BASE",
    "CB_COLOR0_BASE_EXT", "CB_COLOR0_CMASK_BASE_EXT", "CB_COLOR0_FMASK_BASE_EXT",
    "CB_COLOR0_DCC_BASE_EXT", "CB_COLOR0_ATTRIB2", "CB_COLOR0_ATTRIB3",
]


def register_offsets():
    data = json.loads(REGISTERS.read_text())
    found = {}

    def walk(node):
        if isinstance(node, dict):
            name, mapping = node.get("name"), node.get("map")
            if (name and isinstance(mapping, dict) and mapping.get("to") == "mm"
                    and isinstance(mapping.get("at"), int)):
                found.setdefault(name, (mapping["at"] - MM_BASE) // 4)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(data["register_mappings"])
    return found


def table_from_contract():
    text = CONTRACT.read_text()
    block = re.search(r"ps5vk_color_attachment_offsets\[2\]\[[^]]*\] = \{(.*?)\n\};",
                      text, re.DOTALL)
    if not block:
        raise AssertionError("colour-target offset table not found")
    rows = re.findall(r"\{([^{}]*)\}", block.group(1))
    return [[int(value, 16) for value in re.findall(r"0x[0-9a-f]+", row)]
            for row in rows]


class ColorAttachmentOffsets(unittest.TestCase):
    def offsets(self):
        """The pinned register table, or a skip where it is not vendored.

        third_party is not part of this repository's history, so a checkout
        without the pinned PSBC sources cannot recompute anything; the same
        rule the upstream-selection check follows."""
        if not REGISTERS.is_file():
            self.skipTest("pinned gfx103 register table not present")
        return register_offsets()

    def test_target_zero_matches_the_pinned_table(self):
        offsets = self.offsets()
        expected = [offsets[name] for name in TARGET0]
        self.assertEqual(expected, table_from_contract()[0])

    def test_target_one_matches_the_pinned_table(self):
        offsets = self.offsets()
        expected = [offsets[name.replace("CB_COLOR0", "CB_COLOR1")] for name in TARGET0]
        self.assertEqual(expected, table_from_contract()[1])

    def test_blend_control_offsets_are_consecutive(self):
        offsets = self.offsets()
        self.assertEqual([0x1e0, 0x1e1],
                         [offsets["CB_BLEND0_CONTROL"], offsets["CB_BLEND1_CONTROL"]])

    def test_target_zero_offsets_still_match_the_builder_they_describe(self):
        """The block the native builder writes must stay the one the table names."""
        if not GEARS_TARGET.is_file():
            self.skipTest("ps5-agc-gears checkout not present")
        source = GEARS_TARGET.read_text()
        block = re.search(r"color_offsets\[[^]]*\] = \{(.*?)\};", source, re.DOTALL)
        self.assertIsNotNone(block)
        self.assertEqual(table_from_contract()[0],
                         [int(value, 16) for value in re.findall(r"0x[0-9a-f]+", block.group(1))])


if __name__ == "__main__":
    unittest.main()
