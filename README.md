# ps5-vulkan

An experimental Vulkan-style graphics and compute API for native PlayStation 5
homebrew, targeting the console's `gfx1013` GPU.

The current implementation renders animated, indexed and textured 3D geometry
through hardware graphics pipelines. It supports depth testing, GPU-backed
images and buffers, explicit upload and layout transitions, and native 1080p
presentation with two-buffer ownership. Compute dispatch uses the same Vulkan
object model and native GPU submission path.

Hardware validation has been performed on an owned PS5 running firmware 12.02.
The demonstrated scene sustains approximately 59.94 presented frames per second,
survives repeated resource reuse, and can be closed and relaunched cleanly.
Deterministic GPU readback checks cover selected texture and depth-overlap
results; visual output is not the sole correctness signal.

## API coverage

- Vulkan 1.0-style instance, physical-device, device and queue objects
- Host-visible buffers and images backed by native direct memory
- Command pools and command buffers with explicit recording state
- Runtime-compiled compute pipelines, storage buffers, dispatch and fences
- Vertex and index buffers, indexed and non-indexed triangle-list draws
- One BGRA8 color attachment and an optional D32 depth attachment
- Single-level RGBA8 sampled textures with GPU upload transitions
- Static viewport/scissor state, depth testing and face culling
- Native two-buffer 1920x1080 presentation
- Explicit completion, retirement and bounded resource accounting

The exact supported profile is documented in [API.md](API.md). Build and test
requirements are in [BUILDING.md](BUILDING.md).

## Important boundaries

This is not a Vulkan-conformant driver or ICD, and it does not yet provide WSI,
swapchains, broad format coverage, multiple queues,
semaphores, blending, MSAA, mipmaps, anisotropy or arbitrary shader programs.
Compute SPIR-V is compiled at runtime through the pinned PSBC/ACO GFX1013
backend and cached under a bounded in-memory policy. Graphics programs remain
offline-compiled and accepted only when their complete identity and pipeline
contract match the audited native program library.

## Development

Host validation requires Python 3, Make, a C11 compiler and Git:

```sh
make vulkan-headers
make compiler-deps
make check
make check-sanitize
```

Native compilation also requires the PS5 payload SDK and the companion
`ps5-agc-gears` support library; see [BUILDING.md](BUILDING.md).

This repository currently carries no license grant. Dependency and licensing
choices must be audited before redistribution or incorporation into another
project.
