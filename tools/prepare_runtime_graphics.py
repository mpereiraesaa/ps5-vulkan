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
    # gl_ViewIndex needs the ViewIndex built-in and the MultiView capability,
    # which belong to SPIR-V 1.3: this one module is compiled for the Vulkan 1.1
    # target environment, exactly as the compiler fork's own ViewIndex
    # regression is (opengnm-psbc Makefile's view-index rule). Every other
    # module keeps the default Vulkan 1.0 target this driver has always used.
    module_flags = {"view_index": ("--target-env", "vulkan1.1"),
                    "view_index_instance": ("--target-env", "vulkan1.1")}
    modules = (
        ("experiments/graphics/runtime_vertex_bindings_probe.vert", "runtime_vertex_bindings_probe.vert.spv", "vertex_bindings"),
        ("experiments/graphics/runtime_triangle.vert", "runtime_triangle.vert.spv", "vertex"),
        ("experiments/graphics/runtime_triangle.frag", "runtime_triangle.frag.spv", "fragment"),
        ("experiments/graphics/runtime_view_index.vert", "runtime_view_index.vert.spv", "view_index"),
        ("experiments/graphics/runtime_view_index_instance.vert", "runtime_view_index_instance.vert.spv", "view_index_instance"),
        ("experiments/graphics/runtime_vertex_sint.vert", "runtime_vertex_sint.vert.spv", "vertex_sint"),
        ("experiments/graphics/runtime_vertex_uint.vert", "runtime_vertex_uint.vert.spv", "vertex_uint"),
        ("experiments/graphics/runtime_vertex_unorm.vert", "runtime_vertex_unorm.vert.spv", "vertex_unorm"),
        ("experiments/graphics/runtime_vertex_format.frag", "runtime_vertex_format.frag.spv", "vertex_format_fragment"),
        ("experiments/graphics/runtime_texture.frag", "runtime_texture.frag.spv", "texture_fragment"),
        ("experiments/graphics/runtime_mipmap.vert", "runtime_mipmap.vert.spv", "mipmap_vertex"),
    )
    for source_name,binary_name,stage in modules:
        binary=args.out.parent/binary_name
        subprocess.run([compiler,"-V",*module_flags.get(stage,()),str(ROOT/source_name),
                        "-o",str(binary)],check=True)
        data=binary.read_bytes()
        words=struct.unpack(f"<{len(data)//4}I",data)
        declarations.append(f"static const uint32_t ps5vk_runtime_{stage}[]={{"+
                            ",".join(f"0x{word:08x}u" for word in words)+"};")
    args.out.write_text("\n".join(declarations)+"\n")

if __name__=="__main__":
    main()
