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

All seven remaining Vulkan 1.0 dynamic-state setters are public and retain
validated command-buffer state: line width, depth bias, blend constants, depth
bounds and the three stencil masks/references. Because `wideLines` and
`depthBiasClamp` are not advertised, their valid recording subset is line width
`1.0` and depth-bias clamp `0.0`. Graphics pipeline creation still rejects
these seven `VkDynamicState` values: the native draw backend does not yet
consume them. Recording therefore establishes a real, non-interfering state
contract without claiming dynamic blending, stencil, depth bounds or depth
bias execution. Viewport and scissor remain the only dynamic states consumed by
draws.

## Images and sampling

| Format | Supported role |
| --- | --- |
| `VK_FORMAT_B8G8R8A8_UNORM` | Color attachment and native presentation |
| `VK_FORMAT_D32_SFLOAT` | Depth attachment |
| `VK_FORMAT_R8G8B8A8_UNORM` | Single-level sampled/upload image, off-screen color attachment plus transfer-source readback, or transfer-only image (`TRANSFER_SRC` and/or `TRANSFER_DST`) |
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
fail-closed for that tiled role.

A second RGBA8 role is real and independent of the tiled one: an image created
with `TRANSFER_SRC` and/or `TRANSFER_DST` alone is backed by the padded linear
layout the upload path already uses, so no GPU stage samples, renders into or
detiles it. Copy, clear and the buffer transfers over that role are host copies
whose destination range is flushed through the memory backend in recorded order;
layout transitions are bookkeeping and are validated against the image's
committed layout. Anything outside the padded linear geometry stays refused
rather than being approximated with a linear write.

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
- A single serial native queue with Vulkan 1.0 binary semaphore signal, wait
  and consumption across ordered `VkSubmitInfo` records. Signals become visible
  only when their record retires; the final fence follows the final record.
  Multi-queue, timeline semaphore and synchronization2 contracts are absent.
- Vulkan 1.0 events support host and recorded device set/reset plus waits inside
  or across primary command buffers. Event transitions are segmented from GPU
  jobs, and `vkCmdWaitEvents` preserves its validated memory dependency without
  forwarding frontend event operations to AGC.
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

## Buffer transfer commands

- `vkCmdCopyBuffer` supports one or more byte-granular regions. The complete
  call is validated before recording, including usage, bounds, source/destination
  non-overlap and non-overlapping destination regions across aliased buffers.
- `vkCmdUpdateBuffer` owns a recording-time copy of at most 65,536 bytes; its
  offset and size are multiples of four.
- `vkCmdFillBuffer` writes a repeated 32-bit value. An explicit size is a
  multiple of four; `VK_WHOLE_SIZE` rounds the remaining bound span down to a
  multiple of four.
- These frontend operations retain their position among GPU operations. A
  transfer executes only after an earlier GPU segment completes and before a
  later segment is prepared or launched. Destination ranges are flushed through
  the memory backend before following GPU use.

The transfer family remains deliberately bounded. The image-copy profile below
does not imply general image formats, tiling, blit or resolve support; blit and
resolve remain fail-closed entry points.

## Image copy and colour clear

- `vkCmdCopyImage` copies one or more base-level RGBA8 regions between two
  images of the transfer-only role. Both images must be single-mip, single-layer
  2D images whose usage is drawn from `TRANSFER_SRC`/`TRANSFER_DST`, the source
  must carry `TRANSFER_SRC` and the destination `TRANSFER_DST`, the regions must
  stay inside both extents, and each image must already be in the layout the
  call declares. Self-copy, depth/stencil aspects, mip or layer selection,
  multi-layer regions and 3D extents are refused.
- `vkCmdClearColorImage` writes one RGBA8 word to every pixel of a transfer-
  destination image. The clear value is converted with the same encoder the
  render-pass clear path uses, so a value outside the finite `[0,1]` range is
  refused instead of being clamped silently. A subresource range may name the
  single level/layer explicitly or use `VK_REMAINING_MIP_LEVELS` /
  `VK_REMAINING_ARRAY_LAYERS`; nothing else is accepted.
- The observable path for that role is the buffer transfer:
  `vkCmdCopyBufferToImage` uploads and `vkCmdCopyImageToBuffer` reads back, with
  bounded pitched rows: zero means the region width/height, otherwise
  `bufferRowLength` and `bufferImageHeight` must be at least the copied
  width/height. `bufferOffset` is four-byte aligned and every addressed row is
  bounds-checked before recording. These are frontend host copies over the
  padded rows. A source buffer is invalidated before CPU reads; a partially
  written destination buffer is invalidated before CPU stores so adjacent
  GPU-produced bytes survive, then flushed afterward. CPU-written image and
  buffer destinations are flushed before later GPU use.
- All of it is ordered like the buffer transfers: an operation runs when its
  segment reaches the head of the queue chain, after any earlier GPU segment
  retired and before any later backend segment is prepared. Every backend
  segment following a frontend operation is prepared lazily at queue head.
  Recording is transactional; a
  rejected call leaves no partial operation behind, and referenced images and
  buffers stay alive until the owning command buffer retires. Distinct handles
  that resolve to overlapping image/image or buffer/image backing spans are
  rejected before mutation.
- The tiled colour-attachment role is deliberately not copyable or clearable
  here, because 64KB_R_X has no linear addressing in this codebase.
  `vkCmdClearDepthStencilImage` and `vkCmdClearAttachments` are structurally
  exposed and always invalidate recording: the depth role has no pixel
  addressing and a mid-render-pass attachment clear would need a DCB clear path
  that does not exist yet. `vkCmdBlitImage` and `vkCmdResolveImage` likewise
  always invalidate recording because no proven scaling/filter or multisample
  contract exists.

## Indirect commands

- `vkCmdDispatchIndirect`, `vkCmdDrawIndirect` and
  `vkCmdDrawIndexedIndirect` are exposed through the public SDK and native
  queue backends.
- Indirect arguments are resolved only when their queue segment reaches the
  head. This preserves compute-write -> barrier -> indirect-consume ordering;
  the exact non-coherent argument range is invalidated before the CPU-side
  command snapshot is read and encoded.
- Recorded operations remain immutable. Resolution produces a local direct-op
  snapshot whose dimensions, counts and `firstInstance` are validated again
  after visibility is established.
- The profile reports `maxDrawIndirectCount = 1`,
  `multiDrawIndirect = VK_FALSE` and
  `drawIndirectFirstInstance = VK_FALSE`. A draw count of zero is a legal
  no-op; multi-draw and indirect-count extension commands are unsupported.
- Pending command-buffer ownership protects referenced indirect buffers from
  destruction until every segment retires. A deferred prepare failure marks
  the device lost and does not signal the submission fence.

The original upstream compute upload and compute-generated indirect-dispatch
oracles pass on hardware. Indirect graphics commands have native encoding and
host-contract coverage, but no separate indirect-draw pixel oracle is claimed.

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
budget.

### Pipeline cache

The Vulkan pipeline-cache object is implemented for the bounded profile:
`vkCreatePipelineCache`, `vkDestroyPipelineCache`, `vkGetPipelineCacheData` and
`vkMergePipelineCaches`. A cache is device-parented and may be passed to
`vkCreateComputePipelines` and `vkCreateGraphicsPipelines`; doing so changes
nothing about compilation, which always goes through the bounded internal
compilation cache described above.

`vkGetPipelineCacheData` exports exactly the normative 32-byte
`VkPipelineCacheHeaderVersionOne` (header size 32, version 1, current vendor and
device IDs and the device's 16-byte `pipelineCacheUUID`). A `pData == NULL` call
reports that size; a buffer smaller than the header returns `VK_INCOMPLETE`,
writes nothing and reports zero; a larger buffer is written only up to the
header. Externally supplied `pInitialData` is treated as untrusted: a short,
corrupt, wrong-vendor/device/version/UUID or oversized blob is ignored and cache
creation still succeeds with an empty cache, and a header-only blob round-trips
byte for byte. `vkMergePipelineCaches` requires at least one source and validates
the destination and every source (the destination is forbidden as a source,
duplicate sources are legal and foreign handles are rejected). It is a no-op
because the cache stores no records.

No compiled-code record is serialized and no restored cache hit is reported.
The digest-addressed key schema that a later record format must carry already
exists for the in-process cache (SPIR-V SHA-256, entry point, compiler/ABI
version, target, layout, specialization and push-range state), and
`pipelineCacheUUID` is derived deterministically from public compatibility
inputs (vendor/device, GFX1013 target, driver version, compiler identity and
cache ABI revision) so any change invalidates previously exported data.

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

## Bookkeeping and core command surface

The public driver interface exposes standard Vulkan 1.0 bookkeeping entry points:

- `vkEnumerateDeviceLayerProperties`: Validates arguments and reports zero device layers (`*pPropertyCount = 0`), returning `VK_SUCCESS` in conformance with modern Vulkan conventions deprecating separate device layers.
- `vkGetDeviceMemoryCommitment`: Queries memory commitment in `*pCommittedMemoryInBytes`. Because ps5vk exposes no memory type with `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` and does not support lazily allocated memory, ordinary allocations are not reported as lazily committed, and the query safely reports 0 bytes committed (`*pCommittedMemoryInBytes = 0`).
- `vkGetImageSubresourceLayout`: Queries image subresource layout. Because ps5vk images are backed by optimal GPU-tiled memory and linear image layout access is not supported, the layout structure is safely zeroed (`*pLayout = {0}`) rather than fabricating fictitious linear row or depth pitches.
- `vkGetRenderAreaGranularity`: Returns `(1, 1)` pixel render area granularity for valid render passes (`offset = 0`, full pixel granularity).
- `vkResetDescriptorPool`: Resets all descriptor sets allocated from a descriptor pool back to the pool, preserving pool allocation state without requiring pool destruction.

## Queries and sparse image queries

Query pools exist as a device-parented object for the one query type this
profile can honestly create:

- `vkCreateQueryPool` accepts `VK_QUERY_TYPE_OCCLUSION` (mandatory core and
  ungated) with a bounded `queryCount` (at most 4096). `VK_QUERY_TYPE_TIMESTAMP`
  and `VK_QUERY_TYPE_PIPELINE_STATISTICS` are refused with
  `VK_ERROR_FEATURE_NOT_PRESENT` because the corresponding feature bits are
  reported false, and non-zero `flags` or `pipelineStatistics` on an occlusion
  pool are rejected.
- `vkDestroyQueryPool` follows the standard parentage rules, and the device
  cannot be destroyed while a query pool child remains.
- `vkCmdResetQueryPool` records an ordered frontend operation. Query slots stay
  uninitialized until that operation executes, then become unavailable.
- `vkGetQueryPoolResults` implements only that observable unavailable state:
  it preserves result words, writes zero availability when requested and
  returns `VK_NOT_READY`. `WAIT_BIT` and `PARTIAL_BIT` fail closed because no
  native query can publish a real result yet.
- `vkCmdBeginQuery`, `vkCmdEndQuery`, `vkCmdCopyQueryPoolResults` and
  `vkCmdWriteTimestamp` are structurally exported but invalidate recording.
  No draw-derived fake counter or CPU-derived timestamp is substituted for a
  native GFX1013 result.

Sparse binding is not advertised and images cannot be created with sparse flags,
so the two mandatory sparse queries report an empty list rather than inventing
requirements: `vkGetImageSparseMemoryRequirements` sets the count to 0 for a
valid non-sparse image, and
`vkGetPhysicalDeviceSparseImageFormatProperties` reports zero properties for
every format/type/tiling/usage combination.
`vkQueueBindSparse` is also exported, but every call returns
`VK_ERROR_VALIDATION_FAILED`: the sole queue family does not advertise
`VK_QUEUE_SPARSE_BINDING_BIT`, and even a zero-count call cannot waive that
valid-usage requirement. It does not inspect bind arrays or mutate fences,
semaphores or queue state.

`vkCmdNextSubpass` and `vkCmdExecuteCommands` are explicit fail-closed
boundaries. The implementation accepts exactly one subpass and allocates only
primary command buffers, so neither command currently has a valid reachable
invocation. Their presence is structural API coverage, not subpass or
secondary-command-buffer support.

## Compatibility boundary

Object creation can describe some state that the native executor later rejects.
Successful creation must not be interpreted as general Vulkan compatibility.
There is no conformance claim, loader/ICD integration or promise that an
unmodified Vulkan application, emulator or translation layer will run.
