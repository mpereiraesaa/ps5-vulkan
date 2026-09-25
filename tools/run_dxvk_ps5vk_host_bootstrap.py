#!/usr/bin/env python3
"""Exercise pinned DXVK's first Vulkan negotiation against ps5vk host code.

The temporary libvulkan.so is built from the host SDK source list. It is a
loader-shaped host witness, not a PS5 payload or a Vulkan WSI implementation.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import run_dxvk_host_smoke as smoke


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_REFUSAL = "Required Vulkan extension VK_KHR_surface not supported"


def host_sources() -> list[str]:
    """Use exactly the sources that the SDK uses for its host archive."""
    tree = ast.parse((ROOT / "tools/build_sdk.py").read_text())
    assignments = [node for node in ast.walk(tree)
                   if isinstance(node, ast.Assign) and
                   any(isinstance(target, ast.Name) and target.id == "host_sources"
                       for target in node.targets)]
    if len(assignments) != 1:
        raise ValueError("host SDK source list not found uniquely")
    sources = ast.literal_eval(assignments[0].value)
    if not isinstance(sources, list) or not sources or not all(
            isinstance(source, str) and source.startswith(("src/", "native/"))
            for source in sources):
        raise ValueError("invalid host SDK source list")
    return sources


def build_loader(directory: Path) -> Path:
    objects = []
    for index, source in enumerate(host_sources()):
        obj = directory / f"{index}.o"
        smoke.run(["cc", "-std=c11", "-O2", "-fPIC",
                   f"-I{ROOT / 'third_party/vulkan-headers/include'}",
                   f"-I{ROOT / 'include'}",
                   f"-I{ROOT / 'src'}", "-c", str(ROOT / source),
                   "-o", str(obj)])
        objects.append(str(obj))
    loader = directory / "libvulkan.so.1"
    smoke.run(["cc", "-shared", "-Wl,-soname,libvulkan.so.1",
               "-Wl,--no-undefined", *objects, "-o", str(loader)])
    (directory / "libvulkan.so").symlink_to(loader.name)
    return loader


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk-dir", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, help="Write compact receipt as JSON")
    args = parser.parse_args()
    dxvk = args.dxvk_dir.resolve()
    build = (args.build_dir or dxvk / "build-native").resolve()
    try:
        expected = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                              .read_text())["source"]["commit"]
        actual = smoke.run(["git", "-C", str(dxvk), "rev-parse", "HEAD"]).stdout.strip()
        if actual != expected:
            raise ValueError(f"DXVK source commit {actual} differs from pinned {expected}")
        d3d11 = build / "src/d3d11/libdxvk_d3d11.so.0.20602"
        dxgi = build / "src/dxgi/libdxvk_dxgi.so.0.20602"
        if not d3d11.is_file() or not dxgi.is_file():
            raise ValueError("pinned DXVK native D3D11/DXGI build is missing")
        cflags = smoke.run(["sdl2-config", "--cflags"]).stdout.split()
        libs = smoke.run(["sdl2-config", "--libs"]).stdout.split()
        with tempfile.TemporaryDirectory(prefix="ps5vk-dxvk-bootstrap-") as temp:
            directory = Path(temp)
            loader = build_loader(directory)
            source = directory / "smoke.cpp"
            binary = directory / "smoke"
            source.write_text(smoke.SOURCE)
            smoke.run(["c++", "-std=c++17",
                       f"-I{dxvk / 'include/native/directx'}",
                       f"-I{dxvk / 'include/native/windows'}", *cflags,
                       str(source), str(d3d11), *libs,
                       f"-Wl,-rpath,{d3d11.parent}",
                       f"-Wl,-rpath,{dxgi.parent}", "-o", str(binary)])
            env = dict(os.environ, DXVK_WSI_DRIVER="SDL2")
            env["LD_LIBRARY_PATH"] = str(directory) + os.pathsep + env.get(
                "LD_LIBRARY_PATH", "")
            run = subprocess.run([str(binary)], text=True, capture_output=True,
                                 env=env, check=False)
            loader_hash = smoke.sha256(loader)
        match = smoke.MARKER.search(run.stdout)
        if (run.returncode != 0 or not match or
                (match.group(1), match.group(3), match.group(4)) !=
                ("80004005", "0", "0") or
                EXPECTED_REFUSAL not in run.stderr or
                "DxvkInstance: Required instance extensions not supported" not in run.stderr):
            raise ValueError("DXVK did not stop at the expected ps5vk surface-extension boundary:\n" +
                             (run.stderr + run.stdout)[-2000:])
        receipt = {
            "profile": "dxvk-v262-ps5vk-host-bootstrap",
            "dxvk_commit": actual,
            "d3d11_sha256": smoke.sha256(d3d11),
            "dxgi_sha256": smoke.sha256(dxgi),
            "ps5vk_host_loader_sha256": loader_hash,
            "ps5vk_source_commit": smoke.run(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"]).stdout.strip(),
            "hresult": "0x" + match.group(1),
            "first_refusal": EXPECTED_REFUSAL,
            "scope": "DXVK against ps5vk host source; no PS5 execution",
        }
        print(json.dumps(receipt, indent=2, sort_keys=True))
        if args.output:
            args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK ps5vk host bootstrap failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-2000:], file=sys.stderr)
            print(error.stderr[-2000:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
