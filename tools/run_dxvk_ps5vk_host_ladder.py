#!/usr/bin/env python3
"""DIAGNOSTIC refusal ladder: pinned DXVK D3D11 against ps5vk host code.

Each rung runs one minimal offscreen D3D11 workload (feature level 11_0,
64x64 R8G8B8A8_UNORM render target, DXBC VS/PS, clear, Draw(3),
CopyResource, Map(READ), release) and records the first refusal. A rung adds
exactly one labelled diagnostic bypass to the previous one, so the next real
refusal is discovered on the host in seconds:

* a consumer-side interposer (tools/dxvk_host_ladder_shim.c, built only into
  the temporary directory as libvulkan.so.1) that forwards to a host build of
  the ps5vk SDK sources and may fake instance-level surface names, rewrite
  the instance apiVersion or alias core command names;
* a DIAGNOSTIC patched copy of the pinned DXVK that keeps adapters reporting
  Vulkan < 1.3 (only DxvkDeviceFilter::testAdapter changes);
* a host platform that reports the console's query tables
  (tools/dump_device_reporting.c) instead of the compute-only host platform.

Nothing here runs on a PS5 or implements WSI. A rung that uses a bypass or
the patched DXVK is diagnostic: it is never evidence for an unmodified DXVK
run or for a ps5vk capability. The receipt records the DXVK commit, the
sha256 of the diagnostic diff, library hashes and the ps5vk commit.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import run_dxvk_host_smoke as smoke
import run_dxvk_ps5vk_host_bootstrap as bootstrap


ROOT = Path(__file__).resolve().parents[1]
SHIM = ROOT / "tools/dxvk_host_ladder_shim.c"
WORKLOAD = ROOT / "tools/dxvk_host_ladder_workload.cpp"
LIB_SUFFIX = "so.0.20602"

# The only source change of the diagnostic DXVK build: one edit of the pinned
# DxvkDeviceFilter::testAdapter that keeps adapters below Vulkan 1.3.
DIAGNOSTIC_FILE = "src/dxvk/dxvk_device_filter.cpp"
DIAGNOSTIC_OLD = (
    '      Logger::warn(str::format("Skipping Vulkan ",\n'
    '        VK_API_VERSION_MAJOR(properties.apiVersion), ".",\n'
    '        VK_API_VERSION_MINOR(properties.apiVersion), " adapter: ",\n'
    '        properties.deviceName));\n'
    '      return false;\n')
DIAGNOSTIC_NEW = (
    '      // PS5VK DIAGNOSTIC: not upstream DXVK. Keep a Vulkan < 1.3 adapter\n'
    '      // so later refusals can be observed; never shipping evidence.\n'
    '      Logger::warn(str::format("PS5VK_DIAGNOSTIC keeping Vulkan ",\n'
    '        VK_API_VERSION_MAJOR(properties.apiVersion), ".",\n'
    '        VK_API_VERSION_MINOR(properties.apiVersion), " adapter: ",\n'
    '        properties.deviceName));\n')
DIAGNOSTIC_MARKER = "PS5VK_DIAGNOSTIC keeping Vulkan"


@dataclass(frozen=True)
class Rung:
    name: str
    dxvk: str               # "pinned" (unmodified) or "diagnostic" (patched)
    bypass: tuple[str, ...]  # shim knobs, see tools/dxvk_host_ladder_shim.c
    platform: str           # "host", "console-report" or "console-report-objects"
    purpose: str


def _ladder() -> tuple[Rung, ...]:
    """Each rung keeps everything below it and adds one bypass or change."""
    steps = (
        ("r0-unmodified", "pinned", None, "host",
         "unmodified DXVK, ps5vk host platform; the shim only traces"),
        ("r1-surface", "pinned", "surface", "host",
         "fake the host WSI's surface names that ps5vk does not report"),
        ("r2-api10", "pinned", "api10", "host",
         "also rewrite the instance apiVersion 1.3 -> 1.0"),
        ("r3-filter", "diagnostic", None, "host",
         "also keep the Vulkan 1.0 adapter (patched DXVK filter)"),
        ("r4-core-alias", "diagnostic", "core_alias", "host",
         "also resolve core 1.1/1.2 names to ps5vk KHR commands"),
        ("r5-console-report", "diagnostic", None, "console-report",
         "also report the console's queue/feature/format tables on the host"),
        ("r6-fake-features", "diagnostic", "fake_features", "console-report",
         "LIE: report the D3D11 FL11_0 feature bits and required device "
         "extensions, to record the exact vkCreateDevice request"),
        ("r7-strip-device", "diagnostic", "strip_device", "console-report",
         "also open the device with only what ps5vk reports, to trace DXVK's "
         "device bring-up"),
        ("r8-emulate-mem-reqs", "diagnostic", "emulate_mem_reqs", "console-report",
         "also answer the 1.1/1.3 memory-requirement queries through 1.0 commands"),
        ("r9-drop-mutable", "diagnostic", "drop_mutable", "console-report",
         "also drop MUTABLE_FORMAT and the UNORM/SRGB format list from images"),
        ("r10-host-image-objects", "diagnostic", None, "console-report-objects",
         "also enable the host device's graphics object model with the native "
         "image layout (no GPU compiler or queue backend exists on the host)"),
        ("r11-drop-storage-texel", "diagnostic", "drop_storage_texel",
         "console-report-objects", "also drop STORAGE_TEXEL_BUFFER usage from buffers"),
        ("r12-fake-coherent", "diagnostic", "fake_coherent", "console-report-objects",
         "LIE: also report HOST_COHERENT on the host-visible memory type"),
        ("r13-format-properties3", "diagnostic", "format_properties3",
         "console-report-objects",
         "also translate ps5vk's format features into VkFormatProperties3"),
    )
    rungs, bypass = [], ()
    for name, dxvk, added, platform, purpose in steps:
        bypass += (added,) if added else ()
        rungs.append(Rung(name, dxvk, bypass, platform, purpose))
    return tuple(rungs)


RUNGS = _ladder()

# The boundary each rung stopped at on the pinned DXVK and ps5vk main when
# the ladder was written. --check reports a rung whose refusal moved.
EXPECTED = {
    # ps5vk reports VK_KHR_surface and VK_KHR_display (the PS5 WSI adapter's
    # request) but no windowing-system surface, so the host SDL2 driver
    # cannot load Vulkan. This rung is a host-WSI artefact.
    "r0-unmodified": ("D3D11CreateDevice hr=0x80004005",
                      "SDL2 WSI: no VK_KHR_xlib_surface, instance extension query fails"),
    "r1-surface": ("D3D11CreateDevice hr=0x80004005",
                   "vkCreateInstance(apiVersion 1.3) -> VK_ERROR_INCOMPATIBLE_DRIVER; "
                   "the first refusal on the PS5 WSI route"),
    "r2-api10": ("D3D11CreateDevice hr=0x887a0002",
                 "DxvkDeviceFilter skips the Vulkan 1.0 adapter: no adapter"),
    "r3-filter": ("signal 11 after start",
                  "DxvkAdapter::queryDeviceInfo calls NULL vkGetPhysicalDeviceProperties2"),
    "r4-core-alias": ("D3D11CreateDevice hr=0x80070057",
                      "compute-only host platform lacks the D3D11 baseline features"),
    "r5-console-report": ("D3D11CreateDevice hr=0x80070057",
                          "no Vulkan1x feature structs, VK_EXT_transform_feedback"),
    "r6-fake-features": ("D3D11CreateDevice hr=0x80004005",
                         "vkCreateDevice -> VK_ERROR_EXTENSION_NOT_PRESENT"),
    "r7-strip-device": ("signal 11 after start",
                        "DxvkMemoryAllocator calls NULL vkGetDeviceBufferMemoryRequirements"),
    "r8-emulate-mem-reqs": ("CreateTexture2D.render_target hr=0x80070057",
                            "image format query refuses MUTABLE_FORMAT R8G8B8A8_UNORM"),
    "r9-drop-mutable": ("CreateTexture2D.render_target hr=0x80070057",
                        "host device has no graphics object model: vkCreateImage refused"),
    "r10-host-image-objects": ("CreateTexture2D.staging hr=0x80070057",
                               "vkCreateBuffer refuses STORAGE_TEXEL_BUFFER usage"),
    "r11-drop-storage-texel": ("signal 11 after CreateTexture2D.render_target",
                               "no HOST_VISIBLE|HOST_COHERENT memory type"),
    "r12-fake-coherent": ("CreateRenderTargetView hr=0x80070057",
                          "VkFormatProperties3 not filled: no format features"),
    "r13-format-properties3": ("signal 11 after CopyResource",
                               "first flush calls NULL vkCmdPipelineBarrier2"),
}

# Host device overlay for the "console-report-objects" platform: the console
# report tables plus a configure hook that turns on the graphics object model
# with the native image-requirement layout. It supplies no compiler and no
# queue backend, so it cannot execute GPU work.
OBJECTS_OVERLAY = r"""
#include "vk_internal.h"
VkResult ps5vk_ladder_report_query(struct ps5vk_platform *platform);
VkResult ps5vk_native_image_requirements(VkDevice, const VkImageCreateInfo *,
                                         VkMemoryRequirements *);
static void ladder_configure(VkDevice d)
{
    d->graphics_enabled = VK_TRUE;
    d->image_requirements = ps5vk_native_image_requirements;
}
VkResult ps5vk_platform_query(struct ps5vk_platform *platform)
{
    VkResult result = ps5vk_ladder_report_query(platform);
    if (result == VK_SUCCESS) platform->configure = ladder_configure;
    return result;
}
"""

STEP = re.compile(r"^LADDER_STEP (\S+) hr=0x([0-9a-f]{8})$", re.MULTILINE)
PIXEL = re.compile(r"^LADDER_PIXEL x=32 y=32 rgba=(\d+),(\d+),(\d+),(\d+)", re.MULTILINE)
# (0.25, 0.5, 0.75, 1.0) in UNORM8; 0.5 may round either way (+-1).
EXPECTED_PIXEL = (64, 128, 191, 255)


# ---- DXBC: the two shaders, assembled so no HLSL compiler is needed --------

TEMP, INPUT, OUTPUT = 0, 1, 2


def _op(opcode: int, body: list[int]) -> list[int]:
    return [opcode | (len(body) + 1) << 24] + body


def _dst(kind: int, index: int, mask: int = 0xF) -> list[int]:
    return [2 | mask << 4 | kind << 12 | 1 << 20, index]


def _src(kind: int, index: int, swizzle: tuple[int, ...] = (0, 1, 2, 3)) -> list[int]:
    packed = swizzle[0] | swizzle[1] << 2 | swizzle[2] << 4 | swizzle[3] << 6
    return [2 | 1 << 2 | packed << 4 | kind << 12 | 1 << 20, index]


def _imm(values: list[int]) -> list[int]:
    return [2 | 4 << 12] + [value & 0xFFFFFFFF for value in values]


def _float(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _signature(elements: list[tuple[str, int, int, int, int, int]]) -> bytes:
    header = 8 + 24 * len(elements)
    names, table = b"", []
    for name, system_value, component, register, mask, rw_mask in elements:
        table.append(struct.pack("<6I", header + len(names), 0, system_value,
                                 component, register, mask | rw_mask << 8))
        names += name.encode() + b"\0"
    data = struct.pack("<2I", len(elements), 8) + b"".join(table) + names
    return data + b"\0" * (-len(data) % 4)


_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xFFFFFFFF for i in range(64)]


def _md5_block(state: list[int], block: bytes) -> list[int]:
    words = struct.unpack("<16I", block)
    a, b, c, d = state
    for i in range(64):
        if i < 16:
            f, g = (b & c) | (~b & d), i
        elif i < 32:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | ~d), (7 * i) % 16
        f = (f + a + _K[i] + words[g]) & 0xFFFFFFFF
        a, d, c = d, c, b
        b = (b + ((f << _S[i]) | (f >> (32 - _S[i])))) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip(state, (a, b, c, d))]


def dxbc_checksum(blob: bytes) -> bytes:
    """The DXBC container checksum: MD5 with the container's own padding."""
    data = blob[20:]
    bits = len(data) * 8
    state = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    full = len(data) - len(data) % 64
    for offset in range(0, full, 64):
        state = _md5_block(state, data[offset:offset + 64])
    rest = data[full:]
    tail = struct.pack("<I", (bits >> 2) | 1)
    if len(rest) >= 56:
        state = _md5_block(state, (rest + b"\x80").ljust(64, b"\0"))
        block = struct.pack("<I", bits) + b"\0" * 56 + tail
    else:
        block = (struct.pack("<I", bits) + rest + b"\x80").ljust(60, b"\0") + tail
    return struct.pack("<4I", *_md5_block(state, block))


def _container(program_type: int, tokens: list[int],
               inputs: list[tuple[str, int, int, int, int, int]],
               outputs: list[tuple[str, int, int, int, int, int]]) -> bytes:
    program = [program_type << 16 | 0x40, len(tokens) + 2] + tokens
    chunks = [(b"ISGN", _signature(inputs)), (b"OSGN", _signature(outputs)),
              (b"SHDR", struct.pack(f"<{len(program)}I", *program))]
    start = 32 + 4 * len(chunks)
    body, offsets = b"", []
    for tag, data in chunks:
        offsets.append(start + len(body))
        body += tag + struct.pack("<I", len(data)) + data
    blob = (b"DXBC" + b"\0" * 16 + struct.pack("<3I", 1, start + len(body), len(chunks)) +
            struct.pack(f"<{len(chunks)}I", *offsets) + body)
    return blob[:4] + dxbc_checksum(blob) + blob[20:]


def vertex_shader() -> bytes:
    """vs_4_0: full-screen triangle from SV_VertexID."""
    x, y, any_x = 0x1, 0x2, (0, 0, 0, 0)
    tokens = (_op(96, _dst(INPUT, 0, x) + [6]) +                  # dcl_input_sgv v0.x, vertex_id
              _op(103, _dst(OUTPUT, 0) + [1]) +                   # dcl_output_siv o0, position
              _op(104, [1]) +                                     # dcl_temps 1
              _op(41, _dst(TEMP, 0, x) + _src(INPUT, 0, any_x) + _imm([1] * 4)) +  # ishl
              _op(1, _dst(TEMP, 0, x) + _src(TEMP, 0, any_x) + _imm([2] * 4)) +    # and
              _op(1, _dst(TEMP, 0, y) + _src(INPUT, 0, any_x) + _imm([2] * 4)) +   # and
              _op(86, _dst(TEMP, 0, x | y) + _src(TEMP, 0)) +                      # utof
              _op(50, _dst(OUTPUT, 0, x | y) + _src(TEMP, 0) +                     # mad
                  _imm([_float(2), _float(-2), 0, 0]) + _imm([_float(-1), _float(1), 0, 0])) +
              _op(54, _dst(OUTPUT, 0, 0xC) + _imm([0, 0, 0, _float(1)])) +         # mov o0.zw
              _op(62, []))                                                         # ret
    return _container(1, tokens, [("SV_VertexID", 6, 1, 0, 0x1, 0x1)],
                      [("SV_Position", 1, 3, 0, 0xF, 0x0)])


def pixel_shader() -> bytes:
    """ps_4_0: constant (0.25, 0.5, 0.75, 1.0) to SV_Target0."""
    rgba = [_float(value) for value in (0.25, 0.5, 0.75, 1.0)]
    tokens = (_op(101, _dst(OUTPUT, 0)) + _op(54, _dst(OUTPUT, 0) + _imm(rgba)) +
              _op(62, []))
    return _container(0, tokens, [], [("SV_Target", 0, 3, 0, 0xF, 0x0)])


def shader_header() -> str:
    def array(name: str, blob: bytes) -> str:
        body = ",".join(f"0x{byte:02x}" for byte in blob)
        return f"static const unsigned char {name}[{len(blob)}] = {{{body}}};\n"
    return ("// Generated by tools/run_dxvk_ps5vk_host_ladder.py\n" +
            array("kLadderVertexShader", vertex_shader()) +
            array("kLadderPixelShader", pixel_shader()))


# ---- run classification ----------------------------------------------------

def classify(returncode: int, stdout: str, stderr: str) -> dict[str, object]:
    """Reduce one run to its first refusal and supporting trace lines."""
    steps = [(name, code) for name, code in STEP.findall(stdout)]
    passed = [name for name, code in steps if code == "00000000"]
    failed = next(((name, code) for name, code in steps if code != "00000000"), None)
    dxvk_errors = [line.strip() for line in stderr.splitlines() if line.startswith("err:")]
    null_procs = sorted({line.split("name=", 1)[1].strip() for line in stderr.splitlines()
                         if line.startswith("LADDER_PROC_NULL") and "name=" in line})
    calls = [line.strip() for line in stderr.splitlines()
             if line.startswith(("LADDER_CALL", "LADDER_FAIL", "LADDER_STUB"))]
    refusals = [line for line in calls if line.startswith("LADDER_FAIL") or
                re.search(r"result=-?[1-9]\d*$", line)]
    pixel = PIXEL.search(stdout)
    result: dict[str, object] = {
        "returncode": returncode,
        "signal": -returncode if returncode < 0 else None,
        "steps_passed": passed,
        "first_failed_step": ({"step": failed[0], "hresult": "0x" + failed[1]}
                              if failed else None),
        "first_dxvk_error": dxvk_errors[0] if dxvk_errors else None,
        "dxvk_errors": dxvk_errors[:8],
        "vulkan_refusals": refusals[:8],
        "null_entry_points": null_procs,
        "creation_trace": [line for line in calls if "vkCreate" in line][:12],
        "diagnostic_filter_used": DIAGNOSTIC_MARKER in stderr,
        "stripped": sorted({line.split(" ", 1)[1].strip() for line in stderr.splitlines()
                            if line.startswith(("LADDER_STRIP", "LADDER_TRANSLATE"))}),
    }
    for key, label in (("checked_missing", "features_missing_checked_by_d3d11"),
                       ("forced_missing", "features_missing_forced_at_create")):
        # The first query is DXVK's adapter query, before any faking.
        found = next((line.split("=", 1)[1].strip() for line in stderr.splitlines()
                      if line.startswith(f"LADDER_FEATURES {key}=")), None)
        if found is not None:
            result[label] = [] if found == "none" else found.split(",")
    if pixel:
        result["pixel"] = [int(value) for value in pixel.groups()]
    if failed:
        result["first_refusal"] = f"{failed[0]} hr=0x{failed[1]}"
    elif returncode < 0:
        result["first_refusal"] = f"signal {-returncode} after {passed[-1] if passed else 'start'}"
    elif pixel and any(abs(got - want) > 1 for got, want in
                       zip(result["pixel"], EXPECTED_PIXEL)):  # type: ignore[call-overload]
        result["first_refusal"] = f"wrong pixel {result['pixel']}"
    elif returncode == 0 and "Unmap" in passed:
        result["first_refusal"] = None
    else:
        result["first_refusal"] = f"exit {returncode} after {passed[-1] if passed else 'start'}"
    return result


# ---- builds ----------------------------------------------------------------

def dxvk_libraries(build: Path) -> tuple[Path, Path]:
    d3d11 = build / f"src/d3d11/libdxvk_d3d11.{LIB_SUFFIX}"
    dxgi = build / f"src/dxgi/libdxvk_dxgi.{LIB_SUFFIX}"
    if not d3d11.is_file() or not dxgi.is_file():
        raise ValueError(f"DXVK D3D11/DXGI native libraries missing in {build}")
    return d3d11, dxgi


def patched_source(original: str) -> str:
    if original.count(DIAGNOSTIC_OLD) != 1:
        raise ValueError(f"pinned {DIAGNOSTIC_FILE} does not match the diagnostic edit")
    return original.replace(DIAGNOSTIC_OLD, DIAGNOSTIC_NEW)


def check_diagnostic_tree(tree: Path, pinned: str) -> str:
    """The diagnostic tree must be the pinned commit plus exactly our edit.

    Returns the sha256 of its `git diff`, the identity of the patch."""
    commit = smoke.run(["git", "-C", str(tree), "rev-parse", "HEAD"]).stdout.strip()
    if commit != pinned:
        raise ValueError(f"diagnostic DXVK tree is at {commit}, not {pinned}")
    changed = smoke.run(["git", "-C", str(tree), "diff", "--name-only"]).stdout.split()
    original = smoke.run(["git", "-C", str(tree), "show", f"HEAD:{DIAGNOSTIC_FILE}"]).stdout
    if changed != [DIAGNOSTIC_FILE] or \
            (tree / DIAGNOSTIC_FILE).read_text() != patched_source(original):
        raise ValueError("diagnostic DXVK tree does not carry exactly the ladder edit")
    diff = smoke.run(["git", "-C", str(tree), "diff", "--", DIAGNOSTIC_FILE]).stdout
    return hashlib.sha256(diff.encode()).hexdigest()


def prepare_diagnostic(source: Path, tree: Path, meson: str) -> None:
    """Clone the pinned checkout, apply the patch and build D3D11/DXGI."""
    if not tree.exists():
        smoke.run(["git", "clone", "-q", "--no-checkout", str(source), str(tree)])
        commit = smoke.run(["git", "-C", str(source), "rev-parse", "HEAD"]).stdout.strip()
        smoke.run(["git", "-C", str(tree), "checkout", "-q", commit])
        status = smoke.run(["git", "-C", str(source), "submodule", "status"]).stdout
        for line in status.splitlines():
            path = line.split()[1]
            smoke.run(["git", "-C", str(tree), "config", f"submodule.{path}.url",
                       str(source / path)])
        smoke.run(["git", "-C", str(tree), "-c", "protocol.file.allow=always",
                   "submodule", "update", "--init", "-q"])
        target = tree / DIAGNOSTIC_FILE
        target.write_text(patched_source(target.read_text()))
    build = tree / "build-diag"
    if not (build / "build.ninja").is_file():
        smoke.run([meson, "setup", str(build), "-Denable_d3d8=false",
                   "-Denable_d3d9=false", "-Denable_d3d10=false", "-Denable_d3d11=true",
                   "-Denable_dxgi=true", "-Dnative_sdl2=enabled", "-Dnative_sdl3=disabled",
                   "-Dnative_glfw=disabled", "-Dbuildtype=debugoptimized"], cwd=tree)
    smoke.run(["ninja", "-C", str(build), f"src/d3d11/libdxvk_d3d11.{LIB_SUFFIX}",
               f"src/dxgi/libdxvk_dxgi.{LIB_SUFFIX}"])


def build_ps5vk(directory: Path, platform: str) -> Path:
    """A host build of the SDK host sources, optionally reporting console tables."""
    sources = [(source, []) for source in bootstrap.host_sources()]
    if platform.startswith("console-report"):
        # tools/dump_device_reporting.c installs the platform whose tables
        # come from the console initializer; its main() is renamed away.
        rename = ["-Dmain=ps5vk_ladder_report_main"]
        if platform == "console-report-objects":
            overlay = directory / "objects_overlay.c"
            overlay.write_text(OBJECTS_OVERLAY)
            rename.append("-Dps5vk_platform_query=ps5vk_ladder_report_query")
            sources.append((str(overlay), []))
        sources += [("tools/dump_device_reporting.c", rename),
                    ("native/image_ps5.c", []), ("src/depth_layout.c", [])]
    def compile_one(index: int) -> str:
        source, extra = sources[index]
        obj = directory / f"{platform}-{index}.o"
        smoke.run(["cc", "-std=c11", "-O1", "-g", "-fPIC",
                   f"-I{ROOT / 'third_party/vulkan-headers/include'}",
                   f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'native'}",
                   *extra, "-c", str(ROOT / source), "-o", str(obj)])
        return str(obj)

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        objects = list(pool.map(compile_one, range(len(sources))))
    library = directory / f"libps5vk-{platform}.so"
    smoke.run(["cc", "-shared", "-Wl,--no-undefined", "-Wl,-Bsymbolic", *objects,
               "-o", str(library)])
    return library


def build_shim(directory: Path) -> Path:
    shim_dir = directory / "shim"
    shim_dir.mkdir()
    shim = shim_dir / "libvulkan.so.1"
    smoke.run(["cc", "-std=c11", "-O1", "-g", "-fPIC", "-shared", "-fvisibility=hidden",
               "-Wall", "-Wextra", "-Werror",
               f"-I{ROOT / 'third_party/vulkan-headers/include'}",
               "-Wl,-soname,libvulkan.so.1", str(SHIM), "-ldl", "-o", str(shim)])
    (shim_dir / "libvulkan.so").symlink_to(shim.name)
    return shim


def build_workload(directory: Path, dxvk: Path, d3d11: Path, dxgi: Path, name: str) -> Path:
    (directory / "ladder_shaders.h").write_text(shader_header())
    binary = directory / f"workload-{name}"
    smoke.run(["c++", "-std=c++17", "-O1", "-g", f"-I{directory}",
               f"-I{dxvk / 'include/native/directx'}", f"-I{dxvk / 'include/native/windows'}",
               *smoke.run(["sdl2-config", "--cflags"]).stdout.split(), str(WORKLOAD),
               str(d3d11), *smoke.run(["sdl2-config", "--libs"]).stdout.split(),
               f"-Wl,-rpath,{d3d11.parent}", f"-Wl,-rpath,{dxgi.parent}", "-o", str(binary)])
    return binary


def run_workload(binary: Path, shim_dir: Path | None, real: Path | None,
                 bypass: tuple[str, ...], timeout: int) -> dict[str, object]:
    env = dict(os.environ, DXVK_WSI_DRIVER="SDL2", DXVK_LOG_LEVEL="info",
               DXVK_LOG_PATH="none", DXVK_STATE_CACHE="0")
    env.pop("PS5VK_LADDER_BYPASS", None)
    if shim_dir is not None and real is not None:
        env["LD_LIBRARY_PATH"] = str(shim_dir) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        env["PS5VK_LADDER_REAL"] = str(real)
        env["PS5VK_LADDER_BYPASS"] = ",".join(bypass)
    try:
        run = subprocess.run([str(binary)], text=True, capture_output=True, env=env,
                             timeout=timeout, check=False)
        returncode, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        def text(data: object) -> str:
            return data.decode(errors="replace") if isinstance(data, bytes) else str(data or "")
        returncode, stdout, stderr = -9, text(error.stdout), text(error.stderr) + "\nLADDER_TIMEOUT"
    result = classify(returncode, stdout, stderr)
    result["log_tail"] = (stderr + stdout)[-1500:]
    gdb = shutil.which("gdb")
    if returncode < 0 and gdb:
        # Name the crash site: DXVK calls unchecked NULL entry points.
        trace = subprocess.run([gdb, "-q", "-batch", "-ex", "run", "-ex", "bt 8",
                                "--args", str(binary)], text=True, capture_output=True,
                               env=env, timeout=timeout * 2, check=False)
        result["crash_frames"] = [re.sub(r"\s+", " ", line.strip())[:200]
                                  for line in trace.stdout.splitlines()
                                  if re.match(r"#\d+ ", line)][:8]
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dxvk-dir", type=Path, default=ROOT / "third_party/dxvk-v2.6.2",
                        help="pinned, unmodified DXVK checkout with a native build")
    parser.add_argument("--build-dir", type=Path, help="default: <dxvk-dir>/build-native")
    parser.add_argument("--diagnostic-dir", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2-diag",
                        help="patched DIAGNOSTIC DXVK clone (ignored tree)")
    parser.add_argument("--prepare-diagnostic", action="store_true",
                        help="clone, patch and build the diagnostic DXVK if needed")
    parser.add_argument("--meson", default=shutil.which("meson") or "meson")
    parser.add_argument("--rung", action="append", choices=[r.name for r in RUNGS],
                        help="run only these rungs (default: all)")
    parser.add_argument("--no-host-control", action="store_true",
                        help="skip the workload control on the host's own Vulkan driver")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--output", type=Path, help="write the receipt as JSON")
    parser.add_argument("--check", action="store_true",
                        help="fail when a rung's first refusal moved from EXPECTED")
    parser.add_argument("--print-shaders", action="store_true",
                        help="print the generated DXBC header and exit")
    args = parser.parse_args()
    if args.print_shaders:
        sys.stdout.write(shader_header())
        return 0
    dxvk = args.dxvk_dir.resolve()
    build = (args.build_dir or dxvk / "build-native").resolve()
    rungs = [rung for rung in RUNGS if not args.rung or rung.name in args.rung]
    try:
        pinned = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                            .read_text())["source"]["commit"]
        commit = smoke.run(["git", "-C", str(dxvk), "rev-parse", "HEAD"]).stdout.strip()
        if commit != pinned:
            raise ValueError(f"DXVK source commit {commit} differs from pinned {pinned}")
        if smoke.run(["git", "-C", str(dxvk), "status", "--porcelain", "--", "src"]).stdout:
            raise ValueError("the pinned DXVK tree has local source changes")
        libraries = {"pinned": dxvk_libraries(build)}
        receipt: dict[str, object] = {
            "profile": "dxvk-v262-ps5vk-host-ladder",
            "evidence": ("DIAGNOSTIC host ladder: consumer-side shim and patched DXVK "
                         "rungs are not unmodified-DXVK evidence, not a ps5vk capability "
                         "and not PS5 execution"),
            "dxvk_commit": commit,
            "ps5vk_source_commit": smoke.run(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"]).stdout.strip(),
            "libraries": {"pinned": {"d3d11_sha256": smoke.sha256(libraries["pinned"][0]),
                                     "dxgi_sha256": smoke.sha256(libraries["pinned"][1])}},
            "shim_source_sha256": smoke.sha256(SHIM),
            "workload_source_sha256": smoke.sha256(WORKLOAD),
            "dxbc_sha256": {"vs": hashlib.sha256(vertex_shader()).hexdigest(),
                            "ps": hashlib.sha256(pixel_shader()).hexdigest()},
            "rungs": [],
        }
        diagnostic_dir = args.diagnostic_dir.resolve()
        if any(rung.dxvk == "diagnostic" for rung in rungs):
            if args.prepare_diagnostic:
                prepare_diagnostic(dxvk, diagnostic_dir, args.meson)
            receipt["diagnostic_patch_sha256"] = check_diagnostic_tree(diagnostic_dir, pinned)
            receipt["diagnostic_patch"] = ["src/dxvk/dxvk_device_filter.cpp: keep adapters "
                                           "reporting Vulkan < 1.3 (testAdapter)"]
            libraries["diagnostic"] = dxvk_libraries(diagnostic_dir / "build-diag")
            receipt["libraries"]["diagnostic"] = {  # type: ignore[index]
                "d3d11_sha256": smoke.sha256(libraries["diagnostic"][0]),
                "dxgi_sha256": smoke.sha256(libraries["diagnostic"][1])}
        with tempfile.TemporaryDirectory(prefix="ps5vk-dxvk-ladder-") as temp:
            directory = Path(temp)
            shim = build_shim(directory)
            receipt["shim_sha256"] = smoke.sha256(shim)
            binaries = {variant: build_workload(directory, dxvk if variant == "pinned"
                                                else diagnostic_dir, *libs, variant)
                        for variant, libs in libraries.items()}
            loaders: dict[str, Path] = {}
            if not args.no_host_control:
                control = run_workload(binaries["pinned"], None, None, (), args.timeout)
                receipt["host_driver_control"] = {
                    "purpose": "unmodified DXVK on the host's own Vulkan driver: validates "
                               "the workload and DXBC, says nothing about ps5vk",
                    **control}
            for rung in rungs:
                if rung.platform not in loaders:
                    loaders[rung.platform] = build_ps5vk(directory, rung.platform)
                    receipt[f"ps5vk_{rung.platform}_library_sha256"] = smoke.sha256(
                        loaders[rung.platform])
                result = run_workload(binaries[rung.dxvk], shim.parent,
                                      loaders[rung.platform], rung.bypass, args.timeout)
                expected, cause = EXPECTED[rung.name]
                receipt["rungs"].append({  # type: ignore[union-attr]
                    "name": rung.name, "dxvk": ("unmodified" if rung.dxvk == "pinned"
                                                else "DIAGNOSTIC-patched"),
                    "bypass": list(rung.bypass), "platform": rung.platform,
                    "purpose": rung.purpose, "expected_refusal": expected,
                    "expected_cause": cause,
                    "moved": result["first_refusal"] != expected, **result})
        compact = {key: value for key, value in receipt.items() if key != "rungs"}
        compact["rungs"] = [{key: rung[key] for key in ("name", "dxvk", "bypass", "platform",
                                                          "first_refusal", "expected_cause",
                                                          "moved")}
                            for rung in receipt["rungs"]]  # type: ignore[union-attr]
        if "host_driver_control" in receipt:
            control = receipt["host_driver_control"]
            compact["host_driver_control"] = {key: control[key]  # type: ignore[index]
                                              for key in ("first_refusal", "pixel")
                                              if key in control}  # type: ignore[operator]
        print(json.dumps(compact, indent=2, sort_keys=True))
        if args.output:
            args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        moved = [rung["name"] for rung in receipt["rungs"]  # type: ignore[union-attr]
                 if rung["moved"]]
        if moved:
            print("refusal moved on: " + ", ".join(moved), file=sys.stderr)
            if args.check:
                return 1
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK ps5vk host ladder failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print((error.stdout or "")[-2000:], file=sys.stderr)
            print((error.stderr or "")[-2000:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
