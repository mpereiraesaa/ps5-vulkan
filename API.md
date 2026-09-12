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

- Compute pipelines compiled from SPIR-V at runtime through PSBC/ACO for
  GFX1013, with a bounded in-memory compilation cache.
- Storage-buffer descriptors, command-buffer dispatch and fences.
- Exact GPU completion and checked readback, including guard validation.
- A single serial native queue; no multi-queue or semaphore contract.

## Programs and compilation

Compute shaders are compiled at runtime using a pinned PSBC/NIR/ACO fork. The
supported profile currently covers compute entry points with scalar
storage-buffer descriptors in set 0; additional sets, descriptor arrays,
scratch and workgroup-shared memory are rejected. Cache keys
include the complete SPIR-V digest, entry point, compiler/ABI versions and
pipeline-layout state, and entries are constrained by count and byte budgets.

Runtime graphics uses the same pinned PSBC/NIR/ACO stack for vertex and
fragment SPIR-V. The current profile supports procedural triangle lists,
smooth float32 scalar/vector interfaces at matching whole locations 0–31,
one vec4 fragment output at location 0, BGRA8 UNORM/sample1 and full color
writes. VertexIndex is supported; vertex buffers, graphics descriptors,
push constants, blending, additional targets and other interpolation modes
are rejected. Interface reflection is bounded to 65,536 IDs and is not a
complete SPIR-V validator; use developer-owned valid shader modules.

Pair-cache identity includes both complete modules, both entrypoints and
compiler/profile options. Cache leases protect metadata and ISA while the
native backend copies them into direct memory. The native SDK retains at most
32 pairs / 4 MiB; transient compilation allocations are outside that retained
budget. No persistent Vulkan pipeline-cache format is provided.

The existing textured scene retains its offline exact-program path. Its
capabilities must not be inferred for the narrower runtime graphics profile.

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
