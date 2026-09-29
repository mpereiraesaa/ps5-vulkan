#!/usr/bin/env python3
"""Build x64/x86 D3D11 triangle PE controls against pinned DXVK 2.6.2 DLLs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from build_dxvk_pe_frontends import command, load_dll_builds, sha256


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "examples/dxvk_pe_frontends/d3d11_draw.cpp"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll-variant", choices=("unmodified", "ps5-wsi"),
                        default="unmodified",
                        help="Win32 WSI for Prospero Win, or experimental PS5 display WSI")
    parser.add_argument("--overlay-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-ps5-wsi")
    parser.add_argument("--unmodified-dir", type=Path, default=ROOT / "build",
                        help="directory containing pinned dxvk-pe-x64/x86 builds")
    parser.add_argument("--dxvk-source", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2",
                        help="clean pinned DXVK source for DirectX headers")
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-d3d11-draw")
    args = parser.parse_args()
    overlay = args.overlay_dir.resolve()
    unmodified = args.unmodified_dir.resolve()
    source = args.dxvk_source.resolve()
    output = args.output_dir.resolve()
    try:
        pinned = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                            .read_text())["source"]["commit"]
        builds, modules, adapter_sha = load_dll_builds(
            args.dll_variant, overlay, unmodified, pinned)
        if args.dll_variant == "unmodified":
            if (command(["git", "-C", source, "rev-parse", "HEAD"]).strip() != pinned or
                    command(["git", "-C", source, "status", "--porcelain",
                             "--untracked-files=no"]).strip()):
                raise ValueError("DXVK source checkout is not clean at the pin")
        source_headers = source / "include/native/directx"
        if args.dll_variant == "ps5-wsi":
            source_headers = overlay / "source/include/native/directx"
        if not source_headers.is_dir():
            raise ValueError("pinned DXVK 2.6.2 DirectX headers missing")
        results = {}
        for arch, prefix, format_name in (
                ("x64", "x86_64-w64-mingw32", "pei-x86-64"),
                ("x86", "i686-w64-mingw32", "pei-i386")):
            target = output / arch
            target.mkdir(parents=True, exist_ok=True)
            executable = target / "d3d11-draw.exe"
            imports = [builds[arch] / "src" / name / f"{name}.dll.a"
                       for name in ("d3d11", "dxgi")]
            if any(not path.is_file() for path in imports):
                raise ValueError(f"{arch} pinned DXVK import library missing")
            command([f"{prefix}-g++", "-std=c++17", "-O2", "-Wall", "-Wextra",
                     "-Werror", "-isystem", source_headers, SOURCE, *imports,
                     "-ld3dcompiler_47", "-luser32", "-static-libgcc",
                     "-static-libstdc++", "-o", executable])
            header = command([f"{prefix}-objdump", "-f", executable])
            imported = sorted(set(re.findall(
                r"DLL Name: ([^\s]+)",
                command([f"{prefix}-objdump", "-p", executable]))),
                key=str.casefold)
            if f"file format {format_name}" not in header:
                raise ValueError(f"{arch} PE format mismatch")
            if not {"d3d11.dll", "dxgi.dll", "d3dcompiler_47.dll"}.issubset(
                    {name.casefold() for name in imported}):
                raise ValueError(f"{arch} missing DXVK or shader-compiler import")
            results[arch] = {
                "path": str(executable),
                "exe_sha256": sha256(executable),
                "bytes": executable.stat().st_size,
                "pe_imports": imported,
                "dxvk_dll_sha256": {
                    name: modules[arch][name]["sha256"]
                    for name in ("d3d11", "dxgi")},
                "executed": False,
            }
        receipt = {
            "schema": "ps5vk-dxvk-v262-pe-d3d11-draw/1",
            "dxvk_commit": pinned,
            "dll_variant": args.dll_variant,
            "source_sha256": sha256(SOURCE),
            "expected_center_bgra": "0000ffff",
            "expected_corner_bgra": ["844c1cff", "1c4c84ff"],
            "architectures": results,
            "scope": "PE build identities only; hardware pixel oracle pending",
        }
        if adapter_sha:
            receipt["wsi_adapter_sha256"] = adapter_sha
        output.mkdir(parents=True, exist_ok=True)
        (output / "receipt.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        print(json.dumps(receipt, indent=2, sort_keys=True))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK PE draw build failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-1200:], file=sys.stderr)
            print(error.stderr[-1200:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
