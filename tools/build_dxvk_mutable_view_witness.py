#!/usr/bin/env python3
"""Build the bounded public-SDK mutable-format view witness executable.

The SDK is built with the default-off PS5VK_DXVK_FORMAT_ROUTES_DIAGNOSTIC
switch, which makes the platform report VK_KHR_format_feature_flags2 and
VK_KHR_image_format_list (with the RGBA8 UNORM <-> SRGB mutable views) so the
witness can negotiate them through the public API. The switch is a
measurement build, never the shipping profile."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_sdk import get_ps5_toolchain  # noqa: E402
from lab import lab_root  # noqa: E402
from prepare_consumer_sync_shaders import emit_array  # noqa: E402

PROFILE = "dxvk-mutable-view-public-sdk-witness"
SWITCH = "PS5VK_DXVK_FORMAT_ROUTES_DIAGNOSTIC"
EXTENT = 16
PIXELS = EXTENT * EXTENT
SHADERS = {
    "mutable_view_vert_spirv": "experiments/graphics/consumer_cube_array.vert",
    "mutable_view_frag_spirv": "experiments/graphics/dxvk_mutable_view.frag",
}


def texels() -> bytes:
    """R and G each hold every byte value once, B is a bijective scramble of
    the same codes, and A is arbitrary (alpha is never sRGB-decoded)."""
    out = bytearray()
    for index in range(PIXELS):
        out += bytes((index, 255 - index, (index * 97 + 13) & 0xff, index ^ 0x5a))
    return bytes(out)


def srgb_decode_code(code: int) -> int:
    """IEC 61966-2-1 sRGB -> linear, then the UNORM8 target's
    round-to-nearest encode of the linear value."""
    value = code / 255.0
    linear = value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4
    return int(linear * 255.0 + 0.5)


def srgb_exact() -> bytes:
    """The exact result of sampling texels() through an SRGB view and
    writing it to an RGBA8 UNORM target: RGB decoded, alpha unchanged."""
    source = texels()
    return bytes(value if index % 4 == 3 else srgb_decode_code(value)
                 for index, value in enumerate(source))


def min_differing() -> int:
    """RGB bytes whose SRGB read must differ from the UNORM read even at the
    witness's one-code tolerance: the exact decode is at least two codes
    away from the raw byte."""
    source = texels()
    return sum(1 for index, value in enumerate(source)
               if index % 4 != 3 and abs(srgb_decode_code(value) - value) >= 2)


def checked_spirv(payload: bytes) -> None:
    """A plain Vulkan-1.0 SPIR-V module with the Shader capability only."""
    if len(payload) % 4:
        raise ValueError("SPIR-V length is not word aligned")
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    if len(words) < 7 or words[:2] != (0x07230203, 0x00010000):
        raise ValueError("witness requires SPIR-V 1.0")
    capabilities = set()
    index = 5
    while index < len(words):
        size, opcode = words[index] >> 16, words[index] & 0xffff
        if not size or index + size > len(words):
            raise ValueError("malformed SPIR-V instruction stream")
        if opcode == 17 and size == 2:  # OpCapability
            capabilities.add(words[index + 1])
        index += size
    if capabilities != {1}:
        raise ValueError("witness shaders use the Shader capability only")


def c_bytes(name: str, payload: bytes) -> str:
    rows = [",".join(str(value) for value in payload[offset:offset + 32])
            for offset in range(0, len(payload), 32)]
    return f"static const uint8_t {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def data_header() -> str:
    return ("#include <stdint.h>\n" + c_bytes("mutable_view_texels", texels()) +
            c_bytes("mutable_view_srgb_exact", srgb_exact()) +
            f"#define MUTABLE_VIEW_MIN_DIFFERING {min_differing()}u\n")


def run(*command: str, env: dict | None = None) -> None:
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def main() -> None:
    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    sdk, clang_wrapper = get_ps5_toolchain()
    if not sdk or not clang_wrapper:
        raise SystemExit("PS5 native toolchain is required")
    builder = foundation / "build/host/ps5-native-tool"
    glslang = shutil.which("glslangValidator")
    if not glslang or not builder.is_file():
        raise SystemExit("glslangValidator and ps5-native-tool are required")
    logger = lab / "projects/logging_server/client"
    build = ROOT / "build/dxvk-mutable-view-witness"
    dist = build / "dist/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    arrays = []
    shader_hashes = {}
    for name, relative in SHADERS.items():
        target = build / f"{name}.spv"
        run(glslang, "-V", "--target-env", "vulkan1.0", str(ROOT / relative), "-o", str(target))
        payload = target.read_bytes()
        checked_spirv(payload)
        arrays.append(emit_array(name, payload))
        shader_hashes[name] = hashlib.sha256(payload).hexdigest()
    (build / "dxvk_mutable_view_shaders.h").write_text(
        "#include <stdint.h>\n" + "\n".join(arrays), encoding="utf-8")
    (build / "dxvk_mutable_view_data.h").write_text(data_header(), encoding="utf-8")

    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk), **{SWITCH: "1"})
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / "examples/dxvk_mutable_view_witness/main.c"
    obj = build / "main.o"
    dep = build / "main.d"
    run("sh", str(clang_wrapper), "-std=c11", "-O2", "-g", "-Wall",
        "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        "-MD", "-MP", "-MF", str(dep),
        "-I" + str(staged / "include"), "-I" + str(build),
        "-I" + str(logger), "-c", str(source), "-o", str(obj), env=sdk_env)
    dependencies = dep.read_text()
    if str(ROOT / "src/") in dependencies or str(ROOT / "native/") in dependencies:
        raise RuntimeError("witness includes a private runtime header")
    undefined = subprocess.check_output(["nm", "-u", str(obj)], text=True)
    if any(line.split()[-1].startswith(("ps5vk_", "ps5_"))
           for line in undefined.splitlines() if line.split()):
        raise RuntimeError("witness calls a private runtime symbol")

    linker = sdk / "bin/prospero-lld"
    pie = build / "witness_pie.elf"
    eboot_elf = build / "eboot.elf"
    eboot = dist / "eboot.bin"
    link = [str(linker), "-L" + str(sdk / "target/lib"),
            "-T", str(staged / "lib/ps5-pie.ld"), "--eh-frame-hdr",
            "--version-script", str(staged / "lib/app-symbols.map"),
            "-Map=" + str(build / "witness.map"), "-e", "_start", "-o", str(pie),
            str(staged / "lib/crt.o"), str(obj),
            str(staged / "lib/libps5vk.a"), str(staged / "lib/libpsbc.a"),
            *[str(sdk / f"target/lib/{name}") for name in
              ("libc++.a", "libc++abi.a", "libunwind.a", "libpthread.a", "libc.a")],
            "--as-needed",
            *sorted(str(path) for path in (sdk / "target/lib").glob("*.so")),
            str(staged / "lib/libSceAgc.so"),
            str(staged / "lib/libSceAgcDriver.so")]
    run(*link)
    run(str(builder), "link", "--in", str(pie), "--out", str(eboot_elf),
        "--stub-dir", str(sdk / "target/lib"), "--module-sdk", "0x02000009",
        "--stub", str(staged / "lib/libSceAgc.so"),
        "--stub", str(staged / "lib/libSceAgcDriver.so"),
        "--companion-sdk", "0x08050001", "--file-name", "eboot.elf")
    run(str(builder), "self", "--sign", "--in", str(eboot_elf), "--out",
        str(eboot), "--magic", "0x1D3D154F")
    # Leave the ordinary staged SDK behind for every other consumer.
    run(sys.executable, str(ROOT / "tools/build_sdk.py"),
        env=dict(os.environ, PS5_PAYLOAD_SDK=str(sdk)))

    param = json.loads((lab / "projects/ps5-agc-gears/sce_sys/param.json").read_text())
    param.update(titleId="PPSA99994", conceptId="99994",
                 contentId="UP9000-PPSA99994_00-PS5VKMUTVIEW0001")
    param["localizedParameters"]["en-US"]["titleName"] = "PS5 Vulkan Mutable View Witness"
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {
        "profile": PROFILE, "extent": EXTENT, "diagnostic_switch": SWITCH,
        "min_differing": min_differing(),
        "texels_sha256": hashlib.sha256(texels()).hexdigest(),
        "srgb_exact_sha256": hashlib.sha256(srgb_exact()).hexdigest(),
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "shader_sha256": shader_hashes,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    }
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
