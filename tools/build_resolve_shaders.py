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
    parser.add_argument("--compiler", default=str(Path.home() / ".local/bin/glslangValidator"))
    args = parser.parse_args()
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
