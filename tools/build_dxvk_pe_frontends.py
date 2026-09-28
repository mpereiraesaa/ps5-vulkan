#!/usr/bin/env python3
"""Build four PE application controls against pinned PS5-WSI DXVK DLLs.

The outputs are executable inputs for Prospero Win, not host or PS5 runtime
evidence. Every control creates a fixed 1920x1080 window, presents two colours
and prints a stage/result marker before it closes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "examples/dxvk_pe_frontends"
IMPORTS = {
    "d3d8": ("d3d8.dll",),
    "d3d9": ("d3d9.dll",),
    "d3d10": ("d3d10.dll",),
    "d3d11": ("d3d11.dll", "dxgi.dll"),
}
LINK_DXVK = {
    "d3d8": ("d3d8",),
    "d3d9": ("d3d9",),
    "d3d10": (),
    "d3d11": ("d3d11", "dxgi"),
}
MODULE_DIRECTORIES = {"d3d10core": "d3d10"}


def command(args: list[str | Path]) -> str:
    return subprocess.run([str(arg) for arg in args], check=True,
                          text=True, capture_output=True).stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_arch(arch: str, overlay: Path, output: Path, source: Path,
               modules: dict) -> dict:
    prefix = "x86_64-w64-mingw32" if arch == "x64" else "i686-w64-mingw32"
    compiler, objdump = f"{prefix}-g++", f"{prefix}-objdump"
    expected_format = "pei-x86-64" if arch == "x64" else "pei-i386"
    target = output / arch
    target.mkdir(parents=True, exist_ok=True)
    result = {}
    for api in ("d3d11", "d3d9", "d3d8", "d3d10"):
        source_file = SOURCE / f"{api}.cpp"
        executable = target / f"{api}.exe"
        link = [overlay / arch / "src" / name / f"{name}.dll.a"
                for name in LINK_DXVK[api]]
        if any(not library.is_file() for library in link):
            raise ValueError(f"{arch} {api} DXVK import library missing")
        command([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-isystem", source / "include/native/directx",
                 source_file, *link, *(["-ld3d10"] if api == "d3d10" else []),
                 "-luser32", "-static-libgcc", "-static-libstdc++",
                 "-o", executable])
        header = command([objdump, "-f", executable])
        imports = sorted(set(re.findall(r"DLL Name: ([^\s]+)",
                                        command([objdump, "-p", executable]))),
                         key=str.casefold)
        if f"file format {expected_format}" not in header:
            raise ValueError(f"{arch} {api} PE format mismatch")
        if not {name.casefold() for name in IMPORTS[api]}.issubset(
                {name.casefold() for name in imports}):
            raise ValueError(f"{arch} {api} does not import its intended DLL chain")
        result[api] = {
            "path": str(executable.relative_to(ROOT)) if executable.is_relative_to(ROOT)
                    else str(executable),
            "source_sha256": sha256(source_file),
            "exe_sha256": sha256(executable),
            "bytes": executable.stat().st_size,
            "pe_imports": imports,
            "dxvk_dll_sha256": {name: modules[name]["sha256"]
                                   for name in LINK_DXVK[api]},
            "executed": False,
        }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--overlay-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-ps5-wsi")
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-frontends")
    args = parser.parse_args()
    try:
        overlay = args.overlay_dir.resolve()
        output = args.output_dir.resolve()
        source = overlay / "source"
        base = json.loads((overlay / "receipt.json").read_text())
        pinned = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                            .read_text())["source"]["commit"]
        if (base.get("schema") != "ps5vk-dxvk-v262-pe-ps5-wsi/1" or
                base.get("dxvk_commit") != pinned or
                set(base.get("modules", {})) != {"x64", "x86"}):
            raise ValueError("both pinned PS5 WSI PE architectures must be built first")
        for arch, modules in base["modules"].items():
            for name, identity in modules.items():
                dll = overlay / arch / "src" / MODULE_DIRECTORIES.get(name, name) / f"{name}.dll"
                if sha256(dll) != identity["sha256"]:
                    raise ValueError(f"{arch} {name} DLL hash differs from overlay receipt")
        outputs = {arch: build_arch(arch, overlay, output, source, modules)
                   for arch, modules in base["modules"].items()}
        receipt = {"schema": "ps5vk-dxvk-v262-pe-frontends/1",
                   "dxvk_commit": pinned,
                   "wsi_adapter_sha256": base["overlay"]["adapter_sha256"],
                   "window_source_sha256": sha256(SOURCE / "window.h"),
                   "architectures": outputs,
                   "scope": "PE application binaries only; no Prospero Win or PS5 execution"}
        output.mkdir(parents=True, exist_ok=True)
        path = output / "receipt.json"
        path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        print(json.dumps(receipt, indent=2, sort_keys=True))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK PE frontend build failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-1200:], file=sys.stderr)
            print(error.stderr[-1200:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
