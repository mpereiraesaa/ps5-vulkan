# Supported API profile

`ps5-vulkan` exposes a deliberately bounded Vulkan-style API over the native
PlayStation 5 graphics stack. The object model follows Vulkan 1.0 closely. Each
capability below states its evidence boundary when it is narrower than native
hardware acceptance.

## Graphics

- Exactly one vertex stage and one fragment stage per graphics pipeline.
- Triangle-list topology, fill rasterization and line width 1.
- One vertex binding at binding 0, per-vertex input, with up to 32 attribute
  locations. Supported attributes are one- through four-component 32-bit float
  formats.
- Indexed and non-indexed draws. Index buffers support `uint16` and `uint32`,
  including offsets and signed base vertex.
- One viewport and scissor, supplied statically at pipeline creation or through
  `vkCmdSetViewport` / `vkCmdSetScissor` before each affected draw.
- One BGRA8 presentation attachment or RGBA8 off-screen color attachment, one
  sample and one subpass. `LOAD`, `CLEAR`, `DONT_CARE`, `STORE` and
  `DONT_CARE` store semantics are supported by the bounded native path.
  Blending, logic ops and multisampling are unsupported.
- Optional D32 depth attachment. Depth testing and writing are supported;
  stencil and depth bounds are unsupported.
- Face culling and front-face selection are encoded by the native backend.

## Images and sampling

| Format | Supported role |
| --- | --- |
| `VK_FORMAT_B8G8R8A8_UNORM` | Color attachment and native presentation |
| `VK_FORMAT_D32_SFLOAT` | Depth attachment |
| `VK_FORMAT_R8G8B8A8_UNORM` | Single-level sampled/upload image, or off-screen color attachment plus transfer-source readback |
| `VK_FORMAT_R32_UINT` | Uniform texel buffer, hardware validated in compute |
| `VK_FORMAT_R32_SINT`, `VK_FORMAT_R32_SFLOAT` | Uniform texel buffer object/encoder contract; native execution not yet validated |

Texture uploads use the GPU transfer path and require the sequence
`UNDEFINED -> TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL`. Image layout
state is committed only after exact submission completion.

The off-screen readback profile requires a full single-mip RGBA8 image with
`COLOR_ATTACHMENT | TRANSFER_SRC`, the exact
`COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL` dependency and one full
`vkCmdCopyImageToBuffer` region. The native queue detiles 64KB_R_X color data
only after GPU completion. Partial regions and general image-copy support remain
fail-closed.

Nearest sampling has native visual and deterministic readback evidence. Linear
filter, address-mode and mipmap-mode encodings have host-contract coverage, but
linear filtering is not advertised as a hardware-supported format feature.
Mip chains, anisotropy, image arrays and general descriptor arrays are not
supported.

## Compute

- The reported core API remains Vulkan 1.0. Narrow storage is negotiated through
  `VK_KHR_get_physical_device_properties2`,
  `VK_KHR_storage_buffer_storage_class`, `VK_KHR_8bit_storage` and
  `VK_KHR_16bit_storage`.
- `vkGetPhysicalDeviceFeatures2KHR` reports and `vkCreateDevice` accepts exactly
  `storageBuffer8BitAccess` and `storageBuffer16BitAccess` for this slice.
  `uniformAndStorageBuffer8BitAccess`, `storagePushConstant8`,
  `uniformAndStorageBuffer16BitAccess`, `storagePushConstant16` and
  `storageInputOutput16` remain false.
- `shaderInt8`, `shaderInt16` and float16 arithmetic are not advertised. The
  supported shaders may load, convert and store narrow scalar/vector values in
  storage buffers, but this does not expose general narrow arithmetic.
- Compute pipelines compiled from SPIR-V at runtime through PSBC/ACO for
  GFX1013, with a bounded in-memory compilation cache.
- Up to four descriptor sets in the compute ABI. The native acceptance fixture
  uses three sets simultaneously.
- Storage buffers, uniform buffers and uniform texel buffers. The validated
  texel format is `VK_FORMAT_R32_UINT`, including Vulkan's `(R,0,0,1)`
  one-component completion; broader format support is not implied.
- Partial descriptor-set binding is accepted, but every set and descriptor used
  by the compiled shader must be bound and defined before dispatch.
- Pipeline layouts expose up to 256 bytes of 4-byte-aligned push constants.
  Recorded dispatches own immutable snapshots of the bytes visible to their
  compute stage.
- Pipeline specialization supports up to 64 uniquely numbered scalar constants
  of at most 8 bytes each. Map-entry order does not affect cache identity.
- Command-buffer dispatch and fences.
- Exact GPU completion and checked readback, including guard validation.
- A single serial native queue; no multi-queue or semaphore contract.
- Simultaneous-use command buffers are accepted with serialized retirement.
- Host/compute buffer barriers validate ranges and lifetimes, using stronger
  global cache/completion dependencies. Host-write dependencies accept shader
  and uniform reads; compute shader-write dependencies can feed later compute
  reads or explicit host reads. The supported stage/access subset is checked
  when recording, malformed or unsupported combinations fail closed, and
  queue-family transfers are rejected because the profile exposes one family.
- `vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` operate on
  the reported host-visible, non-coherent memory type. Ranges must belong to
  live mapped allocations and satisfy the reported 64-byte non-coherent atom
  rules except at allocation ends. Vulkan 1.0 requires a positive range count;
  zero-count calls and positive counts without a range array fail closed.
- Shared workgroup memory, workgroup barriers and 32-bit shared atomics execute
  across a validated 128-invocation workgroup (four wave32 waves). This is a
  bounded compute result, not a Vulkan memory-model or subgroup claim.

## Programs and compilation

Compute shaders are compiled at runtime using a pinned PSBC/NIR/ACO fork. The
supported profile currently covers storage, uniform and uniform-texel buffer
descriptors across at most four independent sets. Each shader-used set receives
a distinct, compiler-selected direct user-SGPR table pointer. Descriptor arrays
remain a bounded implementation contract without native acceptance coverage;
scratch is rejected. The compiler adapter conservatively includes every
compute-visible layout binding in execution metadata, so applications must
define those bindings even when static shader use could eliminate one.
Compiler LDS allocation (up to 64 KiB) and the pinned inline workgroup-count ABI
are preserved during dispatch. Focused upstream shared-variable, command-barrier
and shared-atomic cases pass on hardware, including a 128-invocation upstream
workgroup-memory case. A separate public-SDK consumer validates a 128-lane
shared atomic permutation and counter across four wave32 waves. Focused upstream
8/16-bit conversions also pass for the advertised storage-buffer bits; none of
these results establishes broad compute conformance, Vulkan memory-model support
or narrow arithmetic in other storage classes.
Cache keys include the complete SPIR-V digest, entry point, compiler/ABI
versions and pipeline-layout state, and entries are constrained by count and
byte budgets. Cache identity includes canonical specialization values and the
complete push-range stage signature. Specialization-dependent `LocalSizeId`
workgroup dimensions are not yet supported; local size must remain literal.

Runtime graphics uses the same pinned PSBC/NIR/ACO stack for vertex and
fragment SPIR-V. The current profile supports procedural or single-binding
float32 vertex input for triangle lists, smooth float32 scalar/vector interfaces
at matching whole locations 0–31, one vec4 fragment output at location 0,
BGRA8/RGBA8 UNORM sample-1 targets and full color writes. VertexIndex, push
constants and scalar specialization constants are supported. Graphics
descriptors, blending, additional targets and other interpolation modes are
rejected by this runtime-compiled profile. Interface
reflection is bounded to 65,536 IDs and is not a complete SPIR-V validator;
use developer-owned valid shader modules.

Pair-cache identity includes both complete modules, both entrypoints,
compiler/profile options, per-stage canonical specialization maps and the push
layout signature. Cache leases protect metadata and ISA while the
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

The graphics profile reports one device-local, host-visible, non-coherent
memory type and one device-local heap. It does not advertise `HOST_COHERENT`.
Physical-device limits are derived from allocator, descriptor and command
encoder bounds and validated fail-closed during instance creation. The exact
values, query semantics, format matrix and known Vulkan 1.0 deficits are in
[PHYSICAL_DEVICE_REPORTING.md](PHYSICAL_DEVICE_REPORTING.md).

Presentation uses a project-native VideoOut adapter with two registered BGRA8
1920x1080 images, matching flip tokens and completion fences. This is not Vulkan
WSI: `VkSurfaceKHR`, `VkSwapchainKHR` and `PRESENT_SRC_KHR` are not implemented.

## Compatibility boundary

Object creation can describe some state that the native executor later rejects.
Successful creation must not be interpreted as general Vulkan compatibility.
There is no conformance claim, loader/ICD integration or promise that an
unmodified Vulkan application, emulator or translation layer will run.
