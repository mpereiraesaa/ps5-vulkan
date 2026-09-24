# Supported API profile

`ps5-vulkan` exposes a deliberately bounded Vulkan-style API over the native
PlayStation 5 graphics stack. The object model follows Vulkan 1.0 closely. Each
capability below states its evidence boundary when it is narrower than native
hardware acceptance.

## Capability introspection

The independently staged consumer includes an optional DXVK 2.6.2 D3D11
feature-level 11_0 probe. It requests Vulkan 1.0 for instance creation, then
queries the reported API version, device extensions and the exact Vulkan
1.0/1.1/1.2/1.3 and extension feature/property structures named by the pinned
DXVK profile. It uses only the public `<ps5vk/ps5vk.h>` ABI and emits one
`ps5log/1` record for each of the 62 profile leaves.

The probe is observational: it does not create a device, submit GPU work or
promote a capability. Its strict verifier recomputes every result from the
pinned profile and rejects missing, duplicated, reordered or self-inconsistent
records. Current Vulkan 1.1+ structures being queryable as C types does not
mean the driver advertises them; the reported device version remains Vulkan
1.0 until their contracts are implemented and validated.

## Core feature negotiation

- `robustBufferAccess` is the one Vulkan 1.0 core feature currently reported
  true. Device creation accepts it through either `pEnabledFeatures` or the
  `VkPhysicalDeviceFeatures2` chain, rejects malformed booleans, and rejects
  every unreported core feature.
- Storage and uniform buffer descriptors carry their actual byte extent and
  use GFX1013 raw out-of-bounds selection. Vertex descriptors are bounded by
  the bound buffer span. This is the implementation basis for the feature, not
  an inference from the GPU name.
- `shaderClipDistance` and `shaderCullDistance` are reported true by the
  graphics build and refused by the compute-only build, together with
  `maxClipDistances` = `maxCullDistances` = `maxCombinedClipAndCullDistances` =
  8: the two packed position registers after POS0 hold eight float components,
  clip first and cull continuing immediately after them, and the stage-interface
  policy bounds a declaration against exactly that width. A pre-raster stage
  exports the distances (static or dynamically indexed) and a fragment stage
  reads the interpolated values; the compiler describes the distance registers
  on both sides and the driver refuses a pair whose read the metadata does not
  describe. `geometryShader` is reported true by the graphics build, with the
  five mandatory limits at the Vulkan floor it measured - 256 output vertices,
  32 invocations, 64 input components, 64 output components and 1024 total
  output components - because the merged vertex+geometry pre-raster program
  runs: the ES->GS input handoff, the point, line and triangle input families,
  `gl_InvocationID`, `gl_PrimitiveIDIn` and each of those five minima are
  hardware-witnessed, and the leaves of the pinned geometry module that pass
  their own upstream oracles are acceptance cases. `tessellationShader` is
  reported true by the native runtime-graphics build, with linked LS/HS and
  TES (including TES/GS) execution. Compute-only and offline graphics builds
  do not gain this feature. The eight tessellation limits are generation level
  64, patch size 32, control per-vertex input/output 128/128 components,
  control per-patch output 120, control total output 4096, and evaluation
  input/output 128/128. See [tessellation status](TESSELLATION_STATUS.md) for
  native witnesses, the focused 403-case upstream result and release status;
  this is not full CTS conformance or DXVK compatibility.
  See [clip-cull native acceptance](VALIDATION.md#clip-cull-native-acceptance).
- The focused suite contains the original upstream
  `device_mandatory_features` oracle plus 12 executable compute scalar
  `R32_UINT` robustness cases: UBO/SSBO OOB reads and SSBO OOB writes over
  1-, 3-, 4- and 32-byte descriptor ranges. Two exact 106/106 hardware runs
  passed. Wider scalar/vector formats and vertex-fetch robustness remain
  separate coverage work; they are not inferred from these cases.
- Multiple logical devices share one serialized process-level AGC session and
  direct-memory budget. The module and shared graphics compiler cache are
  released only after the final device closes; each device still owns and must
  destroy its Vulkan objects independently.

## Graphics

- Exactly one vertex stage and one fragment stage per graphics pipeline.
- Triangle-list and triangle-strip topology, fill rasterization and line
  width 1. Triangle strips are accepted and linked with the pinned GFX1013
  primitive type 6, but no strip draw has a native witness yet; every other
  topology is refused at pipeline creation.
- Up to 16 vertex bindings numbered 0–15, per-vertex input, with up to 32 attribute
  locations. Supported attributes are `R8` and `R8G8` UNORM/SNORM/UINT/SINT;
  `R8G8B8A8` UNORM/SNORM/UINT/SINT; packed `A8B8G8R8`
  UNORM/SNORM/UINT/SINT; `R16`, `R16G16` and `R16G16B16A16`
  UNORM/SNORM/UINT/SINT/SFLOAT; `R32`, `R32G32`, `R32G32B32` and
  `R32G32B32A32` SFLOAT/SINT/UINT; `B8G8R8A8_UNORM`; and
  `A2B10G10R10_UNORM_PACK32`. Forty-one typed conversion rows have exact
  hardware evidence on non-indexed runtime draws. The suite verifies missing
  components, normalized conversion, packed channel order, the two-bit alpha
  field, 1/2-byte strides and an unaligned Vulkan binding offset. Because a
  GFX1013 structured SRD drops its two low base-address bits, native submission
  copies an unaligned accessible span into aligned job-owned storage and
  releases it after exact completion.
  The runtime compiler exports its optimized descriptor-use mask: sparse binding
  numbers are packed in ascending used-bit order and optimized-away bindings
  need not be bound. Separate-buffer, sparse, odd-offset and specialization/cache
  hardware witnesses are documented in [VERTEX_INPUT.md](VERTEX_INPUT.md).
  Instance-rate and zero-stride input remain unsupported.
  Runtime-shader indexed draws are emitted through the same prepared vertex
  table and the same index emitter as the audited offline-program path, whose
  indexed support is the one with a hardware witness. The runtime combination
  has no native witness of its own yet, so this list does not claim it.
- Indexed and non-indexed draws. Index buffers support `uint16` and `uint32`,
  including offsets and signed base vertex.
- One viewport and scissor, supplied statically at pipeline creation or through
  `vkCmdSetViewport` / `vkCmdSetScissor` before each affected draw.
- One BGRA8 presentation attachment or RGBA8 off-screen color attachment and
  one sample. `LOAD`, `CLEAR`, `DONT_CARE`, `STORE` and
  `DONT_CARE` store semantics are supported by the bounded native path.
  Blending, logic ops and multisampling are unsupported.
- One or two subpasses may be described, recorded and **executed**. The native
  two-subpass path is deliberately bounded: both subpasses use the same colour
  and optional D32 attachment with the same layouts, and accept either no
  dependency or one forward `0` to `1` dependency. Wider attachment graphs
  remain fail-closed.
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

The DXVK262-T05 rasterization and viewport states - `depthBiasClamp`,
`depthClamp`, `fillModeNonSolid` and `multiViewport` - are implemented end to
end behind a build-time measurement gate (`PS5VK_RASTER_DIAGNOSTIC`) and are
**not advertised**: the shipping platform mask sets none of the four bits, a
request for one is still rejected before device creation, and the profile keeps
reporting `maxViewports` 1. They are gated rather than shipped because no
upstream CTS leaf is applicable to them yet - the candidate families and their
exact blocking reasons are in [UPSTREAM_CTS.md](UPSTREAM_CTS.md) - and because
the first hardware run of their own witnesses leaves six of thirty-one raster
cases unverified, as recorded in
[VALIDATION.md#rasterization-and-viewport-witnesses](VALIDATION.md#rasterization-and-viewport-witnesses).
The shader-selected viewport path itself is now measured - a geometry stage
routing sixteen primitives to sixteen banks through `gl_ViewportIndex` - so
`multiViewport` is held back by the missing upstream leaf and the six cases
rather than by the driver.
Nothing here should be read as these features being usable by an application
today.

DXVK262-T06 finished at the other end: `dualSrcBlend` **is advertised** by the
shipping platform. The path is the one described above - exact `SRC1` register
encodings, the compiler's `0x44`/`0xff` two-output proof, and the front-end and
compiler gates that still refuse a `SRC1` equation unless the logical device
enabled the feature *and* the selected fragment module carries the proven
secondary export - plus the rest of what the upstream oracle needed: the whole
GFX1013 `CB_BLEND0_CONTROL` blend space instead of the single witnessed shape,
partial colour write masks (carried in the pipeline's render-target block, not
the draw stream) for `VK_FORMAT_R8G8B8A8_UNORM`, and `COLOR_ATTACHMENT_BLEND` on
the two colour formats the profile can blend into. The witness is
`tools/run_dual_source.py`: it draws the packaged two-output fragment module
twice, once with blending disabled and once with the accepted equation, copies
the colour target back through the transfer path and judges both reads on exact
bytes, so a blender that ignored the secondary export could not pass.

The other half of the T06 fragment-output tranche, `sampleRateShading`, is
**advertised** since 2026-09-23: the platform sets
`PS5VK_FEATURE_SAMPLE_RATE_SHADING`, `vkGetPhysicalDeviceFeatures` reports it
true and `vkCreateDevice` accepts a request for it. `src/sample_rate_contract.h`
is the one definition of the counts this profile is built for (1x, 2x and 4x -
`8x` and above are not claimed) and of the CB_COLOR0_ATTRIB sample field they
encode; the reported `framebufferColorSampleCounts` follows the platform mask
rather than the envelope, so a build or device that does not carry the bit still
reports 1x. The render pass and framebuffer object model accept a count only
when the device serves it and require every attachment of a pass to agree on it
(a multisampled **depth** attachment stays unserved, and so do multisampled
sampled images: `sampledImage*SampleCounts` remain 1x, because this profile
reads a multisampled attachment only as an input attachment). The graphics
pipeline accepts `rasterizationSamples` from that same set together with
`sampleShadingEnable` only when the application enabled `sampleRateShading` on
the logical device, with `minSampleShading` bounded to `[0,1]` and
`pSampleMask` restricted to the count's own full mask; the compiled program
identity and the compile cache both carry the flag, the fraction and the
canonical mask.

What the draw programs is the capability itself. The raster stage publishes
`PA_SC_MODE_CNTL_0.MSAA_ENABLE`, the sixteen `PA_SC_AA_SAMPLE_LOCS_PIXEL_*`
context words carrying Vulkan's standard 2x/4x sample positions in 1/16-pixel
units and `PA_SC_AA_CONFIG.MAX_SAMPLE_DIST` for that pattern, and
`SPI_BARYC_CNTL.POS_FLOAT_LOCATION = 2` whenever the wave iterates per sample,
so `gl_FragCoord.xy` is the sample's own position and a fragment that colours
each sample sees one value per sample. A pass that names a resolve target
executes one: the single-sample attachment receives the average of the samples
through a draw the driver generates and carries
(`tools/build_resolve_shaders.py`), and a subpass that reads the colour
attachment as an input attachment can name any sample of it. Both boundaries
publish the colour attachment to the texture path with a completion token and
wait for it, because the colour block writes back asynchronously and a read
issued behind the bare event saw only some tiles of the target.

Two things this profile does not claim. The interpolation-offset limits
(`maxInterpolationOffset`, `minInterpolationOffset`,
`subPixelInterpolationOffsetBits`) are reported as 0: no path lowers an
interpolation offset, so they are documented blockers in
`conformance_inventory/reporting_matrix.json` rather than values nothing
measured. And the multisampled attachment is only served as a colour
attachment and a per-sample input attachment - not as a sampled texture, not as
a storage or transfer-only image.

## Images and sampling

The GFX1013 texture-format table records exact descriptor encodings, Vulkan
component completion and texel sizes for 61 public sampled formats, adapted
from the pinned GPLv3 `ps5-opengl` reference and then validated against Vulkan
oracles on PS5. Two byte-identical runs of each promoted tranche established
image creation, transfer upload, descriptor sampling and deterministic
readback. Each format query exposes only the operations actually established.

| Format | Supported role |
| --- | --- |
| `VK_FORMAT_B8G8R8A8_UNORM` | Color attachment and native presentation |
| `VK_FORMAT_D32_SFLOAT` | Depth attachment (depth aspect only), whole-subresource transfer-destination clear and transfer-source readback over the depth aspect, plus bounded 64×64 sampled depth for Dref gather |
| `VK_FORMAT_D16_UNORM` | One 128×128 depth attachment with no sampled, transfer or stencil role |
| Vulkan 1.0 BC1–BC7 formats (16 variants) | Sampled/upload images with nearest/linear filtering, compatible blit sources and bounded mip/layer transfers |
| `VK_FORMAT_R8_UNORM`, `VK_FORMAT_R8_SNORM`, `VK_FORMAT_R8G8_UNORM`, `VK_FORMAT_R8G8_SNORM` | Sampled/upload image with nearest/linear filtering and Vulkan completion of missing components |
| `VK_FORMAT_R8G8B8A8_UNORM` | Sampled/upload image with nearest/linear filtering and hardware-validated explicit mip LOD, off-screen color attachment plus transfer-source readback, or transfer-only image (`TRANSFER_SRC` and/or `TRANSFER_DST`) |
| `VK_FORMAT_R8G8B8A8_SNORM`, `VK_FORMAT_R8G8B8A8_SRGB` | Sampled/upload image with signed-normalized or hardware sRGB conversion and nearest/linear filtering |
| `VK_FORMAT_E5B9G9R9_UFLOAT_PACK32` | Sampled/upload image with shared-exponent decode, nearest/linear filtering and alpha completion to one |
| `VK_FORMAT_B10G11R11_UFLOAT_PACK32` | Sampled/upload packed floating-point image with nearest/linear filtering and alpha completion to one |
| `VK_FORMAT_R16_UNORM`, `VK_FORMAT_R16_SNORM`, `VK_FORMAT_R16_SFLOAT`, `VK_FORMAT_R16G16_UNORM`, `VK_FORMAT_R16G16_SNORM`, `VK_FORMAT_R16G16_SFLOAT` | Sampled/upload 16-bit normalized or floating-point image with nearest/linear filtering and Vulkan completion of missing components |
| `VK_FORMAT_R16G16B16A16_UNORM`, `VK_FORMAT_R16G16B16A16_SNORM`, `VK_FORMAT_R16G16B16A16_SFLOAT` | Sampled/upload four-component 16-bit image with nearest/linear filtering |
| `VK_FORMAT_R32_SFLOAT`, `VK_FORMAT_R32G32_SFLOAT`, `VK_FORMAT_R32G32B32A32_SFLOAT` | Sampled/upload 32-bit floating-point image with nearest/linear filtering and Vulkan completion where applicable |
| `VK_FORMAT_R8_UINT`, `VK_FORMAT_R8_SINT`, `VK_FORMAT_R8G8_UINT`, `VK_FORMAT_R8G8_SINT`, `VK_FORMAT_R8G8B8A8_UINT`, `VK_FORMAT_R8G8B8A8_SINT` | Typed integer sampled/upload image with nearest filtering and Vulkan completion of missing components |
| `VK_FORMAT_R16_UINT`, `VK_FORMAT_R16_SINT`, `VK_FORMAT_R16G16_UINT`, `VK_FORMAT_R16G16_SINT`, `VK_FORMAT_R16G16B16A16_UINT`, `VK_FORMAT_R16G16B16A16_SINT` | Typed 16-bit integer sampled/upload image with nearest filtering |
| `VK_FORMAT_R32_UINT`, `VK_FORMAT_R32_SINT`, `VK_FORMAT_R32G32_UINT`, `VK_FORMAT_R32G32_SINT`, `VK_FORMAT_R32G32B32A32_UINT`, `VK_FORMAT_R32G32B32A32_SINT` | Typed 32-bit integer sampled/upload image with nearest filtering |
| `VK_FORMAT_A8B8G8R8_UNORM_PACK32`, `VK_FORMAT_A8B8G8R8_SNORM_PACK32`, `VK_FORMAT_A8B8G8R8_SRGB_PACK32` | Packed sampled/upload image with native conversion and nearest/linear filtering; no color-attachment or storage-image role |
| `VK_FORMAT_A8B8G8R8_UINT_PACK32`, `VK_FORMAT_A8B8G8R8_SINT_PACK32` | Packed typed integer sampled/upload image, nearest only |
| 41 normalized, integer and floating-point rows across the `R8`, `R8G8`, `R8G8B8A8`, packed `A8B8G8R8`, `B10G11R11`, `R16`, `R16G16`, `R16G16B16A16`, `R32`, `R32G32` and `R32G32B32A32` families | Uniform texel buffers directly validated with typed compute `texelFetch`; the exact list and receipts are in `conformance_inventory/physical_format_validation.json` and `VALIDATION.md` |

RGBA8 UINT/SINT also serve four-component color attachments with transfer-source
readback for integer gather cases. `shaderImageGatherExtended` reports the
bounded texel-gather offset interval -8 through 7; D32 comparison gather uses
the sampled-depth profile above.

The shared layout and query path exposes bounded complete mip chains for these
sampled formats. Direct multi-level hardware evidence currently covers 2D
`VK_FORMAT_R8G8B8A8_UNORM`; the other formats combine their independently
validated texel encodings with the shared mip-layout contract and have not each
received a separate multi-level hardware run.

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

A third, narrower role exists for the same memory: `VK_FORMAT_R8G8B8A8_UNORM`
as a 2D, single-mip, single-layer, single-sample image with
`VK_IMAGE_TILING_LINEAR` and `VK_IMAGE_USAGE_TRANSFER_DST_BIT` alone is
accepted, and only that combination is. It is the host-readback staging image
the upstream draw tests create: `vkGetImageSubresourceLayout` describes it with
the padded linear `rowPitch`, `depthPitch`, `arrayPitch` and `size`, and
`vkCmdCopyImage` copies the RGBA8 colour attachment in `VK_IMAGE_LAYOUT_GENERAL`
into it in `GENERAL` for a whole-surface region. The copy is the driver's
existing GPU-completion readback: the tiled surface is invalidated, detiled
through the same 64KB_R_X offsets the colour readback uses, written into the
staging image's rows and flushed. Exactly the two transitions that role has are
accepted (`UNDEFINED` to `GENERAL` for the transfer write, `GENERAL` to
`GENERAL` from that write to the host read); every other format, dimension,
mip/layer/sample count, usage, tiling, region, barrier shape or copy direction
stays refused before any state is mutated. `linearTilingFeatures` and
`vkGetPhysicalDeviceImageFormatProperties` report exactly this one shape and
nothing else.

The public table has 40 filterable sampled formats. Nearest and linear sampling
have native deterministic readback evidence for the
original 24 filterable formats. The sixteen BC formats have original CTS
sampling coverage and focused native filtering witnesses. 20 additional signed and unsigned
integer rows have typed `isampler2D`/`usampler2D` nearest-sampling evidence and
do not expose linear filtering. R8 and RG8 verify Vulkan completion of missing
components;
the R16/RG16 and R32/RG32 rows extend that completion evidence to wider
components, while both packed floating-point formats verify alpha completion
to one. SNORM, sRGB, packed-float and 16/32-bit float conversions are checked with
asymmetric or exact source values before the fragment result is written.
Core `REPEAT`, `MIRRORED_REPEAT`, `CLAMP_TO_EDGE` and `CLAMP_TO_BORDER` are encoded
per axis. Two byte-identical hardware runs deterministically verified mirrored
repeat, transparent-black, opaque-black and opaque-white border results, then
separate nearest-versus-linear magnification and minification discriminators;
both float and integer variants of the six fixed `VkBorderColor` enums map to
those three native values. `VK_KHR_sampler_mirror_clamp_to_edge` remains
unadvertised and rejected. `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`
is advertised only for the 40 validated filterable rows. Valid sampled
images can carry complete mip chains up to the per-type query limit. A
public-SDK-linked 2D RGBA8 witness uploaded three levels and selected all three
with runtime-compiled explicit LOD, producing deterministic GPU readback.
`mipLodBias` is accepted from -2 through +2 and encoded as signed 8.8 sampler
state; values outside the reported interval and non-finite values fail closed.
Single-level 1D, 1D-array, 2D-array, cube and 3D view witnesses separately
supported for the sampled-image role. The implementation encodes the distinct
GFX1013 resource types, retains layer/depth bounds in the view, and uploads
multi-layer or multi-slice regions with ordered DMA packets. Exact RGBA8
hardware witnesses select three 1D regions, array layers, cube faces or volume
slices and verify distinct RGB output. The reported 4096 1D dimension, 256
array layers, 4096 cube dimension and 512 3D dimension are frontend floors
backed by descriptor/layout arithmetic and allocation bounds; the hardware
witnesses use small resources and are not exhaustive tests at those maximum
dimensions. The multi-level hardware witness is 2D RGBA8; it does not establish
layered mip selection, anisotropy or general descriptor arrays. A separate
cube-array path sampled two cubes and all twelve faces, including a view
starting at layer one, with exact GPU readback. A separate 256×256 tiled
attachment witness sampled all twelve rendered faces; descriptors refuse
smaller incompatible layer pitches. The graphics profile reports
`imageCubeArray` true.

## Compute

- The reported core API remains Vulkan 1.0. Narrow storage is negotiated through
  `VK_KHR_get_physical_device_properties2`,
  `VK_KHR_storage_buffer_storage_class`, `VK_KHR_8bit_storage` and
  `VK_KHR_16bit_storage`.
- `vkGetPhysicalDeviceFeatures2KHR` reports and `vkCreateDevice` accepts
  `robustBufferAccess` in the core feature block plus exactly
  `storageBuffer8BitAccess` and `storageBuffer16BitAccess` for the narrow
  storage slice.
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
- Storage buffers, uniform buffers and uniform texel buffers. A direct native
  matrix fetches all 41 published texel-buffer formats through typed float,
  unsigned-integer or signed-integer shader interfaces. It covers 1-, 2-, 4-,
  8- and 16-byte elements, component completion, normalized conversion and the
  packed `B10G11R11` decode. Storage texel buffers remain unsupported.
- Dynamic storage and uniform-buffer descriptors use the same executable
  compiler ABI as their static forms. `vkCmdBindDescriptorSets` consumes one
  offset for every dynamic descriptor in increasing set, binding and array
  element order; the offsets are immutable snapshots of the recorded dispatch.
  Counts, alignments, integer overflow and the final base-plus-dynamic range are
  checked fail-closed. The reported per-set floors are four dynamic storage
  buffers and eight dynamic uniform buffers. Host contracts currently prove
  this API state machine; hardware evidence is stated only when a corresponding
  native receipt is listed in `VALIDATION.md`.
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
  `vkCreateDevice` maps the normalized queue priority to explicit low/high
  classes and the physical device conservatively reports the Vulkan 1.0 floor
  of two discrete priorities. Because the family exposes one queue, the two
  classes cannot compete and do not imply a multi-queue scheduler.
  Multi-queue and synchronization2 contracts are absent.
- `VK_KHR_timeline_semaphore` is implemented in the queue frontend: 64-bit
  payloads, `VkTimelineSemaphoreSubmitInfo`, and the KHR host commands
  (`vkGetSemaphoreCounterValueKHR`, `vkWaitSemaphoresKHR` with ALL/ANY and
  zero, finite or unbounded timeouts, `vkSignalSemaphoreKHR`). A payload
  advances only when a host signal executes or a record retires after the
  backend's exact completion. A submission whose head waits on a value no
  queued work signals does not block `vkQueueSubmit`; later records queue
  behind it and native jobs are prepared only when they reach the head.
  `maxTimelineSemaphoreValueDifference` is `UINT64_MAX` because every value is
  ordered with full-width comparisons. The extension is reported only by the
  default-off `PS5VK_TIMELINE_DIAGNOSTIC` measurement build until promoted.
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
- `vkCmdClearDepthStencilImage` clears the **whole subresource** of a
  one-sample `VK_FORMAT_D32_SFLOAT` 2D target in `TRANSFER_DST_OPTIMAL` or
  `GENERAL` with a depth value in `[0,1]`. The image needs
  `VK_IMAGE_USAGE_TRANSFER_DST_BIT`, which Vulkan requires of any cleared
  image, and that is the only usage it needs: **both** accepted profiles are
  `TRANSFER_DST` alone, which is a clear-only target, and
  `DEPTH_STENCIL_ATTACHMENT | TRANSFER_DST`, which is a depth attachment that
  may also be cleared. Every D32 image is the same tiled depth surface, so the
  two profiles differ only in what else the image may be used for.
  `pDepthStencil->stencil` is **ignored, not rejected**: Vulkan reads that
  member only for a range whose aspect mask includes
  `VK_IMAGE_ASPECT_STENCIL_BIT`, and the only accepted range here is
  depth-only, so a nonzero stencil is a conformant call that clears depth
  alone. The surface is
  64KB_Z_X tiled and this driver still does not claim the pipe XOR pixel
  equations; it does not need them here, because a constant depth value is the
  same 32-bit word in every texel, so filling the whole allocation with that
  word is tiling-invariant and yields exactly the image a per-pixel clear
  would. The work is emitted as the same uniform-DWORD GPU DMA fill the render
  pass already uses for its depth load-op clear, ordered at the queue head like
  the other transfers, and the host never writes the surface. Accordingly
  `VK_FORMAT_D32_SFLOAT` advertises `VK_FORMAT_FEATURE_TRANSFER_DST_BIT`, which
  exists solely for this clear and is reachable in both usage profiles above,
  so the bit never advertises something the driver would refuse to create. No
  transfer-source, sampled or blit role is claimed for the format. Everything that would require the missing pixel addressing stays
  fail-closed and records nothing: a partial mip or array range, any stencil
  aspect, a combined depth/stencil format, a multisample image, and a
  rectangle.
- A cleared depth target reaches a depth-tested draw through one bounded
  transition: `TRANSFER_DST_OPTIMAL` to `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`,
  source access `VK_ACCESS_TRANSFER_WRITE_BIT` in
  `VK_PIPELINE_STAGE_TRANSFER_BIT`, destination access
  `DEPTH_STENCIL_ATTACHMENT_READ | DEPTH_STENCIL_ATTACHMENT_WRITE` in either or
  both of `EARLY_FRAGMENT_TESTS` and `LATE_FRAGMENT_TESTS`, with the depth
  aspect over the whole subresource. A narrower single-stage destination mask
  is accepted because a pipeline may test depth at either stage. Nothing else
  is: the depth target never becomes a transfer source or a sampled image, the
  reverse transition is not part of the contract, and the colour roles keep
  their own transitions. The clear is GPU work on a tiled attachment, so the
  queue router sends it to the graphics backend rather than executing it on the
  host.
- Hardware qualification: a native scenario renders with the depth attachment's
  load op set to `LOAD`, so the render pass contributes nothing to the depth
  buffer and only the explicit clear can establish it. The clear runs as its own
  submission and must retire before the render pass is recorded. With scene
  geometry at z=0.4 and z=0.8 under `VK_COMPARE_OP_LESS`, two frames differing
  in nothing but the clear value produced opposite, deterministic results on
  PS5: clearing to 1.0 let the draw reach the colour target (139968 changed
  words of 2228224) and clearing to 0.0 rejected every fragment (0 changed),
  twice, from the same signed artifact, with `allocations_bytes=0` and a clean
  exit. The clear value carried `stencil = 0x10` in both frames, so the ignored
  stencil member is qualified on hardware as well as in host tests.
- `vkCmdClearAttachments` is structurally exposed and always invalidates
  recording: a mid-render-pass attachment clear would need a DCB clear path
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
- Recorded operations remain immutable. Resolution produces one local direct-op
  snapshot per command, read from exactly that command's bytes after
  visibility is established, whose dimensions, counts and `firstInstance` are
  validated again.
- The graphics profile reports `multiDrawIndirect = VK_TRUE` with
  `maxDrawIndirectCount = 65535`, `drawIndirectFirstInstance = VK_TRUE` and
  `fullDrawIndexUint32 = VK_TRUE` with `maxDrawIndexedIndexValue = 2^32-1`.
  Each is reported behind its own platform bit and honoured only on a logical
  device that enabled it: without `multiDrawIndirect` a draw count above one
  is refused at record time and at the queue head, and without
  `drawIndirectFirstInstance` a non-zero `firstInstance` fails the resolved
  command closed. A draw count of zero is a legal no-op. The stride rules apply
  only above one command, and the whole argument span
  (`stride * (drawCount - 1) + sizeof(command)`) is bounds-checked with checked
  arithmetic.
- A multi-command call expands on the CPU at the queue head, in recorded order:
  one prepared draw per operation and one emission per command (and per view
  under multiview), each carrying its own `DrawIndex` (the command index,
  including commands that draw nothing), `firstInstance` and index range.
  The emission spans a bounded chain of 128 KiB command arenas launched in
  order, each retiring before the next begins, so attachment writes stay
  ordered and load/store, binding, subpass and view state are unchanged
  across the chain. This is a CPU-side expansion, not command-processor
  indirect execution; `VK_KHR_draw_indirect_count` is not advertised.
- Pending command-buffer ownership protects referenced indirect buffers from
  destruction until every segment retires. A deferred prepare failure marks
  the device lost and does not signal the submission fence.

Original upstream oracles pass on hardware for compute-generated indirect
dispatch, for indirect draws with non-zero `firstInstance`, for multi-command
draws with `DrawIndex` 0..2 and for uint32-indexed indirect draws, and the
public-SDK witness executes 65535 commands in one call, GPU-generated draw
arguments and 32-bit indices at bit 31; see
[VALIDATION.md](VALIDATION.md#indirect-and-indexed-draw-native-acceptance-2026-09-16).
Instance-rate vertex input and primitive restart remain unsupported, so
`drawIndirectFirstInstance` is witnessed through `gl_InstanceIndex`, not
through per-instance attributes.

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
fragment SPIR-V. The current profile supports procedural or up to 16-binding
8/16/32-bit typed and packed RGBA8/BGRA8/RGB10A2 UNORM vertex input
for triangle lists, smooth float32 scalar/vector interfaces
at matching whole locations 0–31, one vec4 fragment output at location 0,
BGRA8/RGBA8 UNORM sample-1 targets and full color writes. VertexIndex, push
constants and scalar specialization constants are supported. Vertex/fragment combined
image/sampler descriptors have a connected four-set array backend and bounded
shared-stage hardware qualification. See the boundary
below. Other graphics resource types, blending, additional targets and other interpolation modes remain
unsupported by this runtime-compiled profile. Interface
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

The runtime backend accepts combined-image sampler arrays visible to vertex,
fragment or both stages across four sets and sparse binding numbers, within
the canonical layout and compiler declaration bounds. Shared stages receive
the same table address; their resource-layout checks and ownership cover the
union of sets. Independent public-header fragment-only and shared-stage
vertex/fragment diagnostics have qualified 24
elements in each of four sets, four descriptor-update rounds and exact weighted
pixel readback. This stress fixture exceeds the currently advertised sampler
counts of one: it is backend qualification, not a portable Vulkan consumer or
a limit promotion. Earlier single-sampler level-0 and three-level explicit-LOD
results remain separate evidence. Every binding in an active table must be
defined; per-binding static-use elimination and mixed graphics
buffer/image resource delivery are not complete. See [VALIDATION.md](VALIDATION.md).
The shared-stage witness uses procedural vertices, explicit level-zero nearest
sampling from four RGBA8 textures and different weighted sums in each stage.
It does not qualify every supported image format or arbitrary sampler state.
Descriptor layout creation and shared-sampler pipeline consumption accept legal
wider core visibility masks, including `VK_SHADER_STAGE_ALL` and
`VK_SHADER_STAGE_ALL_GRAPHICS`, without creating additional executable stages.
Both convenience masks have bounded public-SDK GPU readback evidence.
Bindings visible exclusively to nonexecuting stages are still rejected by the
runtime graphics profile; together with incomplete static-use elimination,
that remains a semantic gap rather than a Vulkan application requirement.
Recording also conservatively requires all nonempty layout sets to be bound,
even if compilation later eliminates a whole set. That is a remaining Vulkan
semantic gap, not an application requirement of the full API.

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
- `vkGetImageSubresourceLayout`: Queries image subresource layout. Tiled images report a zeroed structure (`*pLayout = {0}`) rather than a fabricated linear row or depth pitch. The one linear image this profile creates - the RGBA8 transfer-destination staging image described above - reports its real padded linear `offset`, `rowPitch`, `depthPitch`, `arrayPitch` and `size`; any other subresource of it, and every subresource of a tiled image, is zeroed.
- `vkGetRenderAreaGranularity`: Returns `(1, 1)` pixel render area granularity for valid render passes (`offset = 0`, full pixel granularity).
- Render-pass `pNext`: exactly one `VkRenderPassMultiviewCreateInfo` is understood and owned. View-mask counts must match the pass; an omitted view-offset array (count zero) means zero offsets. Non-zero offsets require valid view-local dependencies between distinct subpasses. Masks must be all zero or all non-zero; correlation masks must be disjoint. Unknown or duplicate chained structures fail closed. The measured graphics/runtime-compiler path exposes `VK_KHR_multiview`, `multiview=true`, `maxMultiviewViewCount=6` and `maxMultiviewInstanceIndex=134217727`. The bounded path supports up to eight ordered subpasses and vertex/fragment ViewIndex, array attachment clears and readback. Geometry/tessellation multiview remains false. Correlation masks remain hints, not commands. This does not enable Vulkan 1.2 aggregate query structures or change the Vulkan 1.0 API version. See [native validation](VALIDATION.md#multiview-native-acceptance).
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
- `vkCmdResetQueryPool`, `vkCmdBeginQuery`, `vkCmdEndQuery` and
  `vkCmdCopyQueryPoolResults` record bounded ordered operations. The native
  graphics path publishes real occlusion counts; the precise path passed an
  original upstream query oracle twice and a separate
  SDK-linked witness for zero, one and three covered samples, both result
  widths, availability, wait/partial and reset/reuse.
- `vkGetQueryPoolResults` preserves unavailable result words, reports
  availability separately, and reads published counts after completion.
  The bounded `WAIT_BIT` and `PARTIAL_BIT` paths are exercised by the witness.
  `VK_QUERY_CONTROL_PRECISE_BIT` requires the logical device to enable the
  advertised `occlusionQueryPrecise` feature.
- `vkCmdWriteTimestamp` remains fail closed; no CPU-derived timestamp is
  substituted for a native result. Secondary command-buffer inheritance of
  occlusion query state remains refused by this profile.

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

`VK_COMMAND_BUFFER_LEVEL_SECONDARY` command buffers can be allocated, recorded
and reset with the same allocator, device and pool ownership guarantees as
primary ones. The level is fixed at allocation and survives every reset,
including a pool reset, because Vulkan has no operation that changes it. A
secondary must supply `pInheritanceInfo`, which is copied and owned, so a
caller mutating its own structure afterwards cannot change what was recorded;
a primary ignores the pointer and stores nothing. Without
`VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT` Vulkan ignores the
inherited `renderPass`, `framebuffer` and `subpass` members, so any value the
caller supplies is accepted and retained verbatim in that opaque copy, and
nothing in this driver reads it.
With that flag the same members describe the scope the secondary will execute
in and are validated: `renderPass` must be a render pass of this device,
`subpass` must name a subpass the inherited pass actually has, and `framebuffer` is
**optional** - `VK_NULL_HANDLE` is accepted and the executing primary supplies
the framebuffer, while a non-null one must be compatible with the inherited
pass. The flag is accepted on a secondary only; a primary that sets it is
refused, because it describes a scope a primary cannot be executed in. A
continuation secondary enters the inherited scope at
`vkBeginCommandBuffer`, so it records draws exactly as a primary does inside a
pass, commands that may only appear outside one are refused, and
`vkEndCommandBuffer` succeeds with the inherited pass still open, because the
primary owns it.
`VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` and
`VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT` are mutually exclusive for a
primary only, per
`VUID-vkBeginCommandBuffer-commandBuffer-02840`; a secondary may set both.

`vkCmdExecuteCommands` executes the named secondaries, in call order. It
records an ordered array of child references that the driver **owns**, so a
caller mutating its own array afterwards cannot change which buffers run, and
the children are never copied or flattened into the primary: each is expanded
into its own submission segment at submit time and keeps its object identity,
its pending ownership and its reuse rules. A child recorded with
`VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` is consumed by executing exactly
as a primary would be; one recorded with
`VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT` may be named more than once in a
call and stays reusable. A secondary is still refused at `vkQueueSubmit`: it
reaches the queue only through a primary.

A child may be in the **pending or executable** state, per
`VUID-vkCmdExecuteCommands-pCommandBuffers-00089`; `00091` restricts the
pending state to buffers recorded with
`VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT`. There is no limit on how many
children one call may name. An **empty** child executes as a no-op but still
consumes its lifecycle, so a one-time-submit empty child is consumed.

Each child is segmented by the **same per-operation rules as a primary**, not
classified as a whole, so a mixed child - an event followed by a barrier, say -
has both halves executed in order rather than one half silently dropped. The
primary itself stays pending across its children, so resetting or freeing it
mid-flight is refused exactly as for any other submitted buffer.

A recorded reference keeps the child alive in the parent's plan, so resetting,
re-recording or freeing a referenced child before the parent is submitted
**invalidates the parent** rather than leaving a dangling handle behind.

Executing **inside** a render pass is supported, and the scope must match in
both directions. `vkCmdBeginRenderPass` accepts
`VK_SUBPASS_CONTENTS_INLINE` and
`VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`, and the mode it was begun
with is binding: an inline pass carries its own draws and admits no
secondaries, while a secondary-contents pass admits no draw of its own and
only `vkCmdExecuteCommands` and `vkCmdEndRenderPass` inside it. Inside a pass
every named child must be a continuation whose inherited render pass is
**compatible** with the executing one, whose `subpass` is `0`, and whose
inherited framebuffer is either `VK_NULL_HANDLE` or the framebuffer the pass
is executing.

The inherited framebuffer is checked against the inherited render pass by the
same rule, applied to the framebuffer's **roles**: its colour and depth
attachments must agree with the corresponding references on used-versus-unused
and on format and sample count, and their numeric slots need not match. This is
a different and weaker requirement than the one `vkCmdBeginRenderPass` places
on the framebuffer that actually **executes**, which must still line up with
its render pass index for index, because the native path addresses attachments
positionally. That stricter rule is a pre-existing boundary of the executing
path and is not relaxed here.

Compatibility follows Vulkan 1.0 chapter 7.2 and is a property of the
corresponding attachment **references**: each pair must be both
`VK_ATTACHMENT_UNUSED`, or both used and referring to attachments that agree
on format and sample count. The numeric attachment indices, attachments no
reference names, the total attachment count, initial and final layouts, the
layout inside a reference, and load and store ops are all excluded, so a
secondary recorded against a `LOAD` pass runs inside a `CLEAR` pass of the
same shape, and one that reaches its colour attachment through a different
slot is accepted. Object identity is not required. Outside a pass a
continuation child is refused, because its recorded draws would have no scope,
and a continuation child may carry nothing but draws of the pass it inherited.

A render pass is a single scope, so it is not split the way work outside one
is: the primary's `vkCmdBeginRenderPass`-to-`vkCmdEndRenderPass` range and
every child it names are submitted as **one segment**, in recorded order, with
each buffer keeping its own operation range. Nothing is copied or flattened
into the primary, so each child still keeps its identity, its pending
ownership and its one-time-submit consumption, and the children retire with
the pass they are part of.

The call is refused, poisoning the recording with no partial operation left
behind, for nesting (a secondary may not execute anything), an empty or null
array, a child of another device or without a pool, a child that is not a
secondary, a child that is neither pending nor executable, a self-reference, a
pending or repeated child that was not recorded for simultaneous use, and any
of the scope mismatches above.

Accordingly the driver refuses `occlusionQueryEnable`, `queryFlags` and
`pipelineStatistics` in secondary inheritance because it does not execute
inherited occlusion queries or pipeline statistics. Primary precise occlusion
queries are supported. Members Vulkan defines as ignored are not refused. Recording a
primary-only command into a secondary - `vkCmdBeginRenderPass`,
`vkCmdEndRenderPass`, `vkCmdNextSubpass` - or nesting `vkCmdExecuteCommands`
poisons the recording transactionally, leaving no partial operation behind.

A render pass that executes **no work at all** is an explicit fail-closed
boundary of the native path. Vulkan permits an empty pass - its load and store
ops alone are observable - so recording and ending it remain legal. Submission
refuses that recording before any backend sees it because the bounded native
path has no zero-body execution shape. The test is the work that will
**execute**, not the commands written: a pass whose only content is
`vkCmdExecuteCommands` naming empty secondaries executes exactly as little as
one that recorded nothing, and is refused at submission in the same way.
Naming an empty secondary remains legal and simply contributes no draw work.

## Multiple subpasses

A render pass owns its supported shape: the attachment descriptions, one entry
per subpass with that subpass's colour and depth references, and the dependency
array. All of it is copied at creation into a single allocation, so a caller
that mutates its own structures afterwards cannot change what the object
recorded, and a failed creation leaves nothing behind. Preserve lists are not
stored because every non-empty preserve list is outside this bounded profile.

This profile describes **one or two** subpasses, each with exactly one colour
reference and an optional D32 depth reference, over one or two attachments.
**Every subpass names the same attachments**: a framebuffer here carries one
colour role and one depth role derived from the pass, so a pass whose subpasses
disagreed about which attachment is the colour one could be created and then
served by no framebuffer at all. The layouts may still differ per subpass; only
the attachment each role names is fixed.

That constraint also settles the preserve list. Every attachment must be named
by a subpass, and every subpass names the same ones, so no attachment can be
preserved-but-unused: a non-empty `pPreserveAttachments` has no legal form here
and is refused rather than stored where it could never mean anything.

Creation refuses, rather than accepting and ignoring: a subpass count outside
the range; a subpass without exactly one colour attachment; a nonzero
`inputAttachmentCount` or a non-null `pResolveAttachments`, which are
unimplemented - note that a non-null `pInputAttachments` beside a **zero** count
is accepted, because Vulkan ignores the pointer there; an attachment index out
of range; a depth reference that aliases the colour one; subpasses that name
different attachments for a role; an attachment that no subpass references; a
non-empty preserve list; and a dependency whose endpoints do not exist, that is
a self-dependency, that runs backward between subpasses, or that joins external
to external. A forward `0` to `1` edge and both external edges are accepted.

`vkCmdNextSubpass` advances the recording by exactly one subpass. It is
primary-only, requires a pass this buffer began, and requires a next subpass to
exist. An **empty subpass is legal**, and so is an empty render pass: their
load and store ops alone are observable, so recording them is accepted and
recorded faithfully, and whether this driver can execute them is decided at
submission rather than by refusing a conformant program. Each subpass carries
its **own** contents mode, so a pass may draw inline in its first subpass and
name secondaries in its second. `vkCmdEndRenderPass` requires the recording to
have reached the **last** subpass, which is a structural requirement of the
recording rather than a judgement about work: ending early would silently drop
the subpasses never entered.

A graphics pipeline belongs to **one subpass**. `vkCreateGraphicsPipelines`
accepts any `subpass` the render pass actually has, derives the pipeline's
formats from that subpass and stores the index; `vkCmdDraw` then refuses a
pipeline whose subpass differs from the one being recorded, even when every
format agrees, and submission re-derives the same check from the immutable
record. A continuation secondary is likewise recorded for one subpass:
`vkCmdExecuteCommands` requires the child's inherited `subpass` to equal the
subpass it is being named in, not merely to belong to a compatible pass.

**Execution covers one subpass and one exact two-subpass profile, and still
requires work somewhere in the pass.** For two subpasses, the colour and
optional depth roles and layouts must be identical in both; the dependency
graph is either empty or exactly one forward `0` to `1` edge. At
`vkCmdNextSubpass` the native backend emits a full graphics acquire before the
second subpass. This is stronger than the accepted forward dependency and
keeps the shared attachment ordered. Empty individual subpasses remain legal
when the pass as a whole draws. Different attachments or layouts, more than
two subpasses, input/resolve/preserve attachments and wider dependency graphs
are refused before native submission.

## Compatibility boundary

Object creation can describe some state that the native executor later rejects.
Successful creation must not be interpreted as general Vulkan compatibility.
There is no conformance claim, loader/ICD integration or promise that an
unmodified Vulkan application, emulator or translation layer will run.
