#!/usr/bin/env python3
"""Build one bounded public-SDK Vulkan memory-model witness executable."""

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

SHADER_NAMES = ("producer", "consumer")
SPIRV_MAGIC = 0x07230203


def checked_spirv(payload: bytes, scope: str) -> bytes:
    """Verify the model/scope pair and declare Vulkan 1.0 storage class use."""
    if len(payload) % 4:
        raise ValueError("SPIR-V byte count is not word aligned")
    words = list(struct.unpack(f"<{len(payload) // 4}I", payload))
    if len(words) < 7 or words[0] != SPIRV_MAGIC or words[1] != 0x00010000:
        raise ValueError("witness requires SPIR-V 1.0")
    capabilities: set[int] = set()
    extensions: set[str] = set()
    memory_model = None
    barriers = 0
    storage_class = False
    insert_at = 5
    index = 5
    while index < len(words):
        size, opcode = words[index] >> 16, words[index] & 0xffff
        if not size or index + size > len(words):
            raise ValueError("malformed SPIR-V instruction stream")
        operands = words[index + 1:index + size]
        if opcode == 17 and size == 2:  # OpCapability
            capabilities.add(operands[0])
            insert_at = index + size
        elif opcode == 10:  # OpExtension
            encoded = struct.pack(f"<{len(operands)}I", *operands)
            extensions.add(encoded.split(b"\0", 1)[0].decode("ascii"))
        elif opcode == 14 and size == 3:  # OpMemoryModel
            memory_model = tuple(operands)
        elif opcode == 225:  # OpMemoryBarrier
            barriers += 1
        elif opcode == 32 and size >= 4 and operands[1] == 7:  # StorageBuffer pointer
            storage_class = True
        index += size
    required = {1, 5345} | ({5346} if scope == "device" else set())
    if (not required.issubset(capabilities) or (5346 in capabilities) !=
            (scope == "device") or memory_model != (0, 3) or barriers != 1 or
            "SPV_KHR_vulkan_memory_model" not in extensions or not storage_class):
        raise ValueError("shader does not match the requested VulkanKHR scope")
    # glslang emits StorageBuffer pointers for the Vulkan 1.0 source but omits
    # the extension declaration. The device enables its matching KHR extension.
    if "SPV_KHR_storage_buffer_storage_class" not in extensions:
        encoded = b"SPV_KHR_storage_buffer_storage_class\0"
        encoded += b"\0" * (-len(encoded) % 4)
        extension_words = struct.unpack(f"<{len(encoded) // 4}I", encoded)
        words[insert_at:insert_at] = [((len(extension_words) + 1) << 16) | 10,
                                     *extension_words]
    return struct.pack(f"<{len(words)}I", *words)


def run(*command: str, env: dict | None = None) -> None:
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", choices=("queue-family", "device"), required=True)
    args = parser.parse_args()
    scope = args.scope
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
    build = ROOT / f"build/t08-memory-model-{scope}"
    dist = ROOT / f"dist-t08-memory-model-{scope}/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    shader_payloads = {}
    for name in SHADER_NAMES:
        source = ROOT / f"experiments/compute/t08_device_scope_{name}.comp"
        target = build / f"{name}.spv"
        flags = ["-DT08_DEVICE_SCOPE=1"] if scope == "device" else []
        run(glslang, "-V", "--target-env", "vulkan1.0", *flags,
            str(source), "-o", str(target))
        payload = checked_spirv(target.read_bytes(), scope)
        target.write_bytes(payload)
        shader_payloads[name] = payload
    header = build / "t08_memory_model_shaders.h"
    header.write_text("#include <stdint.h>\n" + "\n".join(
        emit_array(f"t08_{name}_spirv", shader_payloads[name])
        for name in SHADER_NAMES), encoding="utf-8")

    # The diagnostic feature switch affects only this payload's SDK archive.
    sdk_env = dict(os.environ, PS5VK_MEMORY_MODEL_DIAGNOSTIC="1",
                   PS5_PAYLOAD_SDK=str(sdk))
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / "examples/t08_memory_model_witness/main.c"
    obj = build / "main.o"
    dep = build / "main.d"
    cflags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
              "-ffunction-sections", "-fdata-sections", "-MD", "-MP",
              "-MF", str(dep), "-I" + str(staged / "include"),
              "-I" + str(build), "-I" + str(logger)]
    if scope == "device":
        cflags.append("-DT08_DEVICE_SCOPE=1")
    run("sh", str(clang_wrapper), *cflags, "-c", str(source), "-o", str(obj),
        env=sdk_env)
    dependencies = dep.read_text()
    if str(ROOT / "src/") in dependencies or str(ROOT / "native/") in dependencies:
        raise RuntimeError("witness includes a private runtime header")
    undefined = subprocess.check_output(["nm", "-u", str(obj)], text=True)
    if any(line.split()[-1].startswith(("ps5vk_", "ps5_"))
           for line in undefined.splitlines() if line.split()):
        raise RuntimeError("witness calls a private runtime symbol")

    pie = build / "witness_pie.elf"
    eboot_elf = build / "eboot.elf"
    eboot = dist / "eboot.bin"
    linker = sdk / "bin/prospero-lld"
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
                 contentId="UP9000-PPSA99994_00-PS5VKCOMPUTE0001")
    param["localizedParameters"]["en-US"]["titleName"] = (
        "PS5 Vulkan Memory Model Witness")
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {
        "profile": "t08-memory-model-public-sdk-witness",
        "scope": scope, "values": 128, "guard_words_per_buffer": 16,
        "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
        "shader_sha256": {name: hashlib.sha256(shader_payloads[name]).hexdigest()
                          for name in SHADER_NAMES},
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    }
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
