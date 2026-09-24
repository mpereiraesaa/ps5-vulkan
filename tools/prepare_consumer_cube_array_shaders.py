#!/usr/bin/env python3
"""Compile owned cube-array sampling shaders for the public SDK witness."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

from prepare_consumer_sync_shaders import emit_array

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    local = ROOT / "build/runtime-graphics/toolchain/usr/bin/glslangValidator"
    compiler = os.environ.get("PS5VK_GLSLANG") or (
        str(local) if local.is_file() else shutil.which("glslangValidator"))
    if not compiler:
        raise SystemExit("glslangValidator is required")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    header = "/* Generated from the public cube-array witness GLSL. */\n#include <stdint.h>\n"
    for source, suffix, name in (
        ("consumer_cube_array.vert", ".vert.spv", "consumer_cube_array_vert_spirv"),
        ("consumer_cube_array.frag", ".frag.spv", "consumer_cube_array_frag_spirv"),
    ):
        spirv = args.out.with_suffix(suffix)
        subprocess.run([
            compiler, "-V", "--target-env", "vulkan1.0",
            str(ROOT / "experiments/graphics" / source), "-o", str(spirv),
        ], check=True)
        data = spirv.read_bytes()
        header += emit_array(name, data)
        header += (f'#define {name.upper()}_SHA256 '
                   f'"{hashlib.sha256(data).hexdigest()}"\n')
    args.out.write_text(header)


if __name__ == "__main__":
    main()
