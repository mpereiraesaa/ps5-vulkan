#!/usr/bin/env python3
"""Stage reusable PS5 Vulkan SDK and exercise from an isolated consumer."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DIST_SDK = ROOT / "dist-sdk"


def main():
    include_dir = DIST_SDK / "include"
    lib_dir = DIST_SDK / "lib"
    for d in (include_dir / "ps5vk", include_dir / "vulkan", lib_dir):
        d.mkdir(parents=True, exist_ok=True)

    # 1. Copy public headers
    shutil.copyfile(ROOT / "include/ps5vk/ps5vk.h", include_dir / "ps5vk/ps5vk.h")
    shutil.copyfile(ROOT / "include/ps5vk/ps5vk_present.h", include_dir / "ps5vk/ps5vk_present.h")

    # 2. Copy Vulkan core headers
    for sub in ("vulkan", "vk_video"):
        src = ROOT / "third_party/vulkan-headers/include" / sub
        if src.is_dir():
            dst = include_dir / sub
            dst.mkdir(parents=True, exist_ok=True)
            for hdr in src.glob("*.h"):
                shutil.copyfile(hdr, dst / hdr.name)

    # 3. Build libps5vk.a archive
    sources = [
        "src/vk_alloc.c", "src/vk_memory.c", "src/vk_descriptor.c",
        "src/vk_pipeline.c", "src/compilation_cache.c", "src/vk_command.c",
        "src/vk_fence.c", "src/vk_queue.c", "src/vk_queue_router.c",
        "src/vk_image_view.c", "src/vk_sampler.c", "src/vk_render_pass.c",
        "src/vk_framebuffer.c", "src/vk_graphics_pipeline.c", "src/graphics_program.c",
        "src/vk_transfer.c", "src/texture_copy.c", "src/texture_layout.c",
        "src/vk_device.c", "src/vk_dispatch.c", "src/platform_host.c"
    ]

    obj_dir = ROOT / "build/sdk-objs"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objs = []
    cflags = [
        "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-I" + str(DIST_SDK / "include"), "-I" + str(ROOT / "src")
    ]
    for s in sources:
        src_path = ROOT / s
        obj_path = obj_dir / (src_path.stem + ".o")
        subprocess.run(["cc", *cflags, "-c", str(src_path), "-o", str(obj_path)], check=True)
        objs.append(str(obj_path))

    lib_path = lib_dir / "libps5vk.a"
    subprocess.run(["ar", "rcs", str(lib_path)] + objs, check=True)

    # 4. Generate SDK README
    readme_text = """# PS5 Vulkan (ps5vk) SDK

Reusable SDK distribution for PlayStation 5 Vulkan development.

## Structure
- `include/ps5vk/`: Public ps5vk headers (`ps5vk.h`, `ps5vk_present.h`)
- `include/vulkan/`: Vulkan 1.0 core standard headers
- `lib/`: Prebuilt runtime libraries (`libps5vk.a`)

## Usage
Include `<ps5vk/ps5vk.h>` and compile with `-Iinclude -Llib -lps5vk -lpthread`.
No private internal headers (`vk_internal.h`, `command_arena_ps5.h`) are required.
"""
    (DIST_SDK / "README.md").write_text(readme_text)

    # 5. Build and run independent consumer test
    consumer_bin = ROOT / "build/tests/test_sdk_consumer"
    consumer_bin.parent.mkdir(parents=True, exist_ok=True)
    # Notice: NO -Isrc, strictly isolated from internal headers
    consumer_cmd = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(include_dir),
        str(ROOT / "tests/test_sdk_consumer.c"),
        "-L" + str(lib_dir), "-lps5vk", "-lpthread",
        "-o", str(consumer_bin)
    ]
    subprocess.run(consumer_cmd, check=True)
    subprocess.run([str(consumer_bin)], check=True)

    print(f"SDK successfully staged to {DIST_SDK} and validated with consumer test.")


if __name__ == "__main__":
    main()
