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


def tess_build_id(probe, variant, no_draw, state_dump="0"):
    """A stable 16-hex-digit identity for the exact tessellation candidate.

    Digests the source families the manifest already hashes (the same set, so
    the two agree by construction) together with the build switches that
    change the emitted code. Deliberately excludes dev.conf and anything else
    private: this value is printed in telemetry.
    """
    digest = hashlib.sha256()
    digest.update(b"ps5vk-tess-candidate/1\n")
    for key, value in (("probe", probe), ("variant", variant),
                       ("no_draw", no_draw), ("state_dump", state_dump)):
        digest.update(f"{key}={value}\n".encode())
    paths = [ROOT / "Makefile"]
    for folder in ("src", "native", "tools", "experiments/graphics"):
        paths += [p for p in (ROOT / folder).rglob("*")
                  if p.is_file() and p.suffix in (".c", ".h", ".py", ".vert",
                                                  ".frag", ".tesc", ".tese",
                                                  ".geom")]
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode())
        digest.update(b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()[:16]


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
    occlusion_precise_probe = os.environ.get("PS5VK_OCCLUSION_PRECISE_PROBE", "0")
    if occlusion_precise_probe not in ("0", "1") or (occlusion_precise_probe == "1" and scissor_probe != "15"):
        raise SystemExit("PS5VK_OCCLUSION_PRECISE_PROBE is a default-off variant of graphics probe 15")
    occlusion_depth_probe = os.environ.get("PS5VK_OCCLUSION_DEPTH_PROBE", "0")
    if occlusion_depth_probe not in ("0", "1") or (occlusion_depth_probe == "1" and scissor_probe != "15"):
        raise SystemExit("PS5VK_OCCLUSION_DEPTH_PROBE is a default-off variant of graphics probe 15")
    occlusion_query_api_probe = os.environ.get("PS5VK_OCCLUSION_QUERY_API_PROBE", "0")
    if occlusion_query_api_probe not in ("0", "1") or (occlusion_query_api_probe == "1" and
            (scissor_probe != "15" or occlusion_depth_probe != "1" or
             os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1")):
        raise SystemExit("PS5VK_OCCLUSION_QUERY_API_PROBE requires runtime graphics, depth-enabled probe 15")
    host_query_reset_probe = os.environ.get("PS5VK_HOST_QUERY_RESET_PROBE", "0")
    if host_query_reset_probe not in ("0", "1") or (host_query_reset_probe == "1" and
            (occlusion_query_api_probe != "1" or os.environ.get("PS5VK_USE_SDK") != "1")):
        raise SystemExit("PS5VK_HOST_QUERY_RESET_PROBE requires SDK-linked occlusion query API probe")
    gather_form = os.environ.get("PS5VK_GATHER_FORM", "0")
    if gather_form not in ("0", "1", "2", "3", "4", "5", "6", "7", "8"):
        raise SystemExit("PS5VK_GATHER_FORM must be 0 through 8")
    if gather_form != "0" and os.environ.get("PS5VK_USE_SDK") != "1":
        raise SystemExit("Image gather hardware witness requires PS5VK_USE_SDK=1")
    if gather_form != "0" and (scissor_probe != "15" or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1" or
            occlusion_precise_probe != "0" or occlusion_depth_probe != "0" or
            occlusion_query_api_probe != "0"):
        raise SystemExit("PS5VK_GATHER_FORM requires a single runtime-graphics draw on probe 15 (1 implicit core, 2 const offset, 3 dynamic offset, 4 four offsets, 5-8 explicit components 0-3)")
    d16_depth_witness = os.environ.get("PS5VK_D16_DEPTH_WITNESS", "0")
    if d16_depth_witness not in ("0", "1"):
        raise SystemExit("PS5VK_D16_DEPTH_WITNESS must be 0 or 1")
    if d16_depth_witness == "1" and (not graphics_api or scissor_probe != "14" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_D16_DEPTH_WITNESS requires graphics API, draw and PS5VK_GRAPHICS_SCISSOR_PROBE=14")
    if d16_depth_witness == "1" and os.environ.get("PS5VK_USE_SDK") != "1":
        raise SystemExit("D16 depth witness must be SDK-linked with PS5VK_USE_SDK=1")
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
    # Private measurement build for DXVK262-T06 sampleRateShading: the platform
    # mask then carries the feature and the 2x/4x framebuffer sample limits, so a
    # witness payload and the focused CTS selection can negotiate the
    # multisample state the frontends accept. Off by default; a shipping
    # platform advertises the feature only through the promotion change.
    sample_rate_diagnostic = os.environ.get("PS5VK_SAMPLE_RATE_DIAGNOSTIC", "0")
    if sample_rate_diagnostic not in ("0", "1") or (sample_rate_diagnostic == "1" and not graphics_api):
        raise SystemExit("PS5VK_SAMPLE_RATE_DIAGNOSTIC requires the graphics profile API and must be 0 or 1")
    sampler_mirror_case = os.environ.get("PS5VK_SAMPLER_MIRROR_CASE", "-1")
    if sampler_mirror_case not in ("-1", *(str(n) for n in range(8, 26))):
        raise SystemExit("PS5VK_SAMPLER_MIRROR_CASE must be -1 or 8..25")
    if sampler_mirror_case != "-1" and (
        not graphics_api or os.environ.get("PS5VK_USE_SDK") != "1"
    ):
        raise SystemExit("PS5VK_SAMPLER_MIRROR_CASE requires an SDK-linked graphics build")
    if sampler_mirror_case != "-1" and (
        scissor_probe != "6" or os.environ.get("PS5VK_GRAPHICS_DRAW") != "1" or
        os.environ.get("PS5VK_GRAPHICS_PRESENT") == "1"
    ):
        raise SystemExit("PS5VK_SAMPLER_MIRROR_CASE requires scissor probe 6, draw and no presentation")
    clip_cull_probe = os.environ.get("PS5VK_CLIP_CULL_PROBE", "0")
    if clip_cull_probe not in ("0", "1") or (clip_cull_probe == "1" and not graphics_api):
        raise SystemExit("PS5VK_CLIP_CULL_PROBE requires the graphics profile API and must be 0 or 1")
    geometry_probe = os.environ.get("PS5VK_GEOMETRY_PROBE", "0")
    if geometry_probe not in ("0", "1") or (geometry_probe == "1" and not graphics_api):
        raise SystemExit("PS5VK_GEOMETRY_PROBE requires the graphics profile API and must be 0 or 1")
    # Bounded diagnostic table mode for that witness: report every case in one
    # run instead of stopping at the first failing verdict, so the run that shows
    # the value verdict also shows the case that has lost the device before. It
    # is meaningless without the witness, and the shipping build keeps the
    # fail-fast behaviour.
    geometry_order_probe = os.environ.get("PS5VK_GEOMETRY_ORDER_PROBE", "0")
    if geometry_order_probe not in ("0", "1") or (geometry_order_probe == "1" and geometry_probe != "1"):
        raise SystemExit("PS5VK_GEOMETRY_ORDER_PROBE is a bounded geometry-probe diagnostic")
    # The tessellation witness is its own bounded diagnostic: it creates the
    # tessellation pipeline, draws two patches with different tessellation
    # levels and reports the quantised-tessCoord oracle. Like the other
    # optional-stage witnesses it skips the feature-negotiation gate.
    tess_probe = os.environ.get("PS5VK_TESS_PROBE", "0")
    if tess_probe not in ("0", "1") or (tess_probe == "1" and not graphics_api):
        raise SystemExit("PS5VK_TESS_PROBE requires the graphics profile API and must be 0 or 1")
    tess_offchip_bind = os.environ.get("PS5VK_TESS_OFFCHIP_BIND", "0")
    tess_offchip_capacity = os.environ.get("PS5VK_TESS_OFFCHIP_CAPACITY_WG", "256")
    if tess_offchip_bind not in ("0", "1") or (tess_offchip_bind == "1" and tess_probe != "1"):
        raise SystemExit("native offchip binding requires a tessellation probe")
    if not tess_offchip_capacity.isdigit() or not 1 <= int(tess_offchip_capacity) <= 65536:
        raise SystemExit("native offchip capacity must be 1..65536 workgroups")
    # ONE materially distinct tessellation candidate per executable. A faulting
    # draw leaves engine state that invalidates whatever runs after it in the
    # same process, so the variant is a build input and the artifact IS the
    # variant: 1 = control A (legal nonzero levels, no off-chip reads),
    # 2 = control B (legal zero outer levels, patch discarded), 3 = witness C
    # (off-chip per-vertex/per-patch readback), 4 = witness D (control A's
    # pipeline with an evaluation half that also writes a storage buffer, so
    # the run answers whether the DOMAIN EXECUTES at all - the one question no
    # register can answer once the image is empty and the hull is proven to
    # store correct factors). There is deliberately no knob
    # for a stage-disabled or triangle-list-drawn tessellation pipeline: those
    # are invalid hardware combinations that can never establish Vulkan
    # behaviour, and a build knob is how such a run becomes acceptance
    # evidence by accident.
    tess_variant = os.environ.get("PS5VK_TESS_VARIANT", "3")
    if tess_variant not in tuple(str(i) for i in range(1,57)):
        raise SystemExit(
            "PS5VK_TESS_VARIANT selects one candidate: 1 through 56")
    if tess_variant != "3" and tess_probe != "1":
        raise SystemExit("PS5VK_TESS_VARIANT requires PS5VK_TESS_PROBE=1")
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
    fragment_store_probe = os.environ.get("PS5VK_FRAGMENT_STORE_PROBE", "0")
    if fragment_store_probe not in ("0", "1"):
        raise SystemExit("PS5VK_FRAGMENT_STORE_PROBE must be 0 or 1")
    if fragment_store_probe == "1" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_FRAGMENT_STORE_PROBE requires graphics API, runtime graphics and draw")
    if fragment_store_probe == "1" and (multiview_view_probe == "1" or
            input_attachment_probe == "1" or clip_cull_probe == "1" or
            geometry_probe == "1" or tess_probe == "1" or
            scissor_probe != "0" or witnesses != "0" or
            continuous == "1" or observe_scene != "0" or scene_split == "1" or
            layer_probe == "1"):
        raise SystemExit("PS5VK_FRAGMENT_STORE_PROBE is a bounded standalone scene")
    dual_source_probe = os.environ.get("PS5VK_DUAL_SOURCE_PROBE", "0")
    if dual_source_probe not in ("0", "1"):
        raise SystemExit("PS5VK_DUAL_SOURCE_PROBE must be 0 or 1")
    if dual_source_probe == "1" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_DUAL_SOURCE_PROBE requires graphics API, runtime graphics and draw")
    if dual_source_probe == "1" and (multiview_view_probe == "1" or
            input_attachment_probe == "1" or fragment_store_probe == "1" or
            clip_cull_probe == "1" or geometry_probe == "1" or tess_probe == "1" or
            scissor_probe != "0" or witnesses != "0" or
            continuous == "1" or observe_scene != "0" or scene_split == "1" or
            layer_probe == "1"):
        raise SystemExit("PS5VK_DUAL_SOURCE_PROBE is a bounded standalone scene")
    two_mrt_probe = os.environ.get("PS5VK_TWO_MRT_PROBE", "0")
    if two_mrt_probe not in ("0", "1"):
        raise SystemExit("PS5VK_TWO_MRT_PROBE must be 0 or 1")
    # Private measurement scene for DXVK262-T06 sampleRateShading: it clears a
    # multisampled colour target through the native path and reads the
    # surface's own storage back. It is a standalone scene and only exists in
    # the same diagnostic build whose platform carries the sample-rate bit, so
    # no shipping payload can contain it.
    sample_rate_probe = os.environ.get("PS5VK_SAMPLE_RATE_PROBE", "0")
    # 1 is the witness scene (clear plus per-sample shaded draw), 2 is the same
    # scene followed by the step walk through the CTS oracle's render pass.
    if sample_rate_probe not in ("0", "1", "2"):
        raise SystemExit("PS5VK_SAMPLE_RATE_PROBE must be 0, 1 or 2")
    if sample_rate_probe != "0" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_SAMPLE_RATE_PROBE requires graphics API, runtime graphics and draw")
    # The sample-rate witness no longer needs the measurement switch: the
    # feature it measures was promoted on 2026-09-23, so the bit is in every
    # build and the witness's own oracle - one distinct fract(gl_FragCoord.xy)
    # per sample - runs against the shipping state. The switch still selects the
    # diagnostic register override the necessity table inside the scene uses.
    if sample_rate_probe != "0" and (multiview_view_probe == "1" or
            input_attachment_probe == "1" or fragment_store_probe == "1" or
            dual_source_probe == "1" or two_mrt_probe == "1" or
            clip_cull_probe == "1" or geometry_probe == "1" or tess_probe == "1" or
            scissor_probe != "0" or witnesses != "0" or continuous == "1" or
            observe_scene != "0" or scene_split == "1" or layer_probe == "1"):
        raise SystemExit("PS5VK_SAMPLE_RATE_PROBE is a bounded standalone scene")
    if two_mrt_probe == "1" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_TWO_MRT_PROBE requires graphics API, runtime graphics and draw")
    if two_mrt_probe == "1" and (multiview_view_probe == "1" or
            input_attachment_probe == "1" or fragment_store_probe == "1" or
            dual_source_probe == "1" or clip_cull_probe == "1" or
            geometry_probe == "1" or tess_probe == "1" or
            scissor_probe != "0" or witnesses != "0" or
            continuous == "1" or observe_scene != "0" or scene_split == "1" or
            layer_probe == "1"):
        raise SystemExit("PS5VK_TWO_MRT_PROBE is a bounded standalone scene")
    if clip_cull_probe == "1" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_CLIP_CULL_PROBE requires graphics API, runtime graphics and draw")
    if clip_cull_probe == "1" and (multiview_view_probe == "1" or input_attachment_probe == "1" or
            geometry_probe == "1" or scissor_probe != "0" or witnesses != "0" or continuous == "1" or
            observe_scene != "0" or scene_split == "1" or layer_probe == "1"):
        raise SystemExit("PS5VK_CLIP_CULL_PROBE is a bounded standalone scene")
    if geometry_probe == "1" and (not graphics_api or
            os.environ.get("PS5VK_RUNTIME_GRAPHICS") != "1" or
            os.environ.get("PS5VK_GRAPHICS_DRAW") != "1"):
        raise SystemExit("PS5VK_GEOMETRY_PROBE requires graphics API, runtime graphics and draw")
    if geometry_probe == "1" and (multiview_view_probe == "1" or input_attachment_probe == "1" or
            scissor_probe != "0" or witnesses != "0" or continuous == "1" or
            observe_scene != "0" or scene_split == "1" or layer_probe == "1"):
        raise SystemExit("PS5VK_GEOMETRY_PROBE is a bounded standalone scene")
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
              "-I" + str(logger), "-I" + str(ROOT / "src"),
              "-I" + str(ROOT / "third_party/psbc-reference"), "-I" + str(out),
              "-DPS5VK_SUBMIT=" + ("1" if os.environ.get("PS5VK_SUBMIT") == "1" else "0"),
              "-DPS5VK_DMA_ONLY=" + ("1" if os.environ.get("PS5VK_DMA_ONLY") == "1" else "0"),
              "-DPS5VK_INSPECT=" + ("1" if os.environ.get("PS5VK_INSPECT") == "1" else "0")]
    objects = []
    if tess_offchip_bind == "1":
        common += ["-DPS5VK_TESS_OFFCHIP_BIND=1",
                   "-DPS5VK_TESS_OFFCHIP_CAPACITY_WG=" + tess_offchip_capacity]
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
    if (use_runtime_sdk and not use_runtime_graphics and
            d16_depth_witness != "1" and sampler_mirror_case == "-1"):
        raise SystemExit("SDK-linked diagnostic requires runtime graphics")
    if use_runtime_graphics and not graphics_api:
        raise SystemExit("Runtime graphics requires a graphics API build")
    if d16_depth_witness == "1" and (use_runtime_graphics or continuous == "1" or
            observe_scene != "0" or witnesses != "0" or scene_split != "0" or
            exit_control or keep_agc_module or shell_close or
            os.environ.get("PS5VK_GRAPHICS_PRESENT") == "1"):
        raise SystemExit("D16 depth witness requires the bounded non-runtime, non-presented probe-14 draw")
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
            ROOT / "src/compute_commands.c", ROOT / "src/dispatch_encode.c", ROOT / "src/descriptor_encode.c",
            ROOT / "src/texture_descriptor.c", ROOT / "src/depth_layout.c"]
        if use_runtime_compiler:
            compute_srcs += [ROOT / "src/compilation_cache.c", ROOT / "src/vk_pipeline_cache.c",
                             ROOT / "src/ps5vk_compiler.c", ROOT / "src/ps5_compiler_shims.c"]
        sources += [(p.stem, p, []) for p in compute_srcs]
        sources += [("log", logger / "ps5log.c", ["-include", logger / "ps5log_ps5_net.h"]),
                    ("net", logger / "ps5log_ps5_net.c", [])]
    if graphics:
        common += ["-I" + str(graphics), "-I" + str(ROOT / "native"),
                   "-I" + str(gears / "src"), "-I" + str(ROOT / "third_party/vulkan-headers/include"),
                   "-I" + str(ROOT / "third_party/psbc-reference")]
        sources = [(p.stem, p, []) for p in (ROOT / "native/graphics_link_main.c",
            ROOT / "native/graphics_pair.c", ROOT / "native/memory_ps5.c",
            ROOT / "src/shader_relocate.c", gears / "src/ps5_shader_header.c")]
        sources += [("log", logger / "ps5log.c", ["-include", logger / "ps5log_ps5_net.h"]),
                    ("net", logger / "ps5log_ps5_net.c", [])]
        if graphics_api:
            sources = [s for s in sources if s[0] != "graphics_link_main"]
            graphics_source = graphics_manifest.get("source")
            if d16_depth_witness == "1" and graphics_source != "experiments/graphics/scene3d.pipe":
                raise SystemExit("D16 depth witness requires experiments/graphics/scene3d.pipe")
            scene = (not use_runtime_graphics or scissor_probe == "12") and graphics_source in (
                "experiments/graphics/scene3d.pipe",
                "experiments/graphics/scene3d-uint.pipe",
                "experiments/graphics/scene3d-sint.pipe",
                "experiments/graphics/scene3d-array.pipe",
                "experiments/graphics/scene3d-cube.pipe",
                "experiments/graphics/scene3d-3d.pipe",
                "experiments/graphics/scene3d-mirror-w.pipe",
                "experiments/graphics/scene3d-1d.pipe",
                "experiments/graphics/scene3d-1d-array.pipe",
                "experiments/graphics/scene3d-mipmap.pipe")
            image_target={
                "experiments/graphics/scene3d-array.pipe":1,
                "experiments/graphics/scene3d-cube.pipe":2,
                "experiments/graphics/scene3d-3d.pipe":3,
                "experiments/graphics/scene3d-mirror-w.pipe":3,
                "experiments/graphics/scene3d-1d.pipe":4,
                "experiments/graphics/scene3d-1d-array.pipe":5,
            }.get(graphics_source,0)
            if scissor_probe=="11" and not image_target:
                raise SystemExit("Layered sampled diagnostic requires scene3d-array.pipe, scene3d-cube.pipe or scene3d-3d.pipe")
            mirror_w = int(sampler_mirror_case) >= 20
            if mirror_w != (graphics_source == "experiments/graphics/scene3d-mirror-w.pipe"):
                raise SystemExit("W mirror cases require scene3d-mirror-w.pipe and vice versa")
            if image_target and scissor_probe!="11" and not (mirror_w and scissor_probe=="6"):
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
            if scissor_probe != "0" and not scene and not (
                    scissor_probe in ("8", "13", "15") and use_runtime_graphics):
                raise SystemExit("Scissor diagnostic requires scene3d.pipe")
            if scissor_probe in ("8", "13") and not use_runtime_graphics:
                raise SystemExit("Vertex-format diagnostic requires runtime graphics")
            common += ["-DPS5VK_GRAPHICS_SCISSOR_PROBE=" + scissor_probe]
            common += ["-DPS5VK_OCCLUSION_PRECISE_PROBE=" + occlusion_precise_probe]
            common += ["-DPS5VK_OCCLUSION_DEPTH_PROBE=" + occlusion_depth_probe]
            common += ["-DPS5VK_OCCLUSION_QUERY_API_PROBE=" + occlusion_query_api_probe]
            common += ["-DPS5VK_HOST_QUERY_RESET_PROBE=" + host_query_reset_probe]
            common += ["-DPS5VK_GATHER_FORM=" + gather_form]
            common += ["-DPS5VK_D16_DEPTH_WITNESS=" + d16_depth_witness]
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
            common += ["-DPS5VK_SAMPLE_RATE_DIAGNOSTIC=" + sample_rate_diagnostic]
            common += ["-DPS5VK_SAMPLER_MIRROR_CASE=" + sampler_mirror_case]
            common += ["-DPS5VK_MULTIVIEW_VIEW_PROBE=" + multiview_view_probe]
            common += ["-DPS5VK_MULTIVIEW_INSTANCE_PROBE=" + multiview_instance_probe]
            common += ["-DPS5VK_INPUT_ATTACHMENT_PROBE=" + input_attachment_probe]
            common += ["-DPS5VK_FRAGMENT_STORE_PROBE=" + fragment_store_probe]
            common += ["-DPS5VK_DUAL_SOURCE_PROBE=" + dual_source_probe]
            common += ["-DPS5VK_TWO_MRT_PROBE=" + two_mrt_probe]
            common += ["-DPS5VK_SAMPLE_RATE_PROBE=" + sample_rate_probe]
            common += ["-DPS5VK_CLIP_CULL_PROBE=" + clip_cull_probe]
            common += ["-DPS5VK_GEOMETRY_PROBE=" + geometry_probe]
            common += ["-DPS5VK_GEOMETRY_ORDER_PROBE=" + geometry_order_probe]
            common += ["-DPS5VK_TESS_PROBE=" + tess_probe]
            common += ["-DPS5VK_TESS_VARIANT=" + tess_variant]
            # The tessellation witness runs with the key-rejection diagnostic
            # on, so a refused pipeline names its rejection site in the log
            # instead of only reporting the Vulkan error code.
            if tess_probe == "1":
                common += ["-DPS5VK_GEOMETRY_KEY_DIAG=1"]
                common += ["-DPS5VK_TESS_NO_DRAW=" +
                           os.environ.get("PS5VK_TESS_NO_DRAW", "0")]
                # Diagnostic only: override VGT_LS_HS_CONFIG NUM_PATCHES to
                # test whether the per-workgroup capacity, not the patch data,
                # is what stalls a single-patch draw. Zero keeps the compiler's
                # published capacity, which is the shipped behaviour.
                common += ["-DPS5VK_TESS_PATCHES_PER_WG=" +
                           os.environ.get("PS5VK_TESS_PATCHES_PER_WG", "0")]
                # Diagnostic only: log the COMPLETE register stream a patch
                # draw emits, in emission order, once per process. Every
                # candidate so far was argued from one register read out of
                # the object holding it, which cannot see a register the
                # pipeline never writes or one written twice where the later
                # write wins. Default off: it costs log records and says
                # nothing a shipped build needs.
                tess_state_dump = os.environ.get("PS5VK_TESS_STATE_DUMP", "0")
                if tess_state_dump not in ("0", "1"):
                    raise SystemExit(
                        "PS5VK_TESS_STATE_DUMP must be 0 or 1")
                common += ["-DPS5VK_TESS_STATE_DUMP=" + tess_state_dump]
                # Diagnostic bisect for VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD,
                # which this driver zeroes because it writes the whole
                # register from a value derived only from the control half's
                # declared interface. Zero keeps the shipped behaviour.
                tess_ds_waves = os.environ.get("PS5VK_TESS_DS_WAVES", "0")
                if not tess_ds_waves.isdigit() or int(tess_ds_waves) > 15:
                    raise SystemExit(
                        "PS5VK_TESS_DS_WAVES is a 4-bit bisect: 0 to 15")
                common += ["-DPS5VK_TESS_DS_WAVES=" + tess_ds_waves]
                # Diagnostic bisect: compile the domain with NGG passthrough
                # forced off, so the generated code and the published
                # PRIMGEN_PASSTHRU_EN move together. Zero is radv's own
                # decision and the shipped behaviour.
                tess_no_passthru = os.environ.get("PS5VK_TESS_NO_PASSTHRU", "0")
                if tess_no_passthru not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_NO_PASSTHRU must be 0 or 1")
                common += ["-DPS5VK_TESS_NO_PASSTHRU=" + tess_no_passthru]
                # Diagnostic bisect for VGT_TF_PARAM.RDREQ_POLICY, which this
                # driver zeroes with the rest of the register's underived
                # fields. 0 is VGT_POLICY_LRU and the shipped behaviour;
                # 2 is VGT_POLICY_BYPASS.
                tess_tf_rdreq = os.environ.get("PS5VK_TESS_TF_RDREQ", "0")
                if tess_tf_rdreq not in ("0", "1", "2"):
                    raise SystemExit(
                        "PS5VK_TESS_TF_RDREQ is LRU 0, STREAM 1 or BYPASS 2")
                common += ["-DPS5VK_TESS_TF_RDREQ=" + tess_tf_rdreq]
                # VGT_TF_PARAM.DISTRIBUTION_MODE. 3 is TRAPEZOIDS, what radv
                # runs this chip with and the current shipped value; 0 is
                # NO_DIST, what the compiler published before. It is a knob
                # because its first measurement was taken upstream of a known
                # defect and therefore proves nothing.
                tess_dist_mode = os.environ.get(
                    "PS5VK_TESS_DISTRIBUTION_MODE", "3")
                if tess_dist_mode not in ("0", "1", "2", "3"):
                    raise SystemExit(
                        "PS5VK_TESS_DISTRIBUTION_MODE is 0..3")
                common += ["-DPS5VK_TESS_DISTRIBUTION_MODE=" + tess_dist_mode]
                # Diagnostic: omit VGT_TF_PARAM entirely, to ask whether
                # zeroing the fields this driver cannot derive is what stops
                # the tessellator. Default 0 writes it as usual.
                tess_no_tf_param = os.environ.get("PS5VK_TESS_NO_TF_PARAM", "0")
                if tess_no_tf_param not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_NO_TF_PARAM must be 0 or 1")
                common += ["-DPS5VK_TESS_NO_TF_PARAM=" + tess_no_tf_param]
                # What sceAgcLinkShaders is TOLD a tessellation pipeline
                # draws. The default is the shipped TRIANGLE_LIST; "patch"
                # passes PS5VK_AGC_PRIMITIVE_TYPE_PATCH instead, which is the
                # honest argument for a patch draw and lets the link derive
                # its whole state from it rather than having one register
                # corrected afterwards.
                # Diagnostic: draw the patch with tessellation levels of 16
                # instead of 2 and 1, so the tessellated work is hundreds of
                # triangles instead of four. Tests whether the domain fails to
                # launch below a batching threshold rather than because of a
                # misconfiguration. Default 0 keeps the shipped fixture.
                # Diagnostic: skip the geometry cases so the tessellation
                # draw is the first and only draw in the process. Tests
                # whether nineteen preceding draws leave state a patch draw
                # cannot recover from - which no register experiment could
                # show. Default 0 keeps the geometry table, whose 19/19 pass
                # is also this probe's evidence that the device is healthy.
                # Diagnostic bisect: set VGT_SHADER_STAGES_EN.GS_EN on a
                # tessellation pipeline with no geometry shader. Not what radv
                # does; it exists because ES_STAGE_DS with GS_EN=0 is the one
                # stage combination no passing pipeline on this device has
                # ever used. Default 0 is the shipped behaviour.
                # Diagnostic bisect: VGT_GS_MAX_VERT_OUT for the domain,
                # which psbc publishes as zero for any non-geometry stage. A
                # zero output bound costs a vertex-fed NGG pipeline nothing
                # because the engine knows the count from the draw; a
                # tessellator-fed one gets its count from the tessellator.
                # 0 keeps the compiler's value, which is the shipped
                # behaviour; 128 is the subgroup limit this pipeline already
                # publishes at GE_MAX_OUTPUT_PER_SUBGROUP.
                # The descriptor-free domain-execution witness: an evaluation
                # half that spins instead of writing memory, so the bounded
                # fence wait answers "did the domain run" with no storage
                # buffer in the path. Default 0.
                # 1 = the triangle domain, 2 = the isoline domain: a
                # materially different tessellator configuration measured by
                # the same descriptor-free mechanism.
                # The POSITIVE CONTROL for the spin witness: the same loop
                # in the CONTROL half, which is proven to execute because its
                # factors are in the ring. Reading "no stall" as "did not
                # execute" is only sound if a stage that does execute
                # produces one.
                # Fill the whole tessellation factor ring with a legal level
                # before the draw, so the engine finds one wherever it reads.
                # Separates "the hull writes where the engine does not read"
                # from "the engine does not read this ring at all". Default 0.
                # Emit the pinned tree's own guard for updating VGT ring
                # pointers - VS_PARTIAL_FLUSH then VGT_FLUSH - before a patch
                # draw's register banks. This driver rewrites the tessellation
                # ring registers on every draw and emits no events at all.
                # Read-only enumeration of AGC's register-defaults library.
                # The driver uses exactly one of its 137 keyed blocks; this
                # asks whether any of the others carries tessellation context
                # registers the platform expects a title to apply.
                tess_defaults_dump = os.environ.get(
                    "PS5VK_TESS_DEFAULTS_DUMP", "0")
                if tess_defaults_dump not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_DEFAULTS_DUMP must be 0 or 1")
                common += ["-DPS5VK_TESS_DEFAULTS_DUMP=" + tess_defaults_dump]
                tess_vgt_flush = os.environ.get("PS5VK_TESS_VGT_FLUSH", "0")
                if tess_vgt_flush not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_VGT_FLUSH must be 0 or 1")
                common += ["-DPS5VK_TESS_VGT_FLUSH=" + tess_vgt_flush]
                tess_prefill = os.environ.get("PS5VK_TESS_PREFILL", "0")
                if tess_prefill not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_PREFILL must be 0 or 1")
                common += ["-DPS5VK_TESS_PREFILL=" + tess_prefill]
                tess_spin_hull = os.environ.get("PS5VK_TESS_SPIN_HULL", "0")
                if tess_spin_hull not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_SPIN_HULL must be 0 or 1")
                common += ["-DPS5VK_TESS_SPIN_HULL=" + tess_spin_hull]
                tess_spin = os.environ.get("PS5VK_TESS_SPIN", "0")
                if tess_spin not in ("0", "1", "2"):
                    raise SystemExit(
                        "PS5VK_TESS_SPIN is 0, 1 (triangles) or 2 (isolines)")
                common += ["-DPS5VK_TESS_SPIN=" + tess_spin]
                tess_max_vert_out = os.environ.get(
                    "PS5VK_TESS_GS_MAX_VERT_OUT", "0")
                if not tess_max_vert_out.isdigit() or int(tess_max_vert_out) > 2047:
                    raise SystemExit(
                        "PS5VK_TESS_GS_MAX_VERT_OUT is 0..2047")
                common += ["-DPS5VK_TESS_GS_MAX_VERT_OUT=" + tess_max_vert_out]
                # Diagnostic bisect: VGT_SHADER_STAGES_EN.DYNAMIC_HS, the
                # only bit in that register naming the HS dispatch mode rather
                # than a stage enable, and the failure now sits exactly at the
                # hull-to-tessellator handoff. No primary source; radv never
                # sets it. Default 0 is the shipped behaviour.
                # Diagnostic bisect: VGT_SHADER_STAGES_EN.VS_EN =
                # V_028B54_VS_STAGE_DS. The other "fed by the tessellator"
                # enumerant in the same register, parallel to ES_EN, which was
                # a real defect here. Never varied in 58 runs. Default 0.
                tess_vs_en_ds = os.environ.get("PS5VK_TESS_VS_EN_DS", "0")
                if tess_vs_en_ds not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_VS_EN_DS must be 0 or 1")
                common += ["-DPS5VK_TESS_VS_EN_DS=" + tess_vs_en_ds]
                tess_dynamic_hs = os.environ.get("PS5VK_TESS_DYNAMIC_HS", "0")
                if tess_dynamic_hs not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_DYNAMIC_HS must be 0 or 1")
                common += ["-DPS5VK_TESS_DYNAMIC_HS=" + tess_dynamic_hs]
                tess_gs_en = os.environ.get("PS5VK_TESS_GS_EN", "0")
                if tess_gs_en not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_GS_EN must be 0 or 1")
                common += ["-DPS5VK_TESS_GS_EN=" + tess_gs_en]
                # Diagnostic: GE_CNTL programmed for a TESSELLATION draw instead
                # of the linked NGG-vertex value. 1 = PAL's gfx10 rule (patches
                # per workgroup, vertex grouping disabled, BREAK_WAVE_AT_EOI),
                # 2 = the same group size with the other two fields clear.
                # Default 0 keeps the linked value, the shipped behaviour.
                tess_ge_cntl = os.environ.get("PS5VK_TESS_GE_CNTL", "0")
                if tess_ge_cntl not in ("0", "1", "2"):
                    raise SystemExit(
                        "PS5VK_TESS_GE_CNTL is 0 (linked), 1 (PAL) or 2 (Mesa)")
                common += ["-DPS5VK_TESS_GE_CNTL=" + tess_ge_cntl]
                # Diagnostic: write VGT_PRIMITIVE_TYPE and VGT_LS_HS_CONFIG as
                # individual SET packets after the bulk register loads, per
                # PAL's rule that indexed registers are written alone.
                # 1 = plain SET (PAL gfx10), 2 = Mesa's indexed form. Default 0.
                tess_direct_indexed = os.environ.get(
                    "PS5VK_TESS_DIRECT_INDEXED", "0")
                if tess_direct_indexed not in ("0", "1", "2"):
                    raise SystemExit(
                        "PS5VK_TESS_DIRECT_INDEXED is 0, 1 (plain SET) or 2 (indexed)")
                common += ["-DPS5VK_TESS_DIRECT_INDEXED=" + tess_direct_indexed]
                # Diagnostic: launch the evaluation half as a LEGACY hardware
                # vertex shader (VS_STAGE_DS, no NGG) instead of an NGG stage.
                # Needs the compiler's legacy-domain publication. Default 0.
                tess_legacy_domain = os.environ.get("PS5VK_TESS_LEGACY_DOMAIN", "0")
                if tess_legacy_domain not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_LEGACY_DOMAIN must be 0 or 1")
                common += ["-DPS5VK_TESS_LEGACY_DOMAIN=" + tess_legacy_domain]
                # Diagnostic, the positive control for the legacy-domain
                # experiment: plain vertex pipelines launched as a LEGACY
                # hardware VS. Default 0.
                tess_legacy_vs = os.environ.get("PS5VK_TESS_LEGACY_VS_CONTROL", "0")
                if tess_legacy_vs not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_LEGACY_VS_CONTROL must be 0 or 1")
                common += ["-DPS5VK_TESS_LEGACY_VS_CONTROL=" + tess_legacy_vs]
                tess_entry = os.environ.get("PS5VK_TESS_ENTRY_WITNESS", "0")
                if tess_entry not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_ENTRY_WITNESS must be 0 or 1")
                common += ["-DPS5VK_TESS_ENTRY_WITNESS=" + tess_entry]
                tess_system_table = os.environ.get("PS5VK_TESS_SYSTEM_TABLE", "0")
                # Pointer delivery is required state, not instrumentation.
                # Permit ordinary shaders without the SGPR-store prefix.
                if tess_system_table not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_SYSTEM_TABLE must be 0 or 1")
                common += ["-DPS5VK_TESS_SYSTEM_TABLE=" + tess_system_table]
                tess_ring_query = os.environ.get("PS5VK_TESS_RING_QUERY", "0")
                if tess_ring_query not in ("0", "1", "2", "3", "4"):
                    raise SystemExit("PS5VK_TESS_RING_QUERY must be 0..4 (3 harness, 4 queue binding)")
                common += ["-DPS5VK_TESS_RING_QUERY=" + tess_ring_query]
                tess_only = os.environ.get("PS5VK_TESS_ONLY", "0")
                if tess_only not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_ONLY must be 0 or 1")
                common += ["-DPS5VK_TESS_ONLY=" + tess_only]
                tess_high_levels = os.environ.get("PS5VK_TESS_HIGH_LEVELS", "0")
                if tess_high_levels not in ("0", "1"):
                    raise SystemExit("PS5VK_TESS_HIGH_LEVELS must be 0 or 1")
                common += ["-DPS5VK_TESS_HIGH_LEVELS=" + tess_high_levels]
                tess_link_prim = os.environ.get("PS5VK_TESS_LINK_PRIM", "tri")
                if tess_link_prim not in ("tri", "patch"):
                    raise SystemExit(
                        "PS5VK_TESS_LINK_PRIM is 'tri' or 'patch'")
                common += ["-DPS5VK_TESS_LINK_PRIMITIVE=" +
                           ("PS5VK_AGC_PRIMITIVE_TYPE_PATCH"
                            if tess_link_prim == "patch"
                            else "PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST")]
                # The source-candidate identity the payload logs before the
                # submit. It digests exactly the source families the manifest
                # already records plus the build inputs that change the
                # generated code, so a receipt line names the tree that
                # produced the package. This is the SOURCE half of the
                # identity chain; the ARTIFACT half is the deployed eboot's
                # own sha256, verified against the built one by reading the
                # file back after the transfer and the mount refresh. Neither
                # is a log boot id, which is only a process token.
                tess_candidate_id = tess_build_id(tess_probe, tess_variant,
                                         os.environ.get("PS5VK_TESS_NO_DRAW", "0"),
                                         tess_state_dump + ":" +
                                         tess_ds_waves + ":" +
                                         tess_no_passthru + ":" +
                                         tess_tf_rdreq + ":" +
                                         tess_dist_mode + ":" +
                                         tess_no_tf_param + ":" +
                                         tess_link_prim + ":" +
                                         tess_high_levels + ":" +
                                         tess_only + ":" + tess_gs_en + ":" +
                                         tess_dynamic_hs + ":" +
                                         tess_vs_en_ds + ":" +
                                         tess_max_vert_out + ":" + tess_spin +
                                         ":" + tess_spin_hull + ":" +
                                         tess_prefill + ":" + tess_vgt_flush +
                                         ":" + tess_defaults_dump + ":entry=" + tess_entry +
                                         ":system=" + tess_system_table + ":query=" + tess_ring_query)
                common += ['-DPS5VK_TESS_BUILD_ID="' + tess_candidate_id + '"']
                os.environ["PS5VK_TESS_VARIANT"] = tess_variant
                os.environ["PS5VK_TESS_STATE_DUMP"] = tess_state_dump
            # Both optional-stage witnesses skip the feature-negotiation gate:
            # they exist to measure capabilities that are not advertised yet.
            # Exported, because the SDK build compiles the same sources.
            optional_stage_diagnostic = "1" if (clip_cull_probe == "1" or geometry_probe == "1" or tess_probe == "1") else "0"
            os.environ["PS5VK_OPTIONAL_STAGE_DIAGNOSTIC"] = optional_stage_diagnostic
            if tess_probe == "1":
                os.environ["PS5VK_GEOMETRY_KEY_DIAG"] = "1"
            common += ["-DPS5VK_OPTIONAL_STAGE_DIAGNOSTIC=" + optional_stage_diagnostic]
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
                ROOT / "src/geometry_witness.c",
                ROOT / "src/dual_source_oracle.c",
                ROOT / "src/color_attachment_contract.c",
                ROOT / "native/queue_ps5.c", ROOT / "native/graphics_pipeline_ps5.c",
                ROOT / "native/image_ps5.c", ROOT / "src/depth_layout.c", ROOT / "src/depth_detile.c", ROOT / "src/texture_format.c", ROOT / "src/texture_layout.c",
                ROOT / "native/draw_prepare_ps5.c", ROOT / "native/draw_emit_ps5.c", ROOT / "native/index_emit_ps5.c",
                ROOT / "native/input_attachment_gate.c",
                ROOT / "native/input_attachment_oracle.c",
                ROOT / "native/input_attachment_probe.c",
                ROOT / "native/fragment_store_probe.c",
                ROOT / "native/dual_source_probe.c",
                ROOT / "native/two_mrt_probe.c",
                ROOT / "native/sample_rate_probe.c",
                ROOT / "src/two_mrt_oracle.c",
                ROOT / "native/command_arena_ps5.c", ROOT / "native/draw_batch_ps5.c", ROOT / "src/graphics_sync.c",
                ROOT / "src/vertex_descriptor.c", ROOT / "src/vertex_fetch.c", ROOT / "src/index_fetch.c",
                ROOT / "src/triangle_readback.c", ROOT / "src/texture_descriptor.c", ROOT / "src/texture_copy.c", ROOT / "src/texture_dma.c", ROOT / "src/image_layout_state.c",
                ROOT / "native/graphics_queue_ps5.c",
                ROOT / "native/present_ps5.c", gears / "src/ps5_videoout.c",
                gears / "src/ps5_present.c", gears / "src/ps5_event_adapter.c", gears / "src/ps5_frame_completion.c",
                ROOT / "native/draw_state_ps5.c", ROOT / "native/tess_ring_lease.c", ROOT / "native/tess_shared_storage.c", ROOT / "native/viewport_ps5.c", ROOT / "native/targets_ps5.c",
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
                               "scene_region", "sampler_core_probe", "sampled_format_probe", "integer_sampled_probe", "vertex_format_probe",
                               "triangle_readback", "fragment_store_probe"}
        application_sources.add("dual_source_probe")
        application_sources.add("two_mrt_probe")
        application_sources.add("sample_rate_probe")
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
    run(*cc, "-fPIC", "-c", ROOT / "native/tess_driver_import_stub.c", "-o", out / "tess_driver.o", env=env)
    run(linker, "--shared", "-soname", "libSceAgcDriver.prx", "-o", driver, out / "driver.o", out / "tess_driver.o")
    extra_libs = []
    if use_runtime_sdk:
        extra_libs.append(str(ROOT / "dist-sdk/lib/libps5vk.a"))
    if (use_runtime_compiler or use_runtime_graphics or
            (use_runtime_sdk and d16_depth_witness == "1") or
            (use_runtime_sdk and sampler_mirror_case != "-1")):
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
        *sorted((sdk / "target/lib").glob("*.so")), stub, driver,
        *(["--wrap=sceAgcInit"] if tess_probe == "1" else []))
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
    if sampler_mirror_case != "-1":
        manifest["sampler_mirror_case"] = int(sampler_mirror_case)
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
            if int(sampler_mirror_case) >= 20:
                geometry_fixture="sampler-mirror-w-3d"
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
                            occlusion_precise_probe=int(occlusion_precise_probe),
                            occlusion_depth_probe=int(occlusion_depth_probe),
                            occlusion_query_api_probe=int(occlusion_query_api_probe),
                            host_query_reset_probe=int(host_query_reset_probe),
                            d16_depth_witness=int(d16_depth_witness),
                            d16_depth_attachment_supported=1,
                            scissor_register_load="indirect-plus-direct-replay" if int(scissor_probe) else "indirect",
                            scene_draw_partition="two-36-index-draws" if scene_split == "1" else "single-draw",
                            exit_control=exit_control, keep_agc_module=keep_agc_module,
                            termination="os-close-during-render" if continuous == "1" else ("shell-close-after-cleanup" if shell_close else "return-main"))
            if gather_form != "0":
                gather_sources = {
                    "1": "runtime_gather_core.frag",
                    "2": "runtime_gather_const_offset.frag",
                    "3": "runtime_gather_dynamic_offset.frag",
                    "4": "runtime_gather_four_offsets.frag",
                    "5": "runtime_gather_component_0.frag",
                    "6": "runtime_gather_component_1.frag",
                    "7": "runtime_gather_component_2.frag",
                    "8": "runtime_gather_component_3.frag",
                }
                manifest.update(gather_probe={
                    "form": int(gather_form),
                    "source": "experiments/graphics/" + gather_sources[gather_form],
                    "texture_extent": [64, 64],
                    "sample_coordinate": [0.5, 0.5],
                    "texel_pattern": "rgba8-x-y-3x-plus-5y",
                    "diagnostic_feature": gather_form in ("2", "3", "4"),
                    "profile_query_logged": True,
                    "offset_limits": ({"min": -8, "max": 7}
                                      if gather_form in ("2", "3", "4")
                                      else {"min": 0, "max": 0}),
                    "gpu_readback": True,
                })
            if clip_cull_probe == "1":
                # The dynamic-index case is part of the drawn set: it is the
                # variant the upstream family registers separately, and the
                # parser checks the manifest's count against its own table.
                manifest.update(scene=None,
                                geometry_fixture="clip-cull-distance-coverage",
                                sample_count=1, clip_cull_probe=1,
                                clip_cull_extent=64, clip_cull_cases=11)
            if geometry_probe == "1":
                # The sentinel and the raw readback are part of the drawn set:
                # they are the cases whose oracle asserts a value the geometry
                # stage read rather than only coverage, and they have to be in
                # the manifest the parser checks against.
                manifest.update(scene=None,
                                geometry_fixture="geometry-stage-coverage",
                                sample_count=1, geometry_probe=1,
                                geometry_extent=64, geometry_cases=19)
            if tess_probe == "1":
                manifest["tessellation_witness"] = {
                    "offchip_bind": int(tess_offchip_bind),
                    "offchip_capacity_workgroups": int(tess_offchip_capacity),
                    "variant": int(tess_variant), "build_id": tess_candidate_id,
                    "ring_mode": int(tess_ring_query),
                    "shared_pipelines": 2 if tess_ring_query == "4" else 0,
                    "no_draw": int(os.environ.get("PS5VK_TESS_NO_DRAW", "0")),
                }
            if fragment_store_probe == "1":
                manifest.update(scene=None,
                                geometry_fixture="fragment-storage-atomic",
                                sample_count=1, fragment_store_probe=1,
                                fragment_store_extent=64,
                                t06_diagnostic_features=True)
            if dual_source_probe == "1":
                manifest.update(scene=None,
                                geometry_fixture="dual-source-blend",
                                sample_count=1, dual_source_probe=1,
                                dual_source_extent=64,
                                dual_source_measurement=True)
            if two_mrt_probe == "1":
                manifest.update(scene=None,
                                geometry_fixture="two-colour-targets",
                                sample_count=1, two_mrt_probe=1,
                                two_mrt_extent=64,
                                two_mrt_measurement=True)
            if os.environ.get("PS5VK_GRAPHICS_DRAW") == "1":
                manifest.update(stage="graphics-api-offscreen-draw", submit_enabled=True,
                                compute_regression="compute-before-and-after-graphics",
                                occlusion_precise_probe=int(occlusion_precise_probe),
                                occlusion_depth_probe=int(occlusion_depth_probe),
                                occlusion_query_api_probe=int(occlusion_query_api_probe),
                                host_query_reset_probe=int(host_query_reset_probe),
                                occlusion_query_secondary=int(occlusion_query_api_probe))
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
        if gather_form != "0":
            gather_sources = {
                "1": "runtime_gather_core.frag",
                "2": "runtime_gather_const_offset.frag",
                "3": "runtime_gather_dynamic_offset.frag",
                "4": "runtime_gather_four_offsets.frag",
                "5": "runtime_gather_component_0.frag",
                "6": "runtime_gather_component_1.frag",
                "7": "runtime_gather_component_2.frag",
                "8": "runtime_gather_component_3.frag",
            }
            manifest["graphics_shader_source"] = "owned-runtime-image-gather-" + gather_form
            runtime_inputs = (("vertex", "runtime_triangle.vert"),
                              ("fragment", gather_sources[gather_form]))
        if clip_cull_probe == "1":
            # The one scene whose pre-raster stage exports clip and cull
            # distances: recorded in the manifest so the artifact identity
            # covers the shader that produced the readback.
            manifest["graphics_shader_source"] = "owned-runtime-clip-cull-distances"
        if geometry_probe == "1":
            # The one scene whose pre-raster stage is a merged vertex+geometry
            # program.
            manifest["graphics_shader_source"] = "owned-runtime-geometry-stage"
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
        if fragment_store_probe == "1":
            manifest["graphics_shader_source"] = "owned-runtime-fragment-storage-atomic"
            manifest["fragment_store_witness"] = {
                "extent": [64, 64], "expected_fragments": 4096,
                "descriptor_set": 0, "binding": 0, "record_bytes": 16,
                "control_compile_define": "CONTROL=1",
                "control_and_candidate_same_submit": True,
                "strict_readback": True}
            runtime_inputs = (("vertex", "runtime_input_attachment.vert"),
                              ("atomic", "runtime_fragment_store.frag"))
        if dual_source_probe == "1":
            manifest["graphics_shader_source"] = "owned-runtime-dual-source"
            manifest["dual_source_witness"] = {
                "extent": [64, 64], "draws": 2,
                "control_equation": "blend-disabled",
                "candidate_equation": "color=SRC1_COLORxZERO+ADD,alpha=ONExZERO+ADD",
                "expected_control_rgba": [64, 128, 191, 255],
                "expected_candidate_rgba": [51, 51, 38, 255],
                "tolerance_lsb": 1, "strict_readback": True}
            runtime_inputs = (("vertex", "runtime_triangle.vert"),
                              ("dual", "runtime_dual_source.frag"))
        if two_mrt_probe == "1":
            manifest["graphics_shader_source"] = "owned-runtime-two-mrt"
            manifest["two_mrt_witness"] = {
                "extent": [64, 64], "draws": 1, "colour_attachments": 2,
                "blending": "disabled-on-both-attachments",
                "expected_target0_rgba": [64, 128, 191, 255],
                "expected_target1_rgba": [204, 102, 51, 128],
                "tolerance_lsb": 1, "strict_readback": True}
            runtime_inputs = (("vertex", "runtime_triangle.vert"),
                              ("two_mrt", "runtime_two_mrt.frag"))
        if sample_rate_probe == "1":
            manifest["graphics_shader_source"] = "owned-runtime-sample-id"
            manifest["sample_rate_probe"] = 1
            manifest["sample_rate_measurement"] = True
            manifest["sample_rate_witness"] = {
                "extent": [64, 64], "samples": 4,
                "clear_rgba": [0.25, 0.5, 0.75, 1.0],
                "clear_word": "ff4080bf",
                "shaded_values": ["ff000000", "ff010000", "ff020000", "ff030000"],
                "phases": ["clear", "shaded"], "strict_readback": True}
            runtime_inputs = (("vertex", "runtime_sample_id.vert"),
                              ("fragment", "runtime_sample_id.frag"))
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
