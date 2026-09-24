#!/usr/bin/env python3
"""Stage reusable PS5 Vulkan SDK and exercise from isolated consumers."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DIST_SDK = ROOT / "dist-sdk"

sys.path.insert(0, str(ROOT / "tools"))
from lab import lab_root  # noqa: E402


def validate_compiler_archive(archive: Path, expected_revision: str) -> None:
    identity_path = archive.with_suffix(".json")
    if not archive.is_file() or not identity_path.is_file():
        raise RuntimeError("native PSBC archive or identity stamp is missing")
    identity = json.loads(identity_path.read_text())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if (identity.get("schema") != 1 or identity.get("target") != "ps5" or
            identity.get("source_commit") != expected_revision or
            identity.get("archive_sha256") != digest):
        raise RuntimeError("native PSBC archive identity is stale or inconsistent")


def archive(tool, output, objects):
    # Recreate instead of ar-updating an old archive: removed source members
    # must not survive a change of SDK profile.
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        fresh = Path(temp) / output.name
        subprocess.run([str(tool), "rcs", str(fresh), *objects], check=True)
        fresh.replace(output)


def get_ps5_toolchain():
    sdk_env = os.environ.get("PS5_PAYLOAD_SDK")
    if sdk_env:
        sdk = Path(sdk_env).resolve()
    else:
        lab = lab_root()
        sdk = lab / "third_party/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk"
    wrapper = lab_root() / "third_party/ps5-native-app-boilerplate/tooling/prospero-clang18"
    if (sdk / "bin/prospero-lld").is_file() and wrapper.is_file():
        return sdk, wrapper
    return None, None


def tess_ring_flags(environment):
    mode = environment.get("PS5VK_TESS_RING_QUERY", "0")
    if mode not in ("0", "4"):
        raise ValueError("SDK ring diagnostics must be 0 (off) or 4 (extended records)")
    flags = ["-DPS5VK_TESS_RING_QUERY=4"] if mode == "4" else []
    experimental = environment.get("PS5VK_TESS_EXPERIMENTAL_API", "0")
    if experimental not in ("0", "1"):
        raise ValueError("experimental tessellation API must be 0 or 1")
    if experimental == "1":
        # TF/offchip ownership is unconditional in the native queue now.
        # Diagnostic logging must not enable/disable required GPU lifecycle.
        if any(environment.get(name, "0") not in ("", "0") for name in
               ("PS5VK_OPTIONAL_STAGE_DIAGNOSTIC", "PS5VK_TESS_PROBE")):
            raise ValueError("public API experiment forbids feature/descriptor bypass probes")
        flags.append("-DPS5VK_TESS_EXPERIMENTAL_API=1")
    return flags


def main():
    ring_flags = tess_ring_flags(os.environ)
    include_dir = DIST_SDK / "include"
    lib_dir = DIST_SDK / "lib"
    for d in (include_dir / "ps5vk", include_dir / "vulkan", lib_dir):
        d.mkdir(parents=True, exist_ok=True)

    # 1. Copy public headers
    shutil.copyfile(ROOT / "include/ps5vk/ps5vk.h", include_dir / "ps5vk/ps5vk.h")
    shutil.copyfile(ROOT / "include/ps5vk/ps5vk_present.h", include_dir / "ps5vk/ps5vk_present.h")
    shutil.copyfile(ROOT / "native/ps5-pie.ld", lib_dir / "ps5-pie.ld")
    shutil.copyfile(ROOT / "native/app-symbols.map", lib_dir / "app-symbols.map")

    # 2. Copy Vulkan core headers
    for sub in ("vulkan", "vk_video"):
        src = ROOT / "third_party/vulkan-headers/include" / sub
        if src.is_dir():
            dst = include_dir / sub
            dst.mkdir(parents=True, exist_ok=True)
            for hdr in src.glob("*.h"):
                shutil.copyfile(hdr, dst / hdr.name)

    sdk, clang_wrapper = get_ps5_toolchain()
    lab = lab_root()
    gears = lab / "projects/ps5-agc-gears"
    logger = lab / "projects/logging_server/client"

    # 3. Build native PS5 runtime if toolchain is available
    has_native_sdk = False
    if sdk and clang_wrapper and gears.is_dir() and logger.is_dir():
        print(f"PS5 toolchain detected: building native PS5 runtime archive...")
        # The driver's own resolve stages (DXVK262-T06): one averaging fragment
        # per served sample count, compiled into a header this archive includes.
        subprocess.run([sys.executable, str(ROOT / "tools/build_resolve_shaders.py"),
            "--out", str(ROOT / "build/resolve/resolve_spirv.h")], check=True)
        native_sources = [
            (ROOT / "src/vk_alloc.c", []),
            (ROOT / "src/vk_memory.c", []),
            (ROOT / "src/vk_descriptor.c", []),
            (ROOT / "src/vk_pipeline.c", []),
            (ROOT / "src/spirv_ubo_layout.c", []),
            (ROOT / "src/compilation_cache.c", []),
            (ROOT / "src/vk_pipeline_cache.c", []),
            (ROOT / "src/vk_command.c", []),
            (ROOT / "src/vk_indirect.c", []),
            (ROOT / "src/vk_fence.c", []),
            (ROOT / "src/vk_query_pool.c", []),
            (ROOT / "src/vk_sync.c", []),
            (ROOT / "src/vk_buffer_transfer.c", []),
            (ROOT / "src/vk_image_transfer.c", []),
            (ROOT / "src/color_clear.c", []),
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
            (ROOT / "src/texture_format.c", []),
            (ROOT / "src/texture_layout.c", []),
            (ROOT / "src/vk_device.c", []),
            (ROOT / "src/vk_dispatch.c", []),
            (ROOT / "native/platform_ps5.c", []),
            (ROOT / "native/memory_ps5.c", []),
            (ROOT / "native/queue_ps5.c", []),
            (ROOT / "native/present_ps5.c", []),
            (ROOT / "native/command_arena_ps5.c", []),
            (ROOT / "native/draw_batch_ps5.c", []),
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
        graphics_sources = (
            "native/graphics_pair.c", "src/shader_relocate.c",
            "native/graphics_pipeline_ps5.c", "native/tess_shared_storage.c",
            "native/tess_ring_lease.c", "native/image_ps5.c",
            "src/depth_layout.c", "src/color_clear.c", "src/color_detile.c",
            # The depth readback detiles its surface with the 64KB_Z_X
            # equation, so the executor needs it beside the colour one.
            "src/depth_detile.c",
            "native/draw_prepare_ps5.c", "native/draw_emit_ps5.c", "native/index_emit_ps5.c",
            "native/input_attachment_gate.c",
            "native/input_attachment_oracle.c",
            "src/graphics_sync.c", "src/vertex_descriptor.c", "src/vertex_fetch.c", "src/index_fetch.c",
            "src/texture_descriptor.c", "src/texture_dma.c", "src/image_layout_state.c",
            "native/graphics_queue_ps5.c", "native/draw_state_ps5.c", "native/viewport_ps5.c",
            "native/targets_ps5.c", "native/runtime_shader.c", "native/runtime_graphics_compiler.c",
            "native/runtime_graphics_cache.c", "native/runtime_graphics_ps5.c",
            "src/spirv_graphics_interface.c", "src/clip_cull_witness.c",
            "src/geometry_witness.c", "src/dual_source_oracle.c",
            "src/two_mrt_oracle.c", "src/color_attachment_contract.c",
            "native/resolve_program.c")
        native_sources += [(ROOT / source, []) for source in graphics_sources]
        native_sources += [(gears / "src" / source, []) for source in (
            "ps5_shader_header.c", "ps5_pipeline.c", "ps5_color_target.c", "ps5_depth_target.c")]

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
            "-I" + str(ROOT / "build/resolve"),
            "-DPS5VK_RUNTIME_COMPILER=1",
            "-DPS5VK_TARGET_PS5=1",
            "-DPS5VK_GRAPHICS_API=1", "-DPS5VK_GRAPHICS_DRAW=1",
            "-DPS5VK_RUNTIME_GRAPHICS=1", "-DPS5VK_NO_OFFLINE_LIBRARY=1",
            # The diagnostic probes are environment-declared for the whole
            # payload: the runtime library the SDK builds compiles the same
            # sources the native build does, so the optional-stage negotiation
            # gate must see the same diagnostic decision. The value is passed
            # through from the caller (build_native.py sets it for its probe
            # builds); the shipping default stays zero.
            *(["-DPS5VK_OPTIONAL_STAGE_DIAGNOSTIC=" +
               os.environ["PS5VK_OPTIONAL_STAGE_DIAGNOSTIC"]]
              if os.environ.get("PS5VK_OPTIONAL_STAGE_DIAGNOSTIC") else []),
            *(["-DPS5VK_TESS_PROBE=" + os.environ["PS5VK_TESS_PROBE"]]
              if os.environ.get("PS5VK_TESS_PROBE") else []),
            # Legacy experiment flags remain attributable in old build recipes.
            # Default native feature eligibility is in tess_profile.h; neither
            # ring logging nor the old experiment enables a probe bypass.
            *ring_flags,
            # The SDK build compiles the same sources, so it must select the
            # same single tessellation candidate the native build selected.
            *(["-DPS5VK_TESS_VARIANT=" + os.environ["PS5VK_TESS_VARIANT"]]
              if os.environ.get("PS5VK_TESS_VARIANT") else []),
            # The register-stream dump lives in the runtime draw path, which
            # the SDK build compiles, so the switch has to reach here too or
            # the dump silently does not exist in the deployed payload.
            *(["-DPS5VK_TESS_STATE_DUMP=" +
               os.environ["PS5VK_TESS_STATE_DUMP"]]
              if os.environ.get("PS5VK_TESS_STATE_DUMP") else []),
            *(["-DPS5VK_GEOMETRY_KEY_DIAG=1"]
              if os.environ.get("PS5VK_GEOMETRY_KEY_DIAG") == "1" else []),
            *(["-DPS5VK_TESS_OFFCHIP_CAPACITY_WG=" +
               os.environ["PS5VK_TESS_OFFCHIP_CAPACITY_WG"]]
              if os.environ.get("PS5VK_TESS_OFFCHIP_CAPACITY_WG") else []),
            *(["-DPS5VK_TESS_GE_CNTL=" + os.environ["PS5VK_TESS_GE_CNTL"]]
              if os.environ.get("PS5VK_TESS_GE_CNTL") else []),
            *(["-DPS5VK_TESS_END_VS_FLUSH=1"]
              if os.environ.get("PS5VK_TESS_END_VS_FLUSH") == "1" else []),
            *(["-DPS5VK_TESS_HULL_TRACE=1"]
              if os.environ.get("PS5VK_TESS_HULL_TRACE") == "1" else []),
            *(["-DPS5VK_TESS_OFFCHIP_BIND=1"]
              if os.environ.get("PS5VK_TESS_OFFCHIP_BIND") == "1" else []),
        ]
        # The DXVK262-T05 measurement switch is gone: the four rasterization
        # and viewport features are advertised by the shipping platform on
        # physical-console evidence, so there is no diagnostic build that
        # reports them ahead of it.
        # Private measurement build (DXVK262-T06 sampleRateShading): report the
        # multisample capability so a witness payload and the focused CTS
        # selection can negotiate it through the public API before any shipping
        # platform advertises it. Off by default, and never set in the shipping
        # build; the promotion is a separate, evidence-backed change.
        sample_rate_diagnostic = os.environ.get("PS5VK_SAMPLE_RATE_DIAGNOSTIC", "0")
        if sample_rate_diagnostic not in ("0", "1"):
            raise SystemExit("PS5VK_SAMPLE_RATE_DIAGNOSTIC must be 0 or 1")
        if sample_rate_diagnostic == "1":
            native_cflags.append("-DPS5VK_SAMPLE_RATE_DIAGNOSTIC=1")
        memory_model_diagnostic = os.environ.get("PS5VK_MEMORY_MODEL_DIAGNOSTIC", "0")
        if memory_model_diagnostic not in ("0", "1"):
            raise SystemExit("PS5VK_MEMORY_MODEL_DIAGNOSTIC must be 0 or 1")
        if memory_model_diagnostic == "1":
            native_cflags.append("-DPS5VK_MEMORY_MODEL_DIAGNOSTIC=1")
        subgroup_broadcast_diagnostic = os.environ.get(
            "PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC", "0")
        if subgroup_broadcast_diagnostic not in ("0", "1"):
            raise SystemExit("PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC must be 0 or 1")
        if subgroup_broadcast_diagnostic == "1":
            native_cflags.append("-DPS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC=1")
        # Private diagnostic build (DXVK262-T04): make the graphics adapter log
        # the pipeline key field by field when it refuses a pipeline, so one
        # CTS run names the refused condition. Same shape as the switch above:
        # off by default, and the logging does not exist without the define.
        geometry_key_diag = os.environ.get("PS5VK_GEOMETRY_KEY_DIAG", "0")
        if geometry_key_diag not in ("0", "1"):
            raise SystemExit("PS5VK_GEOMETRY_KEY_DIAG must be 0 or 1")
        if geometry_key_diag == "1":
            native_cflags.append("-DPS5VK_GEOMETRY_KEY_DIAG=1")

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
        archive(ar_bin, native_lib, native_objs)
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
        app_crt_cpp = lab / "third_party/ps5-native-app-boilerplate/tooling/native/app_crt.cpp"
        crt = lib_dir / "crt.o"
        if app_crt_cpp.is_file():
            subprocess.run(["sh", str(clang_wrapper), "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti",
                            "-c", str(app_crt_cpp), "-o", str(crt)], env=env, check=True)
        else:
            crt = sdk / "target/lib/crt1.o"
        stub = lib_dir / "libSceAgc.so"
        driver = lib_dir / "libSceAgcDriver.so"
        stub_objects = []
        for source in (gears / "native/stubs/libSceAgc.c", ROOT / "native/index_import_stub.c"):
            obj = obj_dir / (source.stem + "_stub.o")
            subprocess.run(["sh", str(clang_wrapper), "-fPIC", "-c", str(source), "-o", str(obj)],
                           env=env, check=True)
            stub_objects.append(str(obj))
        subprocess.run([str(linker), "--shared", "-soname", "libSceAgc.prx", "-o", str(stub),
                        *stub_objects], check=True)
        driver_obj = obj_dir / "driver_stub.o"
        subprocess.run(["sh", str(clang_wrapper), "-fPIC", "-c",
                        str(gears / "native/stubs/libSceAgcDriver.c"), "-o", str(driver_obj)],
                       env=env, check=True)
        tess_driver_obj = obj_dir / "tess_driver_stub.o"
        subprocess.run(["sh", str(clang_wrapper), "-fPIC", "-c",
                        str(ROOT / "native/tess_driver_import_stub.c"), "-o", str(tess_driver_obj)],
                       env=env, check=True)
        subprocess.run([str(linker), "--shared", "-soname", "libSceAgcDriver.prx",
                        "-o", str(driver), str(driver_obj), str(tess_driver_obj)], check=True)
        compiler_source = ROOT / "build/libpsbc.ps5.a"
        expected_revision = subprocess.check_output(
            ["git", "-C", str(ROOT / "third_party/psbc-reference"),
             "rev-parse", "HEAD"], text=True).strip()
        try:
            validate_compiler_archive(compiler_source, expected_revision)
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
            raise SystemExit(f"{error}; run tools/build_psbc.py --target ps5")
        psbc_lib = lib_dir / "libpsbc.a"
        shutil.copyfile(compiler_source, psbc_lib)
        out_elf = ROOT / "build/tests/test_sdk_consumer_native.elf"

        if (stub.is_file() and driver.is_file() and psbc_lib.is_file() and
                pie_ld.is_file() and syms_map.is_file() and crt.is_file()):
            link_cmd = [
                str(linker),
                "-L" + str(sdk / "target/lib"),
                "-T", str(pie_ld),
                "--eh-frame-hdr",
                "--version-script", str(syms_map),
                "-e", "_start",
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
        "src/vk_pipeline.c", "src/spirv_ubo_layout.c", "src/compilation_cache.c", "src/vk_pipeline_cache.c", "src/vk_command.c", "src/vk_indirect.c",
        "src/vk_fence.c", "src/vk_query_pool.c", "src/vk_sync.c", "src/vk_buffer_transfer.c", "src/vk_image_transfer.c", "src/vk_queue.c", "src/vk_queue_router.c",
        # The linear staging readback copy reads the tiled colour surface
        # through the shared 64KB_R_X offset contract.
        # through the shared 64KB_R_X offset contract, and a depth readback
        # reads its own surface through the 64KB_Z_X one.
        "src/color_detile.c", "src/depth_detile.c",
        "src/color_attachment_contract.c",
        "src/vk_image_view.c", "src/vk_sampler.c", "src/vk_render_pass.c",
        "src/vk_framebuffer.c", "src/vk_graphics_pipeline.c", "src/graphics_program.c",
        "src/vk_transfer.c", "src/texture_copy.c", "src/texture_format.c", "src/texture_layout.c", "src/color_clear.c",
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

    host_lib = lib_dir / "libps5vk_host.a"
    archive("ar", host_lib, host_objs)
    if not has_native_sdk:
        shutil.copyfile(host_lib, lib_dir / "libps5vk.a")

    # 5. Generate SDK README
    readme_text = """# PS5 Vulkan (ps5vk) SDK

Reusable SDK distribution for PlayStation 5 Vulkan development.

## Structure
- `include/ps5vk/`: Public ps5vk headers (`ps5vk.h`, `ps5vk_present.h`)
- `include/vulkan/`: Vulkan 1.0 core standard headers
- `lib/`: Prebuilt runtime libraries:
  - `libps5vk.a`: Native PlayStation 5 runtime (gfx1013, graphics/compute, VideoOut)
  - `libps5vk_host.a`: Host test runtime for mock contracts and tooling
  - `libpsbc.a`: Pinned PSBC/NIR/ACO native compiler dependency
  - `libSceAgc.so`, `libSceAgcDriver.so`: Link-time import facades, not system module replacements

The native SDK uses runtime SPIR-V compilation, not the demo's offline shader
libraries. Graphics currently supports procedural triangle-list pipelines,
one BGRA8 UNORM color target at sample count 1, full color writes and no
blending. Vertex/fragment interfaces use matching smooth float32 scalar/vector
locations. Push constants and scalar specialization constants are supported;
Vulkan 1.0 applications may negotiate storage-buffer-only 8/16-bit access
through VK_KHR_get_physical_device_properties2,
VK_KHR_storage_buffer_storage_class, VK_KHR_8bit_storage and
VK_KHR_16bit_storage. Narrow arithmetic and other narrow storage classes are
not advertised. Vertex buffers, graphics descriptors and additional render targets are not
supported by this runtime compiler profile. A bounded
in-process cache retains compiled pairs. This is not a Vulkan-conformant driver.

## License
This SDK is licensed under GPL-3.0-or-later. `libps5vk.a` is a static library;
an application distributed after linking it must comply with the GNU GPL and
provide the corresponding source of the combined work. See the included
`LICENSE` file. Separately supplied console system modules are not part of this
SDK.

## Usage
Include `<ps5vk/ps5vk.h>` and `<ps5vk/ps5vk_present.h>` and compile with:
```sh
prospero-clang -Iinclude consumer.c -Llib -lps5vk -lpsbc ...
```
No private internal headers (`vk_internal.h`, `command_arena_ps5.h`, etc.) are required.
Native applications still need the native-app scaffold, PS5 C/C++ runtime link
dependencies and the AGC import facades. Initialize the lab's TCP logger before
device creation; do not add filesystem/USB logging. Use normal system Close
Game with the existing GPU suspension protocol. The SDK does not own app main,
telemetry configuration, deployment or the application's event loop.

Native consumer validation performed by this script is a cross-compile/link
check, not a hardware run. Host consumer execution uses a mock backend and
does not certify GPU output. If no native toolchain is available, libps5vk.a
contains the host validation backend instead; no native capability is implied.
"""
    (DIST_SDK / "README.md").write_text(readme_text)
    shutil.copyfile(ROOT / "LICENSE", DIST_SDK / "LICENSE")

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
