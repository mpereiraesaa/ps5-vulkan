"""Count the actual generated TCS interface, not just source comments.

This does not establish native execution or the driver's advertised limits.
Includes declared Position and all six tessellation factors explicitly.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from tess_patch_fixture import relocate_total_patch


def declared_output_components(data):
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    types, constants, outputs, patches = {}, {}, [], set()
    at = 5
    while at < len(words):
        size, op = words[at] >> 16, words[at] & 65535
        if not size or at + size > len(words):
            raise ValueError('malformed instruction')
        args = words[at + 1:at + size]
        if op in (21, 22, 23, 28, 30, 32):
            types[args[0]] = (op, args[1:])
        elif op == 43:
            constants[args[1]] = args[2]
        elif op == 59 and args[2] == 3:
            outputs.append((args[1], args[0]))
        elif op == 71 and args[1] == 15:  # Patch
            patches.add(args[0])
        at += size

    def count(type_id):
        op, args = types[type_id]
        if op in (21, 22):
            if args[0] != 32:
                raise ValueError('fixture expects 32-bit scalars')
            return 1
        if op == 23:
            return count(args[0]) * args[1]
        if op == 28:
            return count(args[0]) * constants[args[1]]
        if op == 30:
            return sum(count(member) for member in args)
        raise ValueError('unexpected output type')

    vertex = patch = 0
    for variable, pointer in outputs:
        op, args = types[pointer]
        if op != 32 or args[0] != 3:
            raise ValueError('expected output pointer')
        target = args[1]
        if variable in patches:
            patch += count(target)
        else:
            op, args = types[target]
            if op != 28 or constants[args[1]] != 32:
                raise ValueError('expected 32 output control points')
            vertex += count(args[0])
    return vertex, patch


class TessTotalEnvelopeTests(unittest.TestCase):
    def test_compiled_interface_has_exact_declared_total(self):
        compiler = shutil.which('glslangValidator')
        if not compiler:
            self.skipTest('glslangValidator unavailable')
        with tempfile.TemporaryDirectory() as directory:
            for stage in ('tesc', 'tese'):
                output = Path(directory) / (stage + '.spv')
                subprocess.run([compiler, '-V', '--target-env', 'vulkan1.0',
                                str(ROOT / 'experiments/graphics' /
                                    ('runtime_tess_total.' + stage)), '-o', str(output)],
                               capture_output=True, check=True)
                original = output.read_bytes()
                relocated = relocate_total_patch(original)
                before = struct.unpack('<%dI' % (len(original)//4), original)
                after = struct.unpack('<%dI' % (len(relocated)//4), relocated)
                self.assertEqual(sorted((a,b) for a,b in zip(before,after) if a!=b),
                                 [(31,0), (53,22)])
                with self.assertRaises(ValueError):
                    relocate_total_patch(relocated)
                output.write_bytes(relocated)
                validator = ROOT / 'build/spirv-tools-host/tools/spirv-val'
                if validator.is_file():
                    subprocess.run([str(validator), '--target-env', 'vulkan1.0', str(output)],
                                   capture_output=True, check=True)
                if stage == 'tesc':
                    vertex, patch = declared_output_components(output.read_bytes())
                    self.assertEqual((vertex, patch), (125, 96))
                    self.assertEqual(32 * vertex + patch, 4096)

    def test_malformed_module_rejected(self):
        for data in (b'', b'wrong magic padding.....',
                     struct.pack('<6I', 0x07230203, 0x10000, 0, 5, 0, 0)):
            with self.assertRaises(ValueError):
                relocate_total_patch(data)

    def test_component_values_distinguish_vertices_and_slots(self):
        values = [128*v+c for v in range(32) for c in range(121)]
        values += list(range(8192, 8282))
        self.assertEqual(len(values), len(set(values)))
        self.assertTrue(all(float(v) == v for v in values))


if __name__ == '__main__':
    unittest.main()
