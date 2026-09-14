import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LICENSE_ID = "GPL-3.0-or-later"
PS5_OPENGL_COMMIT = "7f9bfabdddb187a11e4401058eba8c9e55194d0a"


class LicensePolicyTests(unittest.TestCase):
    def test_project_grant_is_explicit_and_complete(self):
        license_text = (ROOT / "LICENSE").read_text()
        self.assertTrue(license_text.startswith("                    GNU GENERAL PUBLIC LICENSE\n"))
        self.assertIn("Version 3, 29 June 2007", license_text)
        self.assertIn("END OF TERMS AND CONDITIONS", license_text)
        self.assertEqual(json.loads((ROOT / "project.json").read_text())["license"],
                         LICENSE_ID)
        readme = (ROOT / "README.md").read_text()
        self.assertIn(LICENSE_ID, readme)
        self.assertIn("[LICENSE](LICENSE)", readme)
        self.assertIn("[LICENSING.md](LICENSING.md)", readme)

    def test_ps5_opengl_provenance_is_pinned_and_reaches_derived_files(self):
        policy = (ROOT / "LICENSING.md").read_text()
        self.assertIn(PS5_OPENGL_COMMIT, policy)
        for relative in ("native/runtime_shader.c", "src/color_detile.c",
                         "src/vk_sampler.c", "src/texture_format.c",
                         "src/texture_descriptor.c", "src/texture_layout.c",
                         "native/runtime_graphics_compiler.c",
                         "src/graphics_formats.h"):
            self.assertIn(f"`{relative}`", policy)
            source = (ROOT / relative).read_text()
            self.assertIn("Copyright (C) 2026 BlackBearReloaded", source)
            self.assertIn("SPDX-License-Identifier: GPL-3.0-or-later", source)
            self.assertIn("ps5-opengl", source.lower())

    def test_staged_sdk_carries_the_same_license_and_static_link_notice(self):
        builder = (ROOT / "tools/build_sdk.py").read_text()
        self.assertIn('shutil.copyfile(ROOT / "LICENSE", DIST_SDK / "LICENSE")',
                      builder)
        self.assertIn("static library", builder)
        self.assertIn(LICENSE_ID, builder)


if __name__ == "__main__":
    unittest.main()
