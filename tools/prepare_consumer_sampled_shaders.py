#!/usr/bin/env python3
"""Generate owned weighted sampler-array SPIR-V for the public SDK consumer."""
import argparse
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
    compiler = os.environ.get("PS5VK_GLSLANG") or (str(local) if local.is_file() else shutil.which("glslangValidator"))
    if not compiler:
        raise SystemExit("glslangValidator is required")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    spirv = args.out.with_suffix(".spv")
    subprocess.run([compiler, "-V", str(ROOT / "experiments/graphics/runtime_sampled_sets.frag"),
                    "-o", str(spirv)], check=True)
    args.out.write_text("/* Generated from owned GLSL. */\n#include <stdint.h>\n" +
                        emit_array("consumer_sampled_sets_spirv", spirv.read_bytes()))

if __name__ == "__main__":
    main()
