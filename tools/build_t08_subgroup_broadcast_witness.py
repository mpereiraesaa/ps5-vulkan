#!/usr/bin/env python3
"""Build the bounded SDK compute Broadcast diagnostic witness."""

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
    if len(payload) % 4:
        raise ValueError("SPIR-V length is not word aligned")
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    if len(words) < 7 or words[0] != 0x07230203:
        raise ValueError("witness requires SPIR-V")
    capabilities = set()
    subgroup_ops = []
    entry_models = []
    index = 5
    while index < len(words):
        size, opcode = words[index] >> 16, words[index] & 0xffff
        if not size or index + size > len(words):
            raise ValueError("malformed SPIR-V instruction stream")
        operands = words[index + 1:index + size]
        if opcode == 17 and size == 2:  # OpCapability
            capabilities.add(operands[0])
        elif opcode == 15:  # OpEntryPoint
            entry_models.append(operands[0])
        elif 333 <= opcode <= 366:
            subgroup_ops.append(opcode)
        index += size
    if (not {61, 64}.issubset(capabilities) or subgroup_ops != [337] or
            entry_models != [5]):
        raise ValueError("shader lacks compute GroupNonUniformBroadcast contract")


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
    build = ROOT / "build/t08-subgroup-broadcast-witness"
    dist = build / "dist/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    shader_source = ROOT / "experiments/compute/t08_subgroup_broadcast_runtime.comp"
    shader_file = build / "broadcast.spv"
    run(glslang, "-V", "--target-env", "vulkan1.2", str(shader_source),
        "-o", str(shader_file))
    shader = shader_file.read_bytes()
    checked_spirv(shader)
    (build / "t08_subgroup_broadcast_shader.h").write_text(
        "#include <stdint.h>\n" + emit_array("t08_subgroup_broadcast_spirv", shader),
        encoding="utf-8")

    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk),
                   PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC="1")
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / "examples/t08_subgroup_broadcast_witness/main.c"
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
                 contentId="UP9000-PPSA99994_00-PS5VKSGRT0000001")
    param["localizedParameters"]["en-US"]["titleName"] = (
        "PS5 Vulkan Subgroup Broadcast Witness")
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {
        "profile": "t08-subgroup-broadcast-diagnostic-witness",
        "outputs": 128, "subgroups": 4,
        "source_lanes": [7, 19, 31, 1],
        "public_profile": "vulkan-1.0-subgroup-disabled",
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "shader_sha256": hashlib.sha256(shader).hexdigest(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    }
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
