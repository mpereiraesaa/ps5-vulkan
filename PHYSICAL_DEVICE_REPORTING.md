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
| heap size | 64 MiB | 256 MiB | bounded project allocation budget |
| minimum allocation charge | 64 KiB | 128 KiB | direct-memory allocator policy |
| maximum allocation count | 1,024 | 2,048 | heap size divided by minimum allocation charge |
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
| Image type/layers: `maxImageDimension1D`, `maxImageDimension3D`, `maxImageDimensionCube`, `maxImageArrayLayers` | 0 / 0 / 0 / 1 | 4096 / 256 / 4096 / 256 |
| Attachments: `maxColorAttachments`, `maxFragmentOutputAttachments`, `maxFragmentCombinedOutputResources` | 1 | 4 |
| Vertex input: `maxVertexInputBindings` | 1 | 16 |
| Descriptors: `maxPerStageDescriptorSamplers`, `maxPerStageDescriptorSampledImages`, `maxPerStageDescriptorStorageImages`, `maxPerStageDescriptorInputAttachments`, `maxDescriptorSetSamplers`, `maxDescriptorSetSampledImages`, `maxDescriptorSetStorageImages`, `maxDescriptorSetInputAttachments`, `maxDescriptorSet*Dynamic` | 0-1 | 4-96 |
| Sampling: `maxSamplerLodBias`, `sampledImage*SampleCounts`, `framebuffer*SampleCounts`, `storageImageSampleCounts`, `sampledImageIntegerSampleCounts` | 0-1 | 2 / 1+4 / 1 |
| Other: `discreteQueuePriorities`, `maxMemoryAllocationCount`, `minTexelOffset`, `maxTexelOffset` | 0 / 2048 / 0 / 0 | 2 / 4096 / -8 / 7 |

The compute-only profile additionally leaves every graphics-object limit at
zero because `ps5vk_graphics_limits` is applied only by the graphics build.

### Still not audited

Fourteen `VkPhysicalDeviceFeatures` bits are reported `VK_FALSE` without a
frontend rejection branch this repository can cite, so the matrix records them
as `not-audited` instead of claiming either support or enforcement:
`robustBufferAccess`, `dualSrcBlend`, `depthBiasClamp`, `occlusionQueryPrecise`,
`vertexPipelineStoresAndAtomics`, `fragmentStoresAndAtomics`,
`shaderImageGatherExtended`, `shaderUniformBufferArrayDynamicIndexing`,
`shaderSampledImageArrayDynamicIndexing`, `shaderStorageBufferArrayDynamicIndexing`,
`shaderStorageImageArrayDynamicIndexing`, `shaderFloat64`, `shaderInt64` and
`shaderInt16`. Their dependent usage is an application-side valid-usage rule or
is rejected by PSBC/ACO without an explicit branch; neither is proven here.

The per-format mandatory rules are likewise recorded as blockers rather than
support: the profile advertises three image formats, so the mandatory format
family tables (including the compressed families) are not implemented. Shader
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

The current graphics format matrix is deliberately small:

| Format | Reported feature |
| --- | --- |
| `B8G8R8A8_UNORM` | optimal color attachment |
| `R8G8B8A8_UNORM` | optimal sampled image |
| `D32_SFLOAT` | optimal depth/stencil attachment |
| `R32_SFLOAT` | vertex buffer and uniform texel buffer |
| `R32_SINT`, `R32_UINT` | uniform texel buffer |
| `R32G32_SFLOAT`, `R32G32B32_SFLOAT`, `R32G32B32A32_SFLOAT` | vertex buffer |

Image-format queries accept only 2D, optimal-tiling, single-level,
single-layer, sample-count-one combinations that have an implemented native
role. Unsupported combinations return `VK_ERROR_FORMAT_NOT_SUPPORTED` and a
zeroed property structure.

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
Vulkan 1.0 graphics requirements. Among them are missing 1D, 3D, cube and array
image profiles, only sample count one, one array layer, a graphics allocation
count below the Vulkan 1.0 minimum, no host-coherent memory type, and only the
small format matrix above. The compute-only 64 MiB profile also reports a
storage-buffer range below the Vulkan 1.0 minimum for a complete
implementation.

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
