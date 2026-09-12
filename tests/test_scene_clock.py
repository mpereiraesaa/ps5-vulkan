import subprocess
import tempfile
from pathlib import Path
import unittest


class SceneClockTests(unittest.TestCase):
    def test_native_time_based_phase(self):
        root = Path(__file__).resolve().parents[1]
        source = r'''
#include "scene_clock.h"
#include <assert.h>
int main(void) {
    assert(ps5vk_scene_angle(0)==0);
    float half=ps5vk_scene_angle(UINT64_C(3000000000));
    assert(half>3.14159f && half<3.14160f);
    assert(ps5vk_scene_angle(UINT64_C(6000000000))==0);
    assert(ps5vk_scene_angle(UINT64_C(9000000000))==half);
    assert(ps5vk_scene_pattern(0)==0);
    assert(ps5vk_scene_pattern(UINT64_C(1999999999))==0);
    assert(ps5vk_scene_pattern(UINT64_C(2000000000))==1);
    assert(ps5vk_scene_pattern(UINT64_C(4000000000))==2);
    assert(ps5vk_scene_pattern(UINT64_C(6000000000))==0);
    assert(ps5vk_scene_angle(UINT64_MAX)>=0);
    assert(ps5vk_scene_angle(UINT64_MAX)<6.28319f);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "clock-test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(root / "native"), "-x", "c", "-",
                            "-o", str(binary)], input=source, text=True, check=True)
            subprocess.run([str(binary)], check=True)
