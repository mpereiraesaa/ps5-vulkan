#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify a saved T08 arithmetic diagnostic receipt without hardware access."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def expected(lane: int, operation: int) -> int:
    kind, mode = operation % 7, operation // 7
    end = 32 if mode == 0 else lane + 1 if mode == 1 else lane
    result = 1 if kind == 1 else 0xffffffff if kind in (2, 4) else 0
    for i in range(end):
        value = (i & 1) + 1
        if kind == 0:
            result += value
        elif kind == 1:
            result *= value
        elif kind == 2:
            result = min(result, value)
        elif kind == 3:
            result = max(result, value)
        elif kind == 4:
            result &= value
        elif kind == 5:
            result |= value
        else:
            result ^= value
        result &= 0xffffffff
    return result


def expected_digest() -> str:
    digest = 2166136261
    for lane in range(128):
        for operation in range(21):
            digest = ((digest ^ expected(lane % 32, operation)) * 16777619) & 0xffffffff
    return f'{digest:08x}'


def fixture_contract(shader_sha256: str) -> dict:
    return {'profile': 't08-subgroup-arithmetic-private-sdk-witness',
            'contract_version': 1, 'groups': 2, 'local_size': [64, 1, 1],
            'operations': 21, 'outputs': 2688, 'public_reporting': False,
            'shader_sha256': shader_sha256, 'expected_digest': expected_digest(),
            'diagnostic_switch': 'PS5VK_SUBGROUP_IADD_DIAGNOSTIC'}


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    shader_sha = artifact.get('shader_sha256', '')
    if not re.fullmatch(r'[0-9a-f]{64}', str(shader_sha)):
        raise ValueError('missing shader hash')
    contract = fixture_contract(shader_sha)
    if any(artifact.get(key) != value for key, value in contract.items()):
        raise ValueError('unexpected T08 arithmetic fixture contract')
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
        if 'T08_ARITHMETIC_' in line:
            markers.append(line[line.index('T08_ARITHMETIC_'):])
    expected_markers = [
        'T08_ARITHMETIC_START groups=2 local=64 operations=21 outputs=2688 public=off',
        'T08_ARITHMETIC_RESULT outputs=2688 mismatches=0 guards=0 '
        f'digest={contract["expected_digest"]} fence=complete',
        'T08_ARITHMETIC_RETIRED resources=clean',
    ]
    if markers != expected_markers:
        raise ValueError('T08 arithmetic numerical oracle, guard, fence or retirement failed')
    return {'strict_verified': True, 'verification_scope': 'log_contents_only',
            'deployment_identity_verified': False, 'run_id': receipt['run_id'],
            'outputs': 2688, 'digest': contract['expected_digest'],
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
