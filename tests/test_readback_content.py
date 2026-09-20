import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReadbackContentTests(unittest.TestCase):
    def test_empty_clear_and_draw_are_distinct(self):
        source = r'''
#include "native/readback_content.h"
#include <assert.h>
#include <string.h>
int main(void) {
    unsigned char p[]={0,0,0,255,128,128,128,255,1,0,0,7};
    unsigned char old[sizeof(p)];memcpy(old,p,sizeof(p));
    struct ps5vk_readback_content r={0};
    assert(!ps5vk_readback_content(p,sizeof(p),&r));
    assert(r.pixels==3 && r.nonblack==2 && r.opaque==2 && r.gray128==1);
    assert(!memcmp(old,p,sizeof(p)));
    uint32_t hash=r.hash;
    assert(ps5vk_readback_content(p,3,&r) && r.hash==hash);
    assert(ps5vk_readback_content(p,0,&r) && r.hash==hash);
    assert(!ps5vk_readback_content(p,4,&r));
    assert(!r.nonblack && r.opaque==1 && !r.gray128 && r.hash!=hash);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "content"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT), "-x", "c", "-", "-o", str(binary)],
                           input=source, text=True, capture_output=True, check=True)
            subprocess.run([str(binary)], check=True, capture_output=True)
