# SPDX-License-Identifier: GPL-3.0-or-later
"""Graphics fixture delivery and offline saved-log negative controls."""
import copy
import hashlib
from pathlib import Path
import re
import shutil
import struct
import tempfile
import unittest

from tools.integer_dot_graphics_fixture import compile_templates, shader_modules, fixture_header
from tools.integer_dot_graphics_witness import GRAPHS, image_fixture
from tools.verify_integer_dot_graphics_witness import CASES, fixture_contract, verify

ROOT = Path(__file__).resolve().parents[1]


def receipt(log):
    return dict(protocol='ps5log/1', title='PPSA99994', app='ps5vk', transport='tcp',
                clean=True, bye=True, gaps=0, sha256=hashlib.sha256(log).hexdigest(), run_id='synthetic')


def saved(name, graph):
    artifact = fixture_contract(name, graph)
    artifact.update({key: 'a'*64 for key in ('eboot_sha256', 'sdk_sha256', 'source_sha256',
                                            'helper_sha256', 'header_sha256')})
    artifact['shader_sha256'] = {stage: 'b'*64 for stage in GRAPHS[graph]}
    log = (f'INTEGER_DOT_GRAPHICS_START case={name} graph={graph} width=16 height=8 instances=128 format=rgba8\n'
           'INTEGER_DOT_GRAPHICS_RESULT pixels=128 mismatches=0 guards=0 input_changes=0 '
           f'digest={artifact["expected_digest"]} fence=complete\n'
           'INTEGER_DOT_GRAPHICS_RETIRED resources=clean\n').encode()
    return log, receipt(log), artifact


class IntegerDotGraphicsNative(unittest.TestCase):
    def test_all_saved_contracts(self):
        for name in CASES:
            for graph in GRAPHS:
                result = verify(*saved(name, graph))
                self.assertTrue(result['strict_verified'])
                self.assertEqual('log_contents_only', result['verification_scope'])
                self.assertFalse(result['deployment_identity_verified'])
                self.assertFalse(result['shader_binary_identity_verified'])

    def test_rejects_wrong_results_identity_and_incomplete_retirement(self):
        log, rec, artifact = saved('su-packed-us-sat', 'all')
        bad = [log.replace(old, new) for old, new in (
            (b'pixels=128', b'pixels=127'), (b'mismatches=0', b'mismatches=1'),
            (b'guards=0', b'guards=1'), (b'input_changes=0', b'input_changes=1'),
            (b'fence=complete', b'fence=timeout'), (b'format=rgba8', b'format=bgra8'),
            (b'graph=all', b'graph=vert'), (b'width=16', b'width=32'),
            (b'instances=128', b'instances=1'), (b'digest=', b'digest=0'))]
        bad += [log+b'INTEGER_DOT_GRAPHICS_PENDING resources=retained\n',
                log+b'INTEGER_DOT_GRAPHICS_FAILURE result=-1\n',
                log+log.splitlines()[1]+b'\n', b'\n'.join(reversed(log.splitlines())),
                b'\n'.join(log.splitlines()[:2])]
        for value in bad:
            with self.assertRaises(ValueError): verify(value, receipt(value), artifact)
        for key, value in (('transport', 'file'), ('gaps', 1), ('clean', False), ('bye', False),
                           ('sha256', 'c'*64), ('run_id', ''), ('title', 'wrong')):
            with self.assertRaises(ValueError): verify(log, dict(rec, **{key: value}), artifact)
        for key, value in (('graph', 'compute'), ('case', 'missing'), ('stages', ['vert', 'frag']),
                           ('descriptor_range', 48), ('format', 'bgra8'), ('header_sha256', 'missing')):
            with self.assertRaises(ValueError): verify(log, rec, dict(artifact, **{key: value}))
        for key in ('shader_sha256', 'fixture_sha256', 'template_sha256'):
            changed = copy.deepcopy(artifact)
            changed[key].pop(next(iter(changed[key])))
            with self.assertRaises(ValueError): verify(log, rec, changed)

    def test_exact_sdk_header_bytes_all_graphs(self):
        glslang = shutil.which('glslangValidator')
        fallback = ROOT/'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
        if not glslang and fallback.is_file(): glslang = str(fallback)
        if not glslang: self.skipTest('requires glslang')
        with tempfile.TemporaryDirectory() as directory:
            templates = compile_templates(directory, glslang)
            for case in CASES.values():
                for graph in GRAPHS:
                    modules = shader_modules(case, graph, templates)
                    header = fixture_header(case, graph, modules)
                    arrays = re.findall(r'static const (?:uint(32)_t|unsigned char) (\w+)\[\] = \{(.*?)\};', header, re.S)
                    decoded = {key: b''.join(struct.pack('<I' if width else '<B', int(word, 16))
                        for word in re.findall(r'0x([0-9a-fA-F]+)', body)) for width, key, body in arrays}
                    expected = {'dot_'+stage: data for stage, data in modules.items()}
                    expected.update(dict(zip(('dot_records', 'dot_expected_rgba'), image_fixture(case, bgra=False))))
                    self.assertEqual(expected, decoded)
                    self.assertIn(f'#define DOT_NEEDS_TESS {int("tesc" in modules)}', header)
                    self.assertIn(f'#define DOT_NEEDS_GEOM {int("geom" in modules)}', header)
                    active = set(modules if graph == 'all' else [graph])
                    for stage, data in modules.items():
                        words = struct.unpack(f'<{len(data)//4}I', data)
                        opcodes = []; offset = 5
                        while offset < len(words):
                            count = words[offset] >> 16
                            self.assertGreater(count, 0)
                            opcodes.append(words[offset] & 65535); offset += count
                        self.assertEqual(offset, len(words))
                        self.assertEqual(int(stage in active), sum(4450 <= op <= 4455 for op in opcodes))
            with self.assertRaises(ValueError): fixture_header(next(iter(CASES.values())), 'all', {})
