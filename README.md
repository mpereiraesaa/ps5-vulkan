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
- Core `robustBufferAccess` reporting, device negotiation and executable
  UBO/SSBO out-of-bounds semantics backed by bounded GFX1013 descriptors; 12
  original upstream access oracles pass in the exact 109-case native suite
- Host-visible buffers and images backed by native direct memory
- Command pools and command buffers with explicit recording state
- Ordered byte-granular buffer copies plus bounded buffer update and fill commands
- Single-dispatch and single-draw indirect commands with execution-time argument resolution
- Runtime-compiled compute pipelines with up to four resource sets
- Storage buffers, uniform buffers and 41 directly hardware-tested uniform
  texel-buffer formats spanning 1-, 2-, 4-, 8- and 16-byte elements
- Extension-negotiated 8-bit and 16-bit storage-buffer access
- Push constants and scalar specialization constants in compute and runtime graphics
- Vulkan pipeline-cache objects with a normative header export (no portable compiled-code records yet)
- Occlusion query-pool lifetime (result retrieval deferred) and empty sparse image queries
- Runtime vertex/fragment compilation for procedural triangles with a bounded pair cache
- Vertex and index buffers, indexed and non-indexed triangle-list and
  triangle-strip draws (the strip has host coverage but no native witness yet);
  core 8-, 16- and 32-bit float/normalized/integer vertex families plus the
  packed `A8B8G8R8_*` and `A2B10G10R10_UNORM` forms currently listed in
  [API.md](API.md). Forty-one conversion cases have exact GPU readback on
  non-indexed runtime draws, including byte strides and unaligned binding
  offsets
- One BGRA8 presentation attachment or RGBA8 off-screen color attachment,
  plus an optional D32 depth attachment
- 44 sampled texture formats spanning 8/16/32-bit UNORM,
  SNORM, signed/unsigned integer and floating-point families, RGBA8 sRGB,
  A8B8G8R8 packed color/integer, RGB9E5 and B10G11R11 packed floating point, with GPU
  upload transitions and deterministic hardware readback; core repeat,
  mirrored-repeat, edge/border clamp and the six fixed border-color enums are
  implemented. Nearest/linear filtering is validated for the 24
  filterable rows; the 20 integer rows use typed samplers and correctly
  remain nearest-only. A three-level RGBA8 chain has deterministic explicit-LOD
  GPU readback through the staged public SDK; signed sampler LOD bias is
  implemented and hardware-qualified at both Vulkan 1.0 boundary values, -2
  and +2
- 1D, 1D-array, 2D-array, cubemap and 3D sampled-image views with layered
  buffer uploads; their current per-region, layer, face or slice RGBA8
  witnesses use one level, while the explicit mip witness is 2D
- One static or dynamic viewport/scissor pair, depth testing and face culling
- Recording support for all Vulkan 1.0 dynamic-state setters; only dynamic
  viewport/scissor currently participate in native draws
- Bounded RGBA8 attachment readback through `vkCmdCopyImageToBuffer`
- Native two-buffer 1920x1080 presentation
- Explicit completion, retirement and bounded resource accounting

The exact supported profile is documented in [API.md](API.md). Build and test
requirements are in [BUILDING.md](BUILDING.md).
Runtime graphics test results and their limits are summarized in [VALIDATION.md](VALIDATION.md).
Vertex-fetch preparation and shader-cache identity are described in [VERTEX_INPUT.md](VERTEX_INPUT.md).
Native tessellation support, its tested limits and the focused hardware
validation are recorded in
[TESSELLATION_STATUS.md](TESSELLATION_STATUS.md).
The provenance and current deficits of physical-device limits, memory, queues
and formats are tracked in
[PHYSICAL_DEVICE_REPORTING.md](PHYSICAL_DEVICE_REPORTING.md).

Two independent suites are integrated against this backend: a focused selection
of **genuine upstream Khronos VK-GL-CTS** code compiled into a native payload
([UPSTREAM_CTS.md](UPSTREAM_CTS.md)), and a synthetic `contract.*` suite modelled
after the CTS *mustpass* selection. They are separate artifacts with separate
verifiers; neither is a claim of Vulkan conformance.

## DXVK target

DXVK support is tracked against the immutable DXVK **v2.6.2** profile
`VP_DXVK_d3d11_level_11_0_baseline`, whose declared API version is Vulkan
1.3.204. The exact upstream profile is hash-pinned; its 62 leaf requirements
(one API version, two extensions, 49 features and ten properties) are derived
into a checked-in machine-readable profile and joined independently to the
current public API, reviewed implementation, CTS and native evidence.

The fail-closed matrix currently proves **15/62** requirements completely:
`robustBufferAccess`, multiview and its two required limits, the three
indirect/indexed draw features `drawIndirectFirstInstance`,
`multiDrawIndirect` (with `maxDrawIndirectCount = 65535`) and
`fullDrawIndexUint32`, the user-defined `shaderClipDistance` and
`shaderCullDistance` pair, `fragmentStoresAndAtomics`, `dualSrcBlend`, and the
four rasterization and viewport features `depthClamp`, `depthBiasClamp`,
`fillModeNonSolid` and `multiViewport`. Multiview is queried through its
explicit KHR route. `geometryShader` and `tessellationShader` are positive on
the API, implementation and CTS axes but their native axis is still
`reported-not-executed`, so they stay among the other 47 requirements that
remain blockers, together with the API-version requirement. This is an
implementation roadmap, not a DXVK compatibility claim.
The public-SDK-only capability probe can be built with
`python3 tools/build_consumer.py --dxvk-v262-probe`; see
[the inventory](conformance_inventory/README.md#dxvk-262-profile) and
[validation notes](VALIDATION.md#dxvk-262-public-abi-capability-probe).

## Important boundaries

This is not a Vulkan-conformant driver or ICD, and it does not yet provide WSI,
swapchains, broad format coverage, general image transfer/blit/resolve, multiple queues,
timeline semaphores, blending, MSAA, anisotropy or arbitrary shader
programs. The single-queue Vulkan 1.0 profile includes binary semaphores and
host/device events; it does not imply multi-queue or synchronization2 support.
Compute SPIR-V is compiled at runtime through the pinned PSBC/ACO GFX1013
backend and cached under a bounded in-memory policy. Runtime vertex/fragment
compilation now supports procedural or multi-binding typed triangles with
matching smooth interfaces, BGRA8/RGBA8 targets, push/specialization constants
and vertex/fragment combined-image samplers. A four-set/96-element shared-stage
sampler stress fixture has exact GPU readback, but exceeds the still-conservative published
sampler limits; it is not a portable consumer or a limit promotion.
Compiled pairs reuse the bounded cache. Two identical-artifact shared-stage runs
passed, as did the default fragment-only regression and clean app closure.
This bounded result does not establish arbitrary shader compatibility; see
[the qualification details](VALIDATION.md#shared-stage-sampler-hardware-qualification).
Other graphics resource types
and arbitrary textured runtime-shader profiles remain unsupported.
The 8/16-bit slice covers storage-buffer access only; narrow integer/float
arithmetic and other narrow storage classes remain unadvertised.

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

## License

`ps5-vulkan` is free software licensed under the GNU General Public License,
version 3 or (at your option) any later version (`GPL-3.0-or-later`). See
[LICENSE](LICENSE) for the complete terms and [LICENSING.md](LICENSING.md) for
copyright, contribution, dependency and source-provenance details.

The staged SDK contains a static `libps5vk.a`. Distributing an application that
links this library creates a combined GPL work and requires providing its
corresponding source under GPL-compatible terms. Console system modules and
their link-time import facades are not distributed as part of this repository.
