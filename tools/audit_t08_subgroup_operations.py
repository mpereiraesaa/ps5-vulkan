#!/usr/bin/env python3
"""Offline compiler census for the pinned CTS subgroup arithmetic/ballot families.
Compiler admission is not native execution or a public subgroup property claim.
"""
from pathlib import Path
import argparse
import shutil
import hashlib
import json
import re
import subprocess
import struct

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument('--out', type=Path, required=True, help='directory for host-only generated modules and report')
args = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
OUT = args.out.resolve()
OUT.mkdir(parents=True, exist_ok=True)
CTS = ROOT / 'third_party/vk-gl-cts/external/vulkancts/modules/vulkan/subgroups/vktSubgroupsArithmeticTests.cpp'
local_glslang = ROOT / 'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
GLSLANG = str(local_glslang) if local_glslang.is_file() else shutil.which('glslangValidator')
if not GLSLANG or not (ROOT / 'build/libpsbc.host.a').is_file():
    raise SystemExit('glslangValidator and the pinned host PSBC archive are required')
PROBE = OUT / 't08_compile_probe'

source = CTS.read_text()
match = re.search(r'enum OpType\s*\{(.*?)OPTYPE_LAST', source, re.S)
assert match
names = re.findall(r'OPTYPE_([A-Z_]+)', match.group(1))
assert len(names) == 21 and len(set(names)) == 21
assert {'ADD','MUL','MIN','MAX','AND','OR','XOR'} <= set(names)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-Ithird_party/psbc-reference/libpsbc','tests/t08_compile_probe.c',
                'build/libpsbc.host.a','-lstdc++','-lm','-lpthread','-o',str(PROBE)],
               cwd=ROOT,check=True,capture_output=True)
base = '''#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_ballot : require
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Data { uint values[]; } data;
void main() { uint at=gl_GlobalInvocationID.x; uint v=data.values[at];
%s
}
'''
rows=[]
def run(name, expression, ty='uint'):
    if ty=='float':
        body='float x=uintBitsToFloat(v); float r='+expression+'; data.values[at]=floatBitsToUint(r);'
    elif ty=='vec4':
        body='vec4 x=vec4(uintBitsToFloat(v)); vec4 r='+expression+'; data.values[at]=floatBitsToUint(r.x);'
    elif ty=='int':
        body='int x=int(v); int r='+expression+'; data.values[at]=uint(r);'
    elif ty=='uvec4':
        body='uvec4 x=uvec4(v,v+1u,v+2u,v+3u); uvec4 r='+expression+'; data.values[at]=r.x^r.y^r.z^r.w;'
    elif ty=='ivec4':
        body='ivec4 x=ivec4(v); ivec4 r='+expression+'; data.values[at]=uint(r.x^r.y^r.z^r.w);'
    elif ty=='bool':
        body='bool r='+expression+'; data.values[at]=r?1u:0u;'
    else:
        body='uint r='+expression+'; data.values[at]=r;'
    shader=OUT/(name+'.comp'); binary=OUT/(name+'.spv')
    shader.write_text(base % body)
    glsl=subprocess.run([str(GLSLANG),'-V','--target-env','vulkan1.2',str(shader),'-o',str(binary)],
                         capture_output=True,text=True)
    item={'name':name,'type':ty,'source_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),
          'glslang_exit':glsl.returncode}
    if glsl.returncode:
        item['glslang_error']=glsl.stderr.strip() or glsl.stdout.strip()
    else:
        item['spirv_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
        result=subprocess.run([str(PROBE),'subgroup',str(binary),'none'],capture_output=True,text=True)
        item['psbc_exit']=result.returncode
        item['psbc_output']=result.stdout.strip()
        if result.stderr.strip(): item['psbc_error']=result.stderr.strip()[:500]
    rows.append(item)
for name in names:
    parts=name.split('_')
    if parts[0] in ('INCLUSIVE','EXCLUSIVE'):
        builtin='subgroup'+parts[0].title()+parts[1].title()
        op=parts[1]
    else:
        builtin='subgroup'+parts[0].title()
        op=parts[0]
    run('uint_'+name.lower(),builtin+'(v)')
    for ty in ('int','uvec4','ivec4'):
        run(ty+'_'+name.lower(),builtin+'(x)',ty)
    if op in ('ADD','MUL','MIN','MAX'):
        for ty in ('float','vec4'):
            run(ty+'_'+name.lower(),builtin+'(x)',ty)
ballot={
 'ballot':'uvec4 m=subgroupBallot(v!=0u); uint r=m.x^m.y; data.values[at]=r;',
 'broadcast':'uint r=subgroupBroadcast(v,data.values[64u]&31u); data.values[at]=r;',
 'broadcast_first':'uint r=subgroupBroadcastFirst(v); data.values[at]=r;',
 'inverse_ballot':'uvec4 m=subgroupBallot(v!=0u); bool r=subgroupInverseBallot(m); data.values[at]=r?1u:0u;',
 'ballot_bit_extract':'uvec4 m=subgroupBallot(v!=0u); bool r=subgroupBallotBitExtract(m,gl_SubgroupInvocationID); data.values[at]=r?1u:0u;',
 'ballot_bit_count':'uvec4 m=subgroupBallot(v!=0u); uint r=subgroupBallotBitCount(m); data.values[at]=r;',
 'ballot_inclusive_bit_count':'uvec4 m=subgroupBallot(v!=0u); uint r=subgroupBallotInclusiveBitCount(m); data.values[at]=r;',
 'ballot_exclusive_bit_count':'uvec4 m=subgroupBallot(v!=0u); uint r=subgroupBallotExclusiveBitCount(m); data.values[at]=r;',
 'ballot_find_lsb':'uvec4 m=subgroupBallot(v!=0u); uint r=subgroupBallotFindLSB(m); data.values[at]=r;',
 'ballot_find_msb':'uvec4 m=subgroupBallot(v!=0u); uint r=subgroupBallotFindMSB(m); data.values[at]=r;',
}
for name,body in ballot.items():
    shader=OUT/('ballot_'+name+'.comp');binary=OUT/('ballot_'+name+'.spv')
    shader.write_text(base % body)
    glsl=subprocess.run([str(GLSLANG),'-V','--target-env','vulkan1.2',str(shader),'-o',str(binary)],
                         capture_output=True,text=True)
    item={'name':'ballot_'+name,'type':'uint','source_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),
          'glslang_exit':glsl.returncode}
    if glsl.returncode:item['glslang_error']=glsl.stderr.strip() or glsl.stdout.strip()
    else:
        item['spirv_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
        result=subprocess.run([str(PROBE),'subgroup',str(binary),'none'],capture_output=True,text=True)
        item['psbc_exit']=result.returncode;item['psbc_output']=result.stdout.strip()
        if result.stderr.strip():item['psbc_error']=result.stderr.strip()[:500]
    rows.append(item)
def group_opcodes(payload):
    words=struct.unpack('<'+str(len(payload)//4)+'I',payload)
    assert words[0]==0x07230203 and len(payload)%4==0
    offset=5; found=[]
    while offset<len(words):
        width=words[offset]>>16; opcode=words[offset]&0xffff
        assert width and offset+width<=len(words)
        if 333<=opcode<=366:found.append(opcode)
        offset+=width
    return sorted(set(found))
for row in rows:
    if not row['glslang_exit']:
        row['group_opcodes']=group_opcodes((OUT/(row['name']+'.spv')).read_bytes())
        assert row['group_opcodes'],row['name']
report={'scope':'host compiler only; no shader execution or public capability',
        'cts_source_sha256':hashlib.sha256(CTS.read_bytes()).hexdigest(),
        'probe_source_sha256':hashlib.sha256((ROOT/'tests/t08_compile_probe.c').read_bytes()).hexdigest(),
        'psbc_archive_sha256':hashlib.sha256((ROOT/'build/libpsbc.host.a').read_bytes()).hexdigest(),
        'operation_enum':names,'rows':rows}
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
from collections import Counter
print('variants',len(rows),'glslang',Counter(x['glslang_exit'] for x in rows),
      'psbc',Counter(x.get('psbc_output','').split(' ')[0] for x in rows if not x['glslang_exit']))
print('failures',[(x['name'],x.get('glslang_error',x.get('psbc_output'))) for x in rows if x['glslang_exit'] or x.get('psbc_exit')])
