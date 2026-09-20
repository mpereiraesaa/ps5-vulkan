"""Real compiler test: packed distance identity must survive input compaction."""
import pathlib
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class DistanceInputTests(unittest.TestCase):
    def test_sparse_and_dense_distance_inputs(self):
        archive = ROOT / 'build/libpsbc.host.a'
        glslang = shutil.which('glslangValidator')
        if not archive.exists() or not glslang:
            self.skipTest('requires actual host PSBC archive and glslangValidator')
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp)
            def run(args, env=None):
                result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=120, env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-Ithird_party/psbc-reference/libpsbc', 'tests/test_distance_input_semantics.c',
                 str(archive), '-lstdc++', '-lm', '-lpthread', '-o', str(path/'probe')])
            cases = [
                ('in float gl_ClipDistance[8];', 'gl_ClipDistance[4]', ['0x31']),
                ('in float gl_ClipDistance[8];', 'gl_ClipDistance[0]', ['0x30']),
                ('in float gl_ClipDistance[8];', 'gl_ClipDistance[0]+gl_ClipDistance[4]', ['0x30','0x31']),
                ('in float gl_CullDistance[4];', 'gl_CullDistance[2]', ['0x30']),
                ('in float gl_CullDistance[8];', 'gl_CullDistance[4]', ['0x31']),
                ('in float gl_ClipDistance[4]; in float gl_CullDistance[4];',
                 'gl_ClipDistance[2]+gl_CullDistance[2]', ['0x30','0x31']),
            ]
            for declarations, expression, expected in cases:
                with self.subTest(expression=expression):
                    shader = path/'test.frag'
                    shader.write_text('#version 450\n'+declarations+'\n'
                        'layout(location=0) in flat vec4 color;\n'
                        'layout(location=0) out vec4 result;\n'
                        'void main(){result=vec4(color.r,'+expression+',0,1);}\n')
                    run([glslang,'-V',str(shader),'-o',str(path/'test.spv')])
                    run([str(path/'probe'),str(path/'test.spv'),*expected,'0x40000f'])
            # Own-source TES exports a generic varying plus both distance
            # registers. The implicit PrimitiveID occupies parameter 1.
            (path/'test.tesc').write_text('''#version 450
layout(vertices=3) out;
void main(){
 gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;
 gl_TessLevelOuter[0]=1;gl_TessLevelOuter[1]=1;gl_TessLevelOuter[2]=1;
 gl_TessLevelInner[0]=1;
}
''')
            (path/'test.tese').write_text('''#version 450
layout(triangles,equal_spacing,ccw) in;
layout(location=0) out vec4 color;
out gl_PerVertex {vec4 gl_Position;float gl_ClipDistance[4];float gl_CullDistance[4];};
void main(){
 gl_Position=gl_in[0].gl_Position*gl_TessCoord.x+
 gl_in[1].gl_Position*gl_TessCoord.y+gl_in[2].gl_Position*gl_TessCoord.z;
 color=vec4(gl_TessCoord,1);
 for(int i=0;i<4;i++){
  gl_ClipDistance[i]=gl_Position.x+float(i);
  gl_CullDistance[i]=gl_Position.y-float(i);
 }
}
''')
            for stage in ('tesc','tese'):
                run([glslang,'-V',str(path/('test.'+stage)),'-o',str(path/(stage+'.spv'))])
            run([str(path/'probe'),'--domain',str(path/'tesc.spv'),str(path/'tese.spv')])
            # Native cull-only witness reads only the second packed register,
            # with no generic varying that could accidentally supply its color.
            run([glslang,'-V','experiments/graphics/runtime_tess_cullonly.frag',
                 '-o',str(path/'cull-witness.spv')])
            run([str(path/'probe'),str(path/'cull-witness.spv'),'0x31'])
            run([glslang,'-V','experiments/graphics/runtime_tess_cullonly.tese',
                 '-o',str(path/'cull-witness.tese.spv')])
            (path/'linked.frag').write_text('''#version 450
in float gl_CullDistance[4];
layout(location=0) out vec4 color;
void main(){color=vec4(gl_CullDistance[2],0,0,1);}
''')
            run([glslang,'-V',str(path/'linked.frag'),'-o',str(path/'linked.spv')])
            for prefix in range(5):
                with self.subTest(producer_clip_prefix=prefix):
                    run([str(path/'probe'),str(path/'linked.spv'),hex(0x30+(prefix+2)//4)],
                        env={**os.environ,'PSBC_TEST_CLIP_PREFIX':str(prefix)})
            (path/'linked.frag').write_text('''#version 450
in float gl_CullDistance[4];
layout(location=0) out vec4 color;
void main(){color=vec4(gl_CullDistance[0],gl_CullDistance[1],gl_CullDistance[2],gl_CullDistance[3]);}
''')
            run([glslang,'-V',str(path/'linked.frag'),'-o',str(path/'linked.spv')])
            for prefix in range(5):
                expected = ['0x30'] if prefix==0 else ['0x31'] if prefix==4 else ['0x30','0x31']
                with self.subTest(vector_producer_clip_prefix=prefix):
                    run([str(path/'probe'),str(path/'linked.spv'),*expected],
                        env={**os.environ,'PSBC_TEST_CLIP_PREFIX':str(prefix)})
            for prefix in (5,8,9,4294967295):
                run([str(path/'probe'),str(path/'linked.spv'),'0'],env={
                    **os.environ,'PSBC_TEST_CLIP_PREFIX':str(prefix),'PSBC_TEST_REJECT':'1'})
            (path/'dynamic.frag').write_text('''#version 450
in float gl_CullDistance[4];
layout(location=0) out vec4 color;
void main(){int i=int(gl_FragCoord.x)&3;color=vec4(gl_CullDistance[i],0,0,1);}
''')
            run([glslang,'-V',str(path/'dynamic.frag'),'-o',str(path/'dynamic.spv')])
            for prefix in range(5):
                expected = ['0x30'] if prefix == 0 else ['0x31'] if prefix == 4 else ['0x30','0x31']
                with self.subTest(dynamic_producer_clip_prefix=prefix):
                    run([str(path/'probe'),str(path/'dynamic.spv'),*expected],env={
                        **os.environ,'PSBC_TEST_CLIP_PREFIX':str(prefix)})
            run([glslang,'-V','experiments/graphics/runtime_tess_dynamic_distance.frag',
                 '-o',str(path/'native-dynamic.spv')])
            run([str(path/'probe'),str(path/'native-dynamic.spv'),'0x30','0x31'],env={
                **os.environ,'PSBC_TEST_CLIP_PREFIX':'3'})
