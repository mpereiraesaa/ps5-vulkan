#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the default-off SDK-linked subgroup arithmetic compute witness offline."""
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
from tools.verify_t08_subgroup_arithmetic_witness import fixture_contract


def arithmetic_instructions(payload: bytes) -> bool:
    if len(payload) % 4:
        return False
    words = struct.unpack('<' + str(len(payload) // 4) + 'I', payload)
    if len(words) < 5 or words[0] != 0x07230203:
        return False
    index = 5
    counts = {(op, mode): 0 for op in (349, 351, 354, 357, 359, 360, 361)
              for mode in (0, 1, 2)}
    capabilities = set()
    compute = False
    while index < len(words):
        width, opcode = words[index] >> 16, words[index] & 0xffff
        if not width or index + width > len(words):
            return False
        if opcode == 17 and width >= 2:
            capabilities.add(words[index + 1])
        if opcode == 15 and width >= 3 and words[index + 1] == 5:
            compute = True
        if opcode in range(349, 362):
            if width < 6:
                return False
            key = (opcode, words[index + 4])
            if key not in counts:
                return False
            counts[key] += 1
        index += width
    return index == len(words) and {61, 63} <= capabilities and compute and all(
        value == 1 for value in counts.values())


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
               PS5VK_SUBGROUP_IADD_DIAGNOSTIC='1')
    run(sys.executable, str(ROOT / 'tools/build_sdk.py'), env=env)
    build = ROOT / 'build/t08-subgroup-arithmetic-witness'
    build.mkdir(parents=True, exist_ok=True)
    shader = build / 'arithmetic.spv'
    run(glslang, '-V', '--target-env', 'vulkan1.2', '-S', 'comp',
        str(ROOT / 'experiments/compute/t08_subgroup_arithmetic_runtime.comp'),
        '-o', str(shader))
    payload = shader.read_bytes()
    if not arithmetic_instructions(payload):
        raise RuntimeError('shader lacks the exact 21 arithmetic operations')
    header = '#include <stdint.h>\n' + emit_array('subgroup_arithmetic_spirv', payload)
    contract = fixture_contract(hashlib.sha256(payload).hexdigest())
    build_payload(build, ROOT / 'examples/t08_subgroup_arithmetic_witness/main.c',
        ROOT / 'examples/dxvk_render_witness/subgroup_arithmetic_compute.h',
        't08_subgroup_arithmetic_fixture.h', header, contract, sdk, clang_wrapper,
        lab, env, 'PS5 Vulkan T08 Arithmetic Witness',
        content_id='UP9000-PPSA99994_00-PS5VKSGAR0000001')


if __name__ == '__main__':
    main()
