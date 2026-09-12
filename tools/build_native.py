"""Build only this project's native bootstrap using read-only lab dependencies."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
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
    if scissor_probe not in ("0", "1", "2", "3", "4", "5") or (scissor_probe != "0" and not graphics_api):
        raise SystemExit("PS5VK_GRAPHICS_SCISSOR_PROBE requires graphics profile API: 0 off, 1 tile, 2 cube quadrants, 3 planar quadrants, 4 sampler UV ladder, 5 RGB presentation")
    scene_split = os.environ.get("PS5VK_GRAPHICS_SCENE_SPLIT", "0")
    if scene_split not in ("0", "1") or (scene_split == "1" and not graphics_api):
        raise SystemExit("PS5VK_GRAPHICS_SCENE_SPLIT requires graphics profile API and must be 0 or 1")
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
    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    pin = subprocess.check_output(["git", "-C", str(foundation),
                                   "rev-parse", "HEAD"], text=True).strip()
    if pin != "37dd53602bdead63936f718004555ba10154be48":
        raise SystemExit("Native foundation changed; review before building")
    sdk = foundation / ".deps/native/ps5-payload-sdk"
    native = foundation / "tooling/native"
    builder = foundation / "build/host/ps5-native-tool"
    gears = lab / "projects/ps5-agc-gears"
    logger = lab / "projects/logging_server/client"
    out = ROOT / ("build/native-compute" if compute else "build/native")
    dist = ROOT / ("dist-compute/PPSA99994" if compute else "dist/PPSA99994")
    if graphics:
        graphics = Path(graphics).resolve()
        if ROOT / "build/graphics" not in graphics.parents:
            raise SystemExit("Use a project-local graphics control")
        graphics_manifest = json.loads((graphics / "manifest.json").read_text())
        if graphics_manifest.get("scope") == "compiler-inspection-only":
            raise SystemExit("Compiler inspection is not a native ABI/library adapter")
        if graphics_manifest.get("native_input_version") != 2:
            raise SystemExit("Graphics control lacks current interpolation ABI; recompile the owned .pipe")
        quantization = graphics_manifest.get("shader_context", {}).get("vertex_quantization", {})
        if quantization.get("byte_address") != 0x28be4 or quantization.get("value") != 0x2d:
            raise SystemExit("Graphics control lacks audited vertex quantization; recompile the owned .pipe")
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
    if compute:
        common += ["-I" + str(ROOT / "third_party/vulkan-headers/include"),
                   "-I" + str(ROOT / "build/program-library")]
        sources = [(p.stem, p, []) for p in sorted((ROOT / "src").glob("vk_*.c"))]
        sources += [(p.stem, p, []) for p in [ROOT / "native/compute_main.c",
            ROOT / "src/graphics_program.c", ROOT / "src/texture_copy.c", ROOT / "src/texture_layout.c",
            ROOT / "native/platform_ps5.c", ROOT / "native/memory_ps5.c", ROOT / "native/queue_ps5.c",
            ROOT / "src/compute_commands.c", ROOT / "src/dispatch_encode.c", ROOT / "src/descriptor_encode.c"]]
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
            scene = graphics_manifest.get("source") == "experiments/graphics/scene3d.pipe"
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
            if scissor_probe != "0" and not scene:
                raise SystemExit("Scissor diagnostic requires scene3d.pipe")
            common += ["-DPS5VK_GRAPHICS_SCISSOR_PROBE=" + scissor_probe]
            if scene_split == "1" and not scene:
                raise SystemExit("Split-draw diagnostic requires scene3d.pipe")
            common += ["-DPS5VK_GRAPHICS_SCENE_SPLIT=" + scene_split]
            common += ["-DPS5VK_GRAPHICS_SCENE=" + ("1" if scene else "0")]
            common += ["-DPS5VK_EXIT_CONTROL=" + str(exit_control)]
            common += ["-DPS5VK_SHELL_CLOSE=" + str(int(shell_close))]
            common += ["-DPS5VK_KEEP_AGC_MODULE=" + str(int(keep_agc_module))]
            common += ["-DPS5VK_GRAPHICS_API=1", "-DPS5VK_GRAPHICS_DRAW=" + ("1" if os.environ.get("PS5VK_GRAPHICS_DRAW") == "1" else "0"),
                       "-DPS5VK_GRAPHICS_PRESENT=" + ("1" if os.environ.get("PS5VK_GRAPHICS_PRESENT") == "1" else "0"),
                       "-I" + str(ROOT / "build/program-library")]
            sources += [(p.stem, p, []) for p in sorted((ROOT / "src").glob("vk_*.c"))]
            sources += [(p.stem, p, []) for p in (
                ROOT / "native/graphics_main.c", ROOT / "native/compute_main.c", ROOT / "native/platform_ps5.c", ROOT / "src/scene_geometry.c", ROOT / "src/scene_region.c", ROOT / "src/color_clear.c",
                ROOT / "native/queue_ps5.c", ROOT / "native/graphics_pipeline_ps5.c",
                ROOT / "native/image_ps5.c", ROOT / "src/depth_layout.c", ROOT / "src/texture_layout.c",
                ROOT / "native/draw_prepare_ps5.c", ROOT / "native/draw_emit_ps5.c", ROOT / "native/index_emit_ps5.c",
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
                ROOT / "src/dispatch_encode.c", ROOT / "src/descriptor_encode.c")]
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
    run(linker, "-T", native / "ps5-pie.ld", "--eh-frame-hdr",
        "--version-script", native / "app-symbols.map", "-e", "_start",
        "-o", out / "pie.elf", crt, *objects, "--as-needed",
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
                "dma_only": os.environ.get("PS5VK_DMA_ONLY") == "1",
                "inspection_hold": os.environ.get("PS5VK_INSPECT") == "1",
                "submit_enabled": os.environ.get("PS5VK_SUBMIT") == "1",
                "foundation": pin, "files": {}}
    if compute:
        manifest.update(stage="compute-api", submit_enabled=True,
                        dma_only=False, inspection_hold=False,
                        program_library=json.loads((ROOT / "build/program-library/manifest.json").read_text()))
    if graphics:
        manifest.update(stage="graphics-native-link-only", submit_enabled=False,
                        graphics=json.loads((graphics / "manifest.json").read_text()))
        if graphics_api:
            manifest.update(stage="graphics-api-creation-only", submit_enabled=False,
                            scene="two-cubes" if scene else "triangle-controls",
                            scissor_probe=int(scissor_probe),
                            scissor_depth_comparison=scissor_probe in ("2", "3"),
                            geometry_fixture="sampler-uv-ladder" if scissor_probe == "4" else ("planar-triangle" if int(scissor_probe)>=3 else "source-default"),
                            visual_hold_seconds=10 if scissor_probe in ("3", "5") else 0,
                            observation_frame_pause_us=60000 if observe_scene == "1" else 0,
                            exact_interior_witnesses=int(witnesses),
                            runtime_mode="continuous" if continuous == "1" else "bounded-diagnostic",
                            scissor_register_load="indirect-plus-direct-replay" if int(scissor_probe) else "indirect",
                            scene_draw_partition="two-36-index-draws" if scene_split == "1" else "single-draw",
                            exit_control=exit_control, keep_agc_module=keep_agc_module,
                            termination="os-close-during-render" if continuous == "1" else ("shell-close-after-cleanup" if shell_close else "return-main"))
            if os.environ.get("PS5VK_GRAPHICS_DRAW") == "1":
                manifest.update(stage="graphics-api-offscreen-draw", submit_enabled=True,
                                compute_regression="compute-before-and-after-graphics")
                if os.environ.get("PS5VK_GRAPHICS_PRESENT") == "1":
                    manifest.update(stage="graphics-api-native-presentation-reuse")
    if exit_control:
        manifest.update(stage={1:"graphics-exit-control-no-graphics",2:"graphics-exit-control-device",3:"graphics-exit-control-logging-load"}[exit_control], submit_enabled=False,
                        scene=None)
    if compute or graphics:
        # Local public-safe source identity; never hash/archive dev.conf contents
        # as source provenance or infer a clean git revision from this worktree.
        source_paths = [ROOT / "Makefile"]
        for folder in ("src", "native", "tools", "experiments/compute"):
            source_paths += [p for p in (ROOT / folder).rglob("*")
                             if p.is_file() and p.suffix in (".c", ".h", ".py", ".comp")]
        manifest["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in sorted(source_paths)}
    for path in sorted(dist.rglob("*")):
        if path.is_file():
            manifest["files"][str(path.relative_to(dist))] = hashlib.sha256(path.read_bytes()).hexdigest()
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {manifest['stage']} submit_enabled={manifest['submit_enabled']}; not deployed or validated.")


if __name__ == "__main__":
    main()
