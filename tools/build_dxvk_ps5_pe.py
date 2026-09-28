#!/usr/bin/env python3
"""Build pinned DXVK 2.6.2 PE DLLs with the PS5 display WSI overlay.

The source checkout is cloned into an ignored build directory before applying
the three-file overlay. The pinned DXVK checkout is never modified. These DLLs
still need a Wine Vulkan loader and execution in Prospero Win; a PE build is
not native runtime evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    "dxgi": "src/dxgi/dxgi.dll",
    "d3d11": "src/d3d11/d3d11.dll",
    "d3d10core": "src/d3d10/d3d10core.dll",
    "d3d9": "src/d3d9/d3d9.dll",
    "d3d8": "src/d3d8/d3d8.dll",
}
SUBMODULES = ("include/native/directx", "include/vulkan", "include/spirv",
              "subprojects/libdisplay-info")


def run(args: list[str | Path], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(arg) for arg in args], check=True, text=True,
                          capture_output=True, **kwargs)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def replace_once(source: str, old: str, new: str, label: str) -> str:
    if source.count(old) != 1:
        raise ValueError(f"pinned DXVK {label} anchor changed")
    return source.replace(old, new)


def overlay_source(source: Path, adapter: Path) -> dict[str, str]:
    platform = source / "src/wsi/wsi_platform.cpp"
    body = platform.read_text()
    body = replace_once(body, "namespace dxvk::wsi {",
                        "namespace dxvk::wsi {\n  extern WsiBootstrap Ps5WSI;",
                        "WSI bootstrap declaration")
    body = replace_once(body, "    &Win32WSI,", "    &Ps5WSI,",
                        "Win32 WSI registration")
    body = replace_once(body, 'hint = "Win32";', 'hint = "PS5";',
                        "Windows default WSI")
    platform.write_text(body)

    meson = source / "src/wsi/meson.build"
    meson.write_text(replace_once(meson.read_text(),
                                  "  'wsi_platform.cpp',",
                                  "  'wsi_platform.cpp',\n  'ps5_wsi.cpp',",
                                  "WSI source list"))
    copied = source / "src/wsi/ps5_wsi.cpp"
    shutil.copyfile(adapter, copied)
    return {"wsi_platform.cpp": sha256(platform),
            "meson.build": sha256(meson),
            "ps5_wsi.cpp": sha256(copied)}


def prepare_source(pinned: Path, output: Path, adapter: Path,
                   commit: str) -> tuple[Path, dict[str, str]]:
    source = output / "source"
    marker = output / "overlay.json"
    if source.exists():
        if not marker.is_file():
            raise ValueError("output source exists without an overlay identity")
        identity = json.loads(marker.read_text())
        if (identity.get("dxvk_commit") != commit or
                identity.get("adapter_sha256") != sha256(adapter) or
                set(identity.get("overlays", {})) !=
                {"wsi_platform.cpp", "meson.build", "ps5_wsi.cpp"} or
                any(sha256(source / "src/wsi" / name) != digest
                    for name, digest in identity.get("overlays", {}).items())):
            raise ValueError("existing overlay differs; choose a new --output-dir")
        pins = prepare_submodules(pinned, source)
        if identity.get("submodules") not in (None, pins):
            raise ValueError("existing overlay submodule pins differ")
        identity["submodules"] = pins
        marker.write_text(json.dumps(identity, indent=2, sort_keys=True) + "\n")
        return source, identity
    output.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--quiet", "--no-hardlinks", "--no-checkout", pinned, source])
    run(["git", "-C", source, "checkout", "--quiet", "--detach", commit])
    pins = prepare_submodules(pinned, source)
    overlays = overlay_source(source, adapter)
    identity = {"dxvk_commit": commit, "adapter_sha256": sha256(adapter),
                "submodules": pins, "overlays": overlays}
    marker.write_text(json.dumps(identity, indent=2, sort_keys=True) + "\n")
    return source, identity


def prepare_submodules(pinned: Path, source: Path) -> dict[str, str]:
    """Clone pinned submodule commits from local checkouts, without network."""
    pins = {}
    for name in SUBMODULES:
        expected = run(["git", "-C", source, "rev-parse", f"HEAD:{name}"]).stdout.strip()
        local = pinned / name
        if run(["git", "-C", local, "rev-parse", "HEAD"]).stdout.strip() != expected:
            raise ValueError(f"pinned DXVK submodule {name} differs from its gitlink")
        target = source / name
        if not (target / ".git").exists():
            run(["git", "clone", "--quiet", "--no-hardlinks", "--no-checkout",
                 local, target])
            run(["git", "-C", target, "checkout", "--quiet", "--detach", expected])
        if run(["git", "-C", target, "rev-parse", "HEAD"]).stdout.strip() != expected:
            raise ValueError(f"output submodule {name} differs from its gitlink")
        pins[name] = expected
    return pins


def build_arch(source: Path, output: Path, arch: str, jobs: int,
               meson: str) -> dict:
    build = output / arch
    if not (build / "build.ninja").is_file():
        if build.exists():
            shutil.rmtree(build)
        run([meson, "setup", build, source,
             "--cross-file", source / f"build-win{'64' if arch == 'x64' else '32'}.txt",
             "--wrap-mode=nodownload", "-Dbuildtype=release",
             "-Denable_d3d8=true", "-Denable_d3d9=true", "-Denable_d3d10=true",
             "-Denable_d3d11=true", "-Denable_dxgi=true"])
    run(["ninja", "-C", build, "-j", str(jobs), *TARGETS.values()])
    compdb = json.loads(run(["ninja", "-C", build, "-t", "compdb",
                             "c_COMPILER", "cpp_COMPILER"]).stdout)
    wsi_units = [entry for entry in compdb
                 if entry["file"].endswith("/src/wsi/ps5_wsi.cpp")]
    if len(wsi_units) != 1 or "-DDXVK_WSI_WIN32" not in wsi_units[0]["command"]:
        raise ValueError(f"{arch} build did not compile the PS5 WSI adapter")
    objdump = "x86_64-w64-mingw32-objdump" if arch == "x64" else "i686-w64-mingw32-objdump"
    nm = "x86_64-w64-mingw32-nm" if arch == "x64" else "i686-w64-mingw32-nm"
    expected_format = "pei-x86-64" if arch == "x64" else "pei-i386"
    exports = {"dxgi": "CreateDXGIFactory", "d3d11": "D3D11CreateDevice",
               "d3d10core": "D3D10CoreCreateDevice", "d3d9": "Direct3DCreate9",
               "d3d8": "Direct3DCreate8"}
    modules = {}
    for name, path in TARGETS.items():
        dll = build / path
        header = run([objdump, "-f", dll]).stdout
        exported = run([objdump, "-p", dll]).stdout
        if f"file format {expected_format}" not in header or exports[name] not in exported:
            raise ValueError(f"{arch} {name} architecture or entrypoint mismatch")
        if name in ("dxgi", "d3d9"):
            symbols = run([nm, "-C", dll]).stdout
            if ("dxvk::wsi::Ps5WSI" not in symbols or
                    "dxvk::wsi::Win32WSI" in symbols):
                raise ValueError(f"{arch} {name} does not select PS5 WSI")
        modules[name] = {"path": str(dll.relative_to(ROOT)) if dll.is_relative_to(ROOT)
                         else str(dll),
                         "sha256": sha256(dll), "bytes": dll.stat().st_size}
    return modules


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk-dir", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-ps5-wsi")
    parser.add_argument("--arch", choices=("x64", "x86", "both"), default="both")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--meson", default="meson",
                        help="Meson executable (default: meson on PATH)")
    args = parser.parse_args()
    try:
        if args.jobs < 1:
            raise ValueError("--jobs must be positive")
        pinned = args.dxvk_dir.resolve()
        output = args.output_dir.resolve()
        expected = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                              .read_text())["source"]["commit"]
        actual = run(["git", "-C", pinned, "rev-parse", "HEAD"]).stdout.strip()
        if actual != expected:
            raise ValueError(f"DXVK source {actual} differs from pin {expected}")
        if run(["git", "-C", pinned, "status", "--porcelain",
                "--untracked-files=no"]).stdout.strip():
            raise ValueError("pinned DXVK tracked source has local changes")
        adapter = ROOT / "tools/dxvk_ps5_wsi.cpp"
        source, identity = prepare_source(pinned, output, adapter, actual)
        architectures = ("x64", "x86") if args.arch == "both" else (args.arch,)
        modules = {arch: build_arch(source, output, arch, args.jobs, args.meson)
                   for arch in architectures}
        receipt = {"schema": "ps5vk-dxvk-v262-pe-ps5-wsi/1",
                   "dxvk_commit": actual, "overlay": identity,
                   "modules": modules,
                   "scope": "PE build with PS5 display WSI; no Wine Vulkan bridge or PS5 execution"}
        (output / "receipt.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        print(json.dumps(receipt, indent=2, sort_keys=True))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK PS5 PE build failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-1500:], file=sys.stderr)
            print(error.stderr[-1500:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
