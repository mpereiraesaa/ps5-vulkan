#!/usr/bin/env python3
"""Verify the BC linear-sampling SDK witness against its exact artifact."""
import re
from prepare_consumer_bc_filter import FORMATS
from verify_cube_array_witness import _require, validate_stream

PROFILE = 'bc-linear-filter-witness'


def validate(log, receipt, artifact):
    _require(artifact.get('title') == 'PPSA99994' and artifact.get('profile') == PROFILE and
             artifact.get('submit_enabled') is True, 'BC filter artifact profile')
    contract = artifact.get('bc_filter', {})
    formats = ['VK_FORMAT_' + name.upper() + '_BLOCK' for name in FORMATS]
    _require(contract.get('format') in formats, 'BC format')
    _require(contract.get('format_value') == 131 + formats.index(contract['format']) and
             contract.get('extent') == [8, 8] and contract.get('target_extent') == [64, 64] and
             contract.get('filter') == 'linear' and contract.get('tolerance') == 2,
             'BC filter shape')
    _require(type(contract.get('distinguishing_pixels')) is int and
             1024 <= contract['distinguishing_pixels'] <= 4096,
             'linear reference must reject nearest')
    digest = artifact.get('files', {}).get('eboot.bin', '')
    hashes = [digest] + [contract.get(key, '') for key in
                        ('input_sha256', 'reference_sha256', 'nearest_sha256',
                         'decoder_sha256', 'reference_generator_sha256', 'vert_spirv_sha256', 'frag_spirv_sha256')]
    _require(all(re.fullmatch('[0-9a-f]{64}', value) for value in hashes), 'artifact hashes')
    _require(contract['reference_sha256'] != contract['nearest_sha256'], 'distinct filter references')
    messages = validate_stream(log, receipt, 'consumer-bc-filter-end')
    def one(prefix):
        found = [m for m in messages if m.startswith(prefix)]
        _require(len(found) == 1, prefix)
        return found[0]
    _require(one('PS5VK_CONSUMER_BC_FILTER_FEATURE ') ==
             'PS5VK_CONSUMER_BC_FILTER_FEATURE textureCompressionBC=1 enabled_by_features2=1',
             'BC feature negotiation')
    _require(one('PS5VK_CONSUMER_BC_FILTER_START ') ==
             f"PS5VK_CONSUMER_BC_FILTER_START format={contract['format_value']} image=8x8 target=64x64 filter=linear "
             f"input_sha256={contract['input_sha256']} reference_sha256={contract['reference_sha256']}",
             'BC input and reference identity')
    result = re.fullmatch(r'PS5VK_CONSUMER_BC_FILTER_RESULT pixels=4096 mismatches=0 max_error=(\d+) tolerance=2',
                          one('PS5VK_CONSUMER_BC_FILTER_RESULT '))
    _require(result is not None and int(result[1]) <= 2, 'GPU filtered pixels')
    for marker in ('PS5VK_CONSUMER_BC_FILTER_RETIRED fence_complete=1 allocations=0',
                   'PS5VK_CONSUMER_TEST_SUCCESS',
                   'PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1',
                   'PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1'):
        _require(one(marker.split(' ')[0]) == marker, 'clean BC retirement')
    return dict(profile=PROFILE, artifact_eboot_sha256=digest, format=contract['format'],
                pixels_checked=4096, mismatches=0, max_error=int(result[1]),
                distinguishing_pixels=contract['distinguishing_pixels'], fence_complete=True)
