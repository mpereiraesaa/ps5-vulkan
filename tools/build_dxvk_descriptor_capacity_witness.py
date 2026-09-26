#!/usr/bin/env python3
"""Build the bounded public-SDK descriptor-set capacity witnesses.

--variant capacity: 1024 distinct sampled images; a compute set of exactly
1024 descriptors (SAMPLED_IMAGE[1023] plus the output buffer), a second one
of UNIFORM_TEXEL_BUFFER[1023] plus the output buffer, and a fragment set of
SAMPLED_IMAGE[1024]. --variant dynamic: per-dispatch and per-draw
dynamic offsets, a live rewrite of a bound set and the refusal of the stale
command buffer. Both use the ordinary SDK; they measure a capacity the device
does not report yet. Shaders use only constant descriptor indices.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_sdk import get_ps5_toolchain  # noqa: E402
from lab import lab_root  # noqa: E402
from prepare_consumer_sync_shaders import emit_array  # noqa: E402
from cts_heap_parameters import use_application_heap  # noqa: E402

IMAGES = 1024
SIDE = 32
DRAWS, STRIDE = 4, 256
VERTEX = "experiments/graphics/consumer_cube_array.vert"
BISECT = (128, 256, 512, 768, 1023)
PROFILES = {"bisect": "descriptor-bisect-public-sdk-witness",
            "capacity": "descriptor-capacity-public-sdk-witness",
            "dynamic": "descriptor-dynamic-public-sdk-witness"}
CONTENT = {"bisect": "UP9000-PPSA99994_00-PS5VKDSBIS000001",
           "capacity": "UP9000-PPSA99994_00-PS5VKDSCAP000001",
           "dynamic": "UP9000-PPSA99994_00-PS5VKDSDYN000001"}


def capacity_compute_source(images: int = IMAGES - 1) -> str:
    lines = ["#version 450", "#extension GL_EXT_samplerless_texture_functions : require",
             "layout(local_size_x=1) in;",
             f"layout(set=0,binding=0) uniform texture2D images[{images}];",
             "layout(set=0,binding=1,std430) writeonly buffer Out { uint v[]; } o;",
             "void main()", "{"]
    lines += [f"    o.v[{k}]=packUnorm4x8(texelFetch(images[{k}],ivec2(0),0));"
              for k in range(images)]
    return "\n".join(lines + ["}"]) + "\n"


def capacity_texel_source(buffers: int = IMAGES - 1) -> str:
    lines = ["#version 450", "#extension GL_EXT_samplerless_texture_functions : require",
             "layout(local_size_x=1) in;",
             f"layout(set=0,binding=0) uniform utextureBuffer texels[{buffers}];",
             "layout(set=0,binding=1,std430) writeonly buffer Out { uint v[]; } o;",
             "void main()", "{"]
    lines += [f"    o.v[{k}]=texelFetch(texels[{k}],0).x;" for k in range(buffers)]
    return "\n".join(lines + ["}"]) + "\n"


def capacity_fragment_source() -> str:
    lines = ["#version 450", "#extension GL_EXT_samplerless_texture_functions : require",
             f"layout(set=0,binding=0) uniform texture2D images[{IMAGES}];",
             "layout(location=0) out vec4 color;", "void main()", "{",
             f"    uint k=uint(gl_FragCoord.y)*{SIDE}u+uint(gl_FragCoord.x);",
             "    vec4 c=vec4(0.0);", "    switch(k) {"]
    lines += [f"    case {k}u: c=texelFetch(images[{k}],ivec2(0),0); break;"
              for k in range(IMAGES)]
    return "\n".join(lines + ["    }", "    color=c;", "}"]) + "\n"


DYNAMIC_COMPUTE = """#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0) uniform U { uvec4 v; } u;
layout(set=0,binding=1,std430) writeonly buffer O { uvec4 v; } o;
void main() { o.v = u.v; }
"""
DYNAMIC_FRAGMENT = """#version 450
layout(set=0,binding=0) uniform U { uvec4 v; } u;
layout(location=0) out vec4 color;
void main() { color = unpackUnorm4x8(u.v.x); }
"""


def texel(k: int) -> int:
    return ((k & 255) | (((k >> 8) | 0x40) << 8) | (((k * 37) & 255) << 16) |
            ((255 - (k & 255)) << 24))


def texel_word(k: int) -> int:
    return texel(k) ^ 0x5a5a5a5a


def source_word(s: int, i: int, c: int) -> int:
    return (0xb0000000 if s else 0xa0000000) | (i << 8) | (c << 4) | 0x5


def shader_sources() -> dict:
    return {"descriptor_capacity_comp_spirv": ("comp", capacity_compute_source()),
            "descriptor_capacity_frag_spirv": ("frag", capacity_fragment_source()),
            "descriptor_capacity_texel_spirv": ("comp", capacity_texel_source()),
            "descriptor_dynamic_comp_spirv": ("comp", DYNAMIC_COMPUTE),
            "descriptor_dynamic_frag_spirv": ("frag", DYNAMIC_FRAGMENT),
            "descriptor_capacity_vert_spirv": ("vert", (ROOT / VERTEX).read_text()),
            **{f"descriptor_bisect_{n}_spirv": ("comp", capacity_compute_source(images))
               for n, images in enumerate(BISECT)}}


def compile_shaders(glslang: str, build: Path) -> dict:
    payloads = {}
    for name, (stage, text) in shader_sources().items():
        source = build / f"{name}.{stage}"
        source.write_text(text)
        target = build / f"{name}.spv"
        subprocess.run([glslang, "-V", "--target-env", "vulkan1.0", str(source), "-o",
                        str(target)], cwd=ROOT, check=True, capture_output=True)
        payloads[name] = target.read_bytes()
    return payloads


def run(*command: str, env: dict | None = None) -> None:
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--variant", choices=sorted(PROFILES))
    parser.add_argument("--shaders-only", type=Path,
                        help="only compile the witness shaders into this directory")
    args = parser.parse_args()
    if args.shaders_only:
        glslang = shutil.which("glslangValidator")
        if not glslang:
            raise SystemExit("glslangValidator is required")
        args.shaders_only.mkdir(parents=True, exist_ok=True)
        compile_shaders(glslang, args.shaders_only)
        return
    if not args.variant:
        parser.error("--variant is required")
    variant = args.variant
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
    build = ROOT / f"build/descriptor-{variant}-witness"
    dist = build / "dist/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)
    payloads = compile_shaders(glslang, build)
    (build / "descriptor_capacity_shaders.h").write_text(
        "#include <stdint.h>\n" + "\n".join(emit_array(n, p) for n, p in payloads.items()),
        encoding="utf-8")

    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / "examples/dxvk_descriptor_capacity_witness/main.c"
    obj = build / "main.o"
    dep = build / "main.d"
    run("sh", str(clang_wrapper), "-std=c11", "-O2", "-g", "-Wall",
        "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        "-MD", "-MP", "-MF", str(dep),
        *(["-DDESCRIPTOR_WITNESS_DYNAMIC=1"] if variant == "dynamic" else []),
        *(["-DDESCRIPTOR_WITNESS_BISECT=1"] if variant == "bisect" else []),
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
    # Application heap mode, as the DXVK payload uses: the foundation's
    # internal-memory libc heap (about 13 MiB) cannot hold a 1024-descriptor
    # compile.
    eboot_elf.write_bytes(use_application_heap(eboot_elf.read_bytes()))
    run(str(builder), "self", "--sign", "--in", str(eboot_elf), "--out",
        str(eboot), "--magic", "0x1D3D154F")

    param = json.loads((lab / "projects/ps5-agc-gears/sce_sys/param.json").read_text())
    param.update(titleId="PPSA99994", conceptId="99994", contentId=CONTENT[variant])
    param["localizedParameters"]["en-US"]["titleName"] = (
        f"PS5 Vulkan Descriptor {variant.title()} Witness")
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {
        "profile": PROFILES[variant], "variant": variant,
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "shader_sha256": {n: hashlib.sha256(p).hexdigest() for n, p in payloads.items()},
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    }
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
