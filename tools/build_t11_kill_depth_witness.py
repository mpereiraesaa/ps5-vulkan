#!/usr/bin/env python3
"""Build the bounded public-SDK pixel-removal (kill/terminate/demote) witness.

The regression witness for the driver's kill export-memory rule, built from
the ordinary staged SDK with no measurement switch.
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

OP_KILL, OP_TERMINATE, OP_DEMOTE = 252, 4416, 5380
CAP_SHADER, CAP_DEMOTE = 1, 5379
SPIRV_1_0, SPIRV_1_6 = 0x00010000, 0x00010600

# name -> (source, glslang arguments, SPIR-V version, capabilities, removal opcode)
SHADERS = {
    "t11_quad_vert_spirv": ("experiments/graphics/t09_depth_stencil_quad.vert", (),
                            SPIRV_1_0, {CAP_SHADER}, None),
    "t11_control_frag_spirv": ("experiments/graphics/runtime_depth_only.frag", (),
                               SPIRV_1_0, {CAP_SHADER}, None),
    "t11_kill_frag_spirv": ("experiments/graphics/runtime_depth_kill.frag", (),
                            SPIRV_1_0, {CAP_SHADER}, OP_KILL),
    "t11_terminate_frag_spirv": ("experiments/graphics/runtime_depth_kill.frag",
                                 ("--target-env", "vulkan1.3"),
                                 SPIRV_1_6, {CAP_SHADER}, OP_TERMINATE),
    "t11_demote_frag_spirv": ("experiments/graphics/runtime_depth_kill.frag",
                              ("--target-env", "vulkan1.3", "-DDEMOTE=1"),
                              SPIRV_1_6, {CAP_SHADER, CAP_DEMOTE}, OP_DEMOTE),
}


def run(*command: str, env: dict | None = None) -> None:
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def checked_spirv(payload: bytes, version: int, capabilities: set,
                  removal: int | None) -> None:
    """Exactly the form a case stands for: its version, its capabilities, its
    one removal instruction and none of the other two."""
    if len(payload) % 4:
        raise ValueError("SPIR-V length is not word aligned")
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    if len(words) < 7 or words[0] != 0x07230203 or words[1] != version:
        raise ValueError("unexpected SPIR-V version")
    declared, removals = set(), set()
    index = 5
    while index < len(words):
        size, opcode = words[index] >> 16, words[index] & 0xffff
        if not size or index + size > len(words):
            raise ValueError("malformed SPIR-V instruction stream")
        if opcode == 17 and size == 2:  # OpCapability
            declared.add(words[index + 1])
        if opcode in (OP_KILL, OP_TERMINATE, OP_DEMOTE):
            removals.add(opcode)
        index += size
    if declared != capabilities:
        raise ValueError("unexpected SPIR-V capabilities")
    if removals != ({removal} if removal is not None else set()):
        raise ValueError("unexpected pixel-removal instruction")


def strip_extensions(payload: bytes) -> bytes:
    """The module without its OpExtension instructions (DXVK's SPIR-V 1.6
    spelling of core demote declares none)."""
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    kept = list(words[:5])
    index = 5
    while index < len(words):
        size = words[index] >> 16
        if not size or index + size > len(words):
            raise ValueError("malformed SPIR-V instruction stream")
        if words[index] & 0xffff != 10:
            kept += words[index:index + size]
        index += size
    if len(kept) == len(words):
        raise ValueError("module declares no extension to strip")
    return struct.pack(f"<{len(kept)}I", *kept)


def build_witness(*, name: str, shaders: dict, header: str, profile: str,
                  content_id: str, title: str, extra: dict) -> Path:
    """Compile and check the shaders, stage the ordinary SDK, link and sign
    examples/<name>/main.c into dist-<name>/PPSA99994, and write its artifact.

    shaders maps an array name to (source, glslang arguments, SPIR-V version,
    capabilities, removal opcode, strip OpExtension)."""
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
    slug = name.replace("_", "-")
    build = ROOT / f"build/{slug}"
    dist = ROOT / f"dist-{slug}/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    arrays = []
    shader_hashes = {}
    for array, (shader, arguments, version, capabilities, removal, strip) in shaders.items():
        target = build / f"{array}.spv"
        run(glslang, "-V", *arguments, str(ROOT / shader), "-o", str(target))
        payload = target.read_bytes()
        if strip:
            payload = strip_extensions(payload)
            target.write_bytes(payload)
        checked_spirv(payload, version, capabilities, removal)
        arrays.append(emit_array(array, payload))
        shader_hashes[array] = hashlib.sha256(payload).hexdigest()
    (build / header).write_text(
        "#include <stdint.h>\n" + "\n".join(arrays), encoding="utf-8")

    # The ordinary staged SDK: the rule is part of the shipping draw path.
    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / f"examples/{name}/main.c"
    obj = build / "main.o"
    dep = build / "main.d"
    run("sh", str(clang_wrapper), "-std=c11", "-O2", "-g", "-Wall",
        "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
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
                 contentId=content_id)
    param["localizedParameters"]["en-US"]["titleName"] = title
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {
        "profile": profile,
        **extra,
        "diagnostic_switch": None,
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "shader_sha256": shader_hashes,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    }
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")
    return artifact_path


def main() -> None:
    build_witness(
        name="t11_kill_depth_witness",
        shaders={array: (*shape, False) for array, shape in SHADERS.items()},
        header="t11_kill_depth_shaders.h",
        profile="t11-kill-depth-public-sdk-witness",
        content_id="UP9000-PPSA99994_00-PS5VKKW000000001",
        title="PS5 Vulkan Pixel Removal Witness",
        extra={"extent": 64, "format": "D32_SFLOAT_S8_UINT",
               "cases": ["control", "kill", "terminate", "demote"]})


if __name__ == "__main__":
    main()
