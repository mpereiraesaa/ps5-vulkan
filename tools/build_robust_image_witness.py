#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build one SDK-linked image robustness measurement executable per fixture, offline."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT/'tools')]
from tools.robust_image_witness import cases, coordinates, coordinate_data, image_data, expected_read, shader
from tools.build_integer_dot_witness import build_payload, run
from tools.build_sdk import get_ps5_toolchain
from tools.build_upstream_cts import tessellation_build_profile
from tools.lab import lab_root
from tools.prepare_consumer_sync_shaders import emit_array


def diagnostic_environment(environment, sdk):
    result = dict(environment)
    for key in tessellation_build_profile({})['switches']:
        result.pop(key, None)
    result.update(PS5_PAYLOAD_SDK=str(sdk), PS5VK_USE_SDK='1', PS5VK_IMAGE_ROBUSTNESS_DIAGNOSTIC='1')
    return result


def fixture_header(case, spirv):
    text = '#include <stdint.h>\n' + emit_array('robust_image_spirv', spirv)
    values = {'ri_coordinates': coordinate_data(case), 'ri_image': image_data(case),
              'ri_after': image_data(case, True) if case.write else image_data(case)}
    for name, value in values.items():
        text += f'static const unsigned char {name}[]={{'+','.join(str(b) for b in value)+'};\n'
    expected = [((i, 0x7351, 0, 1),) if case.write else expected_read(case, c)
                for i, c in enumerate(coordinates(case))]
    text += 'static const uint32_t ri_expected[][4]={'+','.join(
        '{'+','.join(str(v)+'u' for v in options[0])+'}' for options in expected)+'};\n'
    text += 'static const unsigned char ri_alpha_either[]={'+','.join(str(int(len(v)>1)) for v in expected)+'};\n'
    descriptor = {'sampled_image':'COMBINED_IMAGE_SAMPLER', 'storage_image':'STORAGE_IMAGE',
                  'uniform_texel':'UNIFORM_TEXEL_BUFFER', 'storage_texel':'STORAGE_TEXEL_BUFFER'}[case.role]
    dimension = case.dimension.replace('Array','') if case.dimension!='Buffer' else '1D'
    image_extent = case.extent[:int(dimension[0])]
    image_extent += (1,)*(3-len(image_extent))
    view = case.dimension.replace('Array','_ARRAY').upper() if case.dimension!='Buffer' else '1D'
    layers = case.extent[-1] if case.dimension.endswith('Array') else 1
    text += f'#define RI_CASE_NAME "{case.name}"\n'
    text += 'static const struct robust_image_data robust_fixture={\n'
    text += f'.type=VK_DESCRIPTOR_TYPE_{descriptor},.image_type=VK_IMAGE_TYPE_{dimension},.view_type=VK_IMAGE_VIEW_TYPE_{view},\n'
    text += '.format=VK_FORMAT_'+('R32_UINT' if case.components==1 else 'R8G8B8A8_UINT')+',\n'
    text += '.extent={'+','.join(map(str,image_extent))+'},'
    text += f'.layers={layers},.count={len(expected)},.image_bytes={len(values["ri_image"])},.write={int(case.write)},\n'
    text += '.coordinates=ri_coordinates,.image=ri_image,.image_after=ri_after,.expected=ri_expected,.alpha_either=ri_alpha_either};\n'
    return text


def main():
    choices = {case.name:case for case in cases()}
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--all',action='store_true')
    selection.add_argument('--case',choices=choices)
    args = parser.parse_args()
    sdk, wrapper = get_ps5_toolchain()
    if not sdk or not wrapper:
        raise SystemExit('native toolchain required')
    glslang = shutil.which('glslangValidator')
    if not glslang:
        local = ROOT/'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
        if local.is_file(): glslang=str(local)
    if not glslang: raise SystemExit('local GLSLang required')
    environment = diagnostic_environment(os.environ,sdk)
    run(sys.executable,str(ROOT/'tools/build_sdk.py'),env=environment)
    for case in choices.values() if args.all else (choices[args.case],):
        directory = ROOT/'build/robust-image-witness'/case.name
        directory.mkdir(parents=True,exist_ok=True)
        source=directory/'shader.comp';binary=directory/'shader.spv'
        source.write_text(shader(case))
        subprocess.run([glslang,'-V','--target-env','vulkan1.1',str(source),'-o',str(binary)],check=True)
        build_payload(directory,ROOT/'examples/robust_image_witness/main.c',
            ROOT/'examples/robust_image_witness/compute.h','robust_image_fixture.h',
            fixture_header(case,binary.read_bytes()),
            {'case':case.name,'requirement':case.requirement,'coordinates':coordinates(case),
             'native_executed':False,'scope':'integer coordinates at valid mip zero; numerical measurement only'},
            sdk,wrapper,lab_root(),environment,'PS5 Vulkan Image Robustness Witness')


if __name__ == '__main__':
    main()
