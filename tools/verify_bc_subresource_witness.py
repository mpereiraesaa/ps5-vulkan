#!/usr/bin/env python3
"""Strict verification of public SDK BC subresource transfer/sampling evidence."""
import re
from prepare_consumer_bc_subresource import PROFILES, generate
from verify_cube_array_witness import _require, validate_stream

PROFILE = 'bc-subresource-witness'
PREFIX = 'PS5VK_CONSUMER_BC_SUBRESOURCE_'


def validate(log, receipt, artifact):
    _require(artifact.get('title') == 'PPSA99994' and artifact.get('profile') == PROFILE and
             artifact.get('submit_enabled') is True, 'BC subresource artifact profile')
    contract = artifact.get('bc_subresource', {})
    _require(contract.get('profile') in PROFILES, 'BC subresource profile')
    # Recompute every byte of the deterministic oracle. A fabricated contract
    # with self-consistent log hashes must not silently change the expectation.
    expected_contract = generate(contract['profile'])[-1]
    _require(all(contract.get(key) == value for key, value in expected_contract.items()),
             'BC subresource independent contract')
    digest = artifact.get('files', {}).get('eboot.bin', '')
    hashes = [digest] + [contract.get(key, '') for key in ('vert_spirv_sha256', 'frag_spirv_sha256')]
    _require(all(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) for value in hashes),
             'BC subresource artifact hashes')
    messages = validate_stream(log, receipt, 'consumer-bc-subresource-end')
    # validate_stream checks data-record sequence numbers; also pin the final
    # BYE sequence to the receiver's final sequence, rather than only its reason.
    _require(log.decode().splitlines()[-1] ==
             f'BYE seq={len(messages)} reason=consumer-bc-subresource-end', 'BYE sequence')
    _require(not any(m.startswith((PREFIX+'BYTE ', PREFIX+'PIXEL ')) for m in messages),
             'contradictory BC mismatch diagnostics')
    indices = []
    def one(prefix):
        found = [(i,m) for i,m in enumerate(messages) if m.startswith(prefix)]
        _require(len(found) == 1, prefix)
        indices.append(found[0][0])
        return found[0][1]
    _require(one(PREFIX+'FEATURE ') == PREFIX+'FEATURE textureCompressionBC=1 enabled_by_features2=1',
             'BC feature negotiation')
    _require(one(PREFIX+'START ') ==
             f"{PREFIX}START profile={contract['profile']} format={contract['format_value']} image=13x9 mips=4 layers=3 mip={contract['selected_mip']} layer=2 "
             f"input_sha256={contract['input_sha256']} raw_reference_sha256={contract['raw_reference_sha256']} reference_sha256={contract['reference_sha256']}",
             'BC input and reference identity')
    _require(one(PREFIX+'FENCE ') == PREFIX+'FENCE complete=1 timeout_ns=300000000', 'bounded fence completion')
    _require(one(PREFIX+'RAW ') ==
             f"{PREFIX}RAW bytes={contract['readback_bytes']} subresources=12 preserved=11 mismatches=0",
             'all subresource bytes and guards')
    result = re.fullmatch(PREFIX+r'RESULT pixels=4096 mismatches=0 max_error=(\d+) tolerance=1',
                          one(PREFIX+'RESULT '))
    _require(result is not None and int(result[1]) <= 1, 'GPU selected mip/layer pixels')
    for marker in (PREFIX+'RETIRED fence_complete=1 allocations=0',
                   'PS5VK_CONSUMER_TEST_SUCCESS',
                   'PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1',
                   'PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1'):
        _require(one(marker.split(' ')[0]) == marker, 'clean BC retirement')
    _require(indices == sorted(indices), 'BC lifecycle ordering')
    verified = dict(profile=PROFILE, artifact_eboot_sha256=digest, format=contract['format'],
                operation=contract.get('operation', 'buffer-to-image'),
                selected_mip=contract['selected_mip'], selected_layer=2,
                readback_bytes=contract['readback_bytes'], subresources_checked=12,
                preserved_subresources=11, pixels_checked=4096, mismatches=0,
                max_error=int(result[1]), fence_complete=True)
    if contract.get('operation') == 'image-to-image':
        verified.update(source_mip=contract['source_mip'], source_layer=contract['source_layer'])
    return verified
