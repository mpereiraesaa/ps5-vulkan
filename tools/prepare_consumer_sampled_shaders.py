#!/usr/bin/env python3
"""Generate owned weighted sampler-array SPIR-V for the public SDK consumer."""
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
    compiler = os.environ.get("PS5VK_GLSLANG") or (str(local) if local.is_file() else shutil.which("glslangValidator"))
    if not compiler:
        raise SystemExit("glslangValidator is required")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    header = "/* Generated from owned GLSL. */\n#include <stdint.h>\n"
    for source, suffix, name in (
        ("runtime_sampled_sets.frag", ".spv", "consumer_sampled_sets_spirv"),
        ("runtime_single_set_samplers.frag", ".single.spv", "consumer_single_set_spirv"),
        ("runtime_shared_sets.vert", ".shared.vert.spv", "consumer_shared_vertex_spirv"),
        ("runtime_shared_sets.frag", ".shared.frag.spv", "consumer_shared_fragment_spirv"),
    ):
        spirv = args.out.with_suffix(suffix)
        subprocess.run([compiler, "-V", str(ROOT / "experiments/graphics" / source),
                        "-o", str(spirv)], check=True)
        data = spirv.read_bytes()
        header += emit_array(name, data)
        header += f'#define {name.upper()}_SHA256 "{hashlib.sha256(data).hexdigest()}"\n'
    args.out.write_text(header)

if __name__ == "__main__":
    main()
