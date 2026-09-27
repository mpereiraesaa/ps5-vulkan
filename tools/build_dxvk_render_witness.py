#!/usr/bin/env python3
"""Build the bounded public-SDK DXVK first-draw recording witness (DXVK262-T10).

Dynamic rendering, copy_commands2, maintenance1 and extended dynamic state all
ship, so the default witness uses the ordinary SDK. --cache-control builds an
explicit diagnostic SDK variant for compute/graphics cold misses, warm
derivatives and discard execution. --inline-uniform adds the inline compute
update/copy/template oracle and the inline fragment image oracle. Neither build
is hardware evidence."""

import argparse
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


def run(*command: str, env: dict | None = None) -> None:
    subprocess.run(command, cwd=ROOT, env=env, check=True)


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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    variants = parser.add_mutually_exclusive_group()
    variants.add_argument("--cache-control", action="store_true",
                        help="build the diagnostic compute/graphics cache execution variant")
    variants.add_argument("--inline-uniform", action="store_true",
                          help="build the diagnostic inline compute and graphics execution variant")
    parser.add_argument("--inline-graphics-boundary", choices=("vertex", "fragment"),
                        help="exercise four256-byte blocks in the selected graphics stage")
    args = parser.parse_args()
    if args.inline_graphics_boundary and not args.inline_uniform:
        parser.error("--inline-graphics-boundary requires --inline-uniform")
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
    name = ("dxvk-inline-witness" if args.inline_uniform else
            "dxvk-cache-witness" if args.cache_control else "dxvk-render-witness")
    if args.inline_graphics_boundary:
        name += "-" + args.inline_graphics_boundary + "-boundary"
    build = ROOT / "build" / name
    dist = ROOT / ("dist-" + name) / "PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    shaders = {
        "dxvk_render_witness_vert_spirv": ROOT / "experiments/graphics/dxvk_render_witness.vert",
        "dxvk_render_witness_frag_spirv": ROOT / "experiments/graphics/dxvk_render_witness.frag",
    }
    if args.cache_control:
        shaders["dxvk_cache_compute_spirv"] = ROOT / "experiments/compute/cache_witness.comp"
    if args.inline_uniform:
        shaders["dxvk_render_witness_frag_spirv"] = ROOT / "experiments/graphics/dxvk_inline_witness.frag"
        shaders["dxvk_inline_compute_spirv"] = ROOT / "experiments/compute/inline_witness.comp"
        shaders["dxvk_inline_boundary_spirv"] = ROOT / "experiments/compute/inline_boundary.comp"
        shaders["dxvk_inline_split_spirv"] = ROOT / "experiments/compute/inline_split.comp"
    if args.inline_graphics_boundary:
        shaders["dxvk_render_witness_frag_spirv"] = ROOT / "experiments/graphics/dxvk_render_witness.frag"
        stage = "vert" if args.inline_graphics_boundary == "vertex" else "frag"
        shaders["dxvk_render_witness_" + stage + "_spirv"] = ROOT / ("experiments/graphics/dxvk_inline_boundary." + stage)
    arrays = []
    shader_hashes = {}
    for name, shader_source in shaders.items():
        target = build / f"{name}.spv"
        run(glslang, "-V", "--target-env", "vulkan1.0", str(shader_source),
            "-o", str(target))
        payload = target.read_bytes()
        checked_spirv(payload)
        arrays.append(emit_array(name, payload))
        shader_hashes[name] = hashlib.sha256(payload).hexdigest()
    (build / "dxvk_render_witness_shaders.h").write_text(
        "#include <stdint.h>\n" + "\n".join(arrays), encoding="utf-8")

    # Only explicit variants enable their unpromoted diagnostic routes.
    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    # Refuse ambient diagnostics: the receipt must describe the actual SDK.
    for key, value in sdk_env.items():
        if key.startswith("PS5VK_") and "DIAGNOSTIC" in key and value != "0":
            raise SystemExit(f"unset ambient diagnostic {key} before building witness")
    sdk_env["PS5VK_PIPELINE_CACHE_CONTROL_DIAGNOSTIC"] = "1" if args.cache_control else "0"
    sdk_env["PS5VK_INLINE_UNIFORM_DIAGNOSTIC"] = "1" if args.inline_uniform else "0"
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / "examples/dxvk_render_witness/main.c"
    obj = build / "main.o"
    dep = build / "main.d"
    run("sh", str(clang_wrapper), "-std=c11", "-O2", "-g", "-Wall",
        "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        *(["-DPS5VK_CACHE_CONTROL_WITNESS=1"] if args.cache_control else []),
        *(["-DPS5VK_INLINE_UNIFORM_WITNESS=1"] if args.inline_uniform else []),
        *(["-DPS5VK_INLINE_GRAPHICS_STAGE=" + ("1" if args.inline_graphics_boundary == "vertex" else "16")]
          if args.inline_graphics_boundary else []),
        "-MD", "-MP", "-MF", str(dep),
        "-I" + str(staged / "include"), "-I" + str(build),
        "-I" + str(logger),
        "-c", str(source), "-o", str(obj), env=sdk_env)
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
                 contentId="UP9000-PPSA99994_00-PS5VKDR000000001")
    param["localizedParameters"]["en-US"]["titleName"] = (
        "PS5 Vulkan DXVK Render Witness")
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {
        "profile": ("dxvk-inline-public-sdk-witness" if args.inline_uniform else
                    "dxvk-cache-public-sdk-witness" if args.cache_control else "dxvk-render-public-sdk-witness"),
        "inline_execution_version": 5 if args.inline_uniform else None,
        "inline_graphics_stage": (args.inline_graphics_boundary or "small") if args.inline_uniform else None,
        "cache_execution_version": 2 if args.cache_control else None,
        "extent": 64, "format": "R8G8B8A8_UNORM",
        "diagnostic_switch": ("PS5VK_INLINE_UNIFORM_DIAGNOSTIC" if args.inline_uniform else
                              "PS5VK_PIPELINE_CACHE_CONTROL_DIAGNOSTIC" if args.cache_control else None),
        "inline_compute_sha256": hashlib.sha256((source.parent / "inline_compute.h").read_bytes()).hexdigest()
            if args.inline_uniform else None,
        "inline_graphics_sha256": hashlib.sha256((source.parent / "inline_graphics.h").read_bytes()).hexdigest()
            if args.inline_uniform else None,
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "shader_sha256": shader_hashes,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "cache_compute_sha256": hashlib.sha256((source.parent / "cache_compute.h").read_bytes()).hexdigest()
            if args.cache_control else None,
    }
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
