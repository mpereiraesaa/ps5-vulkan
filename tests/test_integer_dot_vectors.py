# SPDX-License-Identifier: GPL-3.0-or-later
"""Numerical oracle and runtime buffer tests, without treating host as GPU evidence."""
import struct
import subprocess
import tempfile
from pathlib import Path
import unittest

from tools.integer_dot_spirv import Case, cases
from tools.integer_dot_vectors import MASK, Sample, buffers, reference, samples


class IntegerDotVectors(unittest.TestCase):
    def test_hand_calculated_results_and_saturation(self):
        examples = (
            (Case('u', 2), Sample((MASK, 2), (2, 3), 99), 4),
            (Case('s', 2), Sample((MASK, 2), (2, MASK), 99), 0xfffffffc),
            (Case('su', 2), Sample((MASK, 2), (MASK, 3), 0), 7),
            (Case('s', 2, True), Sample((1, 2), (2, 3), 0x7fffffff), 0x7fffffff),
            (Case('s', 2, True), Sample((MASK, MASK), (2, 3), 0x80000000), 0x80000000),
            (Case('u', 2, True), Sample((1, 2), (2, 3), MASK), MASK),
            (Case('su', 4, False, (False, True)), Sample((255, 128, 1, 2), (255, 2, 3, 4), 0), (-500) & MASK),
        )
        for case, row, expected in examples:
            with self.subTest(case=case):
                self.assertEqual(expected, reference(case, row))

    def test_partial_overflow_rejected_even_if_final_sum_fits(self):
        row = Sample((0x7fffffff, 0x80000001), (2, 2), 0)
        self.assertEqual(0, reference(Case('s', 2), row))
        with self.assertRaisesRegex(ValueError, 'partial sum'):
            reference(Case('s', 2, True), row)
        with self.assertRaisesRegex(ValueError, 'partial sum'):
            reference(Case('u', 2, True), Sample((MASK, 0), (2, 0), 0))

    def test_buffers_and_carrier_invariance(self):
        for case in cases():
            lhs, rhs, acc, expected = buffers(case)
            self.assertEqual((128 * case.stride,) * 2 + (512, 512),
                             tuple(map(len, (lhs, rhs, acc, expected))))
            self.assertEqual(buffers(case), buffers(case))
            if case.components == 3:
                for payload in (lhs, rhs):
                    self.assertTrue(all(payload[i+12:i+16] == b'\xa5' * 4 for i in range(0, len(payload), 16)))
            if case.packed_types is not None:
                self.assertEqual(buffers(Case(case.mode, 4, case.saturating, (False, False))), buffers(case))
                self.assertEqual(bytes(samples(case)[0].lhs), lhs[:4])
            if case.saturating:
                values = struct.unpack('<128I', expected)
                self.assertIn(MASK if case.mode == 'u' else 0x7fffffff, values)
                if case.mode != 'u':
                    self.assertIn(0x80000000, values)

    def test_dataset_detects_corrupted_execution(self):
        for case in cases():
            rows = samples(case)
            expected = [reference(case, row) for row in rows]
            self.assertGreater(len(set(expected)), 8)
            reversed_rhs = [reference(case, Sample(row.lhs, tuple(reversed(row.rhs)), row.accumulator))
                            for row in rows]
            self.assertNotEqual(expected, reversed_rhs, case.name)
            self.assertNotEqual(expected, [row.lhs[0] for row in rows], case.name)
            if case.saturating:
                ignored_accumulator = [reference(case, Sample(row.lhs, row.rhs, 0)) for row in rows]
                self.assertNotEqual(expected, ignored_accumulator, case.name)
                plain = Case(case.mode, case.components, False, case.packed_types)
                wrapped_add = [(reference(plain, row) + row.accumulator) & MASK for row in rows]
                self.assertNotEqual(expected, wrapped_add, case.name)
            elif case.packed_types is not None:
                wrong_mode = 'u' if case.mode != 'u' else 's'
                wrong_sign = Case(wrong_mode, 4, False, case.packed_types)
                self.assertNotEqual(expected, [reference(wrong_sign, row) for row in rows], case.name)

    def test_invalid_shapes_and_float_rejected(self):
        for row in (Sample((1,), (1,), 0), Sample((-1, 0), (0, 0), 0),
                    Sample((0, 0), (0, 0), MASK+1)):
            with self.assertRaises(ValueError):
                reference(Case('s', 2), row)
        with self.assertRaises(ValueError):
            samples(Case('float', 2))

    def test_independent_wide_c_reference_on_all_runtime_inputs(self):
        # Separate implementation: read serialized SSBO bytes, decode packed
        # lanes explicitly, accumulate in __int128 and clamp once. UBSan guards
        # against accidentally introducing host signed overflow into the oracle.
        source = r'''
#include <stdint.h>
#include <stdio.h>
static uint32_t read32(void) { unsigned char b[4]; if(fread(b,1,4,stdin)!=4) return 0; return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24; }
static int64_t decode(uint32_t v, unsigned bits, unsigned sign) { return sign && (v & (1u<<(bits-1))) ? (int64_t)v-((int64_t)1<<bits) : v; }
int main(void) {
 for(unsigned c=0;c<42;c++) {
  unsigned mode=read32(), n=read32(), sat=read32(), packed=read32();
  for(unsigned row=0;row<128;row++) {
   uint32_t a[4]={0},b[4]={0}; unsigned stride=packed?1:(n==2?2:4);
   for(unsigned i=0;i<stride;i++) a[i]=read32();
   for(unsigned i=0;i<stride;i++) b[i]=read32();
   uint32_t accumulator=read32(); __int128 value=0;
   for(unsigned i=0;i<n;i++) {
    uint32_t x=packed?(a[0]>>(8*i))&255:a[i], y=packed?(b[0]>>(8*i))&255:b[i];
    value+=(__int128)decode(x,packed?8:32,mode!=0)*decode(y,packed?8:32,mode==1);
   }
   if(sat) { __int128 lo=mode?-(INT64_C(1)<<31):0, hi=mode?INT32_MAX:UINT32_MAX;
    value+=decode(accumulator,32,mode!=0); if(value<lo)value=lo; if(value>hi)value=hi;
   }
   uint32_t out=(uint32_t)value;
   unsigned char bytes[4]={out,out>>8,out>>16,out>>24}; fwrite(bytes,1,4,stdout);
  }
 }
 return 0;
}
'''
        payload, expected = bytearray(), bytearray()
        for case in cases():
            lhs, rhs, acc, want = buffers(case)
            payload.extend(struct.pack('<4I', {'u': 0, 's': 1, 'su': 2}[case.mode],
                                       case.components, case.saturating, case.packed_types is not None))
            for i in range(128):
                payload.extend(lhs[i*case.stride:(i+1)*case.stride])
                payload.extend(rhs[i*case.stride:(i+1)*case.stride])
                payload.extend(acc[i*4:i*4+4])
            expected.extend(want)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); src = root/'oracle.c'; exe = root/'oracle'
            src.write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', str(src), '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], input=payload, capture_output=True, check=True)
            self.assertEqual(b'', result.stderr)
            self.assertEqual(expected, result.stdout)
