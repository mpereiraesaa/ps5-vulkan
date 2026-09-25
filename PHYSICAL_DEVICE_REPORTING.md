# Physical-device reporting

`ps5-vulkan` reports the limits of its executable frontend, allocator and
command encoders. These values are not estimates of the PS5's total RAM or of
the maximum capability of the GFX1013 hardware.

The canonical initializer and validation gate are
`src/physical_device_profile.h`. A platform profile that omits a required
limit, reports contradictory memory flags, supplies only one of the format
query callbacks, or advertises graphics without its required limits is rejected
by `vkCreateInstance`.

Both shipped profiles are built by `src/device_profile_report.h`, which the
native platform (`native/platform_ps5.c`) and the host reporting dump
(`tools/dump_device_reporting.c`) share, so the values below are the ones the
console really queries rather than a hand-copied table.

## Native profiles

| Property | Compute build | Graphics build | Provenance |
| --- | ---: | ---: | --- |
| API version | Vulkan 1.0 | Vulkan 1.0 | highest frontend contract currently exposed |
| vendor ID | `0x1002` | `0x1002` | AMD vendor identity; target is the confirmed GFX1013 compiler/backend |
| device ID | `0` | `0` | unresolved; no PCI-style device ID is claimed |
| queue families | 1 | 1 | single native serial queue implementation |
| queue flags | compute | graphics + compute | compiled native queue path |
| heap size | 64 MiB | 1.25 GiB | bounded project allocation budget |
| maximum single allocation | 64 MiB | 1 GiB | heap budget minus 256 MiB headroom for internal arenas and presentation (graphics) |
| minimum allocation charge | 64 KiB | 128 KiB | direct-memory allocator policy |
| maximum allocation count | 1,024 | 2,048 | per-resource bound (at most 256 MiB) divided by minimum allocation charge |
| memory type | device-local, host-visible | device-local, host-visible | one GPU-used direct-memory heap mapped by the CPU |
| host coherent | no | no | flush and invalidate remain explicit; coherence is not established |
| buffer/image granularity | 64 KiB | 128 KiB | conservative separation from the active allocator class |
| non-coherent atom | 64 B | 64 B | cache-maintenance contract used by mapped-range validation |
| UBO/SSBO offset alignment | 256 B | 256 B | descriptor and command encoder requirement |
| push constants | 256 B | 256 B | public pipeline-layout and immutable command snapshot bound |
| compute shared memory | 64 KiB | 64 KiB | PSBC GFX1013 LDS configuration |
| workgroup invocations | 1,024 | 1,024 | dispatch encoder dimension/product gate |

The host library uses the same initializer with a 64 MiB mock heap. Its values
prove API and lifecycle behavior only; they are not PS5 hardware evidence.

## Device API version

The reported version is one macro, `PS5VK_DEVICE_API_VERSION` in
`src/physical_device_profile.h`. `conformance_inventory/core_version_contract.json`
lists, for Vulkan 1.1, 1.2 and 1.3, the commands, mandatory features and
promoted extensions from the pinned registry, plus the limits, query structures
and behaviours each version makes mandatory, with a status and the evidence for
each row. `tools/check_core_version_contract.py` (run by `make check`) refuses a
raised version unless every command of each version up to it resolves by its
core name and every row is satisfied (or its condition is false). It also
checks the instance version. `--assume-version 1.N` lists the unmet items
without editing anything. The device reports 1.0 today; the 1.1 contract alone
still has descriptor-capacity, heap-size and subgroup gaps.

## Reporting audit against the pinned specification and CTS

`tools/check_reporting_matrix.py` (run by `make check`) dumps the reported
values through the public query paths, joins them with the pinned Khronos core
tables in `conformance_inventory/core_target.json`, the pinned registry header
and the pinned CTS consumer rules in `vktApiFeatureInfo.cpp`, and writes
`conformance_inventory/reporting_matrix.json`. Every mandatory limit, feature
bit, format rule and shader-capability gate is classified as one of
`satisfied`, `blocker`, `not-applicable`, `not-audited` or `violation`, and an
undocumented `violation` fails the gate.

The audit corrected seven values that were simply unset and therefore below the
mandatory floor. They are the minimum the specification allows, not a
measurement of GFX1013:

| Limit | Before | After | Basis |
| --- | ---: | ---: | --- |
| `subTexelPrecisionBits` | 0 | 4 | Required Limits floor; texture precision is the texture units' behaviour |
| `mipmapPrecisionBits` | 0 | 4 | Required Limits floor |
| `maxVertexOutputComponents` | 0 | 64 | Floor; `src/spirv_graphics_interface.c` reflects 32 locations x 4 components |
| `maxFragmentInputComponents` | 0 | 64 | Floor; same reflected interface bound |
| `maxSampleMaskWords` | 0 | 1 | The pipeline validates sample-mask word 0 only |
| `pointSizeRange` | `[0, 0]` | `[1, 1]` | `largePoints` is `VK_FALSE`, so the only accepted size is the fixed 1.0 value |
| `lineWidthRange` | `[0, 0]` | `[1, 1]` | `wideLines` is `VK_FALSE`, same fixed 1.0 value |

The same seven floors are asserted by `ps5vk_physical_profile_valid`, so a
future edit cannot silently zero them again, and `tests/test_vk_device.c`
checks both the reported values and the rejection of a profile that drops each
one.

### Limits that stay below the floor (blockers)

These are real restrictions of the executable frontend. They are reported
truthfully and recorded as blockers rather than inflated, which is why
`dEQP-VK.info.device_properties` remains a diagnostic and not an acceptance
case:

| Area | Reported | Mandatory floor |
| --- | ---: | ---: |
| Attachments: `maxColorAttachments`, `maxFragmentOutputAttachments`, `maxFragmentCombinedOutputResources` | 1 | 4 |
| Storage and input descriptors: `maxPerStageDescriptorStorageImages`, `maxDescriptorSetStorageImages`, `maxPerStageDescriptorInputAttachments`, `maxDescriptorSetInputAttachments` | 0 | 4-96 |
| Sampling: color/depth/stencil `sampledImage*SampleCounts`, `framebuffer*SampleCounts`, `storageImageSampleCounts` | 0-1 | 1+4 |
| Other: `maxMemoryAllocationCount`, `minTexelOffset`, `maxTexelOffset` | 2048 / 0 / 0 | 4096 / -8 / 7 |

`maxVertexInputBindings` is **not** one of these blockers: the graphics profile
reports the Vulkan 1.0 floor of 16, and the compiled vertex-input path carries a
16-bit usage mask over the dense native SRDs (`VERTEX_INPUT.md`), so the
generated matrix classifies the row as `satisfied` for that profile. The
compute-only build applies no graphics limits and still records the row as
blocked for its separate profile.

The graphics profile now reaches the Vulkan 1.0 floors for 1D, 2D, 3D, cube
and array-layer image limits. Each image type has its own creation, view,
descriptor, upload and GPU-readback witness; the reported maximum dimensions
remain bounded frontend contracts rather than exhaustive maximum-allocation
tests. The compute-only build intentionally does not apply graphics limits and
continues to classify those rows as blockers for that separate profile.

The same is true of the four sampled-descriptor limits, which therefore left the
table above: the graphics profile reports `maxPerStageDescriptorSamplers = 16`,
`maxPerStageDescriptorSampledImages = 16`, `maxDescriptorSetSamplers = 96` and
`maxDescriptorSetSampledImages = 96`. Those are the Vulkan 1.0 floors, taken from
the shared qualified constants in `src/graphics_limits.h` that
`ps5vk_physical_profile_valid` also bounds against the descriptor table capacity,
so the report cannot drift from the contract it was qualified against. Two owned
witnesses support them and are recorded in `VALIDATION.md`: ninety-six combined
image samplers inside a single set, and ninety-six across four sets read by one
stage. Storage-image and input-attachment descriptor limits remain blocked, and
the compute-only build continues to record all four as below-floor.

`sampledImageIntegerSampleCounts` now reports `VK_SAMPLE_COUNT_1_BIT` in both
profiles, matching the typed integer sampled-image table and its native
readback evidence. Multisampled integer sampling remains unsupported.

The graphics profile reports `maxSamplerLodBias = 2`, the Vulkan 1.0 mandatory
floor. Sampler creation accepts the closed interval `[-2, 2]`, rejects NaN and
out-of-range values, and encodes the signed GFX1013 8.8 field without clamping.
Public-SDK-linked hardware runs qualify both boundaries with deterministic mip
selection; their identities and strict oracle are recorded in `VALIDATION.md`.

The compute-only profile additionally leaves every graphics-object limit at
zero because `ps5vk_graphics_limits` is applied only by the graphics build.

Dynamic uniform and storage-buffer descriptors are no longer exceptions. Both
shipped profiles report the Vulkan 1.0 floors of eight dynamic uniform buffers
and four dynamic storage buffers per descriptor set. Bind-time offsets are
consumed in Vulkan binding-number and array-element order, checked against the
reported alignment, snapshotted into recorded operations and added to the
descriptor's update-time base offset before the native range check. A reporting
regression below either floor is therefore a violation, not a documented
blocker. Host contracts cover ordering, pool accounting, alignment and range
rejection; executable PS5 evidence is tracked separately and is not implied by
these host checks.

### False core features

All `VkPhysicalDeviceFeatures` members reported `VK_FALSE` share a device-level
negotiation gate. `vkCreateDevice` examines every `VkBool32` member and refuses
the request before opening the backend unless it is the one advertised core
feature, `robustBufferAccess`. The regression repeats device creation with each
member enabled individually. This is the relevant contract for optional
compiler-side features such as `shaderInt64`: Vulkan valid usage does not allow
an application to use a false feature without first requesting it. We therefore
do not invent unrelated object-creation rejection branches as evidence. The
matrix records all 110 feature rows across both profiles as satisfied reporting
and negotiation contracts; this does not claim implementation of false bits.

The per-format mandatory rules are likewise recorded as blockers rather than
support: the advertised roles are listed below, but the complete mandatory
format families (including the compressed families) are not implemented. Shader
narrow-storage capabilities are gated on the advertised extension features by
`spirv_narrow_requirements`, and the four capabilities that require
uniform-and-storage narrow access are rejected outright; that pairing is
checked by the matrix against `vkGetPhysicalDeviceFeatures2KHR`.

Shader precision is reported only where the specification forces a value: no
float-control or float16 extension is advertised, so no per-stage precision mode
is claimed, and the texture/mipmap precision values are the mandatory floors.
PSBC/ACO record `float_mode` and `ieee_mode` per compiled program and the
pipeline gate validates them, but they are not exposed as device capabilities.
The compiler's behaviour for each individual SPIR-V precision mode is not
measured here and is recorded as not-audited.

## Query contract

The public API exposes the Vulkan 1.0 queries and the enabled
`VK_KHR_get_physical_device_properties2` forms. Enumeration follows the
two-call idiom, writes no object at zero capacity, reports `VK_INCOMPLETE` where
the Vulkan result-bearing enumeration requires it, and leaves storage beyond
the returned count untouched. Recognized output structures are populated;
unknown `pNext` structures are preserved rather than reinterpreted.

The core feature report contains one true bit: `robustBufferAccess`. The device
creation path accepts that bit through either Vulkan 1.0 feature input form and
rejects every feature it did not report. Buffer SRDs encode the descriptor's
real byte extent with GFX10 raw OOB selection, and vertex SRDs remain bounded by
the bound buffer span. The focused upstream `device_mandatory_features` case
checks the mandatory reporting rule. Twelve original executable upstream cases
also validate compute scalar `R32_UINT` UBO/SSBO OOB reads and SSBO OOB writes
at 1-, 3-, 4- and 32-byte ranges. Two exact 106/106 native runs passed that
combined selection. This evidence does not silently generalize to unselected
vector, format or vertex-fetch cases.

The current graphics format matrix remains deliberately bounded:

| Format | Reported feature |
| --- | --- |
| `B8G8R8A8_UNORM` | optimal color attachment |
| `R8_UNORM`, `R8_SNORM`, `R8G8_UNORM`, `R8G8_SNORM` | optimal sampled image, linear filtering and transfer destination |
| `R8G8B8A8_UNORM` | optimal sampled image, linear filtering, color attachment, transfer source/destination and compatible blit destination |
| `R8G8B8A8_SNORM` | optimal sampled image, linear filtering and transfer destination |
| `R8G8B8A8_SRGB` | optimal sampled image, linear filtering, transfer source/destination and compatible blit destination |
| `E5B9G9R9_UFLOAT_PACK32`, `B10G11R11_UFLOAT_PACK32` | optimal sampled image, linear filtering and transfer destination |
| `R16_UNORM`, `R16_SNORM`, `R16_SFLOAT`, `R16G16_UNORM`, `R16G16_SNORM`, `R16G16_SFLOAT` | optimal sampled image, linear filtering and transfer destination; vertex-buffer support also applies where independently listed by the vertex table |
| `R16G16B16A16_UNORM`, `R16G16B16A16_SNORM`, `R16G16B16A16_SFLOAT` | optimal sampled image, linear filtering and transfer destination; vertex-buffer support also applies where independently listed by the vertex table |
| `R32_SFLOAT`, `R32G32_SFLOAT`, `R32G32B32A32_SFLOAT` | optimal sampled image, linear filtering and transfer destination; vertex-buffer support also applies |
| scalar, two-component and four-component `R8`, `R16`, `R32` UINT/SINT families (18 formats) | typed sampled image and transfer destination, nearest only |
| `A8B8G8R8_UNORM_PACK32`, `A8B8G8R8_SNORM_PACK32`, `A8B8G8R8_SRGB_PACK32` | optimal sampled image, linear filtering and transfer destination |
| `A8B8G8R8_UINT_PACK32`, `A8B8G8R8_SINT_PACK32` | typed sampled image and transfer destination, nearest only |
| `D32_SFLOAT` | optimal depth attachment, whole-subresource transfer-destination clear and transfer-source readback over the depth aspect, plus sampled depth in a bounded 64×64, seven-mip Dref-gather profile |
| `D16_UNORM` | one 128×128 optimal depth attachment; no sampled, transfer or stencil role |
| BC1–BC7 (16 Vulkan 1.0 formats) | optimal sampled image with linear filtering, transfer source/destination and compatible blit source, including bounded mip/layer copies |
| `R8G8B8A8_UINT`, `R8G8B8A8_SINT` | optimal typed sampled image and transfer destination; also four-component color attachments with transfer-source readback for bounded gather sources |
| `R32_SFLOAT` | vertex buffer and uniform texel buffer; the texel-buffer role has host-only evidence |
| `R32_SINT`, `R32_UINT` | uniform texel buffer; UINT has native evidence, SINT is host-only |
| `R8G8B8A8_UNORM`, `R8G8B8A8_SNORM`, `R8G8B8A8_UINT`, `R8G8B8A8_SINT` | uniform texel buffer with four-component identity completion; UNORM is directly covered by the two-run `texelFetch` witness in VALIDATION.md, while the other three compose that shared buffer path with their independently validated format conversion/interface evidence |
| `R32G32_SFLOAT`, `R32G32B32_SFLOAT`, `R32G32B32A32_SFLOAT` | vertex buffer |

Image-format queries accept optimal-tiling, sample-count-one combinations that
have an implemented native role. Sampled 1D, 2D, cube-compatible and 3D images
report their bounded dimensions, layers and complete mip count; attachment and
pure-transfer roles remain single-level. Unsupported combinations return
`VK_ERROR_FORMAT_NOT_SUPPORTED` and a zeroed property structure.

### Format capabilities: implemented versus witnessed

`src/texture_format.c` is the single place that decides a format capability.
Every row carries two masks: `capabilities` (the operation is implemented in
this tree and covered by host tests) and `witnessed` (the enabled subset).
Sampled-image/filter promotions have deterministic on-console witnesses in
`VALIDATION.md`. Two inherited uniform-texel-buffer roles, R32_SINT and
R32_SFLOAT, retain host-only object/encoder evidence as stated in `API.md`;
the historical mask name must not be read as native proof for those roles.
All Vulkan-facing
decisions - the bits `vkGetPhysicalDeviceFormatProperties` reports, the usage
combinations `vkGetPhysicalDeviceImageFormatProperties` and image creation
accept, the T#/S# encoding, the padded-linear layout and the copy planner - are
derived from that one table, and unimplemented combinations are refused before
an object exists.

The two questions are deliberately not the same one. A capability can be
implemented, encoded and host-tested while still being reported as a blocker,
because host tests do not prove hardware behaviour. `tools/dump_device_reporting.c`
emits the capability ledger and `tests/test_format_capabilities.py` re-derives
every published bit from it, so a feature cannot be advertised without an
enabled capability, and `tests/test_reporting_matrix.py` freezes the rows of
every mandatory format table outside the ones a change is scoped to.

The Vulkan 1.0 4-byte and 32-bit mandatory tables are the worked example of
that accounting: 287 baseline deficit cells (not the total mandatory-cell
count), of which 8 sampled/filter capabilities are now enabled after physical
qualification and 279 still need backend work.

| Blocker reason | Cells | Why a diagnostic cannot promote it |
| --- | ---: | --- |
| compute profile has no image model | 165 | that profile installs no format or image-query callback and creates no graphics objects |
| no blit implementation | 40 | `vkCmdBlitImage` rejects every call; BLIT is not COPY, so no blit bit may be published |
| no texel-buffer descriptor | 32 | only the R32 UINT/SINT/SFLOAT uniform rows have buffer views and descriptor formats; there is no storage-texel-buffer type |
| no render-target encoding | 23 | only B8G8R8A8/R8G8B8A8 have a render-target encoding, and the native compiler requires `colorWriteMask` 15 with `blendEnable` false |
| no storage-image ABI | 15 | no storage-image descriptor, image-load/store ABI or atomic path exists |
| no sampled encoding | 4 | BGRA8 sampling needs a component swap the pinned GPL reference does not encode |

`conformance_inventory/physical_format_validation.json` is the machine-readable
record of the eight promoted cells: one entry per cell, naming the exact GPU
operation, the texel fixture, the deterministic expected readback, the shader
type (float, uint, sint or sRGB), physical evidence hashes and why host tests
alone cannot settle it. Two identical-artifact console runs per diagnostic
group passed before publication. Integer rows remain nearest-only; this
promotion adds no storage-image, blit or attachment support.

### Tiling scopes

The audit evaluates every scope carried by the pinned mandatory tables. At the
pinned revision those tables define `optimalTilingFeatures` and
`bufferFeatures` rules only; there is no mandatory `linearTilingFeatures` cell.
This profile reports zero `linearTilingFeatures` for every format and rejects
`VK_IMAGE_TILING_LINEAR` in `vkGetPhysicalDeviceImageFormatProperties`. The
format-query consistency checks compare that rejection with the reported
feature words. Linear tiling is therefore unsupported, not advertised.

### Pipeline cache identity

`pipelineCacheUUID` is no longer left zero. It is derived deterministically from
public, non-secret compatibility inputs only:

| Input | Current value |
| --- | --- |
| vendor / device identity | `0x1002` / `0` (the same values reported in the properties block) |
| graphics target | GFX1013 |
| driver version | the reported `driverVersion` |
| compiler identity and version | the pinned PSBC/ACO adapter identity |
| cache ABI revision | the pinned cache ABI version |
| UUID format counter | 1 |

Changing any input changes the UUID, which invalidates every previously exported
blob. The derivation is a deterministic two-stream FNV-1a mix over those values
(`src/physical_device_profile.h`); it is an identity, not a security boundary,
and it never depends on console state, dumps or proprietary material.

## Known Vulkan 1.0 deficits

Truthful reporting intentionally exposes several failures against the complete
Vulkan 1.0 graphics requirements. The graphics profile now implements bounded
1D, 1D-array, 2D-array, cube and 3D sampled images and reports the corresponding
core floors; the compute-only profile still has no graphics image model. Other
gaps include sample counts above one, a graphics allocation count
below the Vulkan 1.0 minimum, no host-coherent memory type, and only the small
format matrix above. The compute-only 64 MiB profile also reports a
storage-buffer range below the Vulkan 1.0 minimum for a complete implementation.

Consequently, original upstream `device_properties` and
`device_memory_properties` cases are useful diagnostics, not acceptance cases,
until their unmodified Vulkan 1.0 oracles pass. No constant may be raised merely
to satisfy those tests: the underlying implementation and deterministic
hardware witness must exist first.

## Evidence gate

`tests/test_physical_device_profile.c` checks derived values and malformed
profiles. `tests/test_vk_device.c` checks legacy and `*2KHR` queries, capacity
boundaries, untouched tails, formats, image queries and `pNext` preservation.
The standalone public-SDK consumer records a canonical report and checksum over
the native values. Promotion requires two runs of the identical SELF, complete
`ps5log/1` receipts, deterministic query witnesses and clean system Close Game.
