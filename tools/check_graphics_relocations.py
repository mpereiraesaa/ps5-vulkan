"""Exercise the native C relocation implementation on an actual compiler image."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


class Relocation(ctypes.Structure):
    _fields_ = [(key, ctypes.c_uint32) for key in ("offset", "symbol_offset", "addend", "type")]


def main():
    parser = argparse.ArgumentParser(); parser.add_argument("manifest", type=Path)
    parser.add_argument("--agc-upload", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    filename = "agc-upload.unlinked.bin" if args.agc_upload else "upload.unlinked.bin"
    original = (args.manifest.parent / filename).read_bytes()
    expected_hash = manifest["agc_upload"]["image_sha256"] if args.agc_upload else manifest["upload_image_sha256"]
    if hashlib.sha256(original).hexdigest() != expected_hash:
        raise SystemExit("Image identity mismatch")
    out = ROOT / "build/tests"; out.mkdir(parents=True, exist_ok=True)
    library = out / "shader_relocate_host.so"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-shared", "-fPIC", "-Isrc", "src/shader_relocate.c", "-o", str(library)],
                   cwd=ROOT, check=True)
    native = ctypes.CDLL(str(library)).ps5vk_shader_relocate
    native.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64,
                       ctypes.POINTER(Relocation), ctypes.c_size_t]
    native.restype = ctypes.c_int
    records = manifest["agc_upload"]["relocations"] if args.agc_upload else manifest["image_relocations"]
    array = (Relocation * len(records))(*(Relocation(**record) for record in records))
    for base in (0x100000000, 0x1fffffd00, 0x300000000):
        image = ctypes.create_string_buffer(original, len(original))
        if native(image, len(original), base, array, len(records)):
            raise SystemExit("Native relocation rejected audited compiler image")
        expected = bytearray(original)
        for r in records:
            address = base + r["symbol_offset"] + r["addend"]
            value = address & 0xffffffff if r["type"] == 1 else address >> 32
            struct.pack_into("<I", expected, r["offset"], value)
        if image.raw != expected:
            raise SystemExit("Wrong relocated address or corrupted code/constants")
    print(f"Real compiler image: {len(records)} relocations at three bases pass; host only, no GPU execution")


if __name__ == "__main__":
    main()
