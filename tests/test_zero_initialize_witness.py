"""Real shader initializer and strict zero-initialize witness oracle."""
import hashlib
import shutil
import subprocess
import tempfile
from pathlib import Path
import unittest

from tools.build_zero_initialize_witness import workgroup_null_variable
from tools.verify_zero_initialize_witness import fixture_contract, verify

ROOT = Path(__file__).resolve().parents[1]


class ZeroInitializeWitness(unittest.TestCase):
    def test_pinned_shader_has_workgroup_null_initializer(self):
        glslang = shutil.which('glslangValidator')
        if not glslang:
            self.skipTest('glslangValidator required')
        with tempfile.TemporaryDirectory() as directory:
            for defined in (False, True):
                binary = Path(directory) / ('initialized.spv' if defined else 'control.spv')
                cmd = [glslang, '-V', '--target-env', 'vulkan1.0', '-S', 'comp']
                if defined:
                    cmd.append('-DZERO_INITIALIZE=1')
                cmd += ['experiments/compute/zero_initialize_workgroup.comp', '-o', str(binary)]
                subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True)
                self.assertEqual(defined, workgroup_null_variable(binary.read_bytes()))

    def test_host_oracle_detects_wrong_value_and_guards(self):
        program = r'''
#include <ps5vk/ps5vk.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "examples/dxvk_render_witness/zero_initialize_compute.h"
int main(void) {
    unsigned char memory[ZERO_BYTES];
    memset(memory, 0xcd, sizeof(memory));
    for (unsigned i = 0; i < ZERO_WORDS; ++i) {
        uint32_t expected = i % 64 ? 123 : 246;
        memcpy(memory + ZERO_GUARD + i * 4, &expected, 4);
    }
    struct zero_initialize_result out = {0};
    zero_initialize_check(memory, &out);
    assert(out.outputs == 128 && !out.mismatches && !out.guards);
    memory[ZERO_GUARD + 17 * 4] ^= 1;
    memset(&out, 0, sizeof(out));
    zero_initialize_check(memory, &out);
    assert(out.mismatches == 1 && !out.guards);
    memory[ZERO_GUARD + 17 * 4] ^= 1;
    memory[0] ^= 1;
    memset(&out, 0, sizeof(out));
    zero_initialize_check(memory, &out);
    assert(!out.mismatches && out.guards == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'oracle.c'
            exe = Path(directory) / 'oracle'
            source.write_text(program)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-ffunction-sections',
                            '-fdata-sections', '-I.', '-Idist-sdk/include',
                            str(source), '-Wl,--gc-sections', '-o', str(exe)],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True)

    def test_strict_saved_receipt(self):
        artifact = fixture_contract('a' * 64)
        artifact.update({key: 'b' * 64 for key in
                         ('eboot_sha256', 'sdk_sha256', 'source_sha256', 'helper_sha256')})
        log = ('ZERO_INITIALIZE_START groups=2 local=64 outputs=128 initialized_bytes=2048\n'
               'ZERO_INITIALIZE_RESULT outputs=128 mismatches=0 guards=0 '
               f'digest={artifact["expected_digest"]} fence=complete\n'
               'ZERO_INITIALIZE_RETIRED resources=clean\n').encode()
        receipt = {'protocol': 'ps5log/1', 'title': 'PPSA99994', 'app': 'ps5vk',
                   'transport': 'tcp', 'clean': True, 'bye': True, 'gaps': 0,
                   'sha256': hashlib.sha256(log).hexdigest(), 'run_id': 'synthetic-test'}
        self.assertTrue(verify(log, receipt, artifact)['strict_verified'])
        for bad in (log.replace(b'mismatches=0', b'mismatches=1'),
                    log.replace(b'guards=0', b'guards=1'),
                    log.replace(b'fence=complete', b'fence=timeout'),
                    log.replace(b'RETIRED resources=clean', b'PENDING resources=retained'),
                    log + b'ZERO_INITIALIZE_RETIRED resources=clean\n'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                verify(bad, dict(receipt, sha256=hashlib.sha256(bad).hexdigest()), artifact)
        with self.assertRaises(ValueError):
            verify(log, dict(receipt, clean=False), artifact)
        with self.assertRaises(ValueError):
            verify(log, receipt, dict(artifact, initialized_bytes=4))


if __name__ == '__main__':
    unittest.main()
