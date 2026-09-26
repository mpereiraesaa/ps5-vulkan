"""Host contracts for the DXVK native PS5 payload: pixel oracle, SHA-256,
embedded DXBC, the DIAGNOSTIC device-filter patch, build identity and the
run-receipt parser. None of this is evidence of PS5 execution."""

import hashlib
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_dxvk_ps5_native as build  # noqa: E402
import run_dxvk_ps5_native as runner  # noqa: E402

SOURCE = ROOT / "examples/dxvk_native"

ORACLE_PROGRAM = r"""
#include "oracle.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill(uint8_t *image, size_t pitch, int clear_only)
{
    memset(image, 0xa5, pitch * DXVK_ORACLE_HEIGHT); /* padding garbage */
    for (uint32_t y = 0; y < DXVK_ORACLE_HEIGHT; ++y)
        for (uint32_t x = 0; x < DXVK_ORACLE_WIDTH; ++x) {
            uint32_t v = clear_only ? dxvk_oracle_expected(DXVK_ORACLE_WIDTH - 1, 0)
                                    : dxvk_oracle_expected(x, y);
            memcpy(image + y * pitch + 4 * x, &v, 4);
        }
}

static void report(const char *name, int verdict, const dxvk_oracle_result *r)
{
    printf("%s verdict=%d checked=%u mismatches=%u reported=%u checksum=%08x expected=%08x",
           name, verdict, r->checked, r->mismatches, r->reported, r->checksum,
           r->expected_checksum);
    for (uint32_t i = 0; i < r->reported && i < 2; ++i)
        printf(" m%u=%u,%u,%08x,%08x", i, r->first[i].x, r->first[i].y, r->first[i].got,
               r->first[i].expected);
    printf("\n");
}

int main(void)
{
    for (uint32_t y = 0; y < DXVK_ORACLE_HEIGHT; ++y)
        for (uint32_t x = 0; x < DXVK_ORACLE_WIDTH; ++x)
            printf("P %u %u %08x\n", x, y, dxvk_oracle_expected(x, y));
    static uint8_t image[320 * 64];
    dxvk_oracle_result r;
    fill(image, 256, 0);
    report("tight", dxvk_oracle_check(image, 256, &r), &r);
    fill(image, 320, 0);
    report("padded", dxvk_oracle_check(image, 320, &r), &r);
    fill(image, 256, 1);
    report("clear_only", dxvk_oracle_check(image, 256, &r), &r);
    fill(image, 256, 0);
    image[256 * 5 + 4 * 7 + 2] ^= 1;
    report("one_pixel", dxvk_oracle_check(image, 256, &r), &r);
    report("short_pitch", dxvk_oracle_check(image, 252, &r), &r);
    report("null", dxvk_oracle_check(NULL, 256, &r), &r);

    char hex[65];
    dxvk_sha256 sha;
    dxvk_sha256_init(&sha);
    dxvk_sha256_update(&sha, "abc", 3);
    dxvk_sha256_hex(&sha, hex);
    printf("SHA abc %s\n", hex);
    dxvk_sha256_init(&sha);
    for (unsigned i = 0; i < 100000; ++i) {
        unsigned char byte = (unsigned char)(i * 131u + 7u);
        dxvk_sha256_update(&sha, &byte, 1);
    }
    dxvk_sha256_hex(&sha, hex);
    printf("SHA stream %s\n", hex);
    return 0;
}
"""


def expected_pixel(x: int, y: int) -> int:
    if x >= 48:
        r, g, b, a = 64, 128, 192, 255
    else:
        r, g, b, a = x * 4, y * 4, (x * 7 + y * 13) & 255, 255
    return r | g << 8 | b << 16 | a << 24


def fnv1a(pixels) -> int:
    value = 2166136261
    for pixel in pixels:
        for shift in (0, 8, 16, 24):
            value = ((value ^ ((pixel >> shift) & 255)) * 16777619) & 0xffffffff
    return value


EXPECTED_CHECKSUM = fnv1a(expected_pixel(x, y) for y in range(64) for x in range(64))


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class OracleProgram(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="dxvk-native-oracle-")
        source = Path(cls.temp.name) / "oracle_test.c"
        executable = Path(cls.temp.name) / "oracle_test"
        source.write_text(ORACLE_PROGRAM)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{SOURCE}",
                        str(source), "-o", str(executable)], check=True)
        cls.output = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True).stdout.splitlines()
        cls.cases = {line.split()[0]: dict(item.split("=", 1) for item in line.split()[1:])
                     for line in cls.output if not line.startswith(("P ", "SHA "))}

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_expected_function_matches_the_shader_contract(self):
        pixels = {(int(x), int(y)): int(value, 16) for _, x, y, value in
                  (line.split() for line in self.output if line.startswith("P "))}
        self.assertEqual(len(pixels), 64 * 64)
        for (x, y), value in pixels.items():
            self.assertEqual(value, expected_pixel(x, y), (x, y))
        self.assertEqual(pixels[(0, 0)], 0xff000000)
        self.assertEqual(pixels[(47, 63)], expected_pixel(47, 63))
        self.assertEqual(pixels[(48, 0)], 0xffc08040)

    def test_matching_images_pass_with_and_without_row_padding(self):
        for case in ("tight", "padded"):
            result = self.cases[case]
            self.assertEqual(result["verdict"], "0")
            self.assertEqual(result["checked"], "4096")
            self.assertEqual(result["mismatches"], "0")
            self.assertEqual(int(result["checksum"], 16), EXPECTED_CHECKSUM)
            self.assertEqual(int(result["expected"], 16), EXPECTED_CHECKSUM)

    def test_a_clear_without_the_draw_fails_every_viewport_pixel(self):
        result = self.cases["clear_only"]
        self.assertEqual(result["verdict"], "1")
        self.assertEqual(result["mismatches"], str(48 * 64))
        self.assertEqual(result["reported"], "8")
        self.assertEqual(result["m0"], f"0,0,ffc08040,{expected_pixel(0, 0):08x}")

    def test_one_wrong_channel_is_located(self):
        result = self.cases["one_pixel"]
        self.assertEqual(result["mismatches"], "1")
        good = expected_pixel(7, 5)
        self.assertEqual(result["m0"], f"7,5,{good ^ 0x10000:08x},{good:08x}")
        self.assertNotEqual(int(result["checksum"], 16), EXPECTED_CHECKSUM)

    def test_bad_arguments_are_rejected(self):
        self.assertEqual(self.cases["short_pitch"]["verdict"], "-1")
        self.assertEqual(self.cases["null"]["verdict"], "-1")
        self.assertEqual(self.cases["null"]["checked"], "0")

    def test_sha256_matches_hashlib(self):
        digests = {line.split()[1]: line.split()[2] for line in self.output
                   if line.startswith("SHA ")}
        self.assertEqual(digests["abc"], hashlib.sha256(b"abc").hexdigest())
        stream = bytes((i * 131 + 7) & 255 for i in range(100000))
        self.assertEqual(digests["stream"], hashlib.sha256(stream).hexdigest())


class EmbeddedShaders(unittest.TestCase):
    def test_header_bytes_match_their_digests_and_regenerate_identically(self):
        header = (SOURCE / "dxvk_shaders.h").read_text()
        blobs = build.embedded_shaders(header)
        self.assertEqual(set(blobs), {"vs", "ps"})
        for blob in blobs.values():
            self.assertEqual(blob[:4], b"DXBC")
        self.assertEqual(build.shader_header(blobs["vs"], blobs["ps"]), header)

    def test_corrupted_bytes_are_rejected(self):
        header = (SOURCE / "dxvk_shaders.h").read_text()
        index = header.index("dxvk_native_ps_dxbc")
        corrupted = header[:index] + header[index:].replace("0x44, 0x58", "0x44, 0x59", 1)
        with self.assertRaises(ValueError):
            build.embedded_shaders(corrupted)
        with self.assertRaises(ValueError):
            build.shader_header(b"XXXX", b"DXBC")


class DiagnosticPatch(unittest.TestCase):
    SOURCE = "bool f() {\n" + build.FILTER_BLOCK + "\n    return true;\n}\n"

    def test_only_the_version_filter_changes(self):
        patched = build.patch_device_filter(self.SOURCE)
        self.assertNotIn("return false;", patched)
        self.assertIn("DIAGNOSTIC", patched)
        self.assertIn("return true;", patched)
        self.assertEqual(patched.replace(build.FILTER_DIAGNOSTIC, build.FILTER_BLOCK),
                         self.SOURCE)

    def test_feature_level_patch_touches_only_transform_feedback(self):
        source = "void g() {\n" + build.XFB_BLOCK + "    enabled.x = VK_TRUE;\n}\n"
        patched = build.patch_feature_level_xfb(source)
        self.assertIn("supported.extTransformFeedback.transformFeedback", patched)
        self.assertIn("supported.extTransformFeedback.geometryStreams", patched)
        self.assertIn("DIAGNOSTIC", patched)
        self.assertIn("enabled.x = VK_TRUE;", patched)
        self.assertEqual(patched.replace(build.XFB_DIAGNOSTIC, build.XFB_BLOCK), source)
        with self.assertRaises(ValueError):
            build.patch_feature_level_xfb("void g() {}\n")
        demote = source.replace("}\n", build.DEMOTE_BLOCK + "}\n")
        relaxed = build.patch_feature_level_xfb(demote, relax_demote=True)
        self.assertIn("supported.vk13.shaderDemoteToHelperInvocation", relaxed)
        self.assertNotIn("supported.vk13", build.patch_feature_level_xfb(demote))
        with self.assertRaises(ValueError):
            build.patch_feature_level_xfb(source, relax_demote=True)

    def test_changed_or_repeated_upstream_blocks_are_refused(self):
        with self.assertRaises(ValueError):
            build.patch_device_filter(self.SOURCE.replace("1, 3, 0", "1, 4, 0"))
        with self.assertRaises(ValueError):
            build.patch_device_filter(self.SOURCE + build.FILTER_BLOCK)

    def test_variants_label_the_diagnostic_build(self):
        self.assertEqual(build.VARIANTS["unmodified"]["patches"], ())
        self.assertFalse(build.VARIANTS["unmodified"]["diagnostic"])
        self.assertTrue(build.VARIANTS["diagnostic-version-filter"]["diagnostic"])
        header = build.identity_header("diagnostic-version-filter", "a" * 40, "b" * 40,
                                       False, "c" * 64, "d" * 64)
        self.assertIn("#define DXVK_NATIVE_DIAGNOSTIC 1", header)
        self.assertIn("bypass-apiVersion-1.3-filter", header)
        self.assertIn("#define DXVK_NATIVE_COMPAT_LAYER 0", header)
        compat = build.VARIANTS["diagnostic-compat"]
        self.assertTrue(compat["diagnostic"])
        self.assertEqual(compat["patches"][0],
                         build.VARIANTS["diagnostic-version-filter"]["patches"][0])
        self.assertIn("payload:compat-translation-layer-v1", compat["patches"])
        self.assertIn("#define DXVK_NATIVE_COMPAT_LAYER 1", build.identity_header(
            "diagnostic-compat", "a" * 40, "b" * 40, False, "c" * 64, "d" * 64))
        plain = build.identity_header("unmodified", "a" * 40, "b" * 40, True, "c", "d")
        self.assertIn("#define DXVK_NATIVE_DIAGNOSTIC 0", plain)
        self.assertIn('#define DXVK_NATIVE_PATCHES "none"', plain)
        self.assertIn("#define DXVK_NATIVE_COMPAT_LAYER 0", plain)
        self.assertIn("#define DXVK_NATIVE_PS5VK_DIRTY 1", plain)
        with self.assertRaises(ValueError):
            build.identity_header("other", "a", "b", False, "c", "d")
        with self.assertRaises(ValueError):
            build.identity_header("unmodified", 'a"b', "b", False, "c", "d")


def ps5log(lines):
    body = [f"{index}\t{1000 + index}\t{level}\t{text}"
            for index, (level, text) in enumerate(lines, start=1)]
    return "HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=0x1 tag=t\n" + \
        "\n".join(body) + f"\nBYE seq={len(lines)} reason=end\n"


IDENTITY = ("MARK", "DXVK_NATIVE_IDENTITY variant=unmodified label=UNMODIFIED diagnostic=0 "
            "dxvk_commit=9d6f ps5vk_commit=abc ps5vk_dirty=0 patches=none "
            "eboot_sha256=ee eboot_bytes=10 vs_sha256=v ps_sha256=p "
            "oracle_expected_checksum=6e17a4c5")
ARTIFACT = {"profile": build.PROFILE, "variant": "unmodified", "label": "UNMODIFIED",
            "diagnostic": False, "dxvk_commit": "9d6f", "dxvk_source_patches": [],
            "ps5vk_commit": "abc", "eboot_sha256": "ee"}


class DiagnosticIntegrationRecipe(unittest.TestCase):
    def test_only_switches_the_sdk_build_knows_are_applied(self):
        present, absent = build.diagnostic_integration_switches(
            'for name in ("PS5VK_MAINTENANCE4_DIAGNOSTIC",):')
        self.assertEqual(present, ["PS5VK_MAINTENANCE4_DIAGNOSTIC"])
        self.assertEqual(len(present) + len(absent), len(build.DXVK_DIAGNOSTIC_SWITCHES))
        present, absent = build.diagnostic_integration_switches("")
        self.assertEqual(absent, ["PS5VK_MAINTENANCE4_DIAGNOSTIC"])

    def test_every_switch_is_a_default_off_diagnostic(self):
        for name in build.DXVK_DIAGNOSTIC_SWITCHES:
            self.assertTrue(name.startswith("PS5VK_") and name.endswith("_DIAGNOSTIC"), name)

    def test_the_payload_keeps_no_thread_stack_wrap(self):
        self.assertNotIn("pthread_create", build.LINK_WRAPS)
        self.assertNotIn("pthread_create", (SOURCE / "ps5_main.cpp").read_text())


class ReceiptParser(unittest.TestCase):
    def test_surface_refusal_run(self):
        log = ps5log([
            IDENTITY,
            ("MARK", "DXVK_NATIVE_STAGE stage=d3d11.device state=begin"),
            ("INFO", "DXVK_LOG level=info stage=d3d11.device text=DXVK: v2.6.2"),
            ("MARK", "DXVK_NATIVE_STAGE stage=dxvk.load state=begin link=static"),
            ("INFO", "DXVK_LOADER dlopen name=libvulkan.so flags=0x2 resolved=static-ps5vk"),
            ("MARK", "DXVK_NATIVE_STAGE stage=dxvk.load state=ok vkGetInstanceProcAddr=ps5vk-static"),
            ("MARK", "DXVK_NATIVE_STAGE stage=dxvk.instance state=begin first_call=x"),
            ("INFO", "DXVK_VK_EXTENSIONS call=vkEnumerateInstanceExtensionProperties part=0 "
                     "count=2 list=VK_KHR_get_physical_device_properties2:2,VK_KHR_device_group_creation:1"),
            ("INFO", "DXVK_VK_MISSING scope=global name=vkEnumerateInstanceVersion"),
            ("INFO", "DXVK_LOG level=info stage=dxvk.instance text=Required Vulkan extension "
                     "VK_KHR_surface not supported"),
            ("ERR", "DXVK_LOG level=err stage=dxvk.instance text=DxvkInstance: Required "
                    "instance extensions not supported"),
            ("ERR", "DXVK_NATIVE_STAGE stage=dxvk.instance state=fail closed_by=d3d11.device"),
            ("ERR", "DXVK_NATIVE_STAGE stage=d3d11.device state=fail hr=0x80004005"),
            ("ERR", "DXVK_FIRST_REFUSAL source=dxvk_log stage=dxvk.instance level=info result=0 "
                    "last_vk_call=vkEnumerateInstanceExtensionProperties last_vk_params= layer=none "
                    "count=2 query=0 text=Required Vulkan extension VK_KHR_surface not supported"),
            ("ERR", "DXVK_NATIVE_RESULT outcome=refused variant=unmodified label=UNMODIFIED "
                    "last_stage=d3d11.device create_hr=0x80004005 feature_level=0x0000 "
                    "device_refs=0 context_refs=0"),
        ])
        summary = runner.parse_log(log)
        self.assertEqual(summary["identity"]["label"], "UNMODIFIED")
        self.assertEqual(summary["last_stage"], "dxvk.instance")
        refusal = summary["first_refusal"]
        self.assertEqual(refusal["source"], "dxvk_log")
        self.assertEqual(refusal["stage"], "dxvk.instance")
        self.assertEqual(refusal["last_vk_call"], "vkEnumerateInstanceExtensionProperties")
        self.assertEqual(refusal["last_vk_params"], "layer=none count=2 query=0")
        self.assertEqual(refusal["text"], "Required Vulkan extension VK_KHR_surface not supported")
        self.assertEqual(summary["first_refusal_candidate"]["text"], refusal["text"])
        self.assertEqual(summary["first_refusal_candidate"]["level"], "info")
        self.assertEqual(summary["extensions"]["vkEnumerateInstanceExtensionProperties"],
                         ["VK_KHR_get_physical_device_properties2:2",
                          "VK_KHR_device_group_creation:1"])
        self.assertEqual(summary["vk_missing"], ["vkEnumerateInstanceVersion"])
        self.assertEqual(summary["dxvk_log"]["counts"], {"info": 2, "err": 1})
        self.assertFalse(summary["gpu_hang_suspected"])
        receipt = runner.receipt_for(summary, ARTIFACT, {"run_id": "r1", "bye": True,
                                                         "clean": True}, "ff", True)
        self.assertEqual(receipt["identity_mismatches"], [])
        self.assertEqual(receipt["outcome"], "refused")
        self.assertEqual(receipt["last_stage"], "dxvk.instance")
        self.assertTrue(receipt["log_finalized"])
        self.assertEqual(receipt["run_id"], "r1")

    def test_vulkan_refusal_and_rendered_oracle(self):
        log = ps5log([
            IDENTITY,
            ("ERR", "DXVK_VK_REFUSAL call=vkCreateInstance result=-9(VK_ERROR_INCOMPATIBLE_DRIVER) "
                    "stage=dxvk.instance apiVersion=1.3.0 app=eboot.bin"),
            ("MARK", "DXVK_ORACLE checked=4096 mismatches=0 checksum=6e17a4c5 "
                     "expected_checksum=6e17a4c5 first=none"),
            ("ERR", "DXVK_FIRST_REFUSAL source=vulkan stage=dxvk.instance call=vkCreateInstance "
                    "result=-9 params= apiVersion=1.3.0 app=eboot.bin"),
        ])
        summary = runner.parse_log(log)
        self.assertEqual(summary["vk_refusals"][0]["call"], "vkCreateInstance")
        self.assertEqual(summary["vk_refusals"][0]["result"], "-9(VK_ERROR_INCOMPATIBLE_DRIVER)")
        self.assertEqual(summary["first_refusal"]["result"], -9)
        self.assertEqual(summary["first_refusal"]["params"], "apiVersion=1.3.0 app=eboot.bin")
        self.assertEqual(summary["oracle"]["mismatches"], 0)
        self.assertEqual(summary["oracle"]["checksum"], f"{EXPECTED_CHECKSUM:08x}")
        self.assertIsNone(summary["vk_first_call"])

    def test_first_last_call_and_shutdown(self):
        log = ps5log([
            IDENTITY,
            ("INFO", "DXVK_VK_CALL call=vkEnumerateInstanceExtensionProperties result=0(VK_SUCCESS)"),
            ("INFO", "DXVK_VK_CALL call=vkCreateImage result=0(VK_SUCCESS) type=1"),
            ("MARK", "DXVK_NATIVE_STAGE stage=shutdown state=begin"),
            ("MARK", "DXVK_NATIVE_STAGE stage=shutdown state=ok device_refs=0 context_refs=0"),
        ])
        summary = runner.parse_log(log)
        self.assertEqual(summary["vk_first_call"], "vkEnumerateInstanceExtensionProperties")
        self.assertEqual(summary["vk_last_call"], "vkCreateImage")
        receipt = runner.receipt_for(summary, ARTIFACT, None, "ff", True)
        self.assertEqual(receipt["shutdown"], "device_refs=0 context_refs=0")
        self.assertEqual(receipt["vk_last_call"], "vkCreateImage")

    def test_compat_translation_records(self):
        log = ps5log([
            IDENTITY,
            ("WARN", "DXVK_COMPAT_INSTANCE added=VK_KHR_get_physical_device_properties2"),
            ("INFO", "DXVK_VK_FEATURES struct=vk12 sType=51 mask=0x1"),
            ("ERR", "DXVK_COMPAT_REFUSAL call=vkCreateDevice feature=vk13.synchronization2(VK_KHR_synchronization2)"),
            ("ERR", "DXVK_FIRST_REFUSAL source=compat stage=dxvk.device_create call=vkCreateDevice "
                    "result=-8 params= untranslatable=vk13.synchronization2(VK_KHR_synchronization2)"),
        ])
        summary = runner.parse_log(log)
        self.assertEqual(summary["compat"]["refusals"],
                         ["vk13.synchronization2(VK_KHR_synchronization2)"])
        self.assertEqual(len(summary["compat"]["translations"]), 1)
        self.assertEqual(summary["features"], [{"struct": "vk12", "sType": "51", "mask": "0x1"}])
        self.assertEqual(summary["first_refusal"]["source"], "compat")
        self.assertEqual(summary["first_refusal"]["result"], -8)

    def test_feature_members_match_the_vulkan_headers(self):
        header = (ROOT / "third_party/vulkan-headers/include/vulkan/vulkan_core.h")
        if not header.is_file():
            self.skipTest("Vulkan headers not prepared")
        text = header.read_text()
        structs = {"core": "VkPhysicalDeviceFeatures", "vk11": "VkPhysicalDeviceVulkan11Features",
                   "vk12": "VkPhysicalDeviceVulkan12Features",
                   "vk13": "VkPhysicalDeviceVulkan13Features"}
        for key, name in structs.items():
            body = re.search(r"typedef struct %s \{(.*?)\} %s;" % (name, name), text, re.S).group(1)
            members = re.findall(r"VkBool32\s+(\w+);", body)
            self.assertEqual(members, runner.FEATURE_MEMBERS[key], key)

    def test_feature_level_gate_is_explicit(self):
        core = sum(1 << runner.FEATURE_MEMBERS["core"].index(name) for name in (
            "fullDrawIndexUint32 imageCubeArray independentBlend geometryShader tessellationShader "
            "sampleRateShading dualSrcBlend multiDrawIndirect drawIndirectFirstInstance depthClamp "
            "depthBiasClamp fillModeNonSolid multiViewport textureCompressionBC "
            "occlusionQueryPrecise fragmentStoresAndAtomics shaderImageGatherExtended "
            "shaderClipDistance shaderCullDistance").split())
        log = ps5log([
            IDENTITY,
            ("INFO", "DXVK_VK_EXTENSIONS call=vkEnumerateDeviceExtensionProperties part=0 count=1 "
                     "list=VK_KHR_sampler_mirror_clamp_to_edge:3"),
            ("INFO", f"DXVK_VK_FEATURES struct=core mask={core:#x}"),
            ("INFO", "DXVK_VK_FEATURES struct=vk13 sType=53 mask=0x0"),
            ("INFO", "DXVK_VK_FEATURES struct=vk12 sType=51 mask=0x1"),
        ])
        summary = runner.parse_log(log)
        gate = runner.feature_level_gate(summary, [
            "src/d3d11/d3d11_device.cpp:fl-gate-transform-feedback-relaxed"])
        self.assertTrue(gate["fl11_0"]["core.tessellationShader"])
        self.assertTrue(gate["baseline"]["vk12.samplerMirrorClampToEdge"])
        self.assertFalse(gate["baseline"]["xfb.transformFeedback"])
        self.assertEqual(gate["relaxed_by_diagnostic_patch"],
                         ["xfb.geometryStreams", "xfb.transformFeedback"])
        self.assertEqual(gate["failing"], ["vk13.shaderDemoteToHelperInvocation"])
        unpatched = runner.feature_level_gate(summary, [])
        self.assertEqual(unpatched["failing"], ["vk13.shaderDemoteToHelperInvocation",
                                                "xfb.transformFeedback", "xfb.geometryStreams"])
        relaxed = runner.feature_level_gate(summary, [
            "src/d3d11/d3d11_device.cpp:fl-gate-transform-feedback-relaxed",
            "src/d3d11/d3d11_device.cpp:fl-gate-demote-to-helper-relaxed"])
        self.assertEqual(relaxed["failing"], [])
        self.assertIsNone(runner.feature_level_gate(runner.parse_log(ps5log([IDENTITY])), []))

    def test_crash_without_bye_and_identity_mismatch(self):
        log = ps5log([
            ("MARK", IDENTITY[1].replace("eboot_sha256=ee", "eboot_sha256=00")),
            ("MARK", "DXVK_NATIVE_STAGE stage=d3d11.device state=begin"),
            ("ERR", "DXVK_NATIVE_CRASH signal=11 addr=0x0 stage=d3d11.device"),
            ("ERR", "DXVK_VK_REFUSAL call=vkQueueSubmit result=-4(VK_ERROR_DEVICE_LOST) stage=draw"),
        ]).rsplit("BYE", 1)[0]
        summary = runner.parse_log(log)
        self.assertTrue(summary["crash"].startswith("signal=11"))
        self.assertTrue(summary["gpu_hang_suspected"])
        receipt = runner.receipt_for(summary, ARTIFACT, None, "ff", False)
        self.assertEqual(receipt["outcome"], "crash")
        self.assertFalse(receipt["log_finalized"])
        self.assertEqual(receipt["identity_mismatches"], ["eboot_sha256: run=00 artifact=ee"])


if __name__ == "__main__":
    unittest.main()
