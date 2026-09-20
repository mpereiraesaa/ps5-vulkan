"""Own diagnostic code encoding, bounds and non-clobber regression."""
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class EntryWitnessTests(unittest.TestCase):
    def test_hull_trace_matches_gfx1013_assembler(self):
        assembler = shutil.which('llvm-mc')
        if assembler is None:
            self.skipTest('llvm-mc unavailable; bounded C encoding test still runs')
        targets = subprocess.run([assembler,'-triple=amdgcn-amd-amdhsa',
            '-mcpu=help'],text=True,capture_output=True)
        if 'gfx1013' not in targets.stdout + targets.stderr:
            self.skipTest('installed llvm-mc lacks GFX1013; bounded C test still runs')
        source = r'''
#include "native/tess_entry_witness.h"
#include <stdio.h>
int main(void) {
    uint32_t words[64];
    for(unsigned slot=0;slot<=14;slot+=2) {
        if(ps5vk_tess_hull_trace_prefix(words,sizeof(words),slot,16))return 1;
        if(fwrite(words,1,sizeof(words),stdout)!=sizeof(words))return 2;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / 'trace-bytes'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-I',str(ROOT),'-x','c','-','-o',str(exe)],
                input=source,text=True,check=True,capture_output=True)
            actual = subprocess.run([str(exe)],check=True,capture_output=True).stdout
        self.assertEqual(len(actual),8*256)
        for index,slot in enumerate(range(0,16,2)):
            assembly = f'''
s_load_dwordx2 s[60:61], s[{8+slot}:{9+slot}], 0x70
s_waitcnt lgkmcnt(0)
v_readfirstlane_b32 s62, v2
s_nop 7
s_and_b32 s62, s62, 127
s_lshl_b32 s62, s62, 6
s_add_u32 s60, s60, s62
s_addc_u32 s61, s61, 0
s_store_dwordx4 s[0:3], s[60:61], 0
s_store_dwordx4 s[4:7], s[60:61], 16
v_readfirstlane_b32 s64, v0
v_readfirstlane_b32 s65, v1
v_readfirstlane_b32 s66, v2
v_readfirstlane_b32 s67, v3
s_nop 7
s_store_dwordx4 s[64:67], s[60:61], 32
s_waitcnt lgkmcnt(0)
'''
            result = subprocess.run([assembler,'-triple=amdgcn-amd-amdhsa',
                '-mcpu=gfx1013','-show-encoding'],input=assembly,text=True,
                capture_output=True,check=True)
            encoded = b''.join(bytes(int(v,16) for v in line.split(','))
                for line in re.findall(r'encoding: \[([^]]+)\]',result.stdout))
            self.assertGreater(len(encoded),0)
            self.assertLessEqual(len(encoded),256)
            encoded += struct.pack('<I',0xbf800000)*((256-len(encoded))//4)
            self.assertEqual(actual[index*256:(index+1)*256],encoded)

    def test_queue_bound_timeout_cannot_report_cleanup(self):
        source = (ROOT / 'native/graphics_main.c').read_text()
        wait = source.index('const VkResult coord_wait=')
        legacy = source.index('"PS5VK_TESS_STALL', wait)
        guarded = source[wait:legacy]
        self.assertIn('PS5VK_TESS_RING_QUERY >= 3', guarded)
        self.assertIn('fail("tess bound ring fence",VK_ERROR_DEVICE_LOST)', guarded)
        fail = source[source.index('static void fail('):source.index('#define CHECK')]
        self.assertIn('if(rc==VK_ERROR_DEVICE_LOST)for(;;)sleep(1);', fail)

    def test_entry_report_is_in_retired_not_stalled_path(self):
        source = (ROOT / 'native/graphics_main.c').read_text()
        fault = source.index('if(coord_wait!=VK_SUCCESS)')
        stalled = source.index('"PS5VK_TESS_STALL', fault)
        retired = source.index('"PS5VK_TESS_GPU_LAUNCH', stalled)
        report = source.index('"PS5VK_TESS_ENTRY raw=1')
        self.assertGreater(report, retired)
        self.assertEqual(source.count('"PS5VK_TESS_ENTRY raw=1'), 1)

    def test_prefix_bounds_and_encoding(self):
        source = r'''
#include "native/tess_entry_witness.h"
#include <assert.h>
int main(void) {
    uint32_t words[65];
    for(unsigned i=0;i<65;++i)words[i]=0x12345678;
    assert(ps5vk_tess_entry_prefix(words,255,0,2)==-1);
    assert(ps5vk_tess_entry_prefix(words,256,1,4)==-1);
    assert(ps5vk_tess_entry_prefix(words,256,2,3)==-1);
    assert(ps5vk_tess_entry_prefix(words,256,16,18)==-1);
    assert(ps5vk_tess_entry_prefix(0,256,0,2)==-1);
    for(unsigned i=0;i<65;++i)assert(words[i]==0x12345678);
    for(unsigned slot=0;slot<=14;slot+=2) {
        assert(ps5vk_tess_entry_prefix(words,256,slot,16)==0);
        assert(words[0]==(0xf4480000u|((8+slot)/2)));
        assert(words[1]==0xfa0000c0u);
        assert(words[2]==(0xf4480100u|((8+slot)/2)));
        assert(words[3]==0xfa0000d0u);
        assert(words[4]==0xbf8cc07fu);
        for(unsigned i=5;i<64;++i)assert(words[i]==0xbf800000u);
        assert(words[64]==0x12345678);
    }
    for(unsigned i=0;i<65;++i)words[i]=0x12345678;
    assert(ps5vk_tess_hull_trace_prefix(words,255,0,2)==-1);
    assert(ps5vk_tess_hull_trace_prefix(words,256,1,4)==-1);
    assert(ps5vk_tess_hull_trace_prefix(words,256,2,3)==-1);
    assert(ps5vk_tess_hull_trace_prefix(words,256,16,18)==-1);
    assert(ps5vk_tess_hull_trace_prefix(0,256,0,2)==-1);
    for(unsigned i=0;i<65;++i)assert(words[i]==0x12345678);
    for(unsigned slot=0;slot<=14;slot+=2) {
        assert(ps5vk_tess_hull_trace_prefix(words,256,slot,16)==0);
        assert(words[0]==(0xf4040f00u|((8+slot)/2)));
        assert(words[1]==0xfa000070u); /* owned table entry7 */
        assert(words[6]==127u); /* key bounded to128 x64B records */
        assert(words[10]==0xf448001eu); /* s0:3 -> trace */
        assert(words[12]==0xf448011eu); /* s4:7 -> trace */
        assert(words[19]==0xf448101eu); /* raw first-lane v0:3 */
        assert(words[21]==0xbf8cc07fu);
        for(unsigned i=22;i<64;++i)assert(words[i]==0xbf800000u);
        assert(words[64]==0x12345678);
    }
}
'''
        with tempfile.TemporaryDirectory() as d:
            exe = pathlib.Path(d) / 'entry-test'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-I',str(ROOT),'-x','c','-','-o',str(exe)],
                input=source,text=True,check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,capture_output=True)
