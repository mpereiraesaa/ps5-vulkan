#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned image-robustness fixtures. Numerical expectations are not GPU evidence.

The pinned CTS robustness1 oracle permits zero RGB and alpha zero or one for
out-of-bounds reads. Invalid mip levels are deliberately excluded: that stronger
guarantee belongs to robustness2. These fixtures use valid mip zero and unique
integer coordinates, so storage writes never race another invocation.
Texel-buffer fixtures are separate robustBufferAccess2 controls, not evidence
for robustImageAccess; their missing alpha component must be one.
"""
import argparse
from dataclasses import dataclass, replace
import hashlib
import json
from pathlib import Path
import struct


@dataclass(frozen=True)
class Case:
    name: str
    role: str
    dimension: str
    extent: tuple[int, ...]
    components: int = 1
    write: bool = False

    @property
    def requirement(self):
        return 'robustBufferAccess2' if self.dimension == 'Buffer' else 'robustImageAccess'


def cases():
    result = [Case('storage-2d-r32', 'storage_image', '2D', (7, 5))]
    for dimension, extent in [('1D', (11,)), ('2D', (7, 5)),
                              ('3D', (5, 3, 2)), ('1DArray', (7, 3)),
                              ('2DArray', (7, 5, 3))]:
        result.append(Case('sampled-'+dimension.lower()+'-r32', 'sampled_image', dimension, extent))
    result += [Case('sampled-2d-rgba8', 'sampled_image', '2D', (7, 5), 4),
               Case('uniform-texel-r32', 'uniform_texel', 'Buffer', (43,)),
               Case('storage-texel-r32', 'storage_texel', 'Buffer', (43,))]
    return tuple(result + [replace(c, name=c.name+'-write', write=True)
                           for c in result if c.role in ('storage_image', 'storage_texel')])


def coordinates(case):
    """Both image edges, row padding candidates and extreme signed inputs."""
    center = tuple(n // 2 for n in case.extent)
    found = [tuple(0 for _ in center), tuple(n-1 for n in case.extent), center]
    for axis, bound in enumerate(case.extent):
        for value in (0, bound-1, bound, bound+1, -1, -2, -(1 << 31), (1 << 31)-1):
            coord = list(center)
            coord[axis] = value
            found.append(tuple(coord))
    return tuple(dict.fromkeys(found))


def linear_index(case, coordinate):
    if len(coordinate) != len(case.extent):
        raise ValueError('coordinate dimension mismatch')
    if any(x < 0 or x >= n for x, n in zip(coordinate, case.extent)):
        return None
    index, stride = 0, 1
    for x, n in zip(coordinate, case.extent):
        index += x * stride
        stride *= n
    return index


def texel(case, index):
    if case.components == 1:
        return (((index * 0x10201 + 0x125789) & 0xffffffff), 0, 0, 1)
    return tuple(2 + (index * 17 + channel * 43) % 251 for channel in range(4))


def expected_read(case, coordinate):
    index = linear_index(case, coordinate)
    if index is not None:
        return (texel(case, index),)
    return ((0, 0, 0, 1),) if case.requirement == 'robustBufferAccess2' else ((0, 0, 0, 0), (0, 0, 0, 1))


def store_value(index):
    return 0x54600000 + index * 0x101 + 7


def image_data(case, after_writes=False):
    count = 1
    for n in case.extent:
        count *= n
    data = [texel(case, index)[:case.components] for index in range(count)]
    if after_writes:
        if not case.write or case.components != 1:
            raise ValueError('not a storage-write fixture')
        for invocation, coordinate in enumerate(coordinates(case)):
            index = linear_index(case, coordinate)
            if index is not None:
                data[index] = (store_value(invocation),)
    flat = [v for pixel in data for v in pixel]
    return struct.pack('<'+('I' if case.components == 1 else 'B')*len(flat), *flat)


def coordinate_data(case):
    return b''.join(struct.pack('<4i', *(c+(0,)*(4-len(c)))) for c in coordinates(case))


def shader(case):
    if case.role in ('storage_image', 'storage_texel'):
        declaration = f'layout(set=0,binding=0,r32ui) uniform {"writeonly" if case.write else "readonly"} uimage{case.dimension} target_image;'
        access = 'imageLoad'
    else:
        declaration = f'layout(set=0,binding=0) uniform usampler{case.dimension} target_image;'
        access = 'texelFetch'
    coord = 'input_data.coordinates[index].'+('x', 'xy', 'xyz')[len(case.extent)-1]
    if case.write:
        operation = f'imageStore(target_image,{coord},uvec4(0x54600000u+index*0x101u+7u,0,0,1));\n    output_data.results[index]=uvec4(index,0x7351u,0,1);'
    else:
        lod = ',0' if case.role == 'sampled_image' else ''
        operation = f'output_data.results[index]={access}(target_image,{coord}{lod});'
    return f'''#version 450
layout(local_size_x=32) in;
{declaration}
layout(set=0,binding=1,std430) readonly buffer Coordinates {{ ivec4 coordinates[]; }} input_data;
layout(set=0,binding=2,std430) writeonly buffer Output {{ uvec4 results[]; }} output_data;
void main() {{
    uint index=gl_GlobalInvocationID.x;
    if(index>={len(coordinates(case))}u) return;
    {operation}
}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {'native_executed': False, 'scope': 'integer-coordinate fixtures; valid mip zero', 'cases': []}
    for case in cases():
        directory = args.output / case.name
        directory.mkdir(exist_ok=True)
        (directory/'shader.comp').write_text(shader(case))
        (directory/'coordinates.bin').write_bytes(coordinate_data(case))
        (directory/'image.bin').write_bytes(image_data(case))
        if case.write:
            (directory/'image-after.bin').write_bytes(image_data(case, True))
        files = ['shader.comp', 'coordinates.bin', 'image.bin'] + (['image-after.bin'] if case.write else [])
        manifest['cases'].append({**case.__dict__, 'requirement': case.requirement, 'coordinates': coordinates(case),
            'sha256': {name: hashlib.sha256((directory/name).read_bytes()).hexdigest() for name in files},
            'expected_reads': [expected_read(case, c) for c in coordinates(case)] if not case.write else None})
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')


if __name__ == '__main__':
    main()
