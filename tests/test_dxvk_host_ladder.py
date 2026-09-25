"""Host contracts of the DIAGNOSTIC DXVK refusal ladder.

These tests need neither DXVK nor a GPU. They pin the hand-assembled DXBC,
the one-edit diagnostic DXVK patch, the run classifier and the rung ordering,
and they drive the consumer-side interposer against a host build of the real
ps5vk sources to reproduce the first two instance-level refusals.
"""

import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


import run_dxvk_ps5vk_host_ladder as ladder  # noqa: E402
HEADERS = ROOT / "third_party/vulkan-headers/include"


def chunks(blob):
    count = struct.unpack_from("<I", blob, 28)[0]
    offsets = struct.unpack_from(f"<{count}I", blob, 32)
    return {blob[offset:offset + 4]: blob[offset + 8:offset + 8 +
                                          struct.unpack_from("<I", blob, offset + 4)[0]]
            for offset in offsets}


def with_padding_chunk(blob, pad):
    """Append a PADD chunk, keeping every other chunk, and re-sign."""
    count = struct.unpack_from("<I", blob, 28)[0]
    offsets = [offset + 4 for offset in struct.unpack_from(f"<{count}I", blob, 32)]
    body = blob[32 + 4 * count:] + b"PADD" + struct.pack("<I", pad) + bytes(pad)
    start = 32 + 4 * (count + 1)
    offsets.append(start + len(body) - 8 - pad)
    unsigned = (b"DXBC" + bytes(16) + struct.pack("<3I", 1, start + len(body), count + 1) +
                struct.pack(f"<{count + 1}I", *offsets) + body)
    return unsigned[:4] + ladder.dxbc_checksum(unsigned) + unsigned[20:]


class DxbcTests(unittest.TestCase):
    def test_shaders_are_the_validated_blobs(self):
        # These exact bytes were accepted (checksum and token stream) by
        # vkd3d-shader's DXBC-to-SPIR-V translator and by DXVK on a host
        # Vulkan driver, which rendered the expected pixel.
        self.assertEqual(hashlib.sha256(ladder.vertex_shader()).hexdigest(),
                         "124f7a35c80ebcf6e22e1eabbcb629075c785151d5185367473b59da40ba6fff")
        self.assertEqual(hashlib.sha256(ladder.pixel_shader()).hexdigest(),
                         "c527ac8abd2c0cc91cbf098e3fe7855fcf34c11813b85c6cfed2ffaa1378bfae")

    def test_checksum_covers_both_tail_paths(self):
        vs = ladder.vertex_shader()
        self.assertLess((len(vs) - 20) % 64, 56)
        self.assertEqual(vs[4:20].hex(), "64cac5b078f2d711a2858251c1ab5d79")
        padded = with_padding_chunk(ladder.pixel_shader(), 16)
        self.assertEqual((len(padded) - 20) % 64, 56)
        self.assertEqual(padded[4:20].hex(), "92f0e3dc77c4511b9e145a58cadadb2d")

    def test_container_layout(self):
        for blob, program_type in ((ladder.vertex_shader(), 1), (ladder.pixel_shader(), 0)):
            self.assertEqual(blob[:4], b"DXBC")
            self.assertEqual(struct.unpack_from("<2I", blob, 20), (1, len(blob)))
            parts = chunks(blob)
            self.assertEqual(sorted(parts), [b"ISGN", b"OSGN", b"SHDR"])
            shdr = parts[b"SHDR"]
            version, length = struct.unpack_from("<2I", shdr, 0)
            self.assertEqual(version, program_type << 16 | 0x40)
            self.assertEqual(length * 4, len(shdr))
            # the last instruction is ret
            self.assertEqual(struct.unpack_from("<I", shdr, len(shdr) - 4)[0], 62 | 1 << 24)

    def test_signatures(self):
        vs, ps = chunks(ladder.vertex_shader()), chunks(ladder.pixel_shader())
        self.assertIn(b"SV_VertexID\0", vs[b"ISGN"])
        self.assertEqual(struct.unpack_from("<6I", vs[b"ISGN"], 8)[2:], (6, 1, 0, 0x101))
        self.assertIn(b"SV_Position\0", vs[b"OSGN"])
        self.assertEqual(struct.unpack_from("<2I", ps[b"ISGN"], 0), (0, 8))
        self.assertIn(b"SV_Target\0", ps[b"OSGN"])

    @unittest.skipUnless(shutil.which("vkd3d-compiler"), "vkd3d-compiler not installed")
    def test_vkd3d_translates_the_blobs(self):
        with tempfile.TemporaryDirectory() as temp:
            for name, blob in (("vs", ladder.vertex_shader()), ("ps", ladder.pixel_shader()),
                               ("pad", with_padding_chunk(ladder.pixel_shader(), 16))):
                source = Path(temp) / f"{name}.dxbc"
                source.write_bytes(blob)
                subprocess.run(["vkd3d-compiler", "-x", "dxbc-tpf", "-b", "spirv-binary",
                                "-o", str(Path(temp) / f"{name}.spv"), str(source)],
                               check=True, capture_output=True)

    def test_header_embeds_both_blobs(self):
        header = ladder.shader_header()
        self.assertIn(f"kLadderVertexShader[{len(ladder.vertex_shader())}]", header)
        self.assertIn(f"kLadderPixelShader[{len(ladder.pixel_shader())}]", header)


class DiagnosticPatchTests(unittest.TestCase):
    SOURCE = ("  bool DxvkDeviceFilter::testAdapter(const VkPhysicalDeviceProperties& p) const {\n"
              "    if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {\n" +
              ladder.DIAGNOSTIC_OLD + "    }\n")

    def test_edit_only_keeps_the_adapter(self):
        patched = ladder.patched_source(self.SOURCE)
        self.assertNotIn("return false;", patched)
        self.assertIn(ladder.DIAGNOSTIC_MARKER, patched)
        self.assertIn("PS5VK DIAGNOSTIC: not upstream DXVK", patched)
        self.assertEqual(patched.replace(ladder.DIAGNOSTIC_NEW, ladder.DIAGNOSTIC_OLD),
                         self.SOURCE)

    def test_edit_refuses_an_unexpected_source(self):
        with self.assertRaises(ValueError):
            ladder.patched_source("unrelated")
        with self.assertRaises(ValueError):
            ladder.patched_source(self.SOURCE + ladder.DIAGNOSTIC_OLD)


class ClassifyTests(unittest.TestCase):
    def test_failed_step_is_the_first_refusal(self):
        out = ("LADDER_STEP D3D11CreateDevice hr=0x00000000\n"
               "LADDER_STEP CreateTexture2D.render_target hr=0x80070057\n")
        err = ("err:   D3D11: Cannot create texture:\n"
               "LADDER_PROC_NULL scope=device name=vkCmdPipelineBarrier2\n"
               "LADDER_PROC_NULL scope=device name=vkCmdPipelineBarrier2\n"
               "LADDER_CALL vkCreateDevice result=-7\n"
               "LADDER_FEATURES checked_missing=vk12.samplerMirrorClampToEdge,transformFeedback\n"
               "LADDER_FEATURES forced_missing=none\n")
        result = ladder.classify(1, out, err)
        self.assertEqual(result["first_refusal"], "CreateTexture2D.render_target hr=0x80070057")
        self.assertEqual(result["steps_passed"], ["D3D11CreateDevice"])
        self.assertEqual(result["first_dxvk_error"], "err:   D3D11: Cannot create texture:")
        self.assertEqual(result["null_entry_points"], ["vkCmdPipelineBarrier2"])
        self.assertEqual(result["vulkan_refusals"], ["LADDER_CALL vkCreateDevice result=-7"])
        self.assertEqual(result["features_missing_checked_by_d3d11"],
                         ["vk12.samplerMirrorClampToEdge", "transformFeedback"])
        self.assertEqual(result["features_missing_forced_at_create"], [])

    def test_signal_names_the_last_passed_step(self):
        result = ladder.classify(-11, "LADDER_STEP D3D11CreateDevice hr=0x00000000\n", "")
        self.assertEqual(result["first_refusal"], "signal 11 after D3D11CreateDevice")
        self.assertEqual(ladder.classify(-11, "", "")["first_refusal"], "signal 11 after start")

    def test_pixel_tolerance(self):
        steps = "".join(f"LADDER_STEP {name} hr=0x00000000\n" for name in ("Map.read", "Unmap"))
        good = ladder.classify(0, steps + "LADDER_PIXEL x=32 y=32 rgba=64,127,191,255\n", "")
        self.assertIsNone(good["first_refusal"])
        self.assertEqual(good["pixel"], [64, 127, 191, 255])
        bad = ladder.classify(0, steps + "LADDER_PIXEL x=32 y=32 rgba=255,0,0,255\n", "")
        self.assertEqual(bad["first_refusal"], "wrong pixel [255, 0, 0, 255]")

    def test_success_needs_the_readback(self):
        result = ladder.classify(0, "LADDER_STEP D3D11CreateDevice hr=0x00000000\n", "")
        self.assertEqual(result["first_refusal"], "exit 0 after D3D11CreateDevice")


class LadderShapeTests(unittest.TestCase):
    def test_rungs_are_cumulative_and_labelled(self):
        self.assertEqual(ladder.RUNGS[0].name, "r0-unmodified")
        self.assertEqual(ladder.RUNGS[0].dxvk, "pinned")
        self.assertEqual(ladder.RUNGS[0].bypass, ())
        seen_diagnostic = False
        for previous, rung in zip(ladder.RUNGS, ladder.RUNGS[1:]):
            self.assertEqual(rung.bypass[:len(previous.bypass)], previous.bypass, rung.name)
            self.assertLessEqual(len(rung.bypass) - len(previous.bypass), 1, rung.name)
            seen_diagnostic |= rung.dxvk == "diagnostic"
            if seen_diagnostic:
                self.assertEqual(rung.dxvk, "diagnostic", rung.name)
        for rung in ladder.RUNGS:
            if {"fake_features", "fake_coherent"} & set(rung.bypass[-1:]):
                self.assertTrue(rung.purpose.startswith("LIE:"), rung.name)

    def test_every_rung_has_an_expected_boundary(self):
        self.assertEqual(sorted(ladder.EXPECTED), sorted(rung.name for rung in ladder.RUNGS))

    def test_every_bypass_is_a_shim_knob(self):
        source = ladder.SHIM.read_text()
        knobs = set(re.findall(r'strcmp\(token, "(\w+)"\)', source))
        used = {knob for rung in ladder.RUNGS for knob in rung.bypass}
        self.assertEqual(used, knobs)
        for knob in knobs:
            self.assertRegex(source, rf"\n \*   {knob}\b", f"undocumented knob {knob}")


@unittest.skipUnless(HEADERS.is_dir() and shutil.which("cc"), "pinned Vulkan headers or cc missing")
class ShimAgainstPs5vkHostTests(unittest.TestCase):
    """The shim in front of the real ps5vk host sources, without DXVK."""

    PROGRAM = r"""
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
/* argv[1]: loader library; argv[2..]: instance extensions to request with
 * DXVK's VkApplicationInfo (apiVersion 1.3). */
int main(int argc, char **argv) {
  void *lib = dlopen(argv[1], RTLD_NOW);
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  PFN_vkEnumerateInstanceExtensionProperties enumerate =
      (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
  PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  VkExtensionProperties props[16]; uint32_t count = 16;
  enumerate(NULL, &count, props);
  int advertised = 0;
  for (int e = 2; e < argc; ++e)
    for (uint32_t n = 0; n < count; ++n)
      advertised += !strcmp(props[n].extensionName, argv[e]);
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "t", 0, "DXVK",
                           0, VK_MAKE_API_VERSION(0, 1, 3, 0)};
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL,
                               (uint32_t)(argc - 2), (const char *const *)argv + 2};
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = create(&info, NULL, &instance);
  printf("advertised=%d/%d create=%d features2=%d\n", advertised, argc - 2, (int)r,
         instance && gipa(instance, "vkGetPhysicalDeviceFeatures2") != NULL);
  if (instance) ((PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance"))(instance, NULL);
  return 0;
}
"""
    SDL2_X11 = ("VK_KHR_surface", "VK_KHR_xlib_surface")
    PS5_WSI = ("VK_KHR_surface", "VK_KHR_display")

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        directory = Path(cls.temp.name)
        cls.real = ladder.build_ps5vk(directory, "host")
        cls.shim = ladder.build_shim(directory)
        source = directory / "probe.c"
        source.write_text(cls.PROGRAM)
        cls.probe = directory / "probe"
        subprocess.run(["cc", "-std=c11", f"-I{HEADERS}", str(source), "-ldl", "-o",
                        str(cls.probe)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, bypass, extensions=SDL2_X11):
        env = dict(os.environ, PS5VK_LADDER_REAL=str(self.real),
                   PS5VK_LADDER_BYPASS=",".join(bypass))
        run = subprocess.run([str(self.probe), str(self.shim), *extensions], env=env,
                             text=True, capture_output=True, check=True)
        return run.stdout.strip(), run.stderr

    def test_only_the_loader_entry_points_are_exported(self):
        symbols = subprocess.run(["nm", "-D", "--defined-only", str(self.shim)], text=True,
                                 capture_output=True, check=True).stdout
        exported = sorted(line.split()[-1] for line in symbols.splitlines()
                          if line.split()[-2] == "T")
        self.assertEqual(exported, ["vkCreateInstance", "vkEnumerateInstanceExtensionProperties",
                                    "vkGetDeviceProcAddr", "vkGetInstanceProcAddr"])

    def test_ps5_wsi_request_is_refused_only_for_the_1_3_instance(self):
        # The PS5 WSI adapter's instance extensions are real ps5vk routes; with
        # no bypass the one remaining instance refusal is apiVersion 1.3.
        stdout, _ = self.run_probe((), self.PS5_WSI)
        self.assertEqual(stdout, "advertised=2/2 create=-9 features2=0")

    def test_host_sdl2_surface_name_is_missing_without_the_shim(self):
        stdout, _ = self.run_probe(())
        self.assertEqual(stdout, "advertised=1/2 create=-7 features2=0")

    def test_surface_bypass_fakes_only_the_missing_name(self):
        stdout, stderr = self.run_probe(("surface",))
        self.assertEqual(stdout, "advertised=2/2 create=-9 features2=0")
        self.assertIn("LADDER_STRIP instance_extension=VK_KHR_xlib_surface", stderr)
        self.assertNotIn("instance_extension=VK_KHR_surface\n", stderr)
        self.assertIn("LADDER_CALL vkCreateInstance apiVersion=1.3.0", stderr)

    def test_api10_rewrite_opens_the_instance_without_core_names(self):
        stdout, stderr = self.run_probe(("surface", "api10"))
        self.assertEqual(stdout, "advertised=2/2 create=0 features2=0")
        self.assertIn("LADDER_PROC_NULL scope=instance name=vkGetPhysicalDeviceFeatures2", stderr)

    def test_core_alias_resolves_the_khr_command(self):
        stdout, stderr = self.run_probe(("surface", "api10", "core_alias"))
        self.assertEqual(stdout, "advertised=2/2 create=0 features2=1")
        self.assertIn("LADDER_ALIAS vkGetPhysicalDeviceFeatures2 -> "
                      "vkGetPhysicalDeviceFeatures2KHR", stderr)

if __name__ == "__main__":
    unittest.main()
