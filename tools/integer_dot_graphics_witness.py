# SPDX-License-Identifier: GPL-3.0-or-later
"""Bit-exact integer-dot graphics fixtures for one instanced pixel-grid draw.

Each of 128 instances draws a six-vertex quad into one pixel of a 16x8 image.
The index and result use flat uint interfaces. Final byte extraction occurs
before conversion to normalized RGBA; readback expects physical BGRA8 bytes.
These are owned test fixtures, not runtime shader rewriting.
"""
from tools.integer_dot_vectors import buffers

STAGES = ('vert', 'tesc', 'tese', 'geom', 'frag')
GRAPHS = {
    'vert': ('vert', 'frag'), 'frag': ('vert', 'frag'),
    'geom': ('vert', 'geom', 'frag'),
    'tesc': ('vert', 'tesc', 'tese', 'frag'),
    'tese': ('vert', 'tesc', 'tese', 'frag'), 'all': STAGES,
}
WIDTH, HEIGHT, RECORD_STRIDE, RECORD_COUNT = 16, 8, 256, 128


def image_fixture(case):
    """Return readonly SSBO records and a tightly packed BGRA8 expected image.

    Each record begins with lhs/rhs vec4 slots and accumulator.x. Unused lanes
    and alignment padding are poisoned. Descriptor range covers all records;
    the shader uses 16 vec4 slots per instance, matching 256-byte record stride.
    """
    lhs, rhs, acc, expected = buffers(case)
    records = bytearray(b'\xa5' * (RECORD_COUNT * RECORD_STRIDE))
    lane_bytes = 4 if case.packed_types is not None else case.components * 4
    for i in range(RECORD_COUNT):
        for lane, data in enumerate((lhs, rhs)):
            offset = i * RECORD_STRIDE + 16 * lane
            records[offset:offset+lane_bytes] = data[i*case.stride:i*case.stride+lane_bytes]
        records[i*RECORD_STRIDE+32:i*RECORD_STRIDE+36] = acc[4*i:4*i+4]
    image = bytearray()
    for i in range(RECORD_COUNT):
        rgba = expected[i*4:i*4+4]
        image.extend((rgba[2], rgba[1], rgba[0], rgba[3]))
    return bytes(records), bytes(image)


def template(stage, n=0):
 """Owned float-dot template; transform before using as an integer witness."""
 if stage not in STAGES or n not in (0,2,3,4):
  raise ValueError("unsupported graphics stage or vector width")
 head='#version 450\n'
 if n:
  head+=f"""layout(set=0,binding=0,std430) readonly buffer DotInput {{ vec4 operands[]; }};
uint calculate(uint sample_id) {{ uint base=sample_id*16; return floatBitsToUint(dot(operands[base].{'xyzw'[:n]},operands[base+1].{'xyzw'[:n]}) + operands[base+2].x); }}
"""
 expr='calculate(idx)' if n else ('0xdeadbeefu' if stage=='vert' else 'vin[0]' if stage in ('tesc','tese','geom') else 'vin')
 if stage=='vert':
  return head+f"""layout(location=0) flat out uint vout;layout(location=1) flat out uint sample_out;
void main() {{ uint idx=uint(gl_InstanceIndex);vec2 p[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
vec2 origin=vec2(idx%16,idx/16);gl_Position=vec4((origin+p[gl_VertexIndex%6])/vec2(16,8)*2-1,0,1);vout={expr};sample_out=idx; }}"""
 if stage=='tesc':
  return head+f"""layout(vertices=3) out;
layout(location=0) flat in uint vin[]; layout(location=0) flat out uint vout[];
layout(location=1) flat in uint sample_in[];layout(location=1) flat out uint sample_out[];
void main() {{uint idx=sample_in[0]; gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;
vout[gl_InvocationID]={expr};sample_out[gl_InvocationID]=idx;
if(gl_InvocationID==0) {{gl_TessLevelOuter[0]=1;gl_TessLevelOuter[1]=1;gl_TessLevelOuter[2]=1;gl_TessLevelInner[0]=1;}} }}"""
 if stage=='tese':
  return head+f"""layout(triangles,equal_spacing,ccw) in;
layout(location=0) flat in uint vin[];layout(location=0) flat out uint vout;
layout(location=1) flat in uint sample_in[];layout(location=1) flat out uint sample_out;
void main() {{uint idx=sample_in[0];gl_Position=gl_in[0].gl_Position*gl_TessCoord.x+gl_in[1].gl_Position*gl_TessCoord.y+gl_in[2].gl_Position*gl_TessCoord.z;vout={expr};sample_out=idx;}}"""
 if stage=='geom':
  return head+f"""layout(triangles) in;layout(triangle_strip,max_vertices=3) out;
layout(location=0) flat in uint vin[];layout(location=0) flat out uint vout;
layout(location=1) flat in uint sample_in[];layout(location=1) flat out uint sample_out;
void main() {{uint idx=sample_in[0];for(int i=0;i<3;i++) {{gl_Position=gl_in[i].gl_Position;vout={expr};sample_out=idx;EmitVertex();}} EndPrimitive();}}"""
 return head+f"""layout(location=0) flat in uint vin; layout(location=0) out vec4 color;
layout(location=1) flat in uint sample_in;
void main() {{uint idx=sample_in;uint word={expr};color=vec4(uvec4(word,word>>8,word>>16,word>>24)&uvec4(255))/255.0;}}"""
