#!/usr/bin/env python3
"""Generate typed SPIR-V modules for the native uniform-texel witness."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

from prepare_consumer_sync_shaders import emit_array

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "experiments/compute/texel_formats.comp"
VARIANTS = {
    "float": None,
    "uint": "TEXEL_UINT=1",
    "sint": "TEXEL_SINT=1",
}


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
    header = "/* Generated from experiments/compute/texel_formats.comp. */\n#include <stdint.h>\n"
    for name, define in VARIANTS.items():
        spirv = args.out.with_suffix(f".{name}.spv")
        command = [compiler, "-V", str(SOURCE), "-o", str(spirv)]
        if define:
            command[1:1] = [f"-D{define}"]
        subprocess.run(command, check=True)
        data = spirv.read_bytes()
        header += emit_array(f"consumer_texel_{name}_spirv", data)
        header += (f'#define CONSUMER_TEXEL_{name.upper()}_SHA256 "'
                   f'{hashlib.sha256(data).hexdigest()}"\n')
    args.out.write_text(header)


if __name__ == "__main__":
    main()
