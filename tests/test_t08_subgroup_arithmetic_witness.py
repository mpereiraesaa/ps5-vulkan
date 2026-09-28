"""Exact SPIR-V census and strict arithmetic witness oracle."""
import hashlib
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
import unittest

from tools.build_t08_subgroup_arithmetic_witness import arithmetic_instructions
from tools.verify_t08_subgroup_arithmetic_witness import expected_digest, fixture_contract, verify

ROOT = Path(__file__).resolve().parents[1]


class T08ArithmeticWitness(unittest.TestCase):
    def test_shader_has_full_arithmetic_family(self):
        glslang = shutil.which('glslangValidator')
        if not glslang:
            self.skipTest('glslangValidator required')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'arithmetic.spv'
            subprocess.run([glslang, '-V', '--target-env', 'vulkan1.2', '-S', 'comp',
                            'experiments/compute/t08_subgroup_arithmetic_runtime.comp',
                            '-o', str(binary)], cwd=ROOT, check=True,
                           capture_output=True, text=True)
            payload = binary.read_bytes()
            self.assertTrue(arithmetic_instructions(payload))
            words = list(struct.unpack("<" + str(len(payload) // 4) + "I", payload))
            index = next(i for i in range(5, len(words))
                         if words[i] & 0xffff == 349 and words[i] >> 16 == 6)
            words[index] = (words[index] & ~0xffff) | 362
            self.assertFalse(arithmetic_instructions(struct.pack("<" + str(len(words)) + "I", *words)))

    def test_host_oracle_detects_mutations(self):
        program = r'''
#include <ps5vk/ps5vk.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "examples/dxvk_render_witness/subgroup_arithmetic_compute.h"
int main(void) {
    uint32_t memory[ARITH_TOTAL];
    for (unsigned i = 0; i < ARITH_TOTAL; ++i) memory[i] = 0xcdcdcdcd;
    for (unsigned lane = 0; lane < ARITH_LANES; ++lane)
        for (unsigned op = 0; op < ARITH_OPS; ++op)
            memory[ARITH_GUARD + lane * ARITH_OPS + op] = arithmetic_expected(lane % 32, op);
    struct subgroup_arithmetic_result out = {0};
    arithmetic_check(memory, &out);
    assert(out.outputs == ARITH_WORDS && !out.mismatches && !out.guards);
    assert(out.digest == 0xEXPECTED_DIGEST);
    memory[ARITH_GUARD + 17] ^= 1;
    memset(&out, 0, sizeof(out)); arithmetic_check(memory, &out);
    assert(out.mismatches == 1 && !out.guards);
    memory[ARITH_GUARD + 17] ^= 1; memory[0] ^= 1;
    memset(&out, 0, sizeof(out)); arithmetic_check(memory, &out);
    assert(!out.mismatches && out.guards == 1);
    return 0;
}
'''
        program = program.replace('EXPECTED_DIGEST', expected_digest())
        with tempfile.TemporaryDirectory() as directory:
            source, exe = Path(directory) / 'oracle.c', Path(directory) / 'oracle'
            source.write_text(program)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-ffunction-sections',
                            '-fdata-sections', '-I.', '-Idist-sdk/include', str(source),
                            '-Wl,--gc-sections', '-o', str(exe)], cwd=ROOT,
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True)

    def test_strict_saved_receipt(self):
        artifact = fixture_contract('a' * 64)
        artifact.update({key: 'b' * 64 for key in
                         ('eboot_sha256', 'sdk_sha256', 'source_sha256', 'helper_sha256')})
        log = ('T08_ARITHMETIC_START groups=2 local=64 operations=21 outputs=2688 public=off\n'
               'T08_ARITHMETIC_RESULT outputs=2688 mismatches=0 guards=0 '
               f'digest={artifact["expected_digest"]} fence=complete\n'
               'T08_ARITHMETIC_RETIRED resources=clean\n').encode()
        receipt = {'protocol': 'ps5log/1', 'title': 'PPSA99994', 'app': 'ps5vk',
                   'transport': 'tcp', 'clean': True, 'bye': True, 'gaps': 0,
                   'sha256': hashlib.sha256(log).hexdigest(), 'run_id': 'synthetic-test'}
        self.assertTrue(verify(log, receipt, artifact)['strict_verified'])
        for bad in (log.replace(b'mismatches=0', b'mismatches=1'),
                    log.replace(b'guards=0', b'guards=1'),
                    log.replace(b'fence=complete', b'fence=timeout'),
                    log.replace(b'RETIRED resources=clean', b'PENDING resources=retained'),
                    log + b'T08_ARITHMETIC_RETIRED resources=clean\n'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                verify(bad, dict(receipt, sha256=hashlib.sha256(bad).hexdigest()), artifact)
        with self.assertRaises(ValueError):
            verify(log, dict(receipt, clean=False), artifact)
        with self.assertRaises(ValueError):
            verify(log, receipt, dict(artifact, operations=7))


if __name__ == '__main__':
    unittest.main()
