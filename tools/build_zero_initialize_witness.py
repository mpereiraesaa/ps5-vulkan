#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the default-off SDK-linked zero-initialize workgroup witness offline."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'tools')]
from tools.build_integer_dot_witness import build_payload, run
from tools.build_sdk import get_ps5_toolchain
from tools.build_upstream_cts import tessellation_build_profile
from tools.lab import lab_root
from tools.prepare_consumer_sync_shaders import emit_array
from tools.verify_zero_initialize_witness import fixture_contract


def workgroup_null_variable(payload: bytes) -> bool:
    words = struct.unpack('<' + str(len(payload) // 4) + 'I', payload)
    if len(payload) % 4 or words[0] != 0x07230203:
        return False
    index = 5
    while index < len(words):
        width, opcode = words[index] >> 16, words[index] & 0xffff
        if not width or index + width > len(words):
            return False
        if opcode == 59 and width == 5 and words[index + 3] == 4:
            return True
        index += width
    return False


def main() -> None:
    sdk, clang_wrapper = get_ps5_toolchain()
    lab = lab_root()
    glslang = shutil.which('glslangValidator')
    if not sdk or not clang_wrapper or not glslang:
        raise SystemExit('PS5 toolchain and glslangValidator are required')
    env = dict(os.environ)
    for name in tessellation_build_profile({})['switches']:
        env.pop(name, None)
    env.update(PS5_PAYLOAD_SDK=str(sdk), PS5VK_USE_SDK='1',
               PS5VK_ZERO_INITIALIZE_WORKGROUP_DIAGNOSTIC='1')
    run(sys.executable, str(ROOT / 'tools/build_sdk.py'), env=env)
    build = ROOT / 'build/zero-initialize-witness'
    build.mkdir(parents=True, exist_ok=True)
    shader = build / 'zero_initialize.spv'
    run(glslang, '-V', '--target-env', 'vulkan1.0', '-S', 'comp',
        '-DZERO_INITIALIZE=1',
        str(ROOT / 'experiments/compute/zero_initialize_workgroup.comp'),
        '-o', str(shader))
    payload = shader.read_bytes()
    if not workgroup_null_variable(payload):
        raise RuntimeError('shader omitted the initialized Workgroup OpVariable')
    header = '#include <stdint.h>\n' + emit_array('zero_initialize_spirv', payload)
    contract = fixture_contract(hashlib.sha256(payload).hexdigest())
    build_payload(build, ROOT / 'examples/zero_initialize_witness/main.c',
        ROOT / 'examples/dxvk_render_witness/zero_initialize_compute.h',
        'zero_initialize_fixture.h', header, contract, sdk, clang_wrapper,
        lab, env, 'PS5 Vulkan Zero Initialize Witness',
        content_id='UP9000-PPSA99994_00-PS5VKZERO0000001')


if __name__ == '__main__':
    main()
