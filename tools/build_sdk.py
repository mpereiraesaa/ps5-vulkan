#!/usr/bin/env python3
"""Stage reusable PS5 Vulkan SDK and exercise from isolated consumers."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DIST_SDK = ROOT / "dist-sdk"


def get_ps5_toolchain():
    sdk_env = os.environ.get("PS5_PAYLOAD_SDK")
    if sdk_env:
        sdk = Path(sdk_env).resolve()
    else:
        lab = ROOT.parents[1]
        sdk = lab / "third_party/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk"
    wrapper = ROOT.parents[1] / "third_party/ps5-native-app-boilerplate/tooling/prospero-clang18"
    if (sdk / "bin/prospero-lld").is_file() and wrapper.is_file():
        return sdk, wrapper
    return None, None


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

    sdk, clang_wrapper = get_ps5_toolchain()
    lab = ROOT.parents[1]
    gears = lab / "projects/ps5-agc-gears"
    logger = lab / "projects/logging_server/client"

    # 3. Build native PS5 runtime if toolchain is available
    has_native_sdk = False
    if sdk and clang_wrapper and gears.is_dir() and logger.is_dir():
        print(f"PS5 toolchain detected: building native PS5 runtime archive...")
        native_sources = [
            (ROOT / "src/vk_alloc.c", []),
            (ROOT / "src/vk_memory.c", []),
            (ROOT / "src/vk_descriptor.c", []),
            (ROOT / "src/vk_pipeline.c", []),
            (ROOT / "src/compilation_cache.c", []),
            (ROOT / "src/vk_command.c", []),
            (ROOT / "src/vk_fence.c", []),
            (ROOT / "src/vk_queue.c", []),
            (ROOT / "src/vk_queue_router.c", []),
            (ROOT / "src/vk_image_view.c", []),
            (ROOT / "src/vk_sampler.c", []),
            (ROOT / "src/vk_render_pass.c", []),
            (ROOT / "src/vk_framebuffer.c", []),
            (ROOT / "src/vk_graphics_pipeline.c", []),
            (ROOT / "src/graphics_program.c", []),
            (ROOT / "src/vk_transfer.c", []),
            (ROOT / "src/texture_copy.c", []),
            (ROOT / "src/texture_layout.c", []),
            (ROOT / "src/vk_device.c", []),
            (ROOT / "src/vk_dispatch.c", []),
            (ROOT / "native/platform_ps5.c", []),
            (ROOT / "native/memory_ps5.c", []),
            (ROOT / "native/queue_ps5.c", []),
            (ROOT / "native/present_ps5.c", []),
            (ROOT / "native/command_arena_ps5.c", []),
            (ROOT / "src/ps5vk_present.c", []),
            (ROOT / "src/compute_commands.c", []),
            (ROOT / "src/dispatch_encode.c", []),
            (ROOT / "src/descriptor_encode.c", []),
            (ROOT / "src/ps5vk_compiler.c", []),
            (ROOT / "src/ps5_compiler_shims.c", []),
            (gears / "src/ps5_videoout.c", []),
            (gears / "src/ps5_present.c", []),
            (gears / "src/ps5_event_adapter.c", []),
            (gears / "src/ps5_frame_completion.c", []),
            (gears / "src/ps5_agc_writer.c", []),
            (gears / "src/ps5_gpu_span.c", []),
            (logger / "ps5log.c", ["-include", str(logger / "ps5log_ps5_net.h")]),
            (logger / "ps5log_ps5_net.c", []),
        ]

        native_cflags = [
            "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections",
            "-I" + str(DIST_SDK / "include"),
            "-I" + str(ROOT / "src"),
            "-I" + str(ROOT / "native"),
            "-I" + str(ROOT / "build/program-library"),
            "-I" + str(ROOT / "third_party/vulkan-headers/include"),
            "-I" + str(gears / "include"),
            "-I" + str(gears / "src"),
            "-I" + str(logger),
            "-I" + str(ROOT / "third_party/psbc-reference"),
            "-I" + str(ROOT / "third_party/psbc-reference/src"),
            "-I" + str(ROOT / "third_party/psbc-reference/libpsbc"),
            "-I" + str(ROOT / "third_party/opengnm/include"),
            "-DPS5VK_RUNTIME_COMPILER=1",
            "-DPS5VK_TARGET_PS5=1",
        ]

        obj_dir = ROOT / "build/sdk-objs-native"
        obj_dir.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
        native_objs = []
        for src_p, extra in native_sources:
            obj_p = obj_dir / (src_p.stem + ".o")
            subprocess.run(["sh", str(clang_wrapper), *native_cflags, *extra, "-c", str(src_p), "-o", str(obj_p)],
                           env=env, check=True)
            native_objs.append(str(obj_p))

        native_lib = lib_dir / "libps5vk.a"
        ar_bin = str(sdk / "bin/prospero-ar") if (sdk / "bin/prospero-ar").is_file() else "ar"
        subprocess.run([ar_bin, "rcs", str(native_lib)] + native_objs, check=True)
        has_native_sdk = True

        # Build independent native consumer test
        consumer_obj = ROOT / "build/tests/test_sdk_consumer_native.o"
        consumer_obj.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            "sh", str(clang_wrapper), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(include_dir),
            "-c", str(ROOT / "tests/test_sdk_consumer_native.c"),
            "-o", str(consumer_obj)
        ], env=env, check=True)

        linker = sdk / "bin/prospero-lld"
        pie_ld = ROOT / "native/ps5-pie.ld"
        syms_map = ROOT / "native/app-symbols.map"
        crt = sdk / "target/lib/crt1.o"
        stub = ROOT / "build/native-compute/stubs/libSceAgc.so"
        driver = ROOT / "build/native-compute/stubs/libSceAgcDriver.so"
        psbc_lib = ROOT / "build/libpsbc.ps5.a"
        out_elf = ROOT / "build/tests/test_sdk_consumer_native.elf"

        if (stub.is_file() and driver.is_file() and psbc_lib.is_file() and
                pie_ld.is_file() and syms_map.is_file() and crt.is_file()):
            link_cmd = [
                str(linker),
                "-L" + str(sdk / "target/lib"),
                "-T", str(pie_ld),
                "--eh-frame-hdr",
                "--version-script", str(syms_map),
                "-e", "main",
                "-o", str(out_elf),
                str(crt),
                str(consumer_obj),
                str(native_lib),
                str(psbc_lib),
                str(sdk / "target/lib/libc++.a"),
                str(sdk / "target/lib/libc++abi.a"),
                str(sdk / "target/lib/libunwind.a"),
                str(sdk / "target/lib/libpthread.a"),
                str(sdk / "target/lib/libc.a"),
                "--as-needed",
                *sorted(str(p) for p in (sdk / "target/lib").glob("*.so")),
                str(stub),
                str(driver),
            ]
            subprocess.run(link_cmd, check=True)
            print(f"Native SDK consumer: successfully linked {out_elf} (isolated headers + native libps5vk.a)")

    # 4. Build host runtime library for host-test execution and CI environments
    host_sources = [
        "src/vk_alloc.c", "src/vk_memory.c", "src/vk_descriptor.c",
        "src/vk_pipeline.c", "src/compilation_cache.c", "src/vk_command.c",
        "src/vk_fence.c", "src/vk_queue.c", "src/vk_queue_router.c",
        "src/vk_image_view.c", "src/vk_sampler.c", "src/vk_render_pass.c",
        "src/vk_framebuffer.c", "src/vk_graphics_pipeline.c", "src/graphics_program.c",
        "src/vk_transfer.c", "src/texture_copy.c", "src/texture_layout.c",
        "src/vk_device.c", "src/vk_dispatch.c", "src/platform_host.c",
        "src/ps5vk_present.c"
    ]
    host_obj_dir = ROOT / "build/sdk-objs-host"
    host_obj_dir.mkdir(parents=True, exist_ok=True)
    host_objs = []
    host_cflags = [
        "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-I" + str(include_dir), "-I" + str(ROOT / "src")
    ]
    for s in host_sources:
        src_path = ROOT / s
        obj_path = host_obj_dir / (src_path.stem + ".o")
        subprocess.run(["cc", *host_cflags, "-c", str(src_path), "-o", str(obj_path)], check=True)
        host_objs.append(str(obj_path))

    host_lib = lib_dir / ("libps5vk_host.a" if has_native_sdk else "libps5vk.a")
    subprocess.run(["ar", "rcs", str(host_lib)] + host_objs, check=True)

    # 5. Generate SDK README
    readme_text = """# PS5 Vulkan (ps5vk) SDK

Reusable SDK distribution for PlayStation 5 Vulkan development.

## Structure
- `include/ps5vk/`: Public ps5vk headers (`ps5vk.h`, `ps5vk_present.h`)
- `include/vulkan/`: Vulkan 1.0 core standard headers
- `lib/`: Prebuilt runtime libraries:
  - `libps5vk.a`: Native PlayStation 5 runtime (GFX1013 profile, VideoOut presentation)
  - `libps5vk_host.a`: Host test runtime for mock contracts and tooling

## Usage
Include `<ps5vk/ps5vk.h>` and `<ps5vk/ps5vk_present.h>` and compile with:
```sh
prospero-clang -Iinclude consumer.c -Llib -lps5vk ...
```
No private internal headers (`vk_internal.h`, `command_arena_ps5.h`, etc.) are required.
"""
    (DIST_SDK / "README.md").write_text(readme_text)

    # 6. Build and run host consumer test
    consumer_bin = ROOT / "build/tests/test_sdk_consumer"
    consumer_bin.parent.mkdir(parents=True, exist_ok=True)
    test_lib_flag = "-lps5vk_host" if has_native_sdk else "-lps5vk"
    consumer_cmd = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I" + str(include_dir),
        str(ROOT / "tests/test_sdk_consumer.c"),
        "-L" + str(lib_dir), test_lib_flag, "-lpthread",
        "-o", str(consumer_bin)
    ]
    subprocess.run(consumer_cmd, check=True)
    subprocess.run([str(consumer_bin)], check=True)

    status = "native PS5 runtime + host testing runtime" if has_native_sdk else "host validation runtime"
    print(f"SDK successfully staged to {DIST_SDK} ({status}) and validated with consumer tests.")


if __name__ == "__main__":
    main()
