#!/usr/bin/env python3
"""Generate the driver's own resolve shader blobs (DXVK262-T06).

The executor's resolve is a draw the DRIVER emits, so its stages have to live
in the driver rather than in a probe's payload: this compiles one averaging
fragment stage per served sample count, plus the oversized-triangle vertex stage
they pair with, into a header the SDK build compiles in. Nothing here decides
anything at run time - the executor still refuses every resolve shape it has not
measured.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def variant_source(samples: int) -> str:
    """The averaging stage for one sample count, written out, not looped."""
    reads = " + ".join(f"subpassLoad(imageMS, {index})" for index in range(samples))
    scale = f"{1.0 / samples:.6f}".rstrip("0").rstrip(".")
    return (
        "#version 440\n"
        "/* DXVK262-T06 driver resolve: read every sample of the multisampled\n"
        f" * input attachment and write their average. Generated for {samples}x because\n"
        " * the profile's fragment interface has only been measured on straight-line\n"
        " * reads. */\n"
        "layout(location = 0) out vec4 fs_out_color;\n"
        "layout(set = 0, binding = 0, input_attachment_index = 0) uniform subpassInputMS imageMS;\n"
        "void main (void)\n"
        "{\n"
        f"    vec4 sum = {reads};\n"
        f"    fs_out_color = sum * {scale};\n"
        "}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--samples", default="2,4")
    # The same lookup every other shader tool on this line uses: an explicit
    # PS5VK_GLSLANG, then whatever PATH carries (the CI image installs
    # glslang-tools), then the project-local toolchain the Makefile prefers.
    # A hard-coded ~/.local path made this step fail on a machine whose glslang
    # is installed normally, which is exactly what the compiler-contracts job
    # hit.
    parser.add_argument("--compiler",
                        default=os.environ.get("PS5VK_GLSLANG") or
                        shutil.which("glslangValidator") or
                        str(ROOT / "build/runtime-graphics/toolchain/usr/bin/glslangValidator"))
    args = parser.parse_args()
    # The Makefile hands over its own $(GLSLANG), which may be a bare name that
    # only PATH can resolve - the same shape every other tool on this line
    # accepts. A path is checked as a path, a name through PATH.
    compiler = (args.compiler if os.sep in args.compiler or Path(args.compiler).is_absolute()
                else shutil.which(args.compiler))
    if not compiler or not Path(compiler).is_file():
        raise SystemExit(f"glslangValidator not found at {args.compiler}: "
                         "set PS5VK_GLSLANG or install glslang-tools")
    args.compiler = compiler
    counts = [int(value) for value in args.samples.split(",") if value]
    work = args.out.parent
    work.mkdir(parents=True, exist_ok=True)

    vertex_src = ROOT / "experiments/graphics/runtime_sample_id.vert"
    vertex_spv = work / "resolve.vert.spv"
    subprocess.run([args.compiler, "-V", "--target-env", "vulkan1.0",
                    str(vertex_src), "-o", str(vertex_spv)], check=True)

    declarations = []
    for count in counts:
        source = work / f"resolve_{count}.frag"
        source.write_text(variant_source(count))
        binary = work / f"resolve_{count}.frag.spv"
        subprocess.run([args.compiler, "-V", "--target-env", "vulkan1.0", "-S", "frag",
                        str(source), "-o", str(binary)], check=True)
        words = struct.unpack(f"<{binary.stat().st_size // 4}I", binary.read_bytes())
        declarations.append(
            f"static const uint32_t ps5vk_resolve_fragment_{count}[]={{"
            + ",".join(f"0x{word:08x}u" for word in words) + "};")
    words = struct.unpack(f"<{vertex_spv.stat().st_size // 4}I", vertex_spv.read_bytes())
    declarations.append("static const uint32_t ps5vk_resolve_vertex[]={"
                        + ",".join(f"0x{word:08x}u" for word in words) + "};")
    args.out.write_text("\n".join(declarations) + "\n")
    print(f"wrote {args.out} with resolve fragments for {counts} and one vertex stage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
