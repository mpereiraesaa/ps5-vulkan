# Supported API profile

`ps5-vulkan` exposes a deliberately bounded Vulkan-style API over the native
PlayStation 5 graphics stack. The object model follows Vulkan 1.0 closely, but
support is limited to paths that have both host-contract tests and native
hardware evidence.

## Graphics

- Exactly one vertex stage and one fragment stage per graphics pipeline.
- Triangle-list topology, fill rasterization and line width 1.
- One vertex binding at binding 0, per-vertex input, with up to 32 attribute
  locations. Supported attributes are one- through four-component 32-bit float
  formats.
- Indexed and non-indexed draws. Index buffers support `uint16` and `uint32`,
  including offsets and signed base vertex.
- One static viewport and scissor; no dynamic state.
- One BGRA8 color attachment, one sample and one subpass. Blending, logic ops
  and multisampling are unsupported.
- Optional D32 depth attachment. Depth testing and writing are supported;
  stencil and depth bounds are unsupported.
- Face culling and front-face selection are encoded by the native backend.

## Images and sampling

| Format | Supported role |
| --- | --- |
| `VK_FORMAT_B8G8R8A8_UNORM` | Color attachment and native presentation |
| `VK_FORMAT_D32_SFLOAT` | Depth attachment |
| `VK_FORMAT_R8G8B8A8_UNORM` | Single-level sampled image and transfer destination |

Texture uploads use the GPU transfer path and require the sequence
`UNDEFINED -> TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL`. Image layout
state is committed only after exact submission completion.

Nearest sampling has native visual and deterministic readback evidence. Linear
filter, address-mode and mipmap-mode encodings have host-contract coverage, but
linear filtering is not advertised as a hardware-supported format feature.
Mip chains, anisotropy, image arrays and general descriptor arrays are not
supported.

## Compute

- Compute pipelines backed by offline-compiled, identity-checked programs.
- Storage-buffer descriptors, command-buffer dispatch and fences.
- Exact GPU completion and checked readback, including guard validation.
- A single serial native queue; no multi-queue or semaphore contract.

## Programs and compilation

Shaders are compiled offline using a pinned compiler toolchain. The complete
SPIR-V input, compiler output, metadata, relocations and pipeline-relevant state
form an exact program identity. The runtime accepts only entries represented in
the generated program library; arbitrary SPIR-V and runtime compilation are not
supported.

## Memory and presentation

GPU resources are backed by native direct-memory allocations with conservative
alignment and footprint checks. The implementation enforces a 256 MiB budget;
this is a software safety limit, not a measurement of total console memory.
Flush, invalidate, completion and retirement operations are explicit.

Presentation uses a project-native VideoOut adapter with two registered BGRA8
1920x1080 images, matching flip tokens and completion fences. This is not Vulkan
WSI: `VkSurfaceKHR`, `VkSwapchainKHR` and `PRESENT_SRC_KHR` are not implemented.

## Compatibility boundary

Object creation can describe some state that the native executor later rejects.
Successful creation must not be interpreted as general Vulkan compatibility.
There is no conformance claim, loader/ICD integration or promise that an
unmodified Vulkan application, emulator or translation layer will run.
