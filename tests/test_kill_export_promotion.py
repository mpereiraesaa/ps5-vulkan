"""The kill export-memory rule is promoted: no build may switch it off again.

The T11 pixel-removal witness measured the rule on hardware (without it, every
pixel removed by OpKill, OpTerminateInvocation or OpDemoteToHelperInvocation
still wrote depth and stencil; with it, none did). The measurement switch that
selected it was retired at promotion, and this test forbids its return.
"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RETIRED_SWITCH = "PS5VK_KILL_" + "EXPORT_MEMORY"


class KillExportPromotion(unittest.TestCase):
    def test_retired_switch_does_not_return(self):
        tracked = subprocess.check_output(["git", "ls-files"], cwd=ROOT, text=True).split()
        offenders = []
        for name in tracked:
            path = ROOT / name
            if path == Path(__file__).resolve() or not path.is_file():
                continue
            try:
                if RETIRED_SWITCH in path.read_text(encoding="utf-8"):
                    offenders.append(name)
            except UnicodeDecodeError:
                continue
        self.assertEqual(offenders, [])

    def test_draw_state_applies_rule_unconditionally(self):
        source = (ROOT / "native/draw_state_ps5.c").read_text()
        call = source.index("ps5vk_kill_export_memory_apply(result.cx,result.cx_count)")
        # No preprocessor conditional is open around the call: every #if
        # before it in the file is closed before it.
        prefix = source[:call]
        opened = len(re.findall(r"^\s*#\s*if", prefix, re.M))
        closed = len(re.findall(r"^\s*#\s*endif", prefix, re.M))
        self.assertEqual(opened, closed)


if __name__ == "__main__":
    unittest.main()
