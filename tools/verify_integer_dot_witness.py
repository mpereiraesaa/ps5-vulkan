#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify saved integer-dot witness logs offline; never deploy or execute."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.integer_dot_spirv import cases, binary
from tools.integer_dot_vectors import buffers

CASES = {case.name: case for case in cases()}
PROFILE = "integer-dot-public-sdk-witness"


def fixture_contract(name):
    case = CASES[name]
    values = buffers(case)
    digest = 2166136261
    for word in struct.unpack('<128I', values[3]):
        digest = ((digest ^ word) * 16777619) & 0xffffffff
    return {
        "profile": PROFILE, "contract_version": 1, "case": name,
        "groups": 2, "local_size": [64, 1, 1], "outputs": 128,
        "input_stride": case.stride, "expected_digest": f"{digest:08x}",
        "fixture_sha256": {key: hashlib.sha256(value).hexdigest() for key, value in
            zip(('lhs', 'rhs', 'accumulator', 'expected', 'shader'), (*values, binary(case)))},
    }


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    case = artifact.get('case')
    if not isinstance(case, str) or case not in CASES:
        raise ValueError('unknown integer-dot case')
    contract = fixture_contract(case)
    if any(artifact.get(key) != value for key, value in contract.items()):
        raise ValueError('unexpected integer-dot fixture contract')
    for key in ('eboot_sha256', 'sdk_sha256', 'source_sha256', 'helper_sha256'):
        if not re.fullmatch(r'[0-9a-f]{64}', str(artifact.get(key, ''))):
            raise ValueError('missing artifact hash')
    digest = hashlib.sha256(log).hexdigest()
    if (receipt.get('protocol') != 'ps5log/1' or receipt.get('title') != 'PPSA99994' or
            receipt.get('app') != 'ps5vk' or receipt.get('transport') != 'tcp' or
            receipt.get('clean') is not True or receipt.get('bye') is not True or
            receipt.get('gaps') != 0 or receipt.get('sha256') != digest or not receipt.get('run_id')):
        raise ValueError('incomplete or corrupt receipt')
    markers = []
    for line in log.decode('utf-8', errors='strict').splitlines():
        if 'INTEGER_DOT_' in line:
            markers.append(line[line.index('INTEGER_DOT_'):])
    expected = [
        f'INTEGER_DOT_START case={case} groups=2 local=64 outputs=128',
        'INTEGER_DOT_RESULT outputs=128 mismatches=0 guards=0 input_changes=0 '
        f'digest={contract["expected_digest"]} fence=complete',
        'INTEGER_DOT_RETIRED resources=clean',
    ]
    if markers != expected:
        raise ValueError('integer-dot results, inputs, guards, fence or retirement failed')
    return {'strict_verified': True, 'verification_scope': 'log_contents_only',
            'deployment_identity_verified': False, 'run_id': receipt['run_id'], 'case': case,
            'outputs': 128, 'digest': contract['expected_digest'], 'log_sha256': digest,
            'eboot_sha256': artifact['eboot_sha256'],
            'native_identity_note': 'Requires separate exact deployed-artifact identity evidence'}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                    json.loads(args.artifact.read_text()))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
