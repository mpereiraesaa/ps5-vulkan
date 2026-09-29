#!/usr/bin/env python3
"""Build bounded D3D8-11 scanout and resize PE controls with DXVK 2.6.2."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from build_dxvk_pe_frontends import command, load_dll_builds, sha256

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "d3d8": ROOT / "examples/dxvk_pe_frontends/d3d8_scanout.cpp",
    "d3d9": ROOT / "examples/dxvk_pe_frontends/d3d9_scanout.cpp",
    "d3d10": ROOT / "examples/dxvk_pe_frontends/d3d10_scanout.cpp",
    "d3d11": ROOT / "examples/dxvk_pe_frontends/d3d11_scanout.cpp",
    "d3d11-resize": ROOT / "examples/dxvk_pe_frontends/d3d11_resize.cpp",
}
LINK_MODULES = {
    "d3d8": ("d3d8",),
    "d3d9": ("d3d9",),
    "d3d10": (),  # Wine's d3d10.dll wrapper loads DXVK D3D10 core.
    "d3d11": ("d3d11", "dxgi"),
    "d3d11-resize": ("d3d11", "dxgi"),
}
DLL_IDENTITIES = {
    "d3d8": ("d3d8", "d3d9"),
    "d3d9": ("d3d9",),
    "d3d10": ("d3d10core", "d3d11", "dxgi"),
    "d3d11": ("d3d11", "dxgi"),
    "d3d11-resize": ("d3d11", "dxgi"),
}
EXTRA_LIBRARIES = {
    "d3d10": ("-ld3d10", "-ld3dcompiler_47"),
    "d3d11": ("-ld3dcompiler_47",),
}
REQUIRED_IMPORTS = {
    "d3d8": {"d3d8.dll"},
    "d3d9": {"d3d9.dll"},
    "d3d10": {"d3d10.dll", "d3dcompiler_47.dll"},
    "d3d11": {"d3d11.dll", "dxgi.dll", "d3dcompiler_47.dll"},
    "d3d11-resize": {"d3d11.dll", "dxgi.dll"},
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unmodified-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--dxvk-source", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-scanout")
    args = parser.parse_args()
    source = args.dxvk_source.resolve()
    output = args.output_dir.resolve()
    try:
        pinned = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                            .read_text())["source"]["commit"]
        builds, modules, _ = load_dll_builds(
            "unmodified", ROOT / "build/dxvk-pe-ps5-wsi",
            args.unmodified_dir.resolve(), pinned)
        if (command(["git", "-C", source, "rev-parse", "HEAD"]).strip() != pinned or
                command(["git", "-C", source, "status", "--porcelain",
                         "--untracked-files=no"]).strip()):
            raise ValueError("DXVK source checkout is not clean at the pin")
        headers = source / "include/native/directx"
        if not headers.is_dir():
            raise ValueError("pinned DirectX headers missing")
        results = {}
        for arch, prefix, expected_format in (
                ("x64", "x86_64-w64-mingw32", "pei-x86-64"),
                ("x86", "i686-w64-mingw32", "pei-i386")):
            results[arch] = {}
            for api in SOURCES:
                target = output / arch
                target.mkdir(parents=True, exist_ok=True)
                executable = target / (f"{api}.exe" if api.endswith("resize")
                                       else f"{api}-scanout.exe")
                imports = [builds[arch] / "src" / name / f"{name}.dll.a"
                           for name in LINK_MODULES[api]]
                if any(not library.is_file() for library in imports):
                    raise ValueError(f"{arch} {api} pinned DXVK import library missing")
                compiler = f"{prefix}-g++"
                command([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra",
                         "-Werror", "-isystem", headers, SOURCES[api], *imports,
                         *EXTRA_LIBRARIES.get(api, ()),
                         "-luser32", "-static-libgcc", "-static-libstdc++",
                         "-o", executable])
                header = command([f"{prefix}-objdump", "-f", executable])
                imported = sorted(set(re.findall(
                    r"DLL Name: ([^\s]+)",
                    command([f"{prefix}-objdump", "-p", executable]))),
                    key=str.casefold)
                required = REQUIRED_IMPORTS[api]
                if f"file format {expected_format}" not in header:
                    raise ValueError(f"{arch} {api} PE format mismatch")
                if not required.issubset({name.casefold() for name in imported}):
                    raise ValueError(f"{arch} {api} PE imports mismatch")
                results[arch][api] = {
                    "path": str(executable),
                    "exe_sha256": sha256(executable),
                    "pe_imports": imported,
                    "dxvk_dll_sha256": {
                        name: modules[arch][name]["sha256"]
                        for name in DLL_IDENTITIES[api]},
                    "executed": False,
                }
        output.mkdir(parents=True, exist_ok=True)
        receipt = {
            "schema": "ps5vk-dxvk-v262-pe-scanout/1",
            "dxvk_commit": pinned,
            "dll_variant": "unmodified",
            "source_sha256": {api: sha256(path) for api, path in SOURCES.items()},
            "duration": "30 bounded frames with 250 ms message-pump waits plus Present pacing",
            "pattern": "TL red, TR green, BL blue, BR yellow",
            "controls": {
                "d3d8": "30 bounded quadrant frames at 1920x1080",
                "d3d9": "30 bounded quadrant frames at 1920x1080",
                "d3d10": "30 bounded quadrant frames at 1920x1080",
                "d3d11": "30 bounded quadrant frames at 1920x1080",
                "d3d11-resize": "3 red frames at 1920x1080, ResizeBuffers, then 3 green frames at 3840x2160",
            },
            "architectures": results,
            "scope": "PE build identities; host and visual scanout evidence separate",
        }
        (output / "receipt.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        print(json.dumps(receipt, indent=2, sort_keys=True))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK PE scanout build failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-1200:], file=sys.stderr)
            print(error.stderr[-1200:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
