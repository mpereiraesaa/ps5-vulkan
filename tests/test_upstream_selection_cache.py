import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_gate():
    spec = importlib.util.spec_from_file_location(
        "check_upstream_selection", ROOT / "tools/check_upstream_selection.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


class SelectionGateCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gate = load_gate()

    def test_edited_source_is_read_again(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.cpp"
            path.write_text('addChild("first");\n')
            self.assertEqual('addChild("first");\n', self.gate._read_source(path))
            path.write_text('addChild("second one");\n')
            self.assertEqual('addChild("second one");\n', self.gate._read_source(path))

    def test_same_size_edit_with_new_mtime_is_read_again(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.cpp"
            path.write_text("aaaa")
            self.assertEqual("aaaa", self.gate._read_source(path))
            path.write_text("bbbb")
            stat = path.stat()
            os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000))
            self.assertEqual("bbbb", self.gate._read_source(path))

    def test_module_searchable_tracks_added_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.cpp").write_text('"alpha"')
            first = self.gate._module_searchable("integration", root)
            self.assertFalse(self.gate._quoted_in("beta", first))
            (root / "b.cpp").write_text('"beta"')
            second = self.gate._module_searchable("integration", root)
            self.assertTrue(self.gate._quoted_in("beta", second))
            self.assertTrue(second.startswith("integration\n"))

    def test_quoted_in_matches_only_the_quoted_literal(self):
        self.assertTrue(self.gate._quoted_in("a.b", 'x("a.b")'))
        self.assertFalse(self.gate._quoted_in("a.b", 'x("axb")'))
        self.assertFalse(self.gate._quoted_in("ab", 'x("abc")'))

    def test_memoized_recognizers_return_independent_copies(self):
        text = 'static const X t[] = {{ "one", 1 }, { "two", 2 }};'
        first = self.gate._table_composed_leaf_names(text, text)
        first.add("mutated")
        second = self.gate._table_composed_leaf_names(text, text)
        self.assertNotIn("mutated", second)

    def test_function_extraction_is_unchanged_by_the_cache(self):
        text = "int a() {\n  return 1;\n}\nint b() {\n  if (x) { y(); }\n}\n"
        self.assertEqual(self.gate._source_function_at_line(text, 4),
                         self.gate._source_function_at_line(text, 4))
        self.assertEqual("int b() {\n  if (x) { y(); }\n}",
                         self.gate._source_function_at_line(text, 4))


if __name__ == "__main__":
    unittest.main()
