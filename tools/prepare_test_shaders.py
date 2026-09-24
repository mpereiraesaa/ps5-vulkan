#!/usr/bin/env python3
"""Compile owned GLSL compute fixtures for host compiler integration tests."""

from pathlib import Path
import shutil
import os
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/test-shaders"


def storage8_vk10_extension_form(target: Path) -> None:
    """Convert glslang's Vulkan 1.1 storage-class form to legal SPIR-V 1.0.

    glslang uses the exact StorageBuffer8BitAccess capability only with the
    StorageBuffer storage class, but selects SPIR-V 1.3 for Vulkan 1.1. Vulkan
    1.0 can use that storage class through SPV_KHR_storage_buffer_storage_class;
    encode that extension explicitly and lower only the module version word.
    """
    payload = target.read_bytes()
    words = list(struct.unpack(f"<{len(payload) // 4}I", payload))
    if len(words) < 7 or words[0] != 0x07230203 or words[1] != 0x00010300:
        raise SystemExit("unexpected storage8 SPIR-V header")
    capabilities = []
    cursor = 5
    insert_at = cursor
    while cursor < len(words):
        count = words[cursor] >> 16
        opcode = words[cursor] & 0xffff
        if not count or cursor + count > len(words):
            raise SystemExit("malformed storage8 SPIR-V instruction stream")
        if opcode == 17 and count == 2:
            capabilities.append(words[cursor + 1])
            insert_at = cursor + count
        cursor += count
    if 4448 not in capabilities or 4449 in capabilities:
        raise SystemExit("storage8 shader does not use the exact storage-buffer capability")

    extension = b"SPV_KHR_storage_buffer_storage_class\0"
    extension += b"\0" * (-len(extension) % 4)
    extension_words = list(struct.unpack(f"<{len(extension) // 4}I", extension))
    instruction = [((1 + len(extension_words)) << 16) | 10, *extension_words]
    words[1] = 0x00010000
    words[insert_at:insert_at] = instruction
    target.write_bytes(struct.pack(f"<{len(words)}I", *words))


def main():
    sources = {name: ROOT / f"experiments/compute/{name}.comp" for name in
               ("minimal", "xor", "shared_grid", "resource_abi", "push_specialization",
                "storage8", "storage16", "shader_int16", "sync_producer", "sync_consumer",
                "shared_atomic_multiwave")}
    sources.update({name: ROOT / f"experiments/compute/{name}.comp"
                    for name in ("t08_address", "t08_memory_model_queue", "t08_memory_model")})
    targets = {name: OUTPUT / f"{name}.spv" for name in sources}
    recipe_mtime = Path(__file__).stat().st_mtime
    if all(target.is_file() and target.stat().st_mtime >= max(sources[name].stat().st_mtime,
                                                               recipe_mtime)
           for name, target in targets.items()):
        return
    local = ROOT / "build/runtime-graphics/toolchain/usr/bin/glslangValidator"
    compiler = os.environ.get("GLSLANG") or (str(local) if local.is_file() else shutil.which("glslangValidator"))
    if not compiler:
        raise SystemExit("glslangValidator is required to prepare compiler test shaders")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        target = targets[name]
        # GLSL's Vulkan 1.0 8-bit path requests the broader
        # UniformAndStorageBuffer8BitAccess capability. Start from the exact
        # Vulkan 1.1 storage-buffer form, then express its storage class through
        # the Vulkan 1.0 SPIR-V extension without changing instructions.
        target_env = "vulkan1.1" if name == "storage8" or name.startswith("t08_") else "vulkan1.0"
        subprocess.run(
            [compiler, "-V", "--target-env", target_env, str(source), "-o", str(target)],
            check=True,
        )
        if name == "storage8":
            storage8_vk10_extension_form(target)


if __name__ == "__main__":
    main()
