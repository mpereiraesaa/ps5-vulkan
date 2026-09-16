#!/usr/bin/env python3
"""Generate the owned shader-draw-parameter SPIR-V pair for the SDK consumer."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
from prepare_consumer_sync_shaders import emit_array

ROOT = Path(__file__).resolve().parents[1]


def compile_shader(compiler: str, source: Path, output: Path) -> bytes:
    subprocess.run([compiler, "-V", "--target-env", "vulkan1.0", str(source),
                    "-o", str(output)], check=True)
    return output.read_bytes()


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

    vertex_source = ROOT / "experiments/graphics/runtime_draw_parameters.vert"
    fragment_source = ROOT / "experiments/graphics/runtime_vertex_format.frag"
    vertex = compile_shader(compiler, vertex_source, args.out.with_suffix(".vert.spv"))
    fragment = compile_shader(compiler, fragment_source, args.out.with_suffix(".frag.spv"))
    # The indirect/indexed witness (DXVK262-T03) shares the fragment stage and
    # adds its own vertex shader plus the compute shader that generates draw
    # arguments on the GPU.
    witness_source = ROOT / "experiments/graphics/runtime_indirect_witness.vert"
    arguments_source = ROOT / "experiments/compute/indirect_arguments.comp"
    witness = compile_shader(compiler, witness_source,
                             args.out.with_name("indirect_witness.vert.spv"))
    arguments = compile_shader(compiler, arguments_source,
                               args.out.with_name("indirect_arguments.comp.spv"))

    header = ("/* Generated from experiments/graphics/runtime_draw_parameters.vert,\n"
              " * experiments/graphics/runtime_vertex_format.frag,\n"
              " * experiments/graphics/runtime_indirect_witness.vert and\n"
              " * experiments/compute/indirect_arguments.comp. */\n"
              "#include <stdint.h>\n")
    header += emit_array("consumer_draw_parameters_vert_spirv", vertex)
    header += emit_array("consumer_draw_parameters_frag_spirv", fragment)
    header += emit_array("consumer_indirect_witness_vert_spirv", witness)
    header += emit_array("consumer_indirect_arguments_comp_spirv", arguments)
    header += ('#define CONSUMER_DRAW_PARAMETERS_VERT_SPIRV_SHA256 "'
               + hashlib.sha256(vertex).hexdigest() + '"\n')
    header += ('#define CONSUMER_DRAW_PARAMETERS_FRAG_SPIRV_SHA256 "'
               + hashlib.sha256(fragment).hexdigest() + '"\n')
    header += ('#define CONSUMER_INDIRECT_WITNESS_VERT_SPIRV_SHA256 "'
               + hashlib.sha256(witness).hexdigest() + '"\n')
    header += ('#define CONSUMER_INDIRECT_ARGUMENTS_COMP_SPIRV_SHA256 "'
               + hashlib.sha256(arguments).hexdigest() + '"\n')
    args.out.write_text(header)


if __name__ == "__main__":
    main()
