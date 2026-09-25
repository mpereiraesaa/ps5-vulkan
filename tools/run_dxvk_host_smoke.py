#!/usr/bin/env python3
"""Create a D3D11 device with the pinned, locally built DXVK v2.6.2.

This is a host integration witness for the *actual DXVK code*, not evidence
that ps5vk can load or run it. The DXVK checkout and build stay untracked.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = r"""
#include <d3d11.h>
#include <SDL2/SDL.h>
#include <cstdio>

int main() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL video initialization failed: %s\n", SDL_GetError());
    return 2;
  }
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);
  HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
      nullptr, 0, &requested, 1, D3D11_SDK_VERSION,
      &device, &selected, &context);
  std::printf("DXVK262_D3D11_CREATE hr=0x%08x level=0x%04x device=%d context=%d\n",
      static_cast<unsigned>(result), static_cast<unsigned>(selected),
      device != nullptr, context != nullptr);
  if (context) context->Release();
  if (device) device->Release();
  SDL_Quit();
  return 0;
}
"""
MARKER = re.compile(
    r"^DXVK262_D3D11_CREATE hr=0x([0-9a-f]{8}) level=0x([0-9a-f]{4}) "
    r"device=([01]) context=([01])$", re.MULTILINE)


def run(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, text=True, capture_output=True, check=True,
                          **kwargs)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk-dir", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--build-dir", type=Path,
                        help="Meson build directory (default: <dxvk-dir>/build-native)")
    parser.add_argument("--output", type=Path, help="Write the compact receipt as JSON")
    args = parser.parse_args()
    dxvk = args.dxvk_dir.resolve()
    build = (args.build_dir or dxvk / "build-native").resolve()
    expected = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                          .read_text())["source"]["commit"]
    try:
        commit = run(["git", "-C", str(dxvk), "rev-parse", "HEAD"]).stdout.strip()
        if commit != expected:
            raise ValueError(f"DXVK source commit {commit} differs from pinned {expected}")
        d3d11 = build / "src/d3d11/libdxvk_d3d11.so.0.20602"
        dxgi = build / "src/dxgi/libdxvk_dxgi.so.0.20602"
        if not d3d11.is_file() or not dxgi.is_file():
            raise ValueError("DXVK D3D11/DXGI native libraries are missing; build the pinned source first")
        sdl_cflags = run(["sdl2-config", "--cflags"]).stdout.split()
        sdl_libs = run(["sdl2-config", "--libs"]).stdout.split()
        with tempfile.TemporaryDirectory(prefix="ps5vk-dxvk-host-") as temp:
            source = Path(temp) / "smoke.cpp"
            executable = Path(temp) / "smoke"
            source.write_text(SOURCE)
            run(["c++", "-std=c++17",
                 f"-I{dxvk / 'include/native/directx'}",
                 f"-I{dxvk / 'include/native/windows'}",
                 *sdl_cflags, str(source), str(d3d11), *sdl_libs,
                 f"-Wl,-rpath,{d3d11.parent}",
                 f"-Wl,-rpath,{dxgi.parent}", "-o", str(executable)])
            env = dict(os.environ, DXVK_WSI_DRIVER="SDL2")
            result = run([str(executable)], env=env)
        match = MARKER.search(result.stdout)
        if not match:
            raise ValueError("DXVK did not emit the D3D11 device result")
        receipt = {
            "profile": "dxvk-v262-host-d3d11-smoke",
            "dxvk_commit": commit,
            "d3d11_sha256": sha256(d3d11),
            "dxgi_sha256": sha256(dxgi),
            "result": {
                "hresult": "0x" + match.group(1),
                "feature_level": "0x" + match.group(2),
                "device": match.group(3) == "1",
                "context": match.group(4) == "1",
            },
            "driver": "host Vulkan driver; no ps5vk or PS5 execution",
        }
        print(json.dumps(receipt, indent=2, sort_keys=True))
        if args.output:
            args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        if (match.group(1), match.group(2), match.group(3), match.group(4)) != (
                "00000000", "b000", "1", "1"):
            raise ValueError("DXVK did not create a feature-level 11_0 D3D11 device")
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK host smoke failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-3000:], file=sys.stderr)
            print(error.stderr[-3000:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
