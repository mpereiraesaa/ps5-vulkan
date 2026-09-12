import struct
import unittest
from tools.cts_heap_parameters import use_application_heap


class HeapParametersTests(unittest.TestCase):
    def image(self):
        data = bytearray(b'\x7fELF\x02\x01' + bytes(58))
        for size in (0xa8, 0x38, 0x10, 0x78, 0xc0, 0x38):
            data += struct.pack('<Q', size) + bytes(size - 8)
        struct.pack_into('<II', data, 72, 14, 1)
        data += struct.pack('<QI', 0xffffffffffffffff, 1)
        return bytes(data)

    def test_only_mode_changes(self):
        original = self.image()
        changed = use_application_heap(original)
        self.assertEqual([i for i,(a,b) in enumerate(zip(original,changed)) if a != b], [76])
        self.assertEqual(changed[76:80], bytes(4))

    def test_reject_changed_layout(self):
        data = bytearray(self.image()); data[64 + 0xa8] = 0
        with self.assertRaises(ValueError): use_application_heap(bytes(data))

    def test_reject_duplicate_and_already_modified(self):
        original = self.image()
        with self.assertRaises(ValueError): use_application_heap(original + original[64:])
        with self.assertRaises(ValueError): use_application_heap(use_application_heap(original))

    def test_reject_non_elf(self):
        with self.assertRaises(ValueError): use_application_heap(bytes(100))
