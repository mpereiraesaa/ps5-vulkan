"""Build only this project's native bootstrap using read-only lab dependencies."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from lab import lab_root

ROOT = Path(__file__).resolve().parents[1]


def run(*args, env=None):
    subprocess.run(list(map(str, args)), check=True, cwd=ROOT, env=env)


def main():
    compute = os.environ.get("PS5VK_COMPUTE") == "1"
    graphics = os.environ.get("PS5VK_GRAPHICS_LINK")
    graphics_api = os.environ.get("PS5VK_GRAPHICS_API")
    witnesses = os.environ.get("PS5VK_GRAPHICS_WITNESSES", "0")
    if witnesses not in ("0", "1", "2") or (witnesses != "0" and not graphics_api):
        raise SystemExit("PS5VK_GRAPHICS_WITNESSES requires graphics profile API and must be 0 or 1 or 2 (depth off)")
    continuous = os.environ.get("PS5VK_GRAPHICS_CONTINUOUS", "0")
    if continuous not in ("0", "1") or (continuous == "1" and not graphics_api):
        raise SystemExit("PS5VK_GRAPHICS_CONTINUOUS requires graphics profile API and must be 0 or 1")
    observe_scene = os.environ.get("PS5VK_GRAPHICS_OBSERVE", "0")
    if continuous == "1" and observe_scene != "0":
        raise SystemExit("Continuous mode cannot use observation pauses")
    if observe_scene not in ("0", "1") or (observe_scene == "1" and not graphics_api):
        raise SystemExit("PS5VK_GRAPHICS_OBSERVE requires graphics profile API and must be 0 or 1")
    scissor_probe = os.environ.get("PS5VK_GRAPHICS_SCISSOR_PROBE", "0")
    if scissor_probe not in ("0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15") or (scissor_probe != "0" and not graphics_api):
        raise SystemExit("PS5VK_GRAPHICS_SCISSOR_PROBE requires graphics profile API: 0-10 existing diagnostics, 11 layered images, 12 mipmaps, 13 vertex bindings, 14 explicit depth clear witness, 15 occlusion-counter probe")
    mip_view_base=os.environ.get("PS5VK_MIP_VIEW_BASE","0")
    if mip_view_base not in ("0","1") or (mip_view_base!="0" and scissor_probe!="12"):
        raise SystemExit("PS5VK_MIP_VIEW_BASE must be 0, or 1 only for mipmap diagnostic")
    mip_force_lod=os.environ.get("PS5VK_MIP_FORCE_LOD","-1")
    if mip_force_lod not in ("-1","0","1","2") or (mip_force_lod!="-1" and scissor_probe!="12"):
        raise SystemExit("PS5VK_MIP_FORCE_LOD must be -1, 0, 1, or 2 only for mipmap diagnostic")
    mip_lod_bias=os.environ.get("PS5VK_MIP_LOD_BIAS","0")
    if mip_lod_bias not in ("-2","0","2") or (mip_lod_bias!="0" and scissor_probe!="12"):
        raise SystemExit("PS5VK_MIP_LOD_BIAS must be -2, 0, or 2 only for mipmap diagnostic")
    if mip_lod_bias!="0" and mip_force_lod!="-1":
        raise SystemExit("PS5VK_MIP_LOD_BIAS cannot be combined with a forced LOD clamp")
    scene_split = os.environ.get("PS5VK_GRAPHICS_SCENE_SPLIT", "0")
    if scene_split not in ("0", "1") or (scene_split == "1" and not graphics_api):
        raise SystemExit("PS5VK_GRAPHICS_SCENE_SPLIT requires graphics profile API and must be 0 or 1")
    layer_probe = os.environ.get("PS5VK_LAYER_PROBE", "0")
    if layer_probe not in ("0", "1") or (layer_probe == "1" and not graphics_api):
        raise SystemExit("PS5VK_LAYER_PROBE requires the graphics profile API and must be 0 or 1")
    multiview_diagnostic = os.environ.get("PS5VK_MULTIVIEW_DIAGNOSTIC", "0")
    if multiview_diagnostic not in ("0", "1") or (multiview_diagnostic == "1" and not graphics_api):
        raise SystemExit("PS5VK_MULTIVIEW_DIAGNOSTIC requires the graphics profile API and must be 0 or 1")
    clip_cull_probe = os.environ.get("PS5VK_CLIP_CULL_PROBE", "0")
    if clip_cull_probe not in ("0", "1") or (clip_cull_probe == "1" and not graphics_api):
        raise SystemExit("PS5VK_CLIP_CULL_PROBE requires the graphics profile API and must be 0 or 1")
    # The six-view witness is the only consumer of the diagnostic gate, so it
    # requires both: a real view mask AND a runtime-compiled vertex stage that
    # reads gl_ViewIndex. It is a single bounded scene, never combined with the
    # other probes, with witnesses or with continuous mode, so its telemetry
    # cannot be confused with another diagnostic's.
    multiview_view_probe = os.environ.get("PS5VK_MULTIVIEW_VIEW_PROBE", "0")
    if multiview_view_probe not in ("0", "1"):
        raise SystemExit("PS5VK_MULTIVIEW_VIEW_PROBE must be 0 or 1")
    # The instance witness is the six-view scene with ONE instance whose
    # firstInstance is the pinned floor 0x07ffffff: it needs the scene, so it
    # cannot be selected on its own.
    multiview_instance_probe = os.environ.get("PS5VK_MULTIVIEW_INSTANCE_PROBE", "0")
    if multiview_instance_probe not in ("0", "1"):
        raise SystemExit("PS5VK_MULTIVIEW_INSTANCE_PROBE must be 0 or 1")
    if multiview_instance_probe == "1" and multiview_view_probe != "1":
        raise SystemExit("PS5VK_MULTIVIEW_INSTANCE_PROBE requires PS5VK_MULTIVIEW_VIEW_PROBE=1")
    if multiview_view_probe == "1" and os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1":
        raise SystemExit("PS5VK_MULTIVIEW_VIEW_PROBE requires the runtime graphics profile (PS5VK_RUNTIME_GRAPHICS=1)")
    if multiview_view_probe == "1" and multiview_diagnostic != "1":
        raise SystemExit("PS5VK_MULTIVIEW_VIEW_PROBE requires PS5VK_MULTIVIEW_DIAGNOSTIC=1")
    if multiview_view_probe == "1" and (scissor_probe != "0" or witnesses != "0" or continuous == "1" or
                                        observe_scene != "0" or scene_split == "1"):
        raise SystemExit("PS5VK_MULTIVIEW_VIEW_PROBE is a bounded standalone scene and cannot be combined with other probes, witnesses, observation or continuous mode")
    input_attachment_probe = os.environ.get("PS5VK_INPUT_ATTACHMENT_PROBE", "0")
    if input_attachment_probe not in ("0", "1"):
        raise SystemExit("PS5VK_INPUT_ATTACHMENT_PROBE must be 0 or 1")
    if input_attachment_probe == "1" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_INPUT_ATTACHMENT_PROBE requires graphics API, runtime graphics and draw")
    if input_attachment_probe == "1" and (multiview_view_probe == "1" or
            scissor_probe != "0" or witnesses != "0" or continuous == "1" or
            observe_scene != "0" or scene_split == "1" or layer_probe == "1"):
        raise SystemExit("PS5VK_INPUT_ATTACHMENT_PROBE is a bounded standalone scene")
    if clip_cull_probe == "1" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_CLIP_CULL_PROBE requires graphics API, runtime graphics and draw")
    if clip_cull_probe == "1" and (multiview_view_probe == "1" or input_attachment_probe == "1" or
            scissor_probe != "0" or witnesses != "0" or continuous == "1" or
            observe_scene != "0" or scene_split == "1" or layer_probe == "1"):
        raise SystemExit("PS5VK_CLIP_CULL_PROBE is a bounded standalone scene")
    if scene_split == "1" and int(scissor_probe) >= 3:
        raise SystemExit("Planar triangle diagnostic cannot split the cube draw")
    shell_close = os.environ.get("PS5VK_SHELL_CLOSE") == "1"
    if shell_close and not graphics_api:
        raise SystemExit("Shell-close validation requires graphics profile API")
    keep_agc_module = os.environ.get("PS5VK_KEEP_AGC_MODULE") == "1"
    if keep_agc_module and not graphics_api:
        raise SystemExit("AGC unload diagnostic requires graphics profile API")
    exit_control = os.environ.get("PS5VK_EXIT_CONTROL", "0")
    if exit_control not in ("0", "1", "2", "3"):
        raise SystemExit("PS5VK_EXIT_CONTROL must be 0, 1 (CRT/logging), 2 (device lifecycle), or 3 (logging load)")
    exit_control = int(exit_control)
    if shell_close and (exit_control or keep_agc_module):
        raise SystemExit("Do not combine shell-close validation with exit/unload diagnostics")
    if exit_control and not graphics_api:
        raise SystemExit("Exit control requires the graphics profile API build")
    if graphics_api:
        if graphics or compute:
            raise SystemExit("Choose one native variant")
        graphics = graphics_api
    if graphics and compute:
        raise SystemExit("Choose one native variant")
    graphics_manifest = None
    if graphics:
        graphics = Path(graphics).resolve()
        if ROOT / "build/graphics" not in graphics.parents:
            raise SystemExit("Use a project-local graphics control")
        graphics_manifest = json.loads((graphics / "manifest.json").read_text())
        if graphics_manifest.get("scope") == "compiler-inspection-only":
            raise SystemExit("Compiler inspection is not a native ABI/library adapter")
        if graphics_manifest.get("native_input_version") != 3:
            raise SystemExit("Graphics control lacks current interpolation ABI; recompile the owned .pipe")
        quantization = graphics_manifest.get("shader_context", {}).get("vertex_quantization", {})
        if quantization.get("byte_address") != 0x28be4 or quantization.get("value") != 0x2d:
            raise SystemExit("Graphics control lacks audited vertex quantization; recompile the owned .pipe")
    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    pin = subprocess.check_output(["git", "-C", str(foundation),
                                   "rev-parse", "HEAD"], text=True).strip()
    if pin != "37dd53602bdead63936f718004555ba10154be48":
        raise SystemExit("Native foundation changed; review before building")
    sdk_env = os.environ.get("PS5_PAYLOAD_SDK")
    if sdk_env:
        sdk = Path(sdk_env).resolve()
        if not ((sdk / "bin/prospero-lld").is_file() and (sdk / "target/lib").is_dir()):
            raise SystemExit(f"PS5_PAYLOAD_SDK={sdk_env} is not a valid payload SDK (missing prospero-lld or target/lib)")
    else:
        sdk = foundation / ".deps/native/ps5-payload-sdk"
    native = foundation / "tooling/native"
    builder = foundation / "build/host/ps5-native-tool"
    gears = lab / "projects/ps5-agc-gears"
    logger = lab / "projects/logging_server/client"
    out = ROOT / ("build/native-compute" if compute else "build/native")
    dist = ROOT / ("dist-compute/PPSA99994" if compute else "dist/PPSA99994")
    if graphics:
        out, dist = ROOT / "build/native-graphics-link", ROOT / "dist-graphics-link/PPSA99994"
        if graphics_api:
            out, dist = ROOT / "build/native-graphics-api", ROOT / "dist-graphics-api/PPSA99994"
    if compute or graphics_api:
        run("python3", "tools/prepare_vulkan_headers.py", "--check")
        run("python3", "tools/build_program_library.py")
    for directory in (out, out / "stubs", dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    expected = "4ce5d1dc537e4e0d4a9eee302a087e882a66e541ee53ea209e2ad65f53b0f356"
    candidates = sorted((ROOT / "build/compute").glob("control-*/first.elf"))
    shader = next((p for p in candidates if hashlib.sha256(p.read_bytes()).hexdigest() == expected), None)
    if shader is None:
        raise SystemExit("No audited bootstrap compute profile shader; run make compiler-control")
    run("llvm-objcopy-18", "--dump-section", f".text={out / 'compute-code.bin'}", shader,
        out / "shader-copy.elf")
    code = (out / "compute-code.bin").read_bytes()
    (out / "compute_shader.h").write_text(
        '#define PS5VK_SHADER_ELF_SHA256 "' + expected + '"\n' +
        'static const unsigned char ps5vk_shader_code[] = {' +
        ','.join(str(b) for b in code) + '};\n')
    cc = ["sh", foundation / "tooling/prospero-clang18"]
    common = ["-O2", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
              "-fdata-sections", "-I" + str(gears / "include"),
              "-I" + str(logger), "-I" + str(ROOT / "src"), "-I" + str(out),
              "-DPS5VK_SUBMIT=" + ("1" if os.environ.get("PS5VK_SUBMIT") == "1" else "0"),
              "-DPS5VK_DMA_ONLY=" + ("1" if os.environ.get("PS5VK_DMA_ONLY") == "1" else "0"),
              "-DPS5VK_INSPECT=" + ("1" if os.environ.get("PS5VK_INSPECT") == "1" else "0")]
    objects = []
    sources = [
        ("main", ROOT / "native/main.c", []),
        ("check", ROOT / "src/compute_check.c", []),
        ("commands", ROOT / "src/compute_commands.c", []),
        ("compute", ROOT / "native/compute_run.c", []),
        ("log", logger / "ps5log.c", ["-include", logger / "ps5log_ps5_net.h"]),
        ("net", logger / "ps5log_ps5_net.c", []),
    ]
    use_runtime_compiler = compute and os.environ.get("PS5VK_RUNTIME_COMPILER") != "0"
    use_runtime_graphics = os.environ.get("PS5VK_RUNTIME_GRAPHICS") == "1"
    use_runtime_sdk = os.environ.get("PS5VK_USE_SDK") == "1"
    if use_runtime_sdk and not use_runtime_graphics:
        raise SystemExit("SDK-linked diagnostic requires runtime graphics")
    if use_runtime_graphics and not graphics_api:
        raise SystemExit("Runtime graphics requires a graphics API build")
    if compute:
        common += ["-I" + str(ROOT / "third_party/vulkan-headers/include"),
                   "-I" + str(ROOT / "build/program-library")]
        if use_runtime_compiler:
            common += ["-I" + str(ROOT / "third_party/psbc-reference"),
                       "-I" + str(ROOT / "third_party/psbc-reference/src"),
                       "-I" + str(ROOT / "third_party/psbc-reference/libpsbc"),
                       "-I" + str(ROOT / "third_party/opengnm/include"),
                       "-DPS5VK_RUNTIME_COMPILER=1"]
        sources = [(p.stem, p, []) for p in sorted((ROOT / "src").glob("vk_*.c"))]
        compute_srcs = [ROOT / "native/compute_main.c",
            ROOT / "src/graphics_program.c", ROOT / "src/texture_copy.c", ROOT / "src/texture_format.c", ROOT / "src/texture_layout.c",
            ROOT / "native/platform_ps5.c", ROOT / "native/memory_ps5.c", ROOT / "native/queue_ps5.c",
            ROOT / "src/compute_commands.c", ROOT / "src/dispatch_encode.c", ROOT / "src/descriptor_encode.c"]
        if use_runtime_compiler:
            compute_srcs += [ROOT / "src/compilation_cache.c", ROOT / "src/vk_pipeline_cache.c",
                             ROOT / "src/ps5vk_compiler.c", ROOT / "src/ps5_compiler_shims.c"]
        sources += [(p.stem, p, []) for p in compute_srcs]
        sources += [("log", logger / "ps5log.c", ["-include", logger / "ps5log_ps5_net.h"]),
                    ("net", logger / "ps5log_ps5_net.c", [])]
    if graphics:
        common += ["-I" + str(graphics), "-I" + str(ROOT / "native"),
                   "-I" + str(gears / "src"), "-I" + str(ROOT / "third_party/vulkan-headers/include")]
        sources = [(p.stem, p, []) for p in (ROOT / "native/graphics_link_main.c",
            ROOT / "native/graphics_pair.c", ROOT / "native/memory_ps5.c",
            ROOT / "src/shader_relocate.c", gears / "src/ps5_shader_header.c")]
        sources += [("log", logger / "ps5log.c", ["-include", logger / "ps5log_ps5_net.h"]),
                    ("net", logger / "ps5log_ps5_net.c", [])]
        if graphics_api:
            sources = [s for s in sources if s[0] != "graphics_link_main"]
            graphics_source = graphics_manifest.get("source")
            scene = (not use_runtime_graphics or scissor_probe == "12") and graphics_source in (
                "experiments/graphics/scene3d.pipe",
                "experiments/graphics/scene3d-uint.pipe",
                "experiments/graphics/scene3d-sint.pipe",
                "experiments/graphics/scene3d-array.pipe",
                "experiments/graphics/scene3d-cube.pipe",
                "experiments/graphics/scene3d-3d.pipe",
                "experiments/graphics/scene3d-1d.pipe",
                "experiments/graphics/scene3d-1d-array.pipe",
                "experiments/graphics/scene3d-mipmap.pipe")
            image_target={
                "experiments/graphics/scene3d-array.pipe":1,
                "experiments/graphics/scene3d-cube.pipe":2,
                "experiments/graphics/scene3d-3d.pipe":3,
                "experiments/graphics/scene3d-1d.pipe":4,
                "experiments/graphics/scene3d-1d-array.pipe":5,
            }.get(graphics_source,0)
            if scissor_probe=="11" and not image_target:
                raise SystemExit("Layered sampled diagnostic requires scene3d-array.pipe, scene3d-cube.pipe or scene3d-3d.pipe")
            if image_target and scissor_probe!="11":
                raise SystemExit("Layered sampled controls require PS5VK_GRAPHICS_SCISSOR_PROBE=11")
            if scissor_probe=="12" and graphics_source!="experiments/graphics/scene3d-mipmap.pipe":
                raise SystemExit("Mipmap diagnostic requires scene3d-mipmap.pipe")
            if graphics_source=="experiments/graphics/scene3d-mipmap.pipe" and scissor_probe!="12":
                raise SystemExit("scene3d-mipmap.pipe requires PS5VK_GRAPHICS_SCISSOR_PROBE=12")
            integer_sampled_sign = 0
            if scissor_probe == "10":
                integer_sampled_sign = {
                    "experiments/graphics/scene3d-uint.pipe": 1,
                    "experiments/graphics/scene3d-sint.pipe": 2,
                }.get(graphics_source, 0)
                if not integer_sampled_sign:
                    raise SystemExit("Integer sampled diagnostic requires scene3d-uint.pipe or scene3d-sint.pipe")
            if witnesses != "0" and (not scene or continuous != "0" or observe_scene != "0" or
                    scissor_probe != "0" or scene_split != "0" or exit_control or keep_agc_module or
                    os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
                raise SystemExit("Witnesses require the independent bounded scene draw without other diagnostics")
            common += ["-DPS5VK_GRAPHICS_WITNESSES=" + witnesses]
            witness_header = subprocess.check_output(["python3", "-m", "tools.scene_witnesses"],cwd=ROOT)
            (out / "scene_witnesses.h").write_bytes(witness_header)
            if continuous == "1" and (not scene or scissor_probe != "0" or
                    os.environ.get("PS5VK_GRAPHICS_PRESENT") != "1" or os.environ.get("PS5VK_GRAPHICS_DRAW") != "1" or
                    exit_control or keep_agc_module or shell_close):
                raise SystemExit("Continuous mode requires the normal presented draw scene without lifecycle diagnostics")
            common += ["-DPS5VK_GRAPHICS_CONTINUOUS=" + continuous]
            if observe_scene == "1" and (not scene or scissor_probe != "0" or os.environ.get("PS5VK_GRAPHICS_PRESENT") != "1"):
                raise SystemExit("Scene observation requires the normal presented scene, without scissor diagnostics")
            common += ["-DPS5VK_GRAPHICS_OBSERVE=" + observe_scene]
            if scissor_probe != "0" and not scene and not (scissor_probe in ("8", "13") and use_runtime_graphics):
                raise SystemExit("Scissor diagnostic requires scene3d.pipe")
            if scissor_probe in ("8", "13") and not use_runtime_graphics:
                raise SystemExit("Vertex-format diagnostic requires runtime graphics")
            common += ["-DPS5VK_GRAPHICS_SCISSOR_PROBE=" + scissor_probe]
            common += ["-DPS5VK_MIP_VIEW_BASE=" + mip_view_base]
            common += ["-DPS5VK_MIP_FORCE_LOD=" + mip_force_lod]
            common += ["-DPS5VK_MIP_LOD_BIAS=" + mip_lod_bias]
            common += ["-DPS5VK_IMAGE_TARGET=" + str(image_target)]
            common += ["-DPS5VK_INTEGER_SAMPLED_SIGN=" + str(integer_sampled_sign)]
            if scene_split == "1" and not scene:
                raise SystemExit("Split-draw diagnostic requires scene3d.pipe")
            common += ["-DPS5VK_GRAPHICS_SCENE_SPLIT=" + scene_split]
            common += ["-DPS5VK_LAYER_PROBE=" + layer_probe]
            common += ["-DPS5VK_MULTIVIEW_DIAGNOSTIC=" + multiview_diagnostic]
            common += ["-DPS5VK_MULTIVIEW_VIEW_PROBE=" + multiview_view_probe]
            common += ["-DPS5VK_MULTIVIEW_INSTANCE_PROBE=" + multiview_instance_probe]
            common += ["-DPS5VK_INPUT_ATTACHMENT_PROBE=" + input_attachment_probe]
            common += ["-DPS5VK_CLIP_CULL_PROBE=" + clip_cull_probe]
            common += ["-DPS5VK_GRAPHICS_SCENE=" + ("1" if scene else "0")]
            common += ["-DPS5VK_EXIT_CONTROL=" + str(exit_control)]
            common += ["-DPS5VK_SHELL_CLOSE=" + str(int(shell_close))]
            common += ["-DPS5VK_KEEP_AGC_MODULE=" + str(int(keep_agc_module))]
            common += ["-DPS5VK_GRAPHICS_API=1", "-DPS5VK_GRAPHICS_DRAW=" + ("1" if os.environ.get("PS5VK_GRAPHICS_DRAW") == "1" else "0"),
                       "-DPS5VK_GRAPHICS_PRESENT=" + ("1" if os.environ.get("PS5VK_GRAPHICS_PRESENT") == "1" else "0"),
                       "-I" + str(ROOT / "build/program-library")]
            sources += [(p.stem, p, []) for p in sorted((ROOT / "src").glob("vk_*.c"))]
            sources += [(p.stem, p, []) for p in (
                ROOT / "native/graphics_main.c", ROOT / "native/compute_main.c", ROOT / "native/platform_ps5.c", ROOT / "src/scene_geometry.c", ROOT / "src/scene_region.c", ROOT / "src/sampler_core_probe.c", ROOT / "src/sampled_format_probe.c", ROOT / "src/integer_sampled_probe.c", ROOT / "src/vertex_format_probe.c", ROOT / "src/color_clear.c", ROOT / "src/color_detile.c",
                # The witness oracle is part of the artifact: the same pure
                # classifier the host regressions exercise decides the verdict
                # on the console, so the readback is judged by proven code.
                ROOT / "src/multiview_witness.c",
                ROOT / "src/clip_cull_witness.c",
                ROOT / "native/queue_ps5.c", ROOT / "native/graphics_pipeline_ps5.c",
                ROOT / "native/image_ps5.c", ROOT / "src/depth_layout.c", ROOT / "src/texture_format.c", ROOT / "src/texture_layout.c",
                ROOT / "native/draw_prepare_ps5.c", ROOT / "native/draw_emit_ps5.c", ROOT / "native/index_emit_ps5.c",
                ROOT / "native/input_attachment_gate.c",
                ROOT / "native/input_attachment_oracle.c",
                ROOT / "native/input_attachment_probe.c",
                ROOT / "native/command_arena_ps5.c", ROOT / "src/graphics_sync.c",
                ROOT / "src/vertex_descriptor.c", ROOT / "src/vertex_fetch.c", ROOT / "src/index_fetch.c",
                ROOT / "src/triangle_readback.c", ROOT / "src/texture_descriptor.c", ROOT / "src/texture_copy.c", ROOT / "src/texture_dma.c", ROOT / "src/image_layout_state.c",
                ROOT / "native/graphics_queue_ps5.c",
                ROOT / "native/present_ps5.c", gears / "src/ps5_videoout.c",
                gears / "src/ps5_present.c", gears / "src/ps5_event_adapter.c", gears / "src/ps5_frame_completion.c",
                ROOT / "native/draw_state_ps5.c", ROOT / "native/viewport_ps5.c", ROOT / "native/targets_ps5.c",
                gears / "src/ps5_pipeline.c", gears / "src/ps5_color_target.c", gears / "src/ps5_depth_target.c",
                gears / "src/ps5_agc_writer.c", gears / "src/ps5_gpu_span.c",
                ROOT / "src/graphics_program.c", ROOT / "src/compute_commands.c",
                ROOT / "src/dispatch_encode.c", ROOT / "src/descriptor_encode.c",
                ROOT / "src/compilation_cache.c")]
    if use_runtime_graphics:
        run(sys.executable,ROOT / "tools/prepare_runtime_graphics.py","--out",out / "runtime_graphics_spirv.h")
        common += ["-DPS5VK_RUNTIME_GRAPHICS=1", "-I" + str(ROOT / "third_party/psbc-reference")]
        sources += [(p.stem, p, []) for p in (
            ROOT / "native/runtime_shader.c", ROOT / "native/runtime_graphics_compiler.c",
            ROOT / "native/runtime_graphics_cache.c",
            ROOT / "src/spirv_graphics_interface.c",
            ROOT / "native/runtime_graphics_ps5.c", ROOT / "src/ps5_compiler_shims.c")]
    if use_runtime_sdk:
        run(sys.executable, ROOT / "tools/build_sdk.py")
        # Only application/test-oracle objects remain outside libps5vk.a.
        # The harness can inspect internals, but cannot supply backend objects.
        application_sources = {"graphics_main", "compute_main", "scene_geometry",
                               "scene_region", "sampled_format_probe", "integer_sampled_probe", "vertex_format_probe",
                               "triangle_readback"}
        sources = [item for item in sources if item[0] in application_sources]
    source_names = [name for name, _, _ in sources]
    if len(source_names) != len(set(source_names)):
        duplicates = sorted({name for name in source_names if source_names.count(name) > 1})
        raise SystemExit("Duplicate native object stems: " + ", ".join(duplicates))
    for name, source, extra in sources:
        obj = out / (name + ".o")
        run(*cc, "-std=c11", *common, *extra, "-c", source, "-o", obj, env=env)
        objects.append(obj)
    crt = out / "crt.o"
    run(*cc, "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", "-c",
        native / "app_crt.cpp", "-o", crt, env=env)
    stub = out / "stubs/libSceAgc.so"
    run(*cc, "-fPIC", "-I" + str(gears / "include"), "-c",
        gears / "native/stubs/libSceAgc.c", "-o", out / "agc.o", env=env)
    linker = sdk / "bin/prospero-lld"
    run(*cc, "-fPIC", "-c", ROOT / "native/index_import_stub.c", "-o", out / "agc_index.o", env=env)
    run(linker, "--shared", "-soname", "libSceAgc.prx", "-o", stub, out / "agc.o", out / "agc_index.o")
    driver = out / "stubs/libSceAgcDriver.so"
    run(*cc, "-fPIC", "-I" + str(gears / "include"), "-c",
        gears / "native/stubs/libSceAgcDriver.c", "-o", out / "driver.o", env=env)
    run(linker, "--shared", "-soname", "libSceAgcDriver.prx", "-o", driver, out / "driver.o")
    extra_libs = []
    if use_runtime_sdk:
        extra_libs.append(str(ROOT / "dist-sdk/lib/libps5vk.a"))
    if use_runtime_compiler or use_runtime_graphics:
        psbc_lib = ROOT / ("dist-sdk/lib/libpsbc.a" if use_runtime_sdk else "build/libpsbc.ps5.a")
        if not psbc_lib.is_file():
            run(sys.executable, str(ROOT / "tools/build_psbc.py"), "--target=ps5")
        extra_libs += [
            str(psbc_lib),
            str(sdk / "target/lib/libc++.a"),
            str(sdk / "target/lib/libc++abi.a"),
            str(sdk / "target/lib/libunwind.a"),
            str(sdk / "target/lib/libpthread.a"),
            str(sdk / "target/lib/libc.a"),
        ]
    pie_ld = (ROOT / "native/ps5-pie.ld") if (ROOT / "native/ps5-pie.ld").is_file() else (native / "ps5-pie.ld")
    syms_map = (ROOT / "native/app-symbols.map") if (ROOT / "native/app-symbols.map").is_file() else (native / "app-symbols.map")
    run(linker, "-L" + str(sdk / "target/lib"), "-T", pie_ld, "--eh-frame-hdr",
        "--version-script", syms_map, "-e", "_start",
        "-o", out / "pie.elf", crt, *objects, *extra_libs, "--as-needed",
        *sorted((sdk / "target/lib").glob("*.so")), stub, driver)
    run(builder, "link", "--in", out / "pie.elf", "--out", out / "eboot.elf",
        "--stub-dir", sdk / "target/lib", "--module-sdk", "0x02000009",
        "--stub", stub, "--stub", driver, "--companion-sdk", "0x08050001", "--file-name", "eboot.elf")
    run(builder, "self", "--sign", "--in", out / "eboot.elf", "--out",
        dist / "eboot.bin", "--magic", "0x1D3D154F")
    # Reuse the validated metadata schema, not the other title's identity/art.
    param = json.loads((gears / "sce_sys/param.json").read_text())
    param.update(titleId="PPSA99994", conceptId="99994",
                 contentId="UP9000-PPSA99994_00-PS5VKCOMPUTE0001")
    param["localizedParameters"]["en-US"]["titleName"] = "PS5 Vulkan"
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    # Temporary unmodified boilerplate icon; title metadata is project-specific.
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    manifest = {"title": "PPSA99994", "stage": "compute-bootstrap",
                "runtime_graphics": use_runtime_graphics,
                "runtime_sdk": use_runtime_sdk,
                "dma_only": os.environ.get("PS5VK_DMA_ONLY") == "1",
                "inspection_hold": os.environ.get("PS5VK_INSPECT") == "1",
                "submit_enabled": os.environ.get("PS5VK_SUBMIT") == "1",
                "foundation": pin, "files": {}}
    if use_runtime_sdk:
        manifest["sdk_archive_sha256"] = {
            name: hashlib.sha256((ROOT / "dist-sdk/lib" / name).read_bytes()).hexdigest()
            for name in ("libps5vk.a", "libpsbc.a")}
        manifest["application_objects"] = [name for name, _, _ in sources]
    if compute:
        manifest.update(stage="compute-api", submit_enabled=True,
                        compiler="runtime-psbc-aco" if use_runtime_compiler else "offline-exact-library",
                        dma_only=False, inspection_hold=False,
                        program_library=json.loads((ROOT / "build/program-library/manifest.json").read_text()))
    if graphics:
        manifest.update(stage="graphics-native-link-only", submit_enabled=False,
                        graphics=json.loads((graphics / "manifest.json").read_text()))
        if graphics_api:
            geometry_fixture={1:"sampled-image-array",2:"sampled-image-cube",
                              3:"sampled-image-3d",4:"sampled-image-1d",
                              5:"sampled-image-1d-array"}.get(image_target)
            if not geometry_fixture:
                geometry_fixture=("sampler-uv-ladder" if scissor_probe == "4" else
                    ("sampler-core-addressing" if scissor_probe == "6" else
                    ("sampled-format-candidates" if scissor_probe == "7" else
                    ("vertex-format-cases" if scissor_probe == "8" else
                    ("sampled-format-filtering" if scissor_probe == "9" else
                    ("integer-sampled-formats" if scissor_probe == "10" else
                    ("sampled-image-mipmaps" if scissor_probe == "12" else
                    ("planar-triangle" if int(scissor_probe)>=3 else "source-default"))))))))
            manifest.update(stage="graphics-api-creation-only", submit_enabled=False,
                            scene="two-cubes" if scene else "triangle-controls",
                            scissor_probe=int(scissor_probe),
                            mip_lod_bias=int(mip_lod_bias),
                            scissor_depth_comparison=scissor_probe in ("2", "3"),
                            geometry_fixture=geometry_fixture,
                            image_target={1:"2d-array",2:"cube",3:"3d",4:"1d",
                                          5:"1d-array"}.get(image_target),
                            integer_sampled_sign={1:"uint",2:"sint"}.get(integer_sampled_sign),
                            visual_hold_seconds=10 if scissor_probe in ("3", "5") else 0,
                            observation_frame_pause_us=60000 if observe_scene == "1" else 0,
                            exact_interior_witnesses=int(witnesses),
                            runtime_mode="continuous" if continuous == "1" else "bounded-diagnostic",
                            scissor_register_load="indirect-plus-direct-replay" if int(scissor_probe) else "indirect",
                            scene_draw_partition="two-36-index-draws" if scene_split == "1" else "single-draw",
                            exit_control=exit_control, keep_agc_module=keep_agc_module,
                            termination="os-close-during-render" if continuous == "1" else ("shell-close-after-cleanup" if shell_close else "return-main"))
            if clip_cull_probe == "1":
                manifest.update(scene=None,
                                geometry_fixture="clip-cull-distance-coverage",
                                sample_count=1, clip_cull_probe=1,
                                clip_cull_extent=64, clip_cull_cases=8)
            if os.environ.get("PS5VK_GRAPHICS_DRAW") == "1":
                manifest.update(stage="graphics-api-offscreen-draw", submit_enabled=True,
                                compute_regression="compute-before-and-after-graphics")
                if os.environ.get("PS5VK_GRAPHICS_PRESENT") == "1":
                    manifest.update(stage="graphics-api-native-presentation-reuse")
    if exit_control:
        manifest.update(stage={1:"graphics-exit-control-no-graphics",2:"graphics-exit-control-device",3:"graphics-exit-control-logging-load"}[exit_control], submit_enabled=False,
                        scene=None)
    if use_runtime_graphics:
        vertex_probe = scissor_probe == "8"
        manifest.update(compiler="runtime-psbc-aco", target_gfx=1013,
                        graphics_shader_source="owned-runtime-vertex-formats" if vertex_probe else "owned-runtime-triangle",
                        graphics_offline_library_role="negative-lookup-control-only")
        runtime_inputs = (("vertex", "runtime_triangle.vert"), ("fragment", "runtime_triangle.frag"))
        if clip_cull_probe == "1":
            # The one scene whose pre-raster stage exports clip and cull
            # distances: recorded in the manifest so the artifact identity
            # covers the shader that produced the readback.
            manifest["graphics_shader_source"] = "owned-runtime-clip-cull-distances"
        if multiview_view_probe == "1":
            # The one scene whose vertex stage reads gl_ViewIndex, and the only
            # place the diagnostic gate is exercised. Recorded in the manifest so
            # the artifact's identity covers the shader that produced it.
            manifest["graphics_shader_source"] = "owned-runtime-view-index"
            manifest["multiview_witness"] = {
                "view_mask": "0x3f", "views": 6, "framebuffer_layers": 1,
                "layers_per_image": 7, "extent": [64, 64],
                "guard_layer": 6, "diagnostic_gate": True}
            runtime_inputs = (("vertex", "runtime_view_index.vert"),
                              ("fragment", "runtime_triangle.frag"))
            if multiview_instance_probe == "1":
                manifest["graphics_shader_source"] = "owned-runtime-view-index-instance"
                manifest["multiview_witness"]["instance_witness"] = {
                    "first_instance": "0x07ffffff", "instance_count": 1}
                runtime_inputs = (("vertex", "runtime_view_index_instance.vert"),
                                  ("fragment", "runtime_triangle.frag"))
        if input_attachment_probe == "1":
            manifest["graphics_shader_source"] = "owned-runtime-input-attachment-oracle"
            manifest["input_attachment_witness"] = {
                "extent": [64, 64], "backing_layers": 6, "view_layer": 0,
                "descriptor_set": 0, "binding": 0, "record_bytes": 32,
                "subpasses": 2, "layout": "general", "strict_readback": True}
            runtime_inputs = (("vertex", "runtime_input_attachment.vert"),
                              ("pattern", "runtime_input_attachment_pattern.frag"),
                              ("transform", "runtime_input_attachment_transform.frag"))
        if scissor_probe == "13":
            manifest["graphics_shader_source"] = "owned-runtime-vertex-bindings"
            manifest["geometry_fixture"] = "sixteen-and-sparse-vertex-bindings"
            runtime_inputs = (("vertex", "runtime_vertex_bindings_probe.vert"),
                              ("fragment", "runtime_vertex_format.frag"))
        if vertex_probe:
            runtime_inputs = (("vertex_sint", "runtime_vertex_sint.vert"),
                              ("vertex_uint", "runtime_vertex_uint.vert"),
                              ("vertex_unorm", "runtime_vertex_unorm.vert"),
                              ("fragment", "runtime_vertex_format.frag"))
        manifest["runtime_graphics_inputs"] = {
            stage: {"glsl_sha256": hashlib.sha256((ROOT / "experiments/graphics" / source).read_bytes()).hexdigest(),
                    "spirv_sha256": hashlib.sha256((out / (source + ".spv")).read_bytes()).hexdigest()}
            for stage, source in runtime_inputs
        }
    if compute or graphics:
        # Local public-safe source identity; never hash/archive dev.conf contents
        # as source provenance or infer a clean git revision from this worktree.
        source_paths = [ROOT / "Makefile"]
        for folder in ("src", "native", "tools", "experiments/compute", "experiments/graphics"):
            source_paths += [p for p in (ROOT / folder).rglob("*")
                             if p.is_file() and p.suffix in (".c", ".h", ".py", ".comp", ".vert", ".frag")]
        manifest["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in sorted(source_paths)}
    for path in sorted(dist.rglob("*")):
        if path.is_file():
            manifest["files"][str(path.relative_to(dist))] = hashlib.sha256(path.read_bytes()).hexdigest()
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {manifest['stage']} submit_enabled={manifest['submit_enabled']}; not deployed or validated.")


if __name__ == "__main__":
    main()
