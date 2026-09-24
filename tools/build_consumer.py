#!/usr/bin/env python3
"""Build and package the independent native PS5 Vulkan SDK consumer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

# The draw-parameter witness cases the consumer renders. The verifier keeps its
# own table of the values each case must produce, so a drift between the two is
# a validation failure rather than a silent acceptance.
DRAW_PARAMETER_CASES = (
    "list_direct",
    "list_indexed",
    "strip_indexed_negative",
    "list_indirect",
    "strip_indexed_indirect",
    "strip_direct",
)
# The indirect/indexed witness cases (DXVK262-T03), in the order the consumer
# runs them; tools/verify_consumer_resource_abi.py pins the same list.
INDIRECT_CASES = (
    "indirect_first_instance",
    "indexed_indirect_first_instance",
    "multi_draw",
    "multi_draw_indexed",
    "compute_generated_arguments",
    "max_draw_indirect_count",
    "uint32_bit31_indices",
    "uint32_2pow24_indices",
    "uint16_control_indices",
    "uint32_bit31_indexed_indirect",
)
INDIRECT_EXTENT = 256
INDIRECT_MAX_COMMANDS = 65535
# The rasterization-state witness cases (DXVK262-T05), in the order the
# consumer runs them; tools/verify_consumer_resource_abi.py pins the same list.
RASTER_CASES = (
    "bias_disabled_coplanar",
    "bias_constant_negative",
    "bias_constant_positive",
    "bias_dynamic_constant_negative",
    "bias_disabled_tilted",
    "bias_slope_negative",
    "bias_slope_negative_clamp_negative",
    "bias_slope_negative_clamp_positive",
    "bias_slope_positive",
    "bias_slope_positive_clamp_positive",
    "bias_dynamic_slope_clamp_negative",
    "bias_dynamic_two_draws",
    "clamp_disabled_control",
    "clamp_enabled",
    "clamp_enabled_w2",
    "clamp_disabled_narrow_probe",
    "clamp_enabled_narrow_probe",
    "clamp_enabled_reversed_probe",
    "clamp_disabled_near_probe",
    "clamp_enabled_near_probe",
    "polygon_fill",
    "polygon_line",
    "polygon_point",
    "polygon_line_cull_front",
    "polygon_point_cull_back_front",
    "polygon_line_cull_back",
    "polygon_line_coplanar_unbiased",
    "polygon_line_coplanar_biased",
    "viewport_static_bank0_of_two",
    "viewport_dynamic_partial_updates",
    "viewport_static_scissor_bank0_of_two",
)
RASTER_EXTENT = 64
sys.path.insert(0, str(ROOT / "tools"))
from lab import lab_root  # noqa: E402

DIST_SDK = ROOT / "dist-sdk"
CONSUMER_DIR = ROOT / "examples/native_consumer"
BUILD_DIR = CONSUMER_DIR / "build"
DIST_DIR = ROOT / "dist-consumer/PPSA99994"


def check_isolation(dep_file: Path, obj_file: Path):
    """Verify that consumer depends only on public SDK and standard CRT headers/symbols."""
    print("Checking consumer header isolation in", dep_file)
    content = dep_file.read_text()
    # Find all header dependencies in the .d file
    # Format: target: dep1 \ dep2 ...
    headers = re.findall(r'(\S+\.h|\S+\.hpp)', content)
    for h in headers:
        hp = Path(h).resolve()
        # Header must be in:
        # - dist-sdk/include
        # - examples/native_consumer
        # - third_party/ps5-native-app-boilerplate/... (toolchain / system libc headers)
        is_sdk = str(hp).startswith(str(DIST_SDK / "include"))
        is_local = str(hp).startswith(str(CONSUMER_DIR))
        is_crt = "ps5-native-app-boilerplate" in str(hp) or str(hp).startswith("/usr/")
        if not (is_sdk or is_local or is_crt):
            raise AssertionError(f"Isolation violation: consumer includes forbidden private header {hp}")
        # Explicit check: cannot include anything from src/ or native/
        if str(ROOT / "src") in str(hp) or str(ROOT / "native") in str(hp):
            raise AssertionError(f"Isolation violation: consumer includes private source header {hp}")

    print("Header isolation verified: zero private project headers included.")

    print("Checking consumer symbol isolation in", obj_file)
    nm_out = subprocess.check_output(["nm", "-u", str(obj_file)], text=True)
    for line in nm_out.strip().splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[0] == "U":
            sym = parts[1]
            if sym.startswith("ps5vk_") or sym.startswith("ps5_"):
                raise AssertionError(f"Symbol isolation violation: consumer references private symbol {sym}")
    print("Symbol isolation verified: only standard libc, ps5log, Vulkan core, and public ps5vk* APIs used.")


def main():
    parser = argparse.ArgumentParser(description="Build independent native consumer")
    parser.add_argument("--continuous", action="store_true", help="Compile in continuous rendering mode")
    parser.add_argument("--shared-stage-samplers", action="store_true",
                        help="Finite backend qualification: shared VS/FS samplers above advertised limits")
    parser.add_argument("--mixed-resources", action="store_true",
                        help="Qualify uniform buffers alongside the sampled sets")
    parser.add_argument("--sampler-visibility", choices=("vertex-fragment", "all-graphics", "all"),
                        default="vertex-fragment", help="Descriptor visibility for the shared-stage diagnostic")
    parser.add_argument("--single-set-samplers", action="store_true",
                        help="Qualify all 96 sampled descriptors inside one set")
    parser.add_argument("--check-only", action="store_true", help="Only verify header and symbol isolation")
    parser.add_argument("--texel-rgba8", action="store_true",
                        help="Witness a four-component RGBA8 uniform texel buffer")
    parser.add_argument("--texel-formats", action="store_true",
                        help="Qualify the full staged uniform-texel format matrix")
    parser.add_argument("--use-staged-sdk", action="store_true",
                        help="Reuse dist-sdk without rebuilding it (caller guarantees freshness)")
    parser.add_argument("--dxvk-v262-probe", action="store_true",
                        help="Build the public-ABI-only DXVK v2.6.2 capability probe")
    parser.add_argument("--ubo-standard-layout", action="store_true",
                        help="Build the compact UBO GPU witness entry point")
    parser.add_argument("--cube-array-witness", action="store_true",
                        help="Build the finite two-cube/six-face sampled-image witness")
    parser.add_argument("--cube-array-base-layer", type=int, choices=(0, 1), default=0,
                        help="Cube witness view base; 1 also tests 13-layer storage")
    parser.add_argument("--cube-array-tiled-attachment", action="store_true",
                        help="Fill cube faces as colour attachments before sampling")
    parser.add_argument("--bc-filter-format", choices=__import__("prepare_consumer_bc_filter").FORMATS)
    parser.add_argument("--bc-subresource-profile", choices=__import__("prepare_consumer_bc_subresource").PROFILES)
    args = parser.parse_args()
    if args.bc_subresource_profile and any((args.bc_filter_format, args.cube_array_witness,
                                           args.continuous, args.shared_stage_samplers,
                                           args.single_set_samplers, args.mixed_resources,
                                           args.texel_rgba8, args.texel_formats,
                                           args.dxvk_v262_probe, args.ubo_standard_layout)):
        parser.error("BC subresource witness is an independent finite profile")
    if args.bc_filter_format and any((args.cube_array_witness, args.continuous, args.shared_stage_samplers,
                                      args.single_set_samplers, args.mixed_resources, args.texel_rgba8,
                                      args.texel_formats, args.dxvk_v262_probe, args.ubo_standard_layout)):
        parser.error("BC filter witness is an independent finite profile")
    if args.cube_array_base_layer and not args.cube_array_witness:
        parser.error("Cube-array base layer requires --cube-array-witness")
    if args.cube_array_tiled_attachment and not args.cube_array_witness:
        parser.error("Tiled cube attachment requires --cube-array-witness")
    if args.continuous and args.shared_stage_samplers:
        parser.error("Shared-stage qualification requires the finite consumer")
    if args.continuous and args.single_set_samplers:
        parser.error("Single-set qualification requires the finite consumer")
    if args.continuous and args.mixed_resources:
        parser.error("Mixed-resource qualification requires the finite consumer")
    if args.continuous and args.texel_formats:
        parser.error("Texel-format qualification requires the finite consumer")
    if args.texel_rgba8 and args.texel_formats:
        parser.error("Choose one uniform-texel witness")
    if args.cube_array_witness and any((args.continuous, args.shared_stage_samplers,
                                        args.single_set_samplers, args.mixed_resources,
                                        args.texel_rgba8, args.texel_formats,
                                        args.dxvk_v262_probe, args.ubo_standard_layout)):
        parser.error("Cube-array witness is an independent finite profile")
    if args.dxvk_v262_probe and any((args.continuous, args.shared_stage_samplers,
                                    args.single_set_samplers, args.mixed_resources,
                                    args.texel_rgba8, args.texel_formats,
                                    args.ubo_standard_layout)):
        parser.error("DXVK capability probing is an independent finite profile")
    if args.ubo_standard_layout and any((args.continuous, args.shared_stage_samplers,
                                        args.single_set_samplers, args.mixed_resources,
                                        args.texel_rgba8, args.texel_formats)):
        parser.error("Standard UBO layout witness is an independent finite profile")
    if sum((args.shared_stage_samplers, args.single_set_samplers,
            args.mixed_resources)) > 1:
        parser.error("Choose one sampled-descriptor profile")
    if args.sampler_visibility != "vertex-fragment" and not args.shared_stage_samplers:
        parser.error("Wider sampler visibility requires --shared-stage-samplers")
    sampler_visibility = {"vertex-fragment": 0x11, "all-graphics": 0x1f, "all": 0x7fffffff}[args.sampler_visibility]

    # A merely present archive may predate the source tree.  Fresh staging is
    # the safe default for a standalone consumer and prevents false link
    # failures (or, worse, validation against yesterday's implementation).
    if not args.use_staged_sdk or not (DIST_SDK / "lib/libps5vk.a").is_file():
        print("Staging current SDK with tools/build_sdk.py...")
        build_env = dict(os.environ)
        if args.bc_filter_format or args.bc_subresource_profile:
            build_env["PS5VK_TEXTURE_COMPRESSION_BC_DIAGNOSTIC"] = "1"
        if args.cube_array_witness:
            build_env["PS5VK_IMAGE_CUBE_ARRAY_DIAGNOSTIC"] = "1"
        subprocess.run([sys.executable, str(ROOT / "tools/build_sdk.py")],
                       env=build_env, check=True)

    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    sdk = foundation / ".deps/native/ps5-payload-sdk"
    clang_wrapper = foundation / "tooling/prospero-clang18"
    linker = sdk / "bin/prospero-lld"
    builder = foundation / "build/host/ps5-native-tool"
    gears = lab / "projects/ps5-agc-gears"

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    (DIST_DIR / "sce_sys").mkdir(parents=True, exist_ok=True)
    (DIST_DIR / "sce_module").mkdir(parents=True, exist_ok=True)

    dep_file = BUILD_DIR / "main.d"
    obj_file = BUILD_DIR / "main.o"
    storage_shader_header = BUILD_DIR / "storage_width_shaders.h"
    sync_shader_header = BUILD_DIR / "sync_shaders.h"
    sampled_shader_header = BUILD_DIR / "sampled_set_shaders.h"
    cube_array_shader_header = BUILD_DIR / "cube_array_witness_shaders.h"
    pie_elf = BUILD_DIR / "consumer_pie.elf"
    eboot_elf = BUILD_DIR / "eboot.elf"
    eboot_bin = DIST_DIR / "eboot.bin"

    if args.check_only:
        if not dep_file.is_file() or not obj_file.is_file():
            sys.exit("Cannot check isolation: object or dependency file missing. Run build first.")
        check_isolation(dep_file, obj_file)
        return

    subprocess.run([
        sys.executable, str(ROOT / "tools/prepare_consumer_storage_shaders.py"),
        "--out", str(storage_shader_header),
    ], check=True)
    subprocess.run([
        sys.executable, str(ROOT / "tools/prepare_consumer_sync_shaders.py"),
        "--out", str(sync_shader_header),
    ], check=True)
    draw_parameter_shader_header = BUILD_DIR / "draw_parameter_shaders.h"
    subprocess.run([
        sys.executable, str(ROOT / "tools/prepare_consumer_draw_parameter_shaders.py"),
        "--out", str(draw_parameter_shader_header),
    ], check=True)

    # 1. Compile consumer main.c
    subprocess.run([
        sys.executable, str(ROOT / "tools/prepare_consumer_sampled_shaders.py"),
        "--out", str(sampled_shader_header),
    ], check=True)
    texel_shader_header = BUILD_DIR / "texel_rgba8_shader.h"
    if args.texel_rgba8:
        subprocess.run([
            sys.executable, str(ROOT / "tools/prepare_consumer_texel_shader.py"),
            "--out", str(texel_shader_header),
        ], check=True)
    texel_format_header = BUILD_DIR / "texel_format_shaders.h"
    if args.texel_formats:
        subprocess.run([
            sys.executable, str(ROOT / "tools/prepare_consumer_texel_formats.py"),
            "--out", str(texel_format_header),
        ], check=True)
    ubo_shader_header = BUILD_DIR / "ubo_standard_layout_shader.h"
    if args.ubo_standard_layout:
        subprocess.run([
            sys.executable, str(ROOT / "tools/prepare_consumer_ubo_shader.py"),
            "--out", str(ubo_shader_header),
        ], check=True)
    if args.cube_array_witness:
        subprocess.run([
            sys.executable, str(ROOT / "tools/prepare_consumer_cube_array_shaders.py"),
            "--out", str(cube_array_shader_header),
        ], check=True)
    cflags = [
        "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections",
        "-MD", "-MP", "-MF", str(dep_file),
        "-I" + str(DIST_SDK / "include"),
        "-I" + str(CONSUMER_DIR),
        "-I" + str(BUILD_DIR),
    ]
    if args.continuous:
        cflags.append("-DCONSUMER_CONTINUOUS=1")
    if args.shared_stage_samplers:
        cflags.append("-DCONSUMER_SHARED_STAGE_SAMPLERS=1")
        cflags.append(f"-DCONSUMER_SAMPLER_VISIBILITY={sampler_visibility}")
    if args.single_set_samplers:
        cflags.append("-DCONSUMER_SINGLE_SET_SAMPLERS=1")
    if args.mixed_resources:
        cflags.append("-DCONSUMER_MIXED_RESOURCES=1")
    if args.texel_rgba8:
        cflags.append("-DCONSUMER_TEXEL_RGBA8=1")
    if args.texel_formats:
        cflags.append("-DCONSUMER_TEXEL_FORMATS=1")
    if args.dxvk_v262_probe:
        cflags.append("-DCONSUMER_DXVK262_PROBE=1")
    if args.cube_array_witness:
        cflags.append("-DCONSUMER_CUBE_ARRAY_WITNESS=1")
        cflags.append(f"-DCONSUMER_CUBE_ARRAY_BASE_LAYER={args.cube_array_base_layer}")
        if args.cube_array_tiled_attachment:
            cflags.append("-DCONSUMER_CUBE_ARRAY_TILED_ATTACHMENT=1")
    if args.bc_filter_format:
        subprocess.run([sys.executable, str(ROOT / "tools/prepare_consumer_bc_filter.py"),
                        "--format", args.bc_filter_format, "--out", str(BUILD_DIR)], check=True)
        cflags.append("-DCONSUMER_BC_FILTER_WITNESS=1")
    if args.bc_subresource_profile:
        subprocess.run([sys.executable, str(ROOT / "tools/prepare_consumer_bc_subresource.py"),
                        "--profile", args.bc_subresource_profile, "--out", str(BUILD_DIR)], check=True)
        cflags.append("-DCONSUMER_BC_SUBRESOURCE_WITNESS=1")
    consumer_source = (CONSUMER_DIR / "ubo_layout_main.c" if args.ubo_standard_layout
                       else CONSUMER_DIR / "main.c")

    has_native_toolchain = clang_wrapper.is_file() and linker.is_file() and builder.is_file()

    print(f"Compiling consumer {consumer_source.name}...")
    if has_native_toolchain:
        env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
        subprocess.run(
            ["sh", str(clang_wrapper), *cflags, "-c", str(consumer_source), "-o", str(obj_file)],
            env=env, check=True
        )
    else:
        subprocess.run(
            ["cc", *cflags, "-c", str(consumer_source), "-o", str(obj_file)],
            check=True
        )

    # Verify isolation immediately after compilation
    check_isolation(dep_file, obj_file)

    if not has_native_toolchain:
        print("Native PS5 toolchain not found; verified isolation on host and skipping packaging.")
        return

    # 2. Link PIE ELF
    map_file = BUILD_DIR / "consumer.map"
    link_cmd = [
        str(linker),
        "-L" + str(sdk / "target/lib"),
        "-T", str(DIST_SDK / "lib/ps5-pie.ld"),
        "--eh-frame-hdr",
        "--version-script", str(DIST_SDK / "lib/app-symbols.map"),
        f"-Map={map_file}",
        "-e", "_start",
        "-o", str(pie_elf),
        str(DIST_SDK / "lib/crt.o"),
        str(obj_file),
        str(DIST_SDK / "lib/libps5vk.a"),
        str(DIST_SDK / "lib/libpsbc.a"),
        str(sdk / "target/lib/libc++.a"),
        str(sdk / "target/lib/libc++abi.a"),
        str(sdk / "target/lib/libunwind.a"),
        str(sdk / "target/lib/libpthread.a"),
        str(sdk / "target/lib/libc.a"),
        "--as-needed",
        *sorted(str(p) for p in (sdk / "target/lib").glob("*.so")),
        str(DIST_SDK / "lib/libSceAgc.so"),
        str(DIST_SDK / "lib/libSceAgcDriver.so"),
    ]
    print("Linking consumer PIE ELF...")
    subprocess.run(link_cmd, check=True)

    # 3. Create eboot.elf and signed eboot.bin
    print("Signing eboot.bin via ps5-native-tool...")
    subprocess.run([
        str(builder), "link", "--in", str(pie_elf), "--out", str(eboot_elf),
        "--stub-dir", str(sdk / "target/lib"), "--module-sdk", "0x02000009",
        "--stub", str(DIST_SDK / "lib/libSceAgc.so"),
        "--stub", str(DIST_SDK / "lib/libSceAgcDriver.so"),
        "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"
    ], check=True)

    subprocess.run([
        str(builder), "self", "--sign", "--in", str(eboot_elf), "--out",
        str(eboot_bin), "--magic", "0x1D3D154F"
    ], check=True)

    # 4. Package metadata and assets
    param = json.loads((gears / "sce_sys/param.json").read_text())
    param.update(
        titleId="PPSA99994",
        conceptId="99994",
        contentId="UP9000-PPSA99994_00-PS5VKCOMPUTE0001"
    )
    param["localizedParameters"]["en-US"]["titleName"] = "PS5 Vulkan SDK Consumer"
    (DIST_DIR / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")

    shutil.copyfile(foundation / "runtime/libc.prx", DIST_DIR / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", DIST_DIR / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", DIST_DIR / "dev.conf")

    files = {}
    for path in sorted(p for p in DIST_DIR.rglob("*") if p.is_file()):
        files[str(path.relative_to(DIST_DIR))] = hashlib.sha256(path.read_bytes()).hexdigest()
    artifact = {
        "title": "PPSA99994",
        "profile": "public-consumer-resource-abi",
        "submit_enabled": True,
        "files": files,
        "buffer_transfer": {
            "api": "Vulkan 1.0",
            "copy_bytes": 7,
            "update_bytes": 8,
            "fill_bytes": 20,
            "whole_tail_bytes": 3,
        },
        "indirect_dispatch": {
            "api": "Vulkan 1.0",
            "groups": [1, 1, 1],
            "offset": 512,
            "result_elements": 64,
        },
        "dynamic_descriptors": {
            "storage_buffers": 2,
            "uniform_buffers": 1,
            "offsets": [256, 256, 256],
            "base_plus_dynamic": True,
            "result_elements": 64,
            "guard_words": 192,
        },
        "storage_width": {
            "storageBuffer8BitAccess": True,
            "storageBuffer16BitAccess": True,
            "shaderInt8": False,
            "shaderInt16": False,
            "storage8_spirv_sha256": hashlib.sha256(
                (ROOT / "build/test-shaders/storage8.spv").read_bytes()).hexdigest(),
            "storage16_spirv_sha256": hashlib.sha256(
                (ROOT / "build/test-shaders/storage16.spv").read_bytes()).hexdigest(),
        },
        "draw_parameters": {
            "api": "Vulkan 1.0 extension",
            "extension": "VK_KHR_shader_draw_parameters",
            "cases": list(DRAW_PARAMETER_CASES),
            "vertex_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_suffix(".vert.spv").read_bytes()
            ).hexdigest(),
            "fragment_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_suffix(".frag.spv").read_bytes()
            ).hexdigest(),
        },
        # DXVK262-T03 witness: indirect firstInstance, multi-draw DrawIndex,
        # GPU-generated arguments, the 65535-command floor and the 32-bit index
        # range, judged per grid cell from the linear staging readback.
        "indirect_draws": {
            "api": "Vulkan 1.0 core features",
            "features": ["drawIndirectFirstInstance", "fullDrawIndexUint32",
                         "multiDrawIndirect"],
            "cases": list(INDIRECT_CASES),
            "extent": INDIRECT_EXTENT,
            "max_commands": INDIRECT_MAX_COMMANDS,
            "vertex_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_name("indirect_witness.vert.spv").read_bytes()
            ).hexdigest(),
            "compute_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_name("indirect_arguments.comp.spv").read_bytes()
            ).hexdigest(),
            "fragment_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_suffix(".frag.spv").read_bytes()
            ).hexdigest(),
        },
        # DXVK262-T05 witness: depth bias with negative/positive/zero clamps,
        # depth clamp against clipping, LINE/POINT polygon modes and the
        # viewport/scissor arrays' bank zero, judged from the linear staging
        # readback of a 64 x 64 frame with a D32 depth attachment. Runs only
        # where the device reports the four features.
        "raster_state": {
            "api": "Vulkan 1.0 core features",
            "features": ["depthBiasClamp", "depthClamp", "fillModeNonSolid",
                         "multiViewport"],
            # True when the staged SDK was built with PS5VK_RASTER_DIAGNOSTIC=1,
            # i.e. the features were reported by the measurement gate rather
            # than by the shipping platform mask. A receipt from such a build
            # is measurement evidence, never a shipping-profile claim.
            "diagnostic_features": os.environ.get("PS5VK_RASTER_DIAGNOSTIC", "0") == "1",
            "cases": list(RASTER_CASES),
            "extent": RASTER_EXTENT,
            "vertex_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_name("raster_witness.vert.spv").read_bytes()
            ).hexdigest(),
            "fragment_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_suffix(".frag.spv").read_bytes()
            ).hexdigest(),
        },
        # DXVK262-T05 end-to-end multiViewport witness: a geometry stage routes
        # primitive i to viewport i across a 4 x 4 tile grid of sixteen
        # viewports. Runs only where geometryShader and multiViewport are both
        # reported (core Vulkan lets only a geometry stage select a viewport).
        "raster_viewport_index": {
            "api": "Vulkan 1.0 core features",
            "features": ["geometryShader", "multiViewport"],
            "cases": ["viewport_index_routing"],
            "extent": RASTER_EXTENT,
            "tiles": 16,
            "vertex_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_name("raster_witness.vert.spv").read_bytes()
            ).hexdigest(),
            "geometry_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_name("raster_viewport_index.geom.spv").read_bytes()
            ).hexdigest(),
            "fragment_shader_sha256": hashlib.sha256(
                draw_parameter_shader_header.with_suffix(".frag.spv").read_bytes()
            ).hexdigest(),
        },
        "synchronization": {
            "api": "Vulkan 1.0",
            "local_size": 128,
            "wave_size": 32,
            "binary_semaphore": True,
            "host_event": True,
            "device_event": True,
            "sync_producer_spirv_sha256": hashlib.sha256(
                (ROOT / "build/test-shaders/sync_producer.spv").read_bytes()).hexdigest(),
            "sync_consumer_spirv_sha256": hashlib.sha256(
                (ROOT / "build/test-shaders/sync_consumer.spv").read_bytes()).hexdigest(),
            "shared_atomic_multiwave_spirv_sha256": hashlib.sha256(
                (ROOT / "build/test-shaders/shared_atomic_multiwave.spv").read_bytes()).hexdigest(),
        },
        "fixed_function": {
            "api": "Vulkan 1.0",
            "width": 1920,
            "height": 1080,
            "frames": 18,
            "color_format": "VK_FORMAT_B8G8R8A8_UNORM",
            "depth_format": "VK_FORMAT_D32_SFLOAT",
            "samples": 1,
            "load_preservation": True,
            "dynamic_viewport": True,
            "dynamic_scissor": True,
        },
        "sampled_graphics": {
            "sets": 4, "descriptors": 96, "rounds": 4,
            "stage_profile": ("single-set" if args.single_set_samplers else
                              "mixed-resources" if args.mixed_resources else
                              "vertex-fragment" if args.shared_stage_samplers else "fragment"),
            "shader_spirv_sha256": hashlib.sha256(sampled_shader_header.with_suffix(
                ".single.spv" if args.single_set_samplers else
                ".mixed.frag.spv" if args.mixed_resources else
                ".shared.frag.spv" if args.shared_stage_samplers else ".spv").read_bytes()).hexdigest(),
        },
    }
    if args.dxvk_v262_probe:
        profile_path = ROOT / "conformance_inventory/dxvk_v262_profile.json"
        matrix_path = ROOT / "conformance_inventory/dxvk_v262_matrix.json"
        profile = json.loads(profile_path.read_text())
        artifact = {
            "title": "PPSA99994",
            "profile": "dxvk-v262-capability-probe",
            "submit_enabled": False,
            "files": files,
            "dxvk": {
                "version": "2.6.2",
                "profile_id": profile["profile"]["id"],
                "target_api": profile["profile"]["api_version"],
                "requirements": profile["summary"]["requirements"],
                "profile_sha256": hashlib.sha256(profile_path.read_bytes()).hexdigest(),
                "matrix_sha256": hashlib.sha256(matrix_path.read_bytes()).hexdigest(),
            },
        }
    if args.ubo_standard_layout:
        artifact = {
            "title": "PPSA99994",
            "profile": "ubo-standard-layout-witness",
            "submit_enabled": True,
            "files": files,
            "ubo_standard_layout": {
                "api": "Vulkan 1.0 KHR extension",
                "shipping_feature": True,
                "shader_spirv_sha256": hashlib.sha256(
                    ubo_shader_header.with_suffix(".spv").read_bytes()).hexdigest(),
                "workgroups": 2,
                "values": 64,
                "ubo_bytes": 136,
                "guarded_output": True,
            },
        }
    if args.cube_array_witness:
        artifact = {
            "title": "PPSA99994",
            "profile": "image-cube-array-witness",
            "submit_enabled": True,
            "files": files,
            "cube_array": {
                "feature": "VkPhysicalDeviceFeatures.imageCubeArray",
                "cubes": 2,
                "faces_per_cube": 6,
                "layers": 12,
                "storage_layers": 12 + args.cube_array_base_layer,
                "base_array_layer": args.cube_array_base_layer,
                "source": "tiled-attachment" if args.cube_array_tiled_attachment
                          else "linear-upload",
                "face_extent": [256, 256] if args.cube_array_tiled_attachment
                               else [4, 4],
                "target_extent": [192, 64],
                "format": "VK_FORMAT_R8G8B8A8_UNORM",
                "vertex_spirv_sha256": hashlib.sha256(
                    cube_array_shader_header.with_suffix(".vert.spv").read_bytes()
                ).hexdigest(),
                "fragment_spirv_sha256": hashlib.sha256(
                    cube_array_shader_header.with_suffix(".frag.spv").read_bytes()
                ).hexdigest(),
            },
        }
    if args.bc_filter_format:
        artifact = {"title": "PPSA99994", "profile": "bc-linear-filter-witness",
                    "submit_enabled": True, "files": files,
                    "bc_filter": json.loads((BUILD_DIR / "bc_filter_contract.json").read_text())}
    if args.bc_subresource_profile:
        artifact = {"title": "PPSA99994", "profile": "bc-subresource-witness",
                    "submit_enabled": True, "files": files,
                    "bc_subresource": json.loads((BUILD_DIR / "bc_subresource_contract.json").read_text())}
    if args.single_set_samplers:
        artifact["sampled_graphics"]["sets"] = 1
        artifact["sampled_graphics"]["elements_per_set"] = 96
    if args.mixed_resources:
        artifact["sampled_graphics"]["uniform_buffers"] = 4
        # VK_SHADER_STAGE_FRAGMENT_BIT: the mixed workload is fragment-only, so
        # its uniform buffers carry the same visibility as its sampled sets.
        artifact["sampled_graphics"]["visibility_mask"] = 0x10
    if args.shared_stage_samplers:
        artifact["sampled_graphics"]["visibility_mask"] = sampler_visibility
        artifact["sampled_graphics"]["vertex_spirv_sha256"] = hashlib.sha256(
            sampled_shader_header.with_suffix(".shared.vert.spv").read_bytes()).hexdigest()
    if args.texel_rgba8:
        artifact["texel_rgba8"] = {
            "format": "VK_FORMAT_R8G8B8A8_UNORM",
            "texels": 64,
            "channels": "R,G,B,A",
            "shader_spirv_sha256": hashlib.sha256(
                texel_shader_header.with_suffix(".spv").read_bytes()).hexdigest(),
        }
    if args.texel_formats:
        artifact["texel_formats"] = {
            "case_count": 41,
            "components_per_case": 4,
            "shader_spirv_sha256": {
                kind: hashlib.sha256(
                    texel_format_header.with_suffix(f".{kind}.spv").read_bytes()
                ).hexdigest()
                for kind in ("float", "uint", "sint")
            },
        }
    artifact_path = DIST_DIR.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")

    print(f"Consumer successfully built and packaged into {DIST_DIR}")
    print(f"eboot.bin sha256: {hashlib.sha256(eboot_bin.read_bytes()).hexdigest()}")
    print(f"artifact manifest: {artifact_path}")


if __name__ == "__main__":
    main()
