#!/usr/bin/env python3
"""Build the native PS5 payload that runs pinned DXVK 2.6.2 D3D11/DXGI on ps5vk.

The DXVK objects are cross-compiled from the pinned native Meson build (as in
build_dxvk_ps5_cross_probe.py) and linked statically, with the public ps5vk
SDK, into one SDK-linked eboot. Each variant is its own executable:

* ``unmodified``: the pinned DXVK source. The only overlay is the PS5 WSI
  bootstrap registration shared with the cross-link probe.
* ``diagnostic-version-filter``: DIAGNOSTIC. Identical, plus one patched copy
  of ``src/dxvk/dxvk_device_filter.cpp`` that logs instead of skipping an
  adapter reporting Vulkan < 1.3. Never report it as unmodified DXVK.
* ``diagnostic-compat``: DIAGNOSTIC. The version-filter patch and the payload translation layer
  (examples/dxvk_native/compat_layer.cpp) between DXVK's Vulkan 1.1/1.2/1.3
  aggregate structures and ps5vk's per-extension structures. The layer only
  copies what ps5vk reports and refuses, by name, any enabled feature without a
  ps5vk route.
* ``diagnostic-compat-fl-relaxed``: DIAGNOSTIC. ``diagnostic-compat`` whose
  feature-level gate requests transform feedback and demote-to-helper only when reported, to
  observe the refusals beyond that gate.

Why static: DXVK's native loader dlopen()s "libvulkan.so" and dlsym()s
vkGetInstanceProcAddr. The payload runtime only loads signed system modules,
while the lab packages one self-contained eboot. Linking DXVK and ps5vk into
that eboot, with dlopen/dlsym provided by the payload itself, binds DXVK's
vkGetInstanceProcAddr to ps5vk without a loader or an unsigned module.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_ps5_cross_probe import (  # noqa: E402
    COMPAT_HEADER, EXCLUDED_SOURCES, NO_EDID, compile_entry, wsi_entry)
from build_sdk import get_ps5_toolchain  # noqa: E402
from cts_heap_parameters import use_application_heap  # noqa: E402
from lab import lab_root  # noqa: E402

PROFILE = "dxvk-v262-ps5-native"
SOURCE = ROOT / "examples/dxvk_native"
VARIANTS = {
    "unmodified": {"diagnostic": False, "patches": ()},
    "diagnostic-version-filter": {
        "diagnostic": True,
        "patches": ("src/dxvk/dxvk_device_filter.cpp:bypass-apiVersion-1.3-filter",),
    },
    "diagnostic-compat": {
        "diagnostic": True,
        "compat_layer": True,
        "patches": ("src/dxvk/dxvk_device_filter.cpp:bypass-apiVersion-1.3-filter",
                    "payload:compat-translation-layer-v1"),
    },
    "diagnostic-compat-fl-relaxed": {
        "diagnostic": True,
        "compat_layer": True,
        "relax_demote": True,
        "patches": ("src/dxvk/dxvk_device_filter.cpp:bypass-apiVersion-1.3-filter",
                    "src/d3d11/d3d11_device.cpp:fl-gate-transform-feedback-relaxed",
                    "src/d3d11/d3d11_device.cpp:fl-gate-demote-to-helper-relaxed",
                    "payload:compat-translation-layer-v1"),
    },
}
# Overlays needed on every variant to build DXVK for the PS5 at all. They do
# not change DXVK's D3D11, DXGI or Vulkan code paths.
PLATFORM_OVERLAYS = (
    "src/wsi/wsi_platform.cpp:register-Ps5WSI-bootstrap",
    "src/wsi/wsi_edid.cpp:replaced-by-no-colorimetry-stub",
    "src/wsi/sdl2:excluded",
    "src/dxgi/dxgi_main.cpp:Logger::s_instance-localized-for-static-link",
    "src/d3d11:private-dxgi_format.cpp-copy-linked-once-from-dxgi",
)
LOGGER_INSTANCE = "_ZN4dxvk6Logger10s_instanceE"
LINK_WRAPS = ("getenv",
              "ps5log_line", "ps5log_printf", "ps5log_hex64")
# The payload defines the dl* entry points itself (vk_trace.cpp), so neither
# libc's dlfcn member nor the system loader is linked.
PAYLOAD_DL = ("dlopen", "dlsym", "dlclose", "dlerror", "dladdr")
REQUIRED_DEFINED = ("vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "D3D11CreateDevice",
                    "CreateDXGIFactory1", "__wrap_getenv", *PAYLOAD_DL)
FILTER_BLOCK = """    if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      Logger::warn(str::format("Skipping Vulkan ",
        VK_API_VERSION_MAJOR(properties.apiVersion), ".",
        VK_API_VERSION_MINOR(properties.apiVersion), " adapter: ",
        properties.deviceName));
      return false;
    }
"""
FILTER_DIAGNOSTIC = """    if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      // DIAGNOSTIC BUILD (ps5vk dxvk-native): the Vulkan 1.3 adapter filter
      // is bypassed to observe later refusals. Not an unmodified DXVK run.
      Logger::warn(str::format("DIAGNOSTIC: not skipping Vulkan ",
        VK_API_VERSION_MAJOR(properties.apiVersion), ".",
        VK_API_VERSION_MINOR(properties.apiVersion), " adapter: ",
        properties.deviceName));
    }
"""


# The measurement switches the native DXVK DIAGNOSTIC runs use: every
# default-off route DXVK 2.6.2 reaches before its first readback. Only the
# ones tools/build_sdk.py knows are applied; the rest are recorded as absent.
DXVK_DIAGNOSTIC_SWITCHES = (
    "PS5VK_MAINTENANCE4_DIAGNOSTIC",
)


def diagnostic_integration_switches(build_sdk_source: str) -> tuple[list[str], list[str]]:
    """Split the DXVK measurement switches into those the SDK build knows
    and those it does not (not merged yet)."""
    known = set(re.findall(r'"(PS5VK_[A-Z0-9_]+)"', build_sdk_source))
    present = [name for name in DXVK_DIAGNOSTIC_SWITCHES if name in known]
    absent = [name for name in DXVK_DIAGNOSTIC_SWITCHES if name not in known]
    return present, absent


def sdk_build_environment(environ: dict, sdk: Path, switches: list[str]) -> dict:
    """Only explicit, recorded profile overrides may affect the SDK build."""
    env = {key: value for key, value in environ.items() if not key.startswith("PS5VK_")}
    env.update(PS5_PAYLOAD_SDK=str(sdk), **{name: "1" for name in switches})
    return env


def run(argv: list, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(arg) for arg in argv], check=True, text=True,
                          capture_output=True, **kwargs)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


XFB_BLOCK = """    enabled.extTransformFeedback.transformFeedback                = VK_TRUE;
    enabled.extTransformFeedback.geometryStreams                  = VK_TRUE;
"""
XFB_DIAGNOSTIC = """    // DIAGNOSTIC BUILD (ps5vk dxvk-native): transform feedback is requested
    // only when reported, so the feature-level gate can be passed without it.
    enabled.extTransformFeedback.transformFeedback                = supported.extTransformFeedback.transformFeedback;
    enabled.extTransformFeedback.geometryStreams                  = supported.extTransformFeedback.geometryStreams;
"""


DEMOTE_BLOCK = """    enabled.vk13.shaderDemoteToHelperInvocation                   = VK_TRUE;
"""
DEMOTE_DIAGNOSTIC = """    // DIAGNOSTIC BUILD (ps5vk dxvk-native): demote-to-helper is requested
    // only when reported, so the feature-level gate can be passed without it.
    enabled.vk13.shaderDemoteToHelperInvocation                   = supported.vk13.shaderDemoteToHelperInvocation;
"""


def patch_feature_level_xfb(source: str, relax_demote: bool = False) -> str:
    """Return the DIAGNOSTIC D3D11 feature gate; each pinned block must match once."""
    if source.count(XFB_BLOCK) != 1:
        raise ValueError("D3D11 feature-level transform feedback terms changed")
    source = source.replace(XFB_BLOCK, XFB_DIAGNOSTIC)
    if relax_demote:
        if source.count(DEMOTE_BLOCK) != 1:
            raise ValueError("D3D11 feature-level demote-to-helper term changed")
        source = source.replace(DEMOTE_BLOCK, DEMOTE_DIAGNOSTIC)
    return source


def patch_device_filter(source: str) -> str:
    """Return the DIAGNOSTIC device filter; the pinned block must match once."""
    if source.count(FILTER_BLOCK) != 1:
        raise ValueError("DXVK device filter no longer matches the pinned 1.3 check")
    return source.replace(FILTER_BLOCK, FILTER_DIAGNOSTIC)


def c_string(value: str) -> str:
    if any(ch in value for ch in '"\\\n') or not value.isprintable():
        raise ValueError(f"identity value is not a plain string: {value!r}")
    return '"' + value + '"'


def identity_header(variant: str, dxvk_commit: str, ps5vk_commit: str,
                    ps5vk_dirty: bool, vs_sha: str, ps_sha: str,
                    integration: str = "none", sdk_switches: str = "none") -> str:
    if variant not in VARIANTS:
        raise ValueError(f"unknown variant {variant}")
    config = VARIANTS[variant]
    patches = ",".join(config["patches"]) or "none"
    return "\n".join((
        "/* Generated by tools/build_dxvk_ps5_native.py. */",
        "#ifndef DXVK_NATIVE_BUILD_IDENTITY_H",
        "#define DXVK_NATIVE_BUILD_IDENTITY_H",
        f"#define DXVK_NATIVE_VARIANT {c_string(variant)}",
        f"#define DXVK_NATIVE_DIAGNOSTIC {1 if config['diagnostic'] else 0}",
        f"#define DXVK_NATIVE_COMPAT_LAYER {1 if config.get('compat_layer') else 0}",
        f"#define DXVK_NATIVE_DXVK_COMMIT {c_string(dxvk_commit)}",
        f"#define DXVK_NATIVE_PS5VK_COMMIT {c_string(ps5vk_commit)}",
        f"#define DXVK_NATIVE_PS5VK_DIRTY {1 if ps5vk_dirty else 0}",
        f"#define DXVK_NATIVE_PATCHES {c_string(patches)}",
        f"#define DXVK_NATIVE_VS_SHA256_BUILD {c_string(vs_sha)}",
        f"#define DXVK_NATIVE_PS_SHA256_BUILD {c_string(ps_sha)}",
        f"#define DXVK_NATIVE_INTEGRATION {c_string(integration)}",
        f"#define DXVK_NATIVE_SDK_SWITCHES {c_string(sdk_switches)}",
        "#endif", ""))


def shader_header(vs: bytes, ps: bytes) -> str:
    lines = [
        "/* Generated DXBC for the DXVK native workload. Do not edit.",
        " *",
        " * Sources: pattern.vs.hlsl (vs_4_0) and pattern.ps.hlsl (ps_4_0), entry point",
        ' * "main", D3DCompile flags 0. Compiler: the vkd3d-shader HLSL front end in',
        " * Wine 8.14 builtin d3dcompiler_47 (the host vkd3d-compiler 1.2 has no HLSL",
        " * source type). Regenerate with tools/build_dxvk_ps5_native.py",
        " * --emit-shader-header VS.dxbc PS.dxbc and record the new digests. */",
        "#ifndef DXVK_NATIVE_SHADERS_H", "#define DXVK_NATIVE_SHADERS_H", ""]
    for name, blob in (("vs", vs), ("ps", ps)):
        if blob[:4] != b"DXBC":
            raise ValueError(f"{name} blob is not DXBC")
        lines.append(f'#define DXVK_NATIVE_{name.upper()}_SHA256 "{sha256_bytes(blob)}"')
        lines.append(f"static const unsigned char dxvk_native_{name}_dxbc[{len(blob)}] = {{")
        for offset in range(0, len(blob), 16):
            lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in blob[offset:offset + 16]) + ",")
        lines += ["};", ""]
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def embedded_shaders(header: str) -> dict[str, bytes]:
    """Parse the committed header and check each array against its digest."""
    blobs = {}
    for name in ("vs", "ps"):
        digest = re.search(rf'#define DXVK_NATIVE_{name.upper()}_SHA256 "([0-9a-f]{{64}})"', header)
        array = re.search(rf"dxvk_native_{name}_dxbc\[(\d+)\] = \{{([^}}]*)\}}", header)
        if not digest or not array:
            raise ValueError(f"{name} shader missing from header")
        blob = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", array.group(2)))
        if len(blob) != int(array.group(1)) or sha256_bytes(blob) != digest.group(1):
            raise ValueError(f"{name} shader bytes do not match their recorded digest")
        blobs[name] = blob
    return blobs


def dxvk_identity(dxvk: Path) -> str:
    expected = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                          .read_text())["source"]["commit"]
    actual = run(["git", "-C", dxvk, "rev-parse", "HEAD"]).stdout.strip()
    if actual != expected:
        raise ValueError(f"DXVK source commit {actual} differs from pinned {expected}")
    if run(["git", "-C", dxvk, "status", "--porcelain", "--untracked-files=no"]).stdout.strip():
        raise ValueError("DXVK tracked source has local changes")
    return actual


def select_entries(build: Path) -> tuple[list, dict]:
    entries = json.loads(run(["ninja", "-C", build, "-t", "compdb",
                              "c_COMPILER", "cpp_COMPILER"]).stdout)
    if not all("-DDXVK_WSI_SDL2" in entry["command"] and
               "-DDXVK_WSI_SDL3" not in entry["command"] and
               "-DDXVK_WSI_GLFW" not in entry["command"]
               for entry in entries if entry["output"].startswith("src/")):
        raise ValueError("configure DXVK native with SDL2 alone for this build")
    # d3d11.dll compiles its own private copy of dxgi_format.cpp; one static
    # image links DXGI's copy only.
    selected = [entry for entry in entries
                if not entry["output"].startswith("subprojects/libdisplay-info/")
                and not entry["file"].endswith(EXCLUDED_SOURCES)
                and not (entry["output"].startswith("src/d3d11/") and
                         entry["file"].endswith("/dxgi/dxgi_format.cpp"))]
    platform = [entry for entry in entries if entry["file"].endswith("/wsi_platform.cpp")]
    if len(platform) != 1:
        raise ValueError("DXVK WSI platform source not found uniquely")
    return selected, platform[0]


def cached_compile(entry: dict, build: Path, cache: Path, cc: Path, cxx: Path,
                   compat: Path) -> tuple[str, Path]:
    """Compile one DXVK unit; the cache key is the command and the source."""
    key = sha256_bytes((entry["command"] + "\0" + entry["file"] + "\0" +
                        sha256_file(build / entry["file"])).encode())
    out = cache / key
    obj = out / "000.o"
    group = "/".join(entry["output"].split("/")[:2])
    if not obj.is_file():
        out.mkdir(parents=True, exist_ok=True)
        compile_entry(0, entry, build, out, cc, cxx, compat)
    return group, obj


def overlay_entry(original: dict, source: Path, extra_include: Path | None = None) -> dict:
    entry = wsi_entry(original, source)
    if extra_include:
        args = shlex.split(entry["command"])
        args.insert(1, f"-I{extra_include}")
        entry["command"] = shlex.join(args)
    return entry


def compile_dxvk(variant: str, dxvk: Path, build: Path, work: Path, cc: Path, cxx: Path,
                 jobs: int) -> dict[str, list[Path]]:
    selected, platform = select_entries(build)
    cache = ROOT / "build/dxvk-ps5-native/object-cache"
    compat = work / "ps5_compat.h"
    compat.write_text(COMPAT_HEADER)
    overlays = work / "overlays"
    overlays.mkdir(exist_ok=True)
    units = []
    for entry in selected:
        if VARIANTS[variant]["diagnostic"] and entry["file"].endswith("/dxvk_device_filter.cpp"):
            patched = overlays / "dxvk_device_filter.cpp"
            patched.write_text(patch_device_filter((build / entry["file"]).read_text()))
            entry = overlay_entry(entry, patched, dxvk / "src/dxvk")
        if (VARIANTS[variant].get("relax_demote") and
                entry["file"].endswith("/d3d11/d3d11_device.cpp")):
            patched = overlays / "d3d11_device.cpp"
            patched.write_text(patch_feature_level_xfb(
                (build / entry["file"]).read_text(),
                bool(VARIANTS[variant].get("relax_demote"))))
            entry = overlay_entry(entry, patched, dxvk / "src/d3d11")
        units.append(entry)
    wsi = (dxvk / "src/wsi/wsi_platform.cpp").read_text()
    anchor = "static const WsiBootstrap *wsiBootstrap[] = {"
    if wsi.count(anchor) != 1:
        raise ValueError("DXVK WSI bootstrap location changed")
    registered = overlays / "wsi_platform_ps5.cpp"
    registered.write_text(wsi.replace(anchor, "extern WsiBootstrap Ps5WSI;\n  " + anchor +
                                      "\n    &Ps5WSI,").replace('#include "../util/', '#include "'))
    units.append(overlay_entry(platform, registered))
    units.append(overlay_entry(platform, ROOT / "tools/dxvk_ps5_wsi.cpp"))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda unit: cached_compile(unit, build, cache, cc, cxx, compat),
                                units))
    no_edid = work / "no_edid.cpp"
    no_edid.write_text(NO_EDID)
    no_edid_obj = work / "no_edid.o"
    run([cxx, "-std=c++17", "-O0", "-fPIC", f"-I{dxvk / 'src/wsi'}", "-c", no_edid,
         "-o", no_edid_obj])
    grouped: dict[str, list[Path]] = {}
    for group, obj in results:
        grouped.setdefault(group, []).append(obj)
    grouped["src/wsi"].append(no_edid_obj)
    # d3d11.dll and dxgi.dll each define dxvk::Logger::s_instance. In one
    # static image DXGI keeps a private, unused copy; both log to D3D11's.
    dxgi_main = [obj for obj, entry in zip([r[1] for r in results], units)
                 if entry["file"].endswith("/dxgi_main.cpp")]
    if len(dxgi_main) != 1:
        raise ValueError("DXGI main object not found uniquely")
    localized = work / "dxgi_main_localized.o"
    objcopy = cxx.parent / "llvm-objcopy"
    run([objcopy, f"--localize-symbol={LOGGER_INSTANCE}", dxgi_main[0], localized])
    grouped["src/dxgi"] = [localized if obj == dxgi_main[0] else obj
                           for obj in grouped["src/dxgi"]]
    return grouped


def archive(ar: Path, path: Path, objects: list[Path]) -> Path:
    path.unlink(missing_ok=True)
    run([ar, "rcs", path, *objects])
    return path


def git_identity() -> tuple[str, bool]:
    commit = run(["git", "-C", ROOT, "rev-parse", "HEAD"]).stdout.strip()
    dirty = bool(run(["git", "-C", ROOT, "status", "--porcelain",
                      "--untracked-files=no"]).stdout.strip())
    return commit, dirty


def check_link(nm: Path, pie: Path, link_map: Path) -> None:
    symbols = run([nm, pie]).stdout.splitlines()
    defined = {line.split()[-1] for line in symbols
               if len(line.split()) == 3 and line.split()[1] in "TtWDdBbRr"}
    missing = [name for name in REQUIRED_DEFINED if name not in defined]
    if missing:
        raise ValueError(f"payload link lacks {', '.join(missing)}")
    undefined = {line.split()[-1] for line in symbols if line.split()[0] == "U"}
    # __real_getenv forwards to the platform getenv; nothing may reach a loader.
    leaked = sorted(set(PAYLOAD_DL) & undefined)
    if leaked or "(dlfcn.o)" in link_map.read_text():
        raise ValueError("a system dlopen/dlsym implementation was linked")


def build_variant(args: argparse.Namespace, variant: str) -> dict:
    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    sdk, _wrapper = get_ps5_toolchain()
    if sdk is None:
        raise ValueError("PS5 payload toolchain is unavailable")
    builder = foundation / "build/host/ps5-native-tool"
    if not builder.is_file():
        raise ValueError("ps5-native-tool is required")
    cc, cxx = sdk / "bin/prospero-clang", sdk / "bin/prospero-clang++"
    dxvk = args.dxvk_dir.resolve()
    build = (args.build_dir or dxvk / "build-native").resolve()
    dxvk_commit = dxvk_identity(dxvk)
    header = (SOURCE / "dxvk_shaders.h").read_text()
    shaders = embedded_shaders(header)
    ps5vk_commit, ps5vk_dirty = git_identity()

    work = args.output_dir.resolve() / variant
    dist = work / "dist/PPSA99994"
    shutil.rmtree(work, ignore_errors=True)
    for directory in (work, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)
    switches = sorted(set(args.sdk_switch or ()))
    absent: list[str] = []
    if args.diagnostic_integration:
        present, absent = diagnostic_integration_switches(
            (ROOT / "tools/build_sdk.py").read_text())
        switches = sorted(set(switches) | set(present))
    if not args.skip_sdk:
        env = sdk_build_environment(os.environ, sdk, switches)
        subprocess.run([sys.executable, str(ROOT / "tools/build_sdk.py")], cwd=ROOT, env=env,
                       check=True)
    staged = ROOT / "dist-sdk"

    grouped = compile_dxvk(variant, dxvk, build, work, cc, cxx, args.jobs)
    ar = sdk / "bin/llvm-ar"
    archives = [archive(ar, work / f"{group.replace('/', '_')}.a", objects)
                for group, objects in sorted(grouped.items())
                if group not in ("src/d3d11", "src/dxgi")]

    (work / "build_identity.h").write_text(identity_header(
        variant, dxvk_commit, ps5vk_commit, ps5vk_dirty,
        sha256_bytes(shaders["vs"]), sha256_bytes(shaders["ps"]),
        integration_label(args), ",".join(switches) or "none"))
    logger = lab / "projects/logging_server/client"
    payload_objects = []
    for source in ("ps5_main.cpp", "workload.cpp", "vk_trace.cpp", "compat_layer.cpp"):
        obj = work / (source + ".o")
        run([cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-Wno-missing-field-initializers",
             f"-I{SOURCE}", f"-I{work}", f"-I{logger}",
             "-isystem", dxvk / "include/native/windows",
             "-isystem", dxvk / "include/native/directx",
             "-isystem", dxvk / "include/vulkan/include", "-c", SOURCE / source, "-o", obj])
        payload_objects.append(obj)

    pie = work / "payload_pie.elf"
    link = [sdk / "bin/prospero-lld", f"-L{sdk / 'target/lib'}",
            "-T", staged / "lib/ps5-pie.ld", "--eh-frame-hdr",
            "--version-script", staged / "lib/app-symbols.map",
            *[f"--wrap={name}" for name in LINK_WRAPS],
            f"-Map={work / 'payload.map'}", f"--why-extract={work / 'why-extract.txt'}",
            "-e", "_start", "-o", pie,
            staged / "lib/crt.o", *payload_objects,
            *grouped["src/d3d11"], *grouped["src/dxgi"],
            "--start-group", *archives, "--end-group",
            staged / "lib/libps5vk.a", staged / "lib/libpsbc.a",
            *[sdk / f"target/lib/{name}" for name in
              ("libc++.a", "libc++abi.a", "libunwind.a", "libpthread.a", "libc.a")],
            "--as-needed", *sorted((sdk / "target/lib").glob("*.so")),
            staged / "lib/libSceAgc.so", staged / "lib/libSceAgcDriver.so"]
    run(link)
    check_link(sdk / "bin/llvm-nm", pie, work / "payload.map")
    eboot_elf = work / "eboot.elf"
    run([builder, "link", "--in", pie, "--out", eboot_elf,
         "--stub-dir", sdk / "target/lib", "--module-sdk", "0x02000009",
         "--stub", staged / "lib/libSceAgc.so", "--stub", staged / "lib/libSceAgcDriver.so",
         "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"])
    eboot_elf.write_bytes(use_application_heap(eboot_elf.read_bytes()))
    eboot = dist / "eboot.bin"
    run([builder, "self", "--sign", "--in", eboot_elf, "--out", eboot, "--magic", "0x1D3D154F"])

    param = json.loads((lab / "projects/ps5-agc-gears/sce_sys/param.json").read_text())
    param.update(titleId="PPSA99994", conceptId="99994",
                 contentId="UP9000-PPSA99994_00-PS5VKDXVKNATIVE0")
    label = " (DIAGNOSTIC)" if VARIANTS[variant]["diagnostic"] else ""
    param["localizedParameters"]["en-US"]["titleName"] = f"PS5 Vulkan DXVK Native{label}"
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")

    config = VARIANTS[variant]
    artifact = {
        "profile": PROFILE,
        "variant": variant,
        "label": ("DIAGNOSTIC-INTEGRATION" if integration_label(args) != "none" else
                  "DIAGNOSTIC" if config["diagnostic"] else "UNMODIFIED"),
        "integration": None if integration_label(args) == "none" else integration_label(args),
        "sdk_switches": switches,
        "sdk_rebuilt": not args.skip_sdk,
        "sdk_switches_unavailable": absent,
        "patch_list_sha256": sha256_bytes("\n".join(
            list(config["patches"]) + list(PLATFORM_OVERLAYS)).encode()),
        "diagnostic": config["diagnostic"],
        "dxvk_commit": dxvk_commit,
        "dxvk_source_patches": list(config["patches"]),
        "platform_overlays": list(PLATFORM_OVERLAYS),
        "link": "static",
        "link_wraps": list(LINK_WRAPS),
        "vulkan_binding": "dlopen(libvulkan.so)/dlsym(vkGetInstanceProcAddr) -> ps5vk static entry",
    "payload_dl_entry_points": list(PAYLOAD_DL),
        "ps5vk_commit": ps5vk_commit,
        "ps5vk_dirty": ps5vk_dirty,
        "libps5vk_sha256": sha256_file(staged / "lib/libps5vk.a"),
        "wsi_adapter_sha256": sha256_file(ROOT / "tools/dxvk_ps5_wsi.cpp"),
        "vs_sha256": sha256_bytes(shaders["vs"]),
        "ps_sha256": sha256_bytes(shaders["ps"]),
        "payload_sources_sha256": {
            path.name: sha256_file(path) for path in sorted(SOURCE.iterdir()) if path.is_file()},
        "eboot_sha256": sha256_file(eboot),
        "eboot_bytes": eboot.stat().st_size,
    }
    (work / "artifact.json").write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n")
    return artifact


def integration_label(args: argparse.Namespace) -> str:
    """The DIAGNOSTIC-INTEGRATION description: explicit, or the ps5vk commit
    when --diagnostic-integration builds from one tree."""
    if args.integration:
        return args.integration
    if args.diagnostic_integration:
        return "tree@" + git_identity()[0][:10]
    return "none"


def host_check(args: argparse.Namespace) -> dict:
    """Run the workload through the host DXVK build on a host Vulkan driver."""
    dxvk = args.dxvk_dir.resolve()
    build = (args.build_dir or dxvk / "build-native").resolve()
    d3d11 = build / "src/d3d11/libdxvk_d3d11.so.0.20602"
    out = args.output_dir.resolve() / "host-check"
    out.mkdir(parents=True, exist_ok=True)
    executable = out / "workload"
    run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-I{SOURCE}",
         f"-I{dxvk / 'include/native/directx'}", f"-I{dxvk / 'include/native/windows'}",
         *run(["sdl2-config", "--cflags"]).stdout.split(),
         SOURCE / "workload.cpp", SOURCE / "host_main.cpp", d3d11,
         *run(["sdl2-config", "--libs"]).stdout.split(),
         f"-Wl,-rpath,{d3d11.parent}", f"-Wl,-rpath,{build / 'src/dxgi'}", "-o", executable])
    env = dict(os.environ, DXVK_WSI_DRIVER="SDL2", DXVK_LOG_PATH="none",
               DXVK_STATE_CACHE="disable")
    result = subprocess.run([str(executable)], env=env, text=True, capture_output=True,
                            timeout=120)
    oracle = re.search(r"DXVK_ORACLE checked=(\d+) mismatches=(\d+) checksum=([0-9a-f]{8}) "
                       r"expected_checksum=([0-9a-f]{8})", result.stdout)
    receipt = {
        "profile": PROFILE + "-host-check",
        "driver": "host Vulkan driver; no ps5vk or PS5 execution",
        "exit": result.returncode,
        "oracle": ({"checked": int(oracle.group(1)), "mismatches": int(oracle.group(2)),
                    "checksum": oracle.group(3), "expected_checksum": oracle.group(4)}
                   if oracle else None),
    }
    if result.returncode or not oracle or oracle.group(2) != "0":
        print(result.stdout[-4000:], result.stderr[-2000:], file=sys.stderr)
        raise ValueError("host workload did not render the expected image")
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--variant", choices=sorted(VARIANTS), action="append")
    parser.add_argument("--dxvk-dir", type=Path, default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--build-dir", type=Path,
                        help="Configured native Meson build (default: <dxvk-dir>/build-native)")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/dxvk-ps5-native")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--sdk-switch", action="append", metavar="NAME",
                        help="Build the SDK with this diagnostic switch set to 1 (recorded)")
    parser.add_argument("--integration", help="DIAGNOSTIC-INTEGRATION description "
                        "(merged driver heads); labels the payload and receipt")
    parser.add_argument("--diagnostic-integration", action="store_true",
                        help="One-command DIAGNOSTIC recipe: build the SDK with every DXVK "
                             "measurement switch it knows, label the payload "
                             "DIAGNOSTIC-INTEGRATION and record the switches")
    parser.add_argument("--skip-sdk", action="store_true",
                        help="Reuse the staged dist-sdk instead of rebuilding it")
    parser.add_argument("--host-check", action="store_true",
                        help="Also run the workload through host DXVK on a host Vulkan driver")
    parser.add_argument("--emit-shader-header", nargs=2, type=Path, metavar=("VS", "PS"),
                        help="Regenerate examples/dxvk_native/dxvk_shaders.h from DXBC blobs")
    args = parser.parse_args()
    try:
        if args.emit_shader_header:
            vs, ps = (path.read_bytes() for path in args.emit_shader_header)
            (SOURCE / "dxvk_shaders.h").write_text(shader_header(vs, ps))
            return 0
        if args.jobs < 1:
            raise ValueError("--jobs must be positive")
        results = {}
        if args.host_check:
            results["host-check"] = host_check(args)
        for variant in args.variant or ["unmodified"]:
            results[variant] = build_variant(args, variant)
            args.skip_sdk = True
        print(json.dumps(results, indent=2, sort_keys=True))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            print(f"DXVK native PS5 build failed: {Path(str(error.cmd[0])).name} "
                  f"exited {error.returncode}", file=sys.stderr)
            print((error.stdout or "")[-3000:], (error.stderr or "")[-3000:], file=sys.stderr)
        else:
            print(f"DXVK native PS5 build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
