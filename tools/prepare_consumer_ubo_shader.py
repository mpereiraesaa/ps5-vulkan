#!/usr/bin/env python3
"""Generate the public consumer's compact UBO compute shader header."""

import argparse
import struct
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "experiments/compute/ubo_standard_layout.comp"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        spirv = Path(directory) / "ubo_standard_layout.spv"
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.0",
                        str(SOURCE), "-o", str(spirv)], check=True,
                       capture_output=True, text=True)
        payload = spirv.read_bytes()
    if len(payload) % 4:
        raise ValueError("SPIR-V byte count is not word aligned")
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    if words[0] != 0x07230203 or words[1] > 0x00010300:
        raise ValueError("unexpected SPIR-V module or version")
    rows = ["    " + ", ".join(f"0x{word:08x}u" for word in words[at:at + 8])
            for at in range(0, len(words), 8)]
    output = (
        "/* Generated from experiments/compute/ubo_standard_layout.comp. */\n"
        "#ifndef PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_SHADER_H\n"
        "#define PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_SHADER_H\n\n"
        "#include <stdint.h>\n\n"
        "static const uint32_t consumer_ubo_standard_layout_spirv[] = {\n"
        + ",\n".join(rows) + "\n};\n\n#endif\n"
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(output, encoding="utf-8")
    args.out.with_suffix(".spv").write_bytes(payload)


if __name__ == "__main__":
    main()
