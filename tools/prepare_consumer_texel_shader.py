#!/usr/bin/env python3
"""Generate the owned RGBA8 uniform-texel-buffer SPIR-V for the SDK consumer."""
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
    spirv = args.out.with_suffix(".spv")
    subprocess.run([compiler, "-V", str(ROOT / "experiments/compute/texel_rgba8.comp"),
                    "-o", str(spirv)], check=True)
    data = spirv.read_bytes()
    header = "/* Generated from experiments/compute/texel_rgba8.comp. */\n#include <stdint.h>\n"
    header += emit_array("consumer_texel_rgba8_spirv", data)
    header += ('#define CONSUMER_TEXEL_RGBA8_SPIRV_SHA256 "'
               + hashlib.sha256(data).hexdigest() + '"\n')
    args.out.write_text(header)

if __name__ == "__main__":
    main()
