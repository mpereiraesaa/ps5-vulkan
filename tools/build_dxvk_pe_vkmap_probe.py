#!/usr/bin/env python3
"""Build x86/x64 PE vkMapMemory and GPU-copy controls for Prospero Win.

The receipt proves architecture, imports and pinned-header identity only.
The executable must still run in Prospero Win against the selected ps5vk SDK.
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
SOURCE = ROOT / "examples/dxvk_pe_vkmap_probe/main.c"
ARCHES = {"x86": ("i686-w64-mingw32", "pei-i386"),
          "x64": ("x86_64-w64-mingw32", "pei-x86-64")}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args: str | Path) -> str:
    return subprocess.run([str(arg) for arg in args], check=True, text=True,
                          capture_output=True).stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk-dir", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/dxvk-pe-vkmap-probe")
    args = parser.parse_args()
    try:
        dxvk = args.dxvk_dir.resolve()
        pinned = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                            .read_text())["source"]["commit"]
        actual = run("git", "-C", dxvk, "rev-parse", "HEAD").strip()
        dirty = run("git", "-C", dxvk, "status", "--porcelain",
                    "--untracked-files=no").strip()
        if actual != pinned or dirty:
            raise ValueError("DXVK headers must be clean at the pinned 2.6.2 commit")
        header = dxvk / "include/vulkan/include/vulkan/vulkan_core.h"
        if not header.is_file():
            raise ValueError("pinned Vulkan headers are missing")
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=True)
        built = {}
        for arch, (prefix, pe_format) in ARCHES.items():
            target = output / arch
            target.mkdir(parents=True, exist_ok=True)
            exe = target / "vkmap.exe"
            run(f"{prefix}-gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I" + str(header.parents[1]),
                SOURCE, "-static-libgcc", "-o", exe)
            file_header = run(f"{prefix}-objdump", "-f", exe)
            imports = sorted(set(re.findall(
                r"DLL Name: ([^\s]+)", run(f"{prefix}-objdump", "-p", exe))),
                key=str.casefold)
            lowered = {name.casefold() for name in imports}
            if f"file format {pe_format}" not in file_header:
                raise ValueError(f"{arch} PE architecture mismatch")
            if "kernel32.dll" not in lowered or "vulkan-1.dll" in lowered:
                raise ValueError(f"{arch} must load Wine Vulkan dynamically")
            built[arch] = {"path": str(exe), "exe_sha256": sha256(exe),
                           "bytes": exe.stat().st_size, "pe_imports": imports,
                           "executed": False}
        receipt = {"schema": "ps5vk-dxvk-v262-pe-vkmap-probe/1",
                   "dxvk_commit": pinned,
                   "vulkan_header_sha256": sha256(header),
                   "source_sha256": sha256(SOURCE),
                   "architectures": built,
                   "scope": "offline PE build only; no guest mapping or GPU execution"}
        path = output / "receipt.json"
        path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        print(json.dumps(receipt, indent=2, sort_keys=True))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"PE vkMapMemory probe build failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-1200:], file=sys.stderr)
            print(error.stderr[-1200:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
