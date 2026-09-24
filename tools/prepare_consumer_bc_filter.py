#!/usr/bin/env python3
"""Generate reproducible BC inputs and independent host linear-filter references."""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import subprocess

from prepare_consumer_sync_shaders import emit_array

ROOT = Path(__file__).resolve().parents[1]
FORMATS = ('bc1_rgb_unorm', 'bc1_rgb_srgb', 'bc1_rgba_unorm', 'bc1_rgba_srgb',
           'bc2_unorm', 'bc2_srgb', 'bc3_unorm', 'bc3_srgb', 'bc4_unorm', 'bc4_snorm',
           'bc5_unorm', 'bc5_snorm', 'bc6h_ufloat', 'bc6h_sfloat', 'bc7_unorm', 'bc7_srgb')
WIDTH = HEIGHT = 8
TARGET = 64
TOLERANCE = 2


def s3tc_rgb(block, family):
    """Khronos Data Format S3TC equations, with no intermediate 8-bit rounding.

    https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html#S3TC
    Endpoints are normalized by31/63; palette interpolation precedes sRGB
    conversion. Keep the independent reference in float through filtering.
    """
    color = block if family == 'bc1' else block[8:]
    c0 = int.from_bytes(color[:2], 'little')
    c1 = int.from_bytes(color[2:4], 'little')
    endpoints = []
    for c in (c0, c1):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        endpoints.append((r / 31, g / 63, b / 31))
    a, b = endpoints
    if family != 'bc1' or c0 > c1:
        palette = [a, b, tuple((2*x+y)/3 for x,y in zip(a,b)),
                   tuple((x+2*y)/3 for x,y in zip(a,b))]
    else:
        palette = [a, b, tuple((x+y)/2 for x,y in zip(a,b)), (0,0,0)]
    indices = int.from_bytes(color[4:8], 'little')
    return [palette[(indices >> (2*i)) & 3] for i in range(16)]


def decode(library, fmt, block):
    """Use the third-party CPU decoder directly, with no driver code linked."""
    source = ctypes.create_string_buffer(block)
    family = fmt.split('_')[0]
    count = 1 if family == 'bc4' else 2 if family == 'bc5' else 3 if family == 'bc6h' else 4
    floating = family in ('bc4', 'bc5', 'bc6h')
    output = ((ctypes.c_float if floating else ctypes.c_ubyte) * (16 * count))()
    if family in ('bc4', 'bc5'):
        getattr(library, 'bcdec_' + family + '_float')(source, output, 4 * count, int('snorm' in fmt))
    elif family == 'bc6h':
        library.bcdec_bc6h_float(source, output, 12, int('sfloat' in fmt))
    else:
        getattr(library, 'bcdec_' + family)(source, output, 16)
    rgb = s3tc_rgb(block, family) if family in ('bc1', 'bc2', 'bc3') else None
    pixels = []
    for i in range(16):
        rgba = [float(output[i * count + c]) / (1 if floating else 255) for c in range(count)]
        if rgb is not None:
            rgba[:3] = list(rgb[i])
        rgba += [0.] * (3 - len(rgba))
        if len(rgba) == 3:
            rgba.append(1.)
        if fmt.startswith('bc1_rgb_'):
            rgba[3] = 1.
        if 'srgb' in fmt:
            rgba[:3] = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in rgba[:3]]
        pixels.append(rgba)
    return pixels


def reference(pixels, linear):
    def pixel(x, y):
        return pixels[min(7, max(0, y)) * 8 + min(7, max(0, x))]
    out = bytearray()
    for y in range(TARGET):
        for x in range(TARGET):
            sx, sy = (x + .5) * 8 / TARGET, (y + .5) * 8 / TARGET
            if linear:
                sx -= .5
                sy -= .5
                ix, iy = math.floor(sx), math.floor(sy)
                fx, fy = sx - ix, sy - iy
                samples = (pixel(ix, iy), pixel(ix + 1, iy), pixel(ix, iy + 1), pixel(ix + 1, iy + 1))
                rgba = [(samples[0][c] * (1-fx) + samples[1][c] * fx) * (1-fy) +
                        (samples[2][c] * (1-fx) + samples[3][c] * fx) * fy for c in range(4)]
            else:
                rgba = pixel(math.floor(sx), math.floor(sy))
            out.extend(int(min(1., max(0., v)) * 255 + .5) for v in rgba)
    return bytes(out)


def generate(library, fmt):
    rng = random.Random(0xBC100 + FORMATS.index(fmt))
    size = 8 if fmt.startswith(('bc1_', 'bc4_')) else 16
    for attempt in range(10000):
        blocks, pixels = [], [[0.] * 4 for _ in range(64)]
        for by in range(2):
            for bx in range(2):
                for _ in range(10000):
                    data = bytearray(rng.getrandbits(8) for _ in range(size))
                    if fmt.startswith('bc7_'):
                        data[0] = (data[0] & 0x80) | 0x40  # valid mode 6
                    if fmt.startswith('bc6h_'):
                        data[0] = (data[0] & 0xe0) | 3  # valid mode 11
                    decoded = decode(library, fmt, bytes(data))
                    if all(math.isfinite(v) and abs(v) <= 2 for p in decoded for v in p):
                        break
                else:
                    raise ValueError('could not construct finite BC block')
                blocks.append(bytes(data))
                for y in range(4):
                    for x in range(4):
                        pixels[(by * 4 + y) * 8 + bx * 4 + x] = decoded[y * 4 + x]
        linear, nearest = reference(pixels, True), reference(pixels, False)
        distinct = sum(any(abs(a-b) > TOLERANCE for a, b in zip(linear[i:i+4], nearest[i:i+4]))
                       for i in range(0, len(linear), 4))
        if distinct >= 1024:
            return b''.join(blocks), linear, nearest, distinct
    raise ValueError('reference does not distinguish linear from nearest')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--format', choices=FORMATS, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    decoder = ROOT / 'third_party/bcdec/bcdec.h'
    shared = args.out / 'bcdec_reference.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-DBCDEC_IMPLEMENTATION',
                    '-DBCDEC_BC4BC5_PRECISE', '-x', 'c', str(decoder), '-o', str(shared)], check=True)
    blocks, expected, nearest, distinct = generate(ctypes.CDLL(str(shared.resolve())), args.format)
    sha = lambda data: hashlib.sha256(data).hexdigest()
    contract = dict(format='VK_FORMAT_' + args.format.upper() + '_BLOCK',
                    format_value=131 + FORMATS.index(args.format), extent=[8, 8],
                    target_extent=[64, 64], filter='linear', tolerance=TOLERANCE,
                    input_sha256=sha(blocks), reference_sha256=sha(expected),
                    nearest_sha256=sha(nearest), decoder_sha256=sha(decoder.read_bytes()),
                    reference_generator_sha256=sha(Path(__file__).read_bytes()),
                    distinguishing_pixels=distinct)
    header = '/* Generated public SDK witness inputs and CPU oracle. */\n#include <stdint.h>\n'
    for name, data in [('bc_filter_blocks', blocks), ('bc_filter_expected', expected)]:
        header += 'static const uint8_t ' + name + '[] = {\n'
        header += ''.join('    ' + ','.join(str(v) for v in data[i:i+32]) + ',\n' for i in range(0,len(data),32)) + '};\n'
    header += f'#define BC_FILTER_FORMAT {contract["format"]}\n#define BC_FILTER_TOLERANCE {TOLERANCE}\n'
    header += f'#define BC_FILTER_INPUT_SHA256 "{sha(blocks)}"\n#define BC_FILTER_REFERENCE_SHA256 "{sha(expected)}"\n'
    (args.out / 'bc_filter_data.h').write_text(header)
    local = ROOT / 'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
    compiler = os.environ.get('PS5VK_GLSLANG') or (str(local) if local.is_file() else shutil.which('glslangValidator'))
    if not compiler:
        raise SystemExit('glslangValidator is required')
    header = '#include <stdint.h>\n'
    for stage, source in [('vert', 'consumer_cube_array.vert'), ('frag', 'consumer_bc_filter.frag')]:
        spirv = args.out / ('bc_filter.' + stage + '.spv')
        subprocess.run([compiler, '-V', '--target-env', 'vulkan1.0', str(ROOT / 'experiments/graphics' / source), '-o', str(spirv)], check=True)
        data = spirv.read_bytes()
        header += emit_array('bc_filter_' + stage + '_spirv', data)
        contract[stage + '_spirv_sha256'] = sha(data)
    (args.out / 'bc_filter_shaders.h').write_text(header)
    (args.out / 'bc_filter_contract.json').write_text(json.dumps(contract, indent=2) + '\n')


if __name__ == '__main__':
    main()
