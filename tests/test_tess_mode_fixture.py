"""Verify fixture transformation and pinned validator acceptance, not pixels."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from tess_mode_fixture import swap_owned_modes,decode

class ModeFixtureTests(unittest.TestCase):
    def test_owned_modes_move_without_shader_instruction_changes(self):
        glslang=shutil.which('glslangValidator')
        if not glslang:self.skipTest('glslang required')
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);original=[]
            for name in ('runtime_tess_coord.tesc','runtime_tess_output_envelope.tese'):
                target=p/name
                subprocess.run([glslang,'-V',str(ROOT/'experiments/graphics'/name),'-o',str(target)],
                               check=True,stdout=subprocess.DEVNULL)
                original.append(target.read_bytes())
            moved=swap_owned_modes(*original)
            for i,(old,new) in enumerate(zip(original,moved)):
                old_ops=decode(old)[1];new_ops=decode(new)[1]
                self.assertEqual([x for x in old_ops if x[0]&65535!=16],
                                 [x for x in new_ops if x[0]&65535!=16])
                modes={x[2] for x in new_ops if x[0]&65535==16}
                self.assertEqual(modes,{1,4,22} if i==0 else {26})
                target=p/str(i);target.write_bytes(new)
                validator=ROOT/'build/spirv-tools-host/tools/spirv-val'
                if validator.exists():
                    subprocess.run([str(validator),'--target-env','vulkan1.0',str(target)],check=True)
            with self.assertRaises(ValueError):swap_owned_modes(*moved)
            with self.assertRaises(ValueError):swap_owned_modes(b'bad',original[1])
