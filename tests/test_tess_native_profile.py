from pathlib import Path
import itertools
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeTessProfileTests(unittest.TestCase):
    def test_only_complete_runtime_path_exposes_feature_and_limits(self):
        source = r'''
#include "native/tess_profile.h"
#include <assert.h>
#include <string.h>
int main(void) {
    struct ps5vk_platform p = {0};
    p.supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
    ps5vk_native_tess_profile(&p);
    assert(p.supported_features == (PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
        (EXPECT_TESS ? PS5VK_FEATURE_TESSELLATION_SHADER : 0)));
    const VkPhysicalDeviceLimits *l = &p.properties.limits;
    assert(l->maxTessellationGenerationLevel == (EXPECT_TESS ? 64u : 0u));
    assert(l->maxTessellationPatchSize == (EXPECT_TESS ? 32u : 0u));
    assert(l->maxTessellationControlPerVertexInputComponents == (EXPECT_TESS ? 128u : 0u));
    assert(l->maxTessellationControlPerVertexOutputComponents == (EXPECT_TESS ? 128u : 0u));
    assert(l->maxTessellationControlPerPatchOutputComponents == (EXPECT_TESS ? 120u : 0u));
    assert(l->maxTessellationControlTotalOutputComponents == (EXPECT_TESS ? 4096u : 0u));
    assert(l->maxTessellationEvaluationInputComponents == (EXPECT_TESS ? 128u : 0u));
    assert(l->maxTessellationEvaluationOutputComponents == (EXPECT_TESS ? 128u : 0u));
    struct ps5vk_platform before = p;
    ps5vk_native_tess_profile(&p);
    assert(memcmp(&before, &p, sizeof(p)) == 0);
}
'''
        names = ('PS5VK_GRAPHICS_API', 'PS5VK_GRAPHICS_DRAW',
                 'PS5VK_RUNTIME_COMPILER', 'PS5VK_RUNTIME_GRAPHICS')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'profile'
            for values in itertools.product((0, 1), repeat=4):
                with self.subTest(values=values):
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(ROOT), '-I', str(ROOT / 'src'),
                                    '-I', str(ROOT / 'third_party/vulkan-headers/include'),
                                    *[f'-D{k}={v}' for k, v in zip(names, values)],
                                    f'-DEXPECT_TESS={int(all(values))}',
                                    '-x', 'c', '-', '-o', str(binary)],
                                   input=source, text=True, capture_output=True, check=True)
                    subprocess.run([str(binary)], check=True)
