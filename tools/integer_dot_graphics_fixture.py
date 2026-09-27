# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned graphics fixture compilation and SDK-only header generation."""
from pathlib import Path
import struct
import subprocess

from tools.integer_dot_spirv import integerize_dot_add
from tools.integer_dot_graphics_witness import STAGES, GRAPHS, template, image_fixture
from tools.prepare_consumer_sync_shaders import emit_array


def compile_templates(directory, glslang):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    result = {}
    for stage in STAGES:
        for components in (0, 2, 3, 4):
            source = directory / f'base{components}.{stage}'
            source.write_text(template(stage, components))
            binary = Path(str(source)+'.spv')
            subprocess.run([str(glslang), '-V', str(source), '-o', str(binary)], check=True,
                           capture_output=True, text=True)
            result[stage, components] = binary.read_bytes()
    return result


def shader_modules(case, graph, templates):
    result = {}
    for stage in GRAPHS[graph]:
        active = graph == 'all' or stage == graph
        blob = templates[stage, case.components if active else 0]
        if active:
            words = struct.unpack(f'<{len(blob)//4}I', blob)
            words = integerize_dot_add(words, case, preserve_result_bits=True)
            blob = struct.pack(f'<{len(words)}I', *words)
        result[stage] = blob
    return result


def fixture_header(case, graph, modules):
    if set(modules) != set(GRAPHS[graph]):
        raise ValueError('unexpected graphics stage set')
    text = '#include <stdint.h>\n'
    for stage in STAGES:
        if stage in modules:
            text += emit_array('dot_'+stage, modules[stage])
    for name, data in zip(('records', 'expected_rgba'), image_fixture(case, bgra=False)):
        text += f'static const unsigned char dot_{name}[] = {{\n'
        text += ',\n'.join('    '+', '.join(f'0x{b:02x}' for b in data[i:i+16])
                           for i in range(0, len(data), 16))+'\n};\n'
    text += f'#define DOT_CASE_NAME "{case.name}"\n#define DOT_GRAPH_NAME "{graph}"\n'
    text += f'#define DOT_NEEDS_TESS {int("tesc" in modules)}\n'
    text += f'#define DOT_NEEDS_GEOM {int("geom" in modules)}\n'
    text += 'static const struct integer_dot_graphics_case dot_fixture = {\n.words={'
    text += ','.join('dot_'+s if s in modules else 'NULL' for s in STAGES)+'},\n.bytes={'
    text += ','.join('sizeof(dot_'+s+')' if s in modules else '0' for s in STAGES)
    return text+'},\n.records=dot_records,.expected_rgba=dot_expected_rgba};\n'
