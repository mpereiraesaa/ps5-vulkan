"""Generate an owned SPIR-V fixture header; native PSBC compiles its ISA at runtime."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--out",required=True,type=Path)
    args=parser.parse_args()
    compiler=os.environ.get("PS5VK_GLSLANG") or shutil.which("glslangValidator")
    if not compiler:
        raise SystemExit("Set PS5VK_GLSLANG to glslangValidator or install glslang-tools")
    args.out.parent.mkdir(parents=True,exist_ok=True)
    declarations=["/* Generated from owned GLSL. Contains SPIR-V, not GPU machine code. */"]
    modules = (
        ("experiments/graphics/runtime_triangle.vert", "runtime_triangle.vert.spv", "vertex"),
        ("experiments/graphics/runtime_triangle.frag", "runtime_triangle.frag.spv", "fragment"),
        ("experiments/graphics/runtime_vertex_sint.vert", "runtime_vertex_sint.vert.spv", "vertex_sint"),
        ("experiments/graphics/runtime_vertex_uint.vert", "runtime_vertex_uint.vert.spv", "vertex_uint"),
        ("experiments/graphics/runtime_vertex_unorm.vert", "runtime_vertex_unorm.vert.spv", "vertex_unorm"),
        ("experiments/graphics/runtime_vertex_format.frag", "runtime_vertex_format.frag.spv", "vertex_format_fragment"),
    )
    for source_name,binary_name,stage in modules:
        binary=args.out.parent/binary_name
        subprocess.run([compiler,"-V",str(ROOT/source_name),"-o",str(binary)],check=True)
        data=binary.read_bytes()
        words=struct.unpack(f"<{len(data)//4}I",data)
        declarations.append(f"static const uint32_t ps5vk_runtime_{stage}[]={{"+
                            ",".join(f"0x{word:08x}u" for word in words)+"};")
    args.out.write_text("\n".join(declarations)+"\n")

if __name__=="__main__":
    main()
