"""Compile-check compute profile frontend objects without rebuilding/replacing the bootstrap compute profile package."""
import os
from pathlib import Path
import subprocess
from lab import lab_root
from prepare_vulkan_headers import main as headers

ROOT = Path(__file__).resolve().parents[1]


def main():
    headers()
    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    pin = subprocess.check_output(["git", "-C", str(foundation), "rev-parse", "HEAD"], text=True).strip()
    if pin != "37dd53602bdead63936f718004555ba10154be48":
        raise SystemExit("Native foundation changed; review before building")
    sdk = foundation / ".deps/native/ps5-payload-sdk"
    out = ROOT / "build/compute-memory"
    out.mkdir(parents=True, exist_ok=True)
    includes = [ROOT / "src", ROOT / "third_party/vulkan-headers/include",
                lab / "projects/ps5-agc-gears/include",
                lab / "projects/ps5-agc-gears/src",
                lab / "projects/logging_server/client"]
    for source in (ROOT / "src/vk_alloc.c", ROOT / "src/vk_device.c",
                   ROOT / "src/vk_dispatch.c",
                   ROOT / "src/vk_descriptor.c",
                   ROOT / "src/vk_pipeline.c",
                   ROOT / "src/vk_graphics_pipeline.c",
                   ROOT / "src/graphics_program.c",
                   ROOT / "src/vk_render_pass.c",
                   ROOT / "src/vk_image_view.c",
                   ROOT / "src/vk_sampler.c",
                   ROOT / "native/index_emit_ps5.c",
                   ROOT / "src/vk_framebuffer.c",
                   ROOT / "src/depth_layout.c",
                   ROOT / "src/texture_format.c", ROOT / "src/texture_layout.c",
                   ROOT / "src/texture_descriptor.c",
                   ROOT / "src/texture_copy.c",
                   ROOT / "src/texture_dma.c",
                   ROOT / "src/image_layout_state.c",
                   ROOT / "src/vk_transfer.c",
                   ROOT / "native/image_ps5.c",
                   ROOT / "native/targets_ps5.c",
                   ROOT / "native/viewport_ps5.c",
                   ROOT / "native/draw_state_ps5.c",
                   ROOT / "native/draw_emit_ps5.c",
                   ROOT / "native/draw_prepare_ps5.c",
                   ROOT / "native/input_attachment_gate.c",
                   ROOT / "native/input_attachment_oracle.c",
                   ROOT / "native/command_arena_ps5.c",
                   ROOT / "native/draw_batch_ps5.c",
                   ROOT / "native/graphics_queue_ps5.c",
                   ROOT / "native/present_ps5.c",
                   ROOT / "src/graphics_sync.c",
                   ROOT / "src/vertex_descriptor.c", ROOT / "src/vertex_fetch.c", ROOT / "src/index_fetch.c",
                   ROOT / "src/vk_command.c",
                   ROOT / "src/vk_indirect.c",
                   ROOT / "src/vk_fence.c",
                   ROOT / "src/vk_queue.c",
                   ROOT / "src/dispatch_encode.c",
                   ROOT / "src/descriptor_encode.c",
                   ROOT / "src/shader_relocate.c",
                   ROOT / "native/graphics_pair.c",
                   ROOT / "native/graphics_pipeline_ps5.c",
                   ROOT / "native/queue_ps5.c",
                   ROOT / "src/vk_memory.c", ROOT / "native/memory_ps5.c"):
        subprocess.run(["sh", str(foundation / "tooling/prospero-clang18"),
                        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        *[f"-I{p}" for p in includes], "-c", str(source),
                        "-o", str(out / (source.stem + ".o"))], check=True,
                       env=dict(os.environ, PS5_PAYLOAD_SDK=str(sdk)))
    print("compute profile frontend cross-compiled; not linked, deployed or hardware-validated.")


if __name__ == "__main__":
    main()
