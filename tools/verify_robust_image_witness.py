#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify saved image robustness logs, receipts and local executables offline.

This verifies saved contents and the local artifact hash. It does not establish
which executable ran on the console, firmware, or Vulkan feature conformance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.build_upstream_cts import tessellation_build_profile
from tools.robust_image_witness import cases, coordinates, coordinate_data, image_data, expected_read, shader

CASES = {case.name: case for case in cases()}
PROFILE = 'image-robustness-public-sdk-witness'
SAMPLE = re.compile(r'ROBUST_IMAGE_SAMPLE index=(0|[1-9][0-9]*) value=([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8})')


def digest(values):
    result = 2166136261
    for value in values:
        result = ((result ^ value) * 16777619) & 0xffffffff
    return f'{result:08x}'


def resource_contract(case):
    if case.role == 'sampled_image':
        return {'mode': 'fetch-only', 'bytes': 0, 'digest': '00000000'}
    data = image_data(case, True) if case.write else image_data(case)
    return {'mode': 'mapped-texel' if case.dimension == 'Buffer' else 'image-copy',
            'bytes': len(data), 'digest': digest(data)}


def fixture_contract(name):
    case = CASES[name]
    payloads = {'coordinates': coordinate_data(case), 'image': image_data(case),
                'image_after': image_data(case, True) if case.write else image_data(case),
                'shader_source': shader(case).encode()}
    return {'profile': PROFILE, 'contract_version': 1, 'case': name,
            'requirement': case.requirement, 'count': len(coordinates(case)), 'write': case.write,
            'coordinates': [list(c) for c in coordinates(case)],
            'scope': 'integer coordinates at valid mip zero; numerical measurement only',
            'resource': resource_contract(case),
            'fixture_sha256': {k: hashlib.sha256(v).hexdigest() for k, v in payloads.items()}}


def verify(log: bytes, receipt: dict, artifact: dict, eboot: bytes) -> dict:
    if not isinstance(receipt, dict) or not isinstance(artifact, dict):
        raise ValueError('receipt and artifact must be objects')
    name = artifact.get('case')
    if not isinstance(name, str) or name not in CASES:
        raise ValueError('unknown image robustness case')
    contract = fixture_contract(name)
    # Compare JSON types too: true is not an integer count or a contract version.
    if any(json.dumps(artifact.get(k), sort_keys=True) != json.dumps(v, sort_keys=True)
           for k, v in contract.items()):
        raise ValueError('unexpected image robustness fixture contract')
    for k in ('eboot_sha256', 'source_sha256', 'helper_sha256', 'sdk_sha256', 'header_sha256', 'shader_sha256'):
        if not isinstance(artifact.get(k), str) or not re.fullmatch('[0-9a-f]{64}', artifact[k]):
            raise ValueError('missing artifact hash')
    if not eboot or hashlib.sha256(eboot).hexdigest() != artifact['eboot_sha256']:
        raise ValueError('local executable does not match artifact')
    expected_profile = tessellation_build_profile({'PS5VK_IMAGE_ROBUSTNESS_DIAGNOSTIC': '1'})
    if json.dumps(artifact.get('build_profile'), sort_keys=True) != json.dumps(expected_profile, sort_keys=True):
        raise ValueError('unexpected measurement build profile')
    log_sha = hashlib.sha256(log).hexdigest()
    if (receipt.get('protocol') != 'ps5log/1' or receipt.get('title') != 'PPSA99994' or
            receipt.get('app') != 'ps5vk' or receipt.get('transport') != 'tcp' or
            receipt.get('clean') is not True or receipt.get('bye') is not True or
            type(receipt.get('gaps')) is not int or receipt['gaps'] != 0 or
            receipt.get('sha256') != log_sha or not isinstance(receipt.get('run_id'), str) or
            not receipt['run_id'].strip()):
        raise ValueError('incomplete or corrupt receipt')
    try:
        lines = log.decode('utf-8', errors='strict').splitlines()
    except UnicodeDecodeError as error:
        raise ValueError('invalid log encoding') from error
    markers = [line[line.index('ROBUST_IMAGE_'):] for line in lines if 'ROBUST_IMAGE_' in line]
    count = contract['count']
    if len(markers) != count + 4:
        raise ValueError('missing, duplicate or failed witness markers')
    if markers[0] != f'ROBUST_IMAGE_START contract=1 case={name} count={count} write={int(contract["write"])}':
        raise ValueError('wrong case or start contract')
    observed = []
    case = CASES[name]
    for index, (line, coordinate) in enumerate(zip(markers[1:count+1], coordinates(case))):
        match = SAMPLE.fullmatch(line)
        if not match or int(match[1]) != index:
            raise ValueError('missing, reordered or malformed sample')
        values = tuple(int(match[k], 16) for k in range(2, 6))
        allowed = ((index, 0x7351, 0, 1),) if case.write else expected_read(case, coordinate)
        if values not in allowed:
            raise ValueError('sample does not match numerical oracle')
        observed.extend(values)
    resource = contract['resource']
    if markers[count+1] != ('ROBUST_IMAGE_RESOURCE mode='+resource['mode']+
            f' bytes={resource["bytes"]} digest={resource["digest"]}'):
        raise ValueError('resource readback does not match oracle or role')
    result_digest = digest(observed)
    if markers[count+2] != (f'ROBUST_IMAGE_RESULT outputs={count} mismatches=0 image_changes=0 '
            f'input_changes=0 guards=0 digest={result_digest} fence=complete'):
        raise ValueError('failed result, digest, guards, inputs or fence')
    if markers[-1] != 'ROBUST_IMAGE_RETIRED resources=clean':
        raise ValueError('resources not retired')
    return {'strict_verified': True, 'verification_scope': 'saved_log_and_local_artifact',
            'local_artifact_identity_verified': True, 'deployment_identity_verified': False,
            'firmware_verified': False, 'native_feature_conformance_verified': False,
            'run_id': receipt['run_id'], 'case': name, 'requirement': case.requirement,
            'outputs': count, 'digest': result_digest, 'resource': resource,
            'log_sha256': log_sha, 'eboot_sha256': artifact['eboot_sha256'],
            'native_identity_note': 'Requires separate exact deployed-artifact identity and firmware evidence'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('log', 'receipt', 'artifact', 'eboot', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                        json.loads(args.artifact.read_text()), args.eboot.read_bytes())
    except (ValueError, OSError) as error:
        result = {'strict_verified': False, 'error': str(error),
                  'deployment_identity_verified': False, 'firmware_verified': False,
                  'native_feature_conformance_verified': False}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    return 0 if result["strict_verified"] else 1


if __name__ == '__main__':
    raise SystemExit(main())
