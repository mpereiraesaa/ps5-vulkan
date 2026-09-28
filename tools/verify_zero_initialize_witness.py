#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify a saved zero-initialize diagnostic receipt without running hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def expected_digest() -> str:
    digest = 2166136261
    for lane in range(128):
        for byte in struct.pack('<I', 246 if lane % 64 == 0 else 123):
            digest = ((digest ^ byte) * 16777619) & 0xffffffff
    return f'{digest:08x}'


def fixture_contract(shader_sha256: str) -> dict:
    return {'profile': 'zero-initialize-public-sdk-witness',
            'contract_version': 1, 'groups': 2, 'local_size': [64, 1, 1],
            'outputs': 128, 'initialized_bytes': 2048,
            'shader_sha256': shader_sha256, 'expected_digest': expected_digest(),
            'diagnostic_switch': 'PS5VK_ZERO_INITIALIZE_WORKGROUP_DIAGNOSTIC'}


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    shader_sha = artifact.get('shader_sha256', '')
    if not re.fullmatch(r'[0-9a-f]{64}', str(shader_sha)):
        raise ValueError('missing shader hash')
    contract = fixture_contract(shader_sha)
    if any(artifact.get(key) != value for key, value in contract.items()):
        raise ValueError('unexpected zero-initialize fixture contract')
    for key in ('eboot_sha256', 'sdk_sha256', 'source_sha256', 'helper_sha256'):
        if not re.fullmatch(r'[0-9a-f]{64}', str(artifact.get(key, ''))):
            raise ValueError('missing artifact hash')
    digest = hashlib.sha256(log).hexdigest()
    if (receipt.get('protocol') != 'ps5log/1' or receipt.get('title') != 'PPSA99994' or
            receipt.get('app') != 'ps5vk' or receipt.get('transport') != 'tcp' or
            receipt.get('clean') is not True or receipt.get('bye') is not True or
            receipt.get('gaps') != 0 or receipt.get('sha256') != digest or
            not receipt.get('run_id')):
        raise ValueError('incomplete or corrupt receipt')
    markers = []
    for line in log.decode('utf-8', errors='strict').splitlines():
        if 'ZERO_INITIALIZE_' in line:
            markers.append(line[line.index('ZERO_INITIALIZE_'):])
    expected = [
        'ZERO_INITIALIZE_START groups=2 local=64 outputs=128 initialized_bytes=2048',
        'ZERO_INITIALIZE_RESULT outputs=128 mismatches=0 guards=0 '
        f'digest={contract["expected_digest"]} fence=complete',
        'ZERO_INITIALIZE_RETIRED resources=clean',
    ]
    if markers != expected:
        raise ValueError('zero-initialize numerical oracle, guard, fence or retirement failed')
    return {'strict_verified': True, 'verification_scope': 'log_contents_only',
            'deployment_identity_verified': False, 'run_id': receipt['run_id'],
            'outputs': 128, 'digest': contract['expected_digest'],
            'log_sha256': digest, 'eboot_sha256': artifact['eboot_sha256']}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                    json.loads(args.artifact.read_text()))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
