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
    for extension,stage in (("vert","vertex"),("frag","fragment")):
        binary=args.out.parent/f"runtime_triangle.{extension}.spv"
        subprocess.run([compiler,"-V",str(ROOT/f"experiments/graphics/runtime_triangle.{extension}"),
                        "-o",str(binary)],check=True)
        data=binary.read_bytes()
        words=struct.unpack(f"<{len(data)//4}I",data)
        declarations.append(f"static const uint32_t ps5vk_runtime_{stage}[]={{"+
                            ",".join(f"0x{word:08x}u" for word in words)+"};")
    args.out.write_text("\n".join(declarations)+"\n")

if __name__=="__main__":
    main()
