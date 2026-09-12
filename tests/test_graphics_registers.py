import hashlib
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import graphics_registers as registers


class RegisterTests(unittest.TestCase):
    def setUp(self):
        # Small explicit schema keeps unit tests independent of the Mesa tree.
        # Actual gfx10.json identity and translation are checked by the compiler
        # control; this fixture checks the packing engine, not hardware values.
        data = json.dumps(dict(register_types={"COLOR": dict(fields=[
            dict(name="OUTPUT0", bits=[0, 3]), dict(name="OUTPUT7", bits=[28, 31])])},
            register_mappings=[dict(name="COLOR", type_ref="COLOR", map=dict(at=0x28238, to="mm"))])).encode()
        with patch.object(registers, "SCHEMA_SHA256", hashlib.sha256(data).hexdigest()):
            self.schema = registers.RegisterSchema(data)

    def test_order_independent_packing(self):
        first = self.schema.pack("COLOR", dict(OUTPUT7=10, OUTPUT0=3))
        second = self.schema.pack("COLOR", dict(OUTPUT0=3, OUTPUT7=10))
        self.assertEqual(first, second)
        self.assertEqual(first["value"], 0xa0000003)
        self.assertEqual(first["byte_address"], 0x28238)

    def test_unknown_and_overflow_rejected_not_masked(self):
        for fields in (dict(OUTPUT0=16), dict(OUTPUT0=-1), dict(OUTPUT0="3"), dict(UNKNOWN=0)):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                self.schema.pack("COLOR", fields)

    def test_schema_identity_is_required(self):
        with self.assertRaises(ValueError):
            registers.RegisterSchema(b"{}")


if __name__ == "__main__":
    unittest.main()
