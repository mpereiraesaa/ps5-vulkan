#!/usr/bin/env python3
"""Build PE presentation and backbuffer-pixel controls for pinned DXVK 2.6.2.

The outputs are executable inputs for Prospero Win, not host or PS5 runtime
evidence. The pixel variants also read the backbuffer's centre pixel through
the D3D API before each Present; they do not measure physical scanout.
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
DXVK_CHAIN = {
    "d3d8": ("d3d8", "d3d9"),
    "d3d9": ("d3d9",),
    "d3d10": ("d3d10core", "d3d11", "dxgi"),
    "d3d11": ("d3d11", "dxgi"),
}
MODULE_DIRECTORIES = {"d3d10core": "d3d10"}
MODULE_NAMES = {"dxgi", "d3d11", "d3d10core", "d3d9", "d3d8"}


def command(args: list[str | Path]) -> str:
    return subprocess.run([str(arg) for arg in args], check=True,
                          text=True, capture_output=True).stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_arch(arch: str, dll_build: Path, output: Path, source: Path,
               modules: dict) -> dict:
    prefix = "x86_64-w64-mingw32" if arch == "x64" else "i686-w64-mingw32"
    compiler, objdump = f"{prefix}-g++", f"{prefix}-objdump"
    expected_format = "pei-x86-64" if arch == "x64" else "pei-i386"
    target = output / arch
    target.mkdir(parents=True, exist_ok=True)
    result = {}
    for api in ("d3d11", "d3d9", "d3d8", "d3d10"):
        source_file = SOURCE / f"{api}.cpp"
        link = [dll_build / "src" / name / f"{name}.dll.a"
                for name in LINK_DXVK[api]]
        if any(not library.is_file() for library in link):
            raise ValueError(f"{arch} {api} DXVK import library missing")
        for pixels in (False, True):
            name = api + ("-pixels" if pixels else "")
            executable = target / f"{name}.exe"
            command([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                     *(["-DPS5VK_PE_PIXEL_ORACLE=1"] if pixels else []),
                     "-isystem", source / "include/native/directx",
                     source_file, *link, *(["-ld3d10"] if api == "d3d10" else []),
                     "-luser32", "-static-libgcc", "-static-libstdc++",
                     "-o", executable])
            header = command([objdump, "-f", executable])
            imports = sorted(set(re.findall(r"DLL Name: ([^\s]+)",
                                            command([objdump, "-p", executable]))),
                             key=str.casefold)
            if f"file format {expected_format}" not in header:
                raise ValueError(f"{arch} {name} PE format mismatch")
            if not {dll.casefold() for dll in IMPORTS[api]}.issubset(
                    {dll.casefold() for dll in imports}):
                raise ValueError(f"{arch} {name} does not import its intended DLL chain")
            result[name] = {
                "path": str(executable.relative_to(ROOT)) if executable.is_relative_to(ROOT)
                        else str(executable),
                "source_sha256": sha256(source_file),
                "pixel_header_sha256": sha256(SOURCE / "pixel_oracle.h") if pixels else None,
                "exe_sha256": sha256(executable),
                "bytes": executable.stat().st_size,
                "pe_imports": imports,
                "dxvk_dll_sha256": {module: modules[module]["sha256"]
                                       for module in DXVK_CHAIN[api]},
                "pixel_oracle": pixels,
                "executed": False,
            }
    return result


def load_dll_builds(variant: str, overlay: Path, unmodified: Path,
                    pinned: str) -> tuple[dict[str, Path], dict[str, dict], str | None]:
    if variant == "ps5-wsi":
        base = json.loads((overlay / "receipt.json").read_text())
        if (base.get("schema") != "ps5vk-dxvk-v262-pe-ps5-wsi/1" or
                base.get("dxvk_commit") != pinned or
                set(base.get("modules", {})) != {"x64", "x86"}):
            raise ValueError("both pinned PS5 WSI PE architectures must be built first")
        builds = {arch: overlay / arch for arch in ("x64", "x86")}
        modules = base["modules"]
        adapter_sha256 = base["overlay"]["adapter_sha256"]
    else:
        builds = {arch: unmodified / f"dxvk-pe-{arch}" for arch in ("x64", "x86")}
        modules = {}
        for arch, build in builds.items():
            inventory = json.loads((build / "inventory.json").read_text())
            if (inventory.get("schema") != "ps5vk-dxvk-pe-inventory/1" or
                    inventory.get("dxvk_commit") != pinned or
                    set(inventory.get("modules", {})) !=
                    {f"{name}.dll" for name in MODULE_NAMES}):
                raise ValueError(f"{arch} unmodified PE inventory is missing or unpinned")
            modules[arch] = {name: identity for dll, identity in
                             inventory["modules"].items()
                             for name in [dll.removesuffix(".dll")]}
        adapter_sha256 = None
    for arch, identities in modules.items():
        if set(identities) != MODULE_NAMES:
            raise ValueError(f"{arch} DXVK module set is incomplete")
        for name, identity in identities.items():
            dll = builds[arch] / "src" / MODULE_DIRECTORIES.get(name, name) / f"{name}.dll"
            if sha256(dll) != identity["sha256"]:
                raise ValueError(f"{arch} {name} DLL hash differs from its receipt")
    return builds, modules, adapter_sha256


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll-variant", choices=("unmodified", "ps5-wsi"),
                        default="unmodified")
    parser.add_argument("--overlay-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-ps5-wsi")
    parser.add_argument("--unmodified-dir", type=Path, default=ROOT / "build",
                        help="directory containing dxvk-pe-x64 and dxvk-pe-x86")
    parser.add_argument("--output-dir", type=Path,
                        help="defaults to a separate output directory per DLL variant")
    args = parser.parse_args()
    try:
        overlay = args.overlay_dir.resolve()
        output = (args.output_dir or ROOT / ("build/dxvk-pe-frontends" if
                  args.dll_variant == "ps5-wsi" else
                  "build/dxvk-pe-frontends-unmodified")).resolve()
        pinned = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                            .read_text())["source"]["commit"]
        builds, modules, adapter_sha256 = load_dll_builds(
            args.dll_variant, overlay, args.unmodified_dir.resolve(), pinned)
        source = (overlay / "source" if args.dll_variant == "ps5-wsi" else
                  ROOT / "third_party/dxvk-v2.6.2")
        if args.dll_variant == "unmodified":
            head = command(["git", "-C", source, "rev-parse", "HEAD"]).strip()
            changed = command(["git", "-C", source, "status", "--porcelain",
                               "--untracked-files=no"]).strip()
            if head != pinned or changed:
                raise ValueError("unmodified DXVK source checkout is not clean at the pin")
        outputs = {arch: build_arch(arch, builds[arch], output, source, identities)
                   for arch, identities in modules.items()}
        receipt = {"schema": "ps5vk-dxvk-v262-pe-frontends/3",
                   "dll_variant": args.dll_variant,
                   "dxvk_commit": pinned,
                   "window_source_sha256": sha256(SOURCE / "window.h"),
                   "expected_center_rgb": [[28, 76, 132], [132, 76, 28]],
                   "architectures": outputs,
                   "scope": "PE application binaries only; no Prospero Win or PS5 execution. Pixel variants compare CPU readback before Present, not physical scanout"}
        if adapter_sha256:
            receipt["wsi_adapter_sha256"] = adapter_sha256
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
