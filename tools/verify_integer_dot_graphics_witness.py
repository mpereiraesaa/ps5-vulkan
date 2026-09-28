#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify saved graphics integer-dot results offline, without deployment claims."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.integer_dot_spirv import cases
from tools.integer_dot_graphics_witness import GRAPHS, image_fixture, template

CASES = {case.name: case for case in cases()}
PROFILE = 'integer-dot-graphics-public-sdk-witness'


def fixture_contract(name, graph):
    case = CASES[name]
    records, expected = image_fixture(case, bgra=False)
    digest = 2166136261
    for word in struct.unpack('<128I', expected):
        digest = ((digest ^ word)*16777619) & 0xffffffff
    return {
        'profile': PROFILE, 'contract_version': 1, 'case': name, 'graph': graph,
        'stages': list(GRAPHS[graph]), 'width': 16, 'height': 8, 'instances': 128,
        'vertices': 6, 'pixels': 128, 'format': 'rgba8', 'record_stride': 256,
        'descriptor_range': 32768, 'expected_digest': f'{digest:08x}',
        'fixture_sha256': {key: hashlib.sha256(value).hexdigest() for key, value in
                           (('records', records), ('expected_rgba', expected))},
        'template_sha256': {stage: hashlib.sha256(template(stage,
            case.components if graph == 'all' or stage == graph else 0).encode()).hexdigest()
            for stage in GRAPHS[graph]},
    }


def verify(log, receipt, artifact):
    name, graph = artifact.get('case'), artifact.get('graph')
    if not isinstance(name, str) or name not in CASES or not isinstance(graph, str) or graph not in GRAPHS:
        raise ValueError('unknown graphics integer-dot case or graph')
    contract = fixture_contract(name, graph)
    if any(artifact.get(key) != value for key, value in contract.items()):
        raise ValueError('unexpected graphics fixture contract')
    shaders = artifact.get('shader_sha256')
    if not isinstance(shaders, dict) or set(shaders) != set(GRAPHS[graph]):
        raise ValueError('unexpected shader identities')
    hashes = [artifact.get(key) for key in ('eboot_sha256', 'sdk_sha256', 'source_sha256',
                                          'helper_sha256', 'header_sha256')]+list(shaders.values())
    if any(not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value) for value in hashes):
        raise ValueError('missing artifact identity')
    digest = hashlib.sha256(log).hexdigest()
    if (receipt.get('protocol') != 'ps5log/1' or receipt.get('title') != 'PPSA99994' or
            receipt.get('app') != 'ps5vk' or receipt.get('transport') != 'tcp' or
            receipt.get('clean') is not True or receipt.get('bye') is not True or
            receipt.get('gaps') != 0 or receipt.get('sha256') != digest or not receipt.get('run_id')):
        raise ValueError('incomplete or corrupt receipt')
    markers = [line[line.index('INTEGER_DOT_GRAPHICS_'):]
               for line in log.decode('utf-8', errors='strict').splitlines()
               if 'INTEGER_DOT_GRAPHICS_' in line]
    expected = [f'INTEGER_DOT_GRAPHICS_START case={name} graph={graph} width=16 height=8 instances=128 format=rgba8',
                'INTEGER_DOT_GRAPHICS_RESULT pixels=128 mismatches=0 guards=0 input_changes=0 '
                f'digest={contract["expected_digest"]} fence=complete',
                'INTEGER_DOT_GRAPHICS_RETIRED resources=clean']
    if markers != expected:
        raise ValueError('graphics results, inputs, guards, fence or retirement failed')
    return {'strict_verified': True, 'verification_scope': 'log_contents_only',
            'deployment_identity_verified': False, 'shader_binary_identity_verified': False,
            'run_id': receipt['run_id'], 'case': name, 'graph': graph, 'pixels': 128,
            'digest': contract['expected_digest'], 'log_sha256': digest,
            'eboot_sha256': artifact['eboot_sha256'],
            'native_identity_note': 'Requires separate exact deployed-artifact and shader identity evidence'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('log', 'receipt', 'artifact', 'out'):
        parser.add_argument('--'+key, type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                    json.loads(args.artifact.read_text()))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
