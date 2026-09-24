#!/usr/bin/env python3
"""Generate independent BC mip/layer transfer and nearest-sampling oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from prepare_consumer_bc_filter import s3tc_rgb
from prepare_consumer_sync_shaders import emit_array

ROOT = Path(__file__).resolve().parents[1]
PROFILES = {'bc1-mip': ('BC1_RGB_UNORM', 131, 1, 8),
            'bc1-layer': ('BC1_RGB_UNORM', 131, 0, 8),
            'bc3-tail': ('BC3_UNORM', 137, 3, 16),
            'bc1-imagecopy': ('BC1_RGB_UNORM', 131, 1, 8),
            'bc1-partial-layers': ('BC1_RGB_UNORM', 131, 1, 8)}
MIPS, LAYERS, TARGET, GUARD = 4, 3, 64, 32


def sha(data):
    return hashlib.sha256(data).hexdigest()


def block(seed, block_bytes):
    # Endpoint zero is selected for every texel; each seed names a different
    # normalized RGB565 endpoint. BC3 uses alpha endpoint zero as well.
    endpoint = (((seed * 7 + 3) % 31 + 1) << 11) | (((seed * 11 + 5) % 63 + 1) << 5) | ((seed * 13 + 9) % 31 + 1)
    color = endpoint.to_bytes(2, 'little') + bytes(6)
    return (bytes((40 + seed % 200, 0)) + bytes(6) if block_bytes == 16 else b'') + color


def generate(profile):
    fmt, value, selected_mip, block_bytes = PROFILES[profile]
    partial = profile == 'bc1-partial-layers'
    selected_layer = 1 if profile == 'bc1-layer' or partial else 2
    extent = [37,29] if partial else [13,9]
    upload = bytearray([0xcd] * GUARD)
    expected = bytearray([0xa5] * GUARD)
    regions = []
    selected_blocks = []
    for layer in range(LAYERS):
        for mip in range(MIPS):
            width, height = max(1, extent[0] >> mip), max(1, extent[1] >> mip)
            columns, rows = (width + 3) // 4, (height + 3) // 4
            pitch = (columns + 1) * block_bytes
            offset = len(upload)
            size = rows * pitch + GUARD
            upload.extend([0xcd] * size)
            expected.extend([0xa5] * size)
            region = dict(offset=offset, row_length=(columns+1)*4,
                          image_height=(rows+1)*4, mip=mip, layer=layer,
                          width=width, height=height, pitch=pitch, rows=rows)
            regions.append(region)
            for y in range(rows):
                for x in range(columns):
                    at = offset + y * pitch + x * block_bytes
                    data = block(layer*53 + mip*17 + y*columns+x, block_bytes)
                    upload[at:at+block_bytes] = data
                    expected[at:at+block_bytes] = data
    selected = regions[selected_layer*MIPS + selected_mip]
    image_copy = profile == 'bc1-imagecopy'
    patch_offset = regions[selected_mip]['offset'] if image_copy else len(upload)
    columns = (selected['width']+3)//4
    if partial:
        # One region covers two layers. Padding separates rows AND layers;
        # upload and readback deliberately use different strides.
        upload.extend([0xcd] * (2*48 + GUARD))
        partial_read_offset = len(expected)
        expected.extend([0xa5] * (2*96 + GUARD))
        for layer_index in range(2):
            destination = regions[(selected_layer+layer_index)*MIPS+selected_mip]
            for x in range(2):
                data = block(701+layer_index*113+x, block_bytes)
                src = patch_offset+layer_index*48+x*block_bytes
                dst = destination['offset']+destination['pitch']+(1+x)*block_bytes
                read = partial_read_offset+layer_index*96+x*block_bytes
                upload[src:src+block_bytes] = data
                expected[dst:dst+block_bytes] = data
                expected[read:read+block_bytes] = data
    else:
        if not image_copy:
            patch_size = selected['rows'] * selected['pitch']
            upload.extend([0xcd] * (patch_size + GUARD))
        for y in range(selected['rows']):
            for x in range(columns):
                at = y*selected['pitch'] + x*block_bytes
                if image_copy:
                    data = bytes(upload[patch_offset+at:patch_offset+at+block_bytes])
                else:
                    data = block(701+y*columns+x, block_bytes)
                    upload[patch_offset+at:patch_offset+at+block_bytes] = data
                expected[selected['offset']+at:selected['offset']+at+block_bytes] = data
    for y in range(selected['rows']):
        for x in range(columns):
            at = selected['offset']+y*selected['pitch']+x*block_bytes
            selected_blocks.append(expected[at:at+block_bytes])
    rgba = bytearray()
    for y in range(TARGET):
        for x in range(TARGET):
            sx, sy = int((x+.5)*selected['width']/TARGET), int((y+.5)*selected['height']/TARGET)
            data = selected_blocks[(sy//4)*columns+sx//4]
            rgb = s3tc_rgb(data, 'bc1' if block_bytes == 8 else 'bc3')[(sy%4)*4+sx%4]
            rgba.extend(int(v*255+.5) for v in rgb)
            rgba.append(data[0] if block_bytes == 16 else 255)
    contract = dict(profile=profile, format='VK_FORMAT_'+fmt+'_BLOCK', format_value=value,
                    extent=extent, mip_levels=MIPS, array_layers=LAYERS,
                    selected_mip=selected_mip, selected_layer=selected_layer,
                    selected_extent=[selected['width'],selected['height']],
                    subresources=12, preserved_subresources=10 if partial else 11, target_extent=[64,64],
                    filter='nearest', tolerance=1, readback_bytes=len(expected),
                    input_sha256=sha(upload), raw_reference_sha256=sha(expected),
                    reference_sha256=sha(rgba), reference_generator_sha256=sha(Path(__file__).read_bytes()),
                    rgb_decoder_sha256=sha((ROOT/'tools/prepare_consumer_bc_filter.py').read_bytes()))
    if partial:
        contract.update(operation='buffer-to-image-interior-multilayer',
                        copy_offset=[4,4,0], copy_extent=[8,4,1], copy_layers=2,
                        upload_row_length=12, upload_image_height=8,
                        readback_row_length=16, readback_image_height=12,
                        partial_read_offset=partial_read_offset)
    if image_copy:
        contract.update(operation='image-to-image', source_mip=selected_mip, source_layer=0)
    return bytes(upload), bytes(expected), bytes(rgba), regions, patch_offset, contract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=PROFILES, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    upload, raw, pixels, regions, patch, contract = generate(args.profile)
    header = '/* Generated public Vulkan inputs and independent CPU references. */\n#include <stdint.h>\n'
    for name, data in [('blocks',upload),('raw_expected',raw),('expected',pixels)]:
        header += 'static const uint8_t bc_subresource_'+name+'[] = {\n'
        header += ''.join('    '+','.join(str(v) for v in data[i:i+32])+',\n' for i in range(0,len(data),32))+'};\n'
    header += 'static const VkBufferImageCopy bc_subresource_regions[] = {\n'
    for r in regions:
        header += f'    {{{r["offset"]}, {r["row_length"]}, {r["image_height"]}, {{VK_IMAGE_ASPECT_COLOR_BIT, {r["mip"]}, {r["layer"]}, 1}}, {{0,0,0}}, {{{r["width"]},{r["height"]},1}}}},\n'
    header += '};\n'
    constants = dict(FORMAT=contract['format'], MIP=contract['selected_mip'], LAYER=contract['selected_layer'],
                     PATCH_OFFSET=patch, TOLERANCE=1,
                     WIDTH=contract['extent'][0], HEIGHT=contract['extent'][1],
                     PRESERVED=contract['preserved_subresources'],
                     COPY_LAYERS=contract.get('copy_layers',1),
                     PARTIAL_LAYERS=int(args.profile == 'bc1-partial-layers'),
                     PARTIAL_READ_OFFSET=contract.get('partial_read_offset',0),
                     IMAGE_COPY=int(args.profile == 'bc1-imagecopy'))
    for name, value in constants.items():
        header += f'#define BC_SUBRESOURCE_{name} {value}\n'
    for name in ('profile','input_sha256','raw_reference_sha256','reference_sha256'):
        header += f'#define BC_SUBRESOURCE_{name.upper()} "{contract[name]}"\n'
    (args.out/'bc_subresource_data.h').write_text(header)
    local = ROOT/'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
    compiler = os.environ.get('PS5VK_GLSLANG') or (str(local) if local.is_file() else shutil.which('glslangValidator'))
    if not compiler:
        raise SystemExit('glslangValidator is required')
    header = '#include <stdint.h>\n'
    for stage, source in [('vert','consumer_cube_array.vert'),('frag','consumer_bc_filter.frag')]:
        spirv = args.out/('bc_subresource.'+stage+'.spv')
        subprocess.run([compiler,'-V','--target-env','vulkan1.0',str(ROOT/'experiments/graphics'/source),'-o',str(spirv)],check=True)
        data = spirv.read_bytes()
        header += emit_array('bc_subresource_'+stage+'_spirv',data)
        contract[stage+'_spirv_sha256'] = sha(data)
    (args.out/'bc_subresource_shaders.h').write_text(header)
    (args.out/'bc_subresource_contract.json').write_text(json.dumps(contract,indent=2)+'\n')


if __name__ == '__main__':
    main()
