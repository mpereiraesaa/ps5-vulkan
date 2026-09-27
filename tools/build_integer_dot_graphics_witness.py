#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build separate SDK-linked graphics integer-dot variants offline."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT/'tools')]
from tools.build_integer_dot_witness import build_payload, diagnostic_environment, run
from tools.build_sdk import get_ps5_toolchain
from tools.lab import lab_root
from tools.integer_dot_graphics_fixture import compile_templates, shader_modules, fixture_header
from tools.integer_dot_graphics_witness import GRAPHS
from tools.verify_integer_dot_graphics_witness import CASES, fixture_contract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument('--case', choices=CASES)
    choice.add_argument('--all', action='store_true')
    parser.add_argument('--graph', choices=GRAPHS)
    args = parser.parse_args()
    if args.case and not args.graph:
        parser.error('--case requires --graph')
    sdk, wrapper = get_ps5_toolchain()
    lab = lab_root()
    glslang = shutil.which('glslangValidator')
    fallback = ROOT/'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
    if not glslang and fallback.is_file(): glslang = str(fallback)
    if not sdk or not wrapper or not glslang:
        raise SystemExit('PS5 native toolchain and glslang are required')
    base = ROOT/'build/integer-dot-graphics-witness'
    templates = compile_templates(base/'templates', glslang)
    env = diagnostic_environment(os.environ, sdk)
    run(sys.executable, str(ROOT/'tools/build_sdk.py'), env=env)
    for case in (CASES.values() if args.all else (CASES[args.case],)):
        for graph in ((args.graph,) if args.graph else GRAPHS):
            modules = shader_modules(case, graph, templates)
            contract = fixture_contract(case.name, graph)
            contract['shader_sha256'] = {stage: hashlib.sha256(data).hexdigest()
                                         for stage, data in modules.items()}
            directory = base/case.name/graph
            directory.mkdir(parents=True, exist_ok=True)
            for stage, data in modules.items():
                (directory/(stage+'.spv')).write_bytes(data)
            build_payload(directory, ROOT/'examples/integer_dot_graphics_witness/main.c',
                ROOT/'examples/dxvk_render_witness/integer_dot_graphics.h',
                'integer_dot_graphics_fixture.h', fixture_header(case, graph, modules), contract,
                sdk, wrapper, lab, env, 'PS5 Vulkan Integer Dot Graphics Witness')


if __name__ == '__main__':
    main()
