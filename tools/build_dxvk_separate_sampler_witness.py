#!/usr/bin/env python3
"""Build the bounded public-SDK separate-sampler and texel-buffer witness.

The ordinary staged SDK serves the storage-texel-buffer role on R32_UINT,
R8G8B8A8_UNORM and R32G32B32A32_SFLOAT; the witness creates typed UAV buffer
views through the public API. It is the regression witness for the promoted
role, so it selects no measurement switch.
"""

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

PROFILE = "dxvk-separate-sampler-public-sdk-witness"
SWITCH = None
WIDTH, HEIGHT = 8, 4
TEXELS = WIDTH * HEIGHT
SHADERS = {
    "separate_sampler_comp_spirv": "experiments/compute/dxvk_separate_sampler_witness.comp",
    "separate_sampler_vert_spirv": "experiments/graphics/consumer_cube_array.vert",
    "separate_sampler_frag_spirv": "experiments/graphics/dxvk_separate_sampler_witness.frag",
}
# SPIR-V capabilities: Shader, SampledBuffer, ImageBuffer, ImageQuery.
CAPABILITIES = {
    "separate_sampler_comp_spirv": {1, 46, 47, 50},
    "separate_sampler_vert_spirv": {1},
    "separate_sampler_frag_spirv": {1, 46, 50},
}


def texture() -> bytes:
    """8x4 RGBA8 texels, every byte distinct within its channel."""
    out = bytearray()
    for index in range(TEXELS):
        out += bytes(((index * 7 + 3) & 0xff, (index * 29 + 101) & 0xff,
                      (255 - index * 5) & 0xff, (index * 53 + 17) & 0xff))
    return bytes(out)


def fetch_values() -> list[int]:
    """The Buffer<uint> SRV contents: large enough to use every bit."""
    return [(0x9e3779b9 * (n + 1)) & 0xffffff for n in range(WIDTH)]


def texel(x: int, y: int) -> bytes:
    offset = 4 * (y * WIDTH + x)
    return texture()[offset:offset + 4]


def expected_u32() -> list[int]:
    """packUnorm4x8(row 1) ^ fetched ^ width: the unorm8 round trip is exact."""
    return [struct.unpack("<I", texel(i, 1))[0] ^ fetch_values()[i] ^ WIDTH
            for i in range(WIDTH)]


def expected_rgba8() -> bytes:
    """Row 2 stored through an R8G8B8A8_UNORM storage texel buffer."""
    return b"".join(texel(i, 2) for i in range(WIDTH))


def expected_rgba32f() -> bytes:
    """Integers below 2^24 as floats: exact."""
    return b"".join(struct.pack("<4f", float(fetch_values()[i]), float(WIDTH),
                                float(HEIGHT), float(i)) for i in range(WIDTH))


def expected_pixels() -> bytes:
    """The fragment pass: each pixel's own texel RGB, alpha the low byte of its
    column's texel-buffer value."""
    out = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            out += texel(x, y)[:3] + bytes((fetch_values()[x] & 0xff,))
    return bytes(out)


def checked_spirv(name: str, payload: bytes) -> None:
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
    if capabilities != CAPABILITIES[name]:
        raise ValueError(f"{name}: unexpected capabilities {sorted(capabilities)}")


def c_bytes(name: str, payload: bytes) -> str:
    rows = [",".join(str(value) for value in payload[offset:offset + 32])
            for offset in range(0, len(payload), 32)]
    return f"static const uint8_t {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def c_words(name: str, values: list[int]) -> str:
    return (f"static const uint32_t {name}[] = {{" +
            ", ".join(f"UINT32_C(0x{value:08x})" for value in values) + "};\n")


def data_header() -> str:
    return ("#include <stdint.h>\n" +
            f"#define SEPARATE_SAMPLER_WIDTH {WIDTH}u\n"
            f"#define SEPARATE_SAMPLER_HEIGHT {HEIGHT}u\n" +
            c_bytes("separate_sampler_texture", texture()) +
            c_words("separate_sampler_fetch", fetch_values()) +
            c_words("separate_sampler_expected_u32", expected_u32()) +
            c_bytes("separate_sampler_expected_rgba8", expected_rgba8()) +
            c_bytes("separate_sampler_expected_rgba32f", expected_rgba32f()) +
            c_bytes("separate_sampler_expected_pixels", expected_pixels()))


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
    build = ROOT / "build/dxvk-separate-sampler-witness"
    dist = build / "dist/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    arrays = []
    shader_hashes = {}
    for name, relative in SHADERS.items():
        target = build / f"{name}.spv"
        run(glslang, "-V", "--target-env", "vulkan1.0", str(ROOT / relative), "-o", str(target))
        payload = target.read_bytes()
        checked_spirv(name, payload)
        arrays.append(emit_array(name, payload))
        shader_hashes[name] = hashlib.sha256(payload).hexdigest()
    (build / "dxvk_separate_sampler_shaders.h").write_text(
        "#include <stdint.h>\n" + "\n".join(arrays), encoding="utf-8")
    (build / "dxvk_separate_sampler_data.h").write_text(data_header(), encoding="utf-8")

    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk), **({SWITCH: "1"} if SWITCH else {}))
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / "examples/dxvk_separate_sampler_witness/main.c"
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

    param = json.loads((lab / "projects/ps5-agc-gears/sce_sys/param.json").read_text())
    param.update(titleId="PPSA99994", conceptId="99994",
                 contentId="UP9000-PPSA99994_00-PS5VKSEPSMP00001")
    param["localizedParameters"]["en-US"]["titleName"] = (
        "PS5 Vulkan Separate Sampler Witness")
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {
        "profile": PROFILE, "width": WIDTH, "height": HEIGHT,
        "diagnostic_switch": SWITCH,
        "data_sha256": hashlib.sha256(data_header().encode()).hexdigest(),
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "shader_sha256": shader_hashes,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    }
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
