# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline fixture packaging and saved evidence contracts, no native execution."""
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

from tools.build_integer_dot_witness import diagnostic_environment, fixture_header
from tools.integer_dot_spirv import binary
from tools.integer_dot_vectors import buffers
from tools.verify_integer_dot_witness import CASES, fixture_contract, verify

ROOT = Path(__file__).resolve().parents[1]


class IntegerDotNative(unittest.TestCase):
    def fixture(self, name):
        artifact = fixture_contract(name)
        artifact.update({key: 'a'*64 for key in ('eboot_sha256','sdk_sha256','source_sha256','helper_sha256')})
        log = (f'INTEGER_DOT_START case={name} groups=2 local=64 outputs=128\n'
               'INTEGER_DOT_RESULT outputs=128 mismatches=0 guards=0 input_changes=0 '
               f'digest={artifact["expected_digest"]} fence=complete\n'
               'INTEGER_DOT_RETIRED resources=clean\n').encode()
        return log, self.receipt(log), artifact

    def receipt(self, log):
        return {'protocol':'ps5log/1','title':'PPSA99994','app':'ps5vk','transport':'tcp',
                'clean':True,'bye':True,'gaps':0,'sha256':hashlib.sha256(log).hexdigest(),'run_id':'host-synthetic'}

    def test_all_fixtures_and_exact_c_bytes(self):
        for name, case in CASES.items():
            result = verify(*self.fixture(name))
            self.assertTrue(result['strict_verified'])
            self.assertFalse(result['deployment_identity_verified'])
            self.assertEqual('log_contents_only',result['verification_scope'])
            header = fixture_header(case)
            arrays = re.findall(r'static const uint(32|8)_t (\w+)\[\] = \{(.*?)\};',header,re.S)
            decoded = {key: b''.join(struct.pack('<I' if width=='32' else '<B',int(word,16)) for word in re.findall(r'0x([0-9a-fA-F]+)',body))
                       for width,key,body in arrays}
            self.assertEqual({'integer_dot_spirv':binary(case),**dict(zip(
                ('dot_lhs','dot_rhs','dot_accumulator','dot_expected'),buffers(case)))},decoded)
            self.assertIn(f'#define DOT_CASE_NAME "{name}"',header)

    def test_rejects_corrupted_missing_duplicate_or_reordered_markers(self):
        log,receipt,artifact = self.fixture('su-packed-us-sat')
        bad = [log.replace(b'mismatches=0',b'mismatches=1'),log.replace(b'guards=0',b'guards=1'),
               log.replace(b'input_changes=0',b'input_changes=1'),log.replace(b'outputs=128',b'outputs=127'),
               log.replace(b'fence=complete',b'fence=timeout'),log.replace(b'groups=2',b'groups=1'),
               log.replace(b'local=64',b'local=32'),log.replace(b'digest=',b'digest=0'),
               log+b'INTEGER_DOT_PENDING resources=retained\n',log+b'INTEGER_DOT_FAILURE result=-1\n',
               log+log.splitlines()[1]+b'\n',b'\n'.join(reversed(log.splitlines())),
               b'\n'.join(log.splitlines()[:2]),log+b'INTEGER_DOT_RESULT malformed\n']
        for value in bad:
            with self.assertRaises(ValueError): verify(value,self.receipt(value),artifact)
        for key,value in (('gaps',1),('bye',False),('clean',False),('sha256','b'*64),('transport','file')):
            with self.assertRaises(ValueError): verify(log,dict(receipt,**{key:value}),artifact)
        for key,value in (('case','s-v2-dot'),('local_size',[32,1,1]),('groups',1),('outputs',127),
                          ('input_stride',16),('expected_digest','00000000'),('helper_sha256','missing')):
            with self.assertRaises(ValueError): verify(log,receipt,dict(artifact,**{key:value}))
        changed=copy.deepcopy(artifact);changed['fixture_sha256']['lhs']='b'*64
        with self.assertRaises(ValueError): verify(log,receipt,changed)

    def test_environment_clears_inherited_diagnostics(self):
        before={'PATH':'/bin','PS5VK_INLINE_UNIFORM_DIAGNOSTIC':'1','PS5VK_TESS_GE_CNTL':'99'}
        self.assertEqual({'PATH':'/bin','PS5_PAYLOAD_SDK':'/sdk','PS5VK_USE_SDK':'1','PS5VK_INTEGER_DOT_DIAGNOSTIC':'1'},
                         diagnostic_environment(before,Path('/sdk')))
        self.assertEqual('1',before['PS5VK_INLINE_UNIFORM_DIAGNOSTIC'])

    def test_saved_log_cli(self):
        log,receipt,artifact=self.fixture('s-v3-sat')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'log').write_bytes(log)
            (root/'receipt.json').write_text(json.dumps(receipt))
            (root/'artifact.json').write_text(json.dumps(artifact))
            result=subprocess.run(['python3',str(ROOT/'tools/verify_integer_dot_witness.py'),
                '--log',str(root/'log'),'--receipt',str(root/'receipt.json'),'--artifact',str(root/'artifact.json'),
                '--out',str(root/'result.json')],capture_output=True,text=True)
            self.assertEqual(0,result.returncode,result.stderr)
            self.assertFalse(json.loads((root/'result.json').read_text())['deployment_identity_verified'])
