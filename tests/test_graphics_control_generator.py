from pathlib import Path
import unittest


SOURCE = (Path(__file__).resolve().parents[1] / "tools/compile_graphics_control.py").read_text()
NATIVE_BUILDER = (Path(__file__).resolve().parents[1] / "tools/build_native.py").read_text()


class GraphicsControlGenerator(unittest.TestCase):
    def test_uses_current_descriptor_signature_schema(self):
        self.assertNotIn(".combined_image", SOURCE)
        self.assertIn(".type={[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}", SOURCE)
        self.assertIn("[0]={.count=1,.first=0", SOURCE)
        self.assertIn(".count=0,.first=1,.stages=0", SOURCE)

    def test_module_keys_use_designated_initializers(self):
        self.assertIn(".vertex = {.words=graphics_vertex_spirv", SOURCE)
        self.assertIn(".fragment = {.words=graphics_fragment_spirv", SOURCE)

    def test_native_builder_rejects_duplicate_object_stems(self):
        self.assertIn('raise SystemExit("Duplicate native object stems:', NATIVE_BUILDER)
        self.assertEqual(NATIVE_BUILDER.count('ROOT / "src/vk_pipeline_cache.c"'), 1)


if __name__ == "__main__":
    unittest.main()
