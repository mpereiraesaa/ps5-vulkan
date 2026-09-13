#!/usr/bin/env python3
"""Generate the public consumer's owned synchronization SPIR-V header."""

import argparse
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHADERS = ("sync_producer", "sync_consumer", "shared_atomic_multiwave")


def emit_array(name: str, payload: bytes) -> str:
    if len(payload) % 4:
        raise ValueError(f"{name}: SPIR-V byte count is not word aligned")
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    if not words or words[0] != 0x07230203:
        raise ValueError(f"{name}: invalid SPIR-V magic")
    rows = []
    for offset in range(0, len(words), 8):
        rows.append("    " + ", ".join(
            f"0x{word:08x}u" for word in words[offset:offset + 8]))
    return (f"static const uint32_t {name}[] = {{\n" +
            ",\n".join(rows) + "\n};\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    subprocess.run([sys.executable, str(ROOT / "tools/prepare_test_shaders.py")],
                   check=True)
    shader_dir = ROOT / "build/test-shaders"
    body = "\n".join(emit_array(f"consumer_{name}_spirv",
                                  (shader_dir / f"{name}.spv").read_bytes())
                     for name in SHADERS)
    text = (
        "/* Generated from owned GLSL fixtures; do not edit. */\n"
        "#ifndef PS5VK_CONSUMER_SYNC_SHADERS_H\n"
        "#define PS5VK_CONSUMER_SYNC_SHADERS_H\n\n"
        "#include <stdint.h>\n\n" + body +
        "\n#endif\n"
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
