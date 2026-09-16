# Focused upstream Vulkan CTS on native PS5

This document covers the integration that cross-compiles a focused selection of
**genuine upstream Khronos VK-GL-CTS** code into a native PlayStation 5 payload
and executes it against ps5vk on the console's `gfx1013` GPU.

It is deliberately separate from the two other suites in this repository:

| Suite | Location | What it is |
| --- | --- | --- |
| Upstream VK-GL-CTS (this document) | `cts/upstream/`, `tools/build_upstream_cts.py`, `cts/upstream_runner.py` | Real upstream test bodies, framework and verification oracles |
| Synthetic Vulkan API contracts | `cts/cts_adapter.c`, `cts/runner.py`, `tools/build_cts_native.py` | Local `contract.*` tests modelled on CTS *mustpass* selection; they are not upstream tests |
| Standalone public SDK consumer | `examples/native_consumer/` | Public-header consumer of the staged SDK, not a CTS test |

Nothing here is a claim of Vulkan conformance or of complete CTS coverage. The
selection is intentionally small and is frozen in a committed manifest.

## Integration layout

| File | Role |
| --- | --- |
| `cts/upstream/main_ps5.cpp` | Entry point: ps5log init, run identity, command line, `tcu::App` iteration loop |
| `cts/upstream/platform_ps5.cpp` | `tcu::Platform` / `vk::Platform` adaptation with static dispatch into the ps5vk driver |
| `cts/upstream/package_ps5.cpp` | Focused `vkt::BaseTestPackage` that registers only the selected upstream groups |
| `cts/upstream/storage8_focus.cpp`, `storage16_focus.cpp` | Thin adapters around generated, registration-pruned copies of the pinned upstream storage modules |
| `cts/upstream/storage_width_focus.hpp` | Declarations for the two focused storage factories |
| `cts/upstream/log_sink_ps5.cpp` | `qpTestLog` output captured through a pipe and streamed as base64 QPA chunks |
| `cts/upstream/thread_atexit_ps5.cpp` | POSIX thread-exit destructor support required by libc++abi on this SDK |
| `cts/upstream/dladdr_ps5.cpp` | `__dladdr` back-end stub; the SDK stubs do not export it |
| `cts/upstream/manifest.json` | Frozen acceptance selection with per-case upstream source and rationale |
| `cts/upstream_runner.py` | Strict verifier: reassembles the QPA stream and enforces the acceptance policy |
| `tools/build_upstream_cts.py` | Reproducible cross-build and packaging of the payload |
| `tools/run_upstream_cts.py` | Launch, capture and verify one native acceptance run |
| `tools/check_upstream_selection.py` | Host-only check that every selected path traces back to upstream sources |

The integration supplies platform adaptation (threading, time, assets, logging,
static Vulkan dispatch), the payload entry point and verified registration
pruning for the two large storage modules. The generator changes only which
leaf factories are added to their trees; selected shader bodies, support checks
and result oracles remain the pinned upstream implementation.

### Linked versus selected versus executed

These are different claims and the selection below must not be read as if they
were the same thing:

* **Linked**: the upstream framework, modules and oracles are compiled into the
  payload, including the reference rasterizer and image-comparison machinery
  (`rrRenderer`, `tcuImageCompare`, `tcuRasterizationVerifier`, ...). The link
  map proves they are present, not that they run.
* **Selected**: the 117 acceptance cases frozen in `cts/upstream/manifest.json`
  (the previously accepted API, synchronization, memory, compute, resource,
  pipeline, push-constant, storage-width, fixed-function, buffer-transfer,
  image-copy and binding-model combined-sampler cases). Only these
  acceptance leaves are registered by
  `cts/upstream/package_ps5.cpp`
  and shipped in the packaged case list. The manifest also carries a
  `diagnostics` list: upstream cases that are intentionally run separately and
  are known not to satisfy acceptance prerequisites, including families kept as
  blocked resource-contract targets. They are never part of strict acceptance.
* **Executed**: what a given report actually contains, which the strict verifier
  checks case by case.

Every selected and diagnostic path is resolved against the pinned CTS revision
in "Pinned inputs": `tools/check_upstream_selection.py` refuses a path whose
leaf name, group segments or source anchor the pinned sources do not produce,
refuses a duplicate selection, and refuses a checkout that is not that
revision. The selection therefore cannot drift from the revision the packaging
build compiles.

The multiview render-pass families are kept as a blocked target rather than
acceptance. Prerequisites alone do not make a family runnable: the pinned
module builds every one of its attachments through `makeImageCreateInfo`
(`external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderUtil.cpp:114`),
which asks for a 2D `R8G8B8A8_UNORM` array image with one mip, one sample,
optimal tiling, `arrayLayers = extent.depth` and usage
`COLOR_ATTACHMENT | TRANSFER_SRC | INPUT_ATTACHMENT | TRANSFER_DST`. This
profile neither accepts nor advertises `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT`
and has no input-attachment descriptor or subpass execution path, so the one
run on canonical main `720ae713` refused every one of those 48 images with
`VK_ERROR_UNKNOWN` from `vkCreateImage`. The manifest records that envelope as
the `multiview-attachment-image` resource contract with its blocker, and the 48
leaves stay listed as diagnostics so the measured failure and the promotion
target remain visible. Within the same module, `renderpass2` needs
`VK_KHR_create_renderpass2`, `dynamic_rendering` needs
`VK_KHR_dynamic_rendering`, `index.geometry_shader` needs the core
`geometryShader` feature plus `multiviewGeometryShader`,
`index.tessellation_shader` needs `multiviewTessellationShader`, and two
view-mask leaves of each family (`8`, `1_2_4_8_16_32`) render extents deeper
than the reported `maxMultiviewViewCount` floor of 6: all of those stay
excluded as before.

`tools/check_upstream_selection.py` extends the frozen selection model from
features, extensions and limits to that resource footprint. The derivation
covers the multiview attachment family today, not every upstream family that
builds resources: it reads the factory branches the selected families use for
the format and sample count, and the attachment constructor for the type,
tiling, mip count, layer expression and usage. Support is not read from source
text at all: `tools/dump_device_reporting.c` asks the public
`vkGetPhysicalDeviceImageFormatProperties` for exactly that shape at the deepest
layer count the selection needs, really creates that image through
`vkCreateImage`, and the gate requires every declared contract field, the
witnessed request and the measured support to agree - so a contract cannot be
promoted by widening one accept/reject list, and the declared `supported` field
cannot stay stale once the driver changes. Acceptance can never reference a
missing, unknown or unsupported contract, the contracts must cover exactly the
selected families, and feature bits alone can never widen the selection.

The selection includes `dEQP-VK.api.smoke.triangle`. Its unchanged upstream
body creates a graphics pipeline, records a real triangle draw into an RGBA8
attachment, copies the complete image to a host-visible buffer and compares
every pixel with the upstream reference renderer. This is a bounded pixel
oracle for that case, not general rasterization or format conformance.

## Pinned inputs

| Input | Pin |
| --- | --- |
| VK-GL-CTS | tag `vulkan-cts-1.3.8.4`, commit `a0270c1897597e6c77679870e10415398a13001c` |
| glslang, SPIRV-Tools, SPIRV-Headers, Amber | The commits actually compiled are recorded in the generated build manifest under `dependency_commits` |

The CTS expects its external dependencies to be fetched with
`external/fetch_sources.py`, which declares its own pins. Those declared pins
and the checkouts present in the working tree can differ; the build manifest
therefore records the commits that were really compiled rather than assuming
the declared ones. `make check-upstream-cts` does not depend on any of these
checkouts being present.

## Build

Cross-building the payload needs the PS5 payload SDK and the lab tooling layout
described in [BUILDING.md](BUILDING.md):

```sh
export PS5VK_LAB_ROOT=/path/to/homebrew_ps5
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
python3 tools/build_upstream_cts.py   # or: make upstream-cts
```

The build compiles the selected upstream framework, module, shader-toolchain
(glslang), SPIRV-Tools and Amber objects for `x86_64-sie-ps5`, links them
against the staged `libps5vk.a` / `libpsbc.a`, and writes:

```text
build/upstream-cts/upstream_cts.map       # link map: provenance of every object
build/upstream-cts/build_manifest.json    # pins, toolchain, selection hash, hashes
dist-upstream-cts/PPSA99994/              # signed payload and package metadata
```

`upstream_cts.map` is the machine-readable provenance: it lists the upstream
framework and test objects that are actually linked into the executable.

## Selection and acceptance policy

`cts/upstream/manifest.json` is the frozen selection. The packaged case list is
derived from it, so the manifest, the on-device case list and the verifier
cannot drift apart. Each entry records the upstream source file that
implements the case, the required features and the rationale.

The acceptance policy is strict by construction:

* the process must report a completed session (start marker, end marker and an
  explicit completion record), and the exit status must be zero;
* the reassembled QPA stream must match its declared byte count and SHA-256, and
  the chunk sequence must be complete with no duplicates or gaps;
* every declared case must be reported exactly once, with no unexpected cases;
* every declared case must carry a `Pass` status from the upstream oracle.

`NotSupported`, `Fail`, `Skip`, unknown statuses, missing or extra cases,
truncated reports and a stopped/signalled process are all non-accepting.
`tools/check_upstream_selection.py` additionally fails if a selected path cannot
be traced back to the upstream sources it cites.

## Native execution and evidence

Runtime evidence uses the lab's TCP `ps5log/1` transport exclusively. The
payload streams the upstream report as ordered base64 chunks rather than
rewriting verdicts into a private format, so the host verifier reconstructs the
verbatim QPA document and applies the upstream status codes.

```sh
python3 tools/run_upstream_cts.py \
  --host <console> --runs-dir <ps5logd runs directory>
```

The run is tied to a build identity: the payload reads the selection hash and
the deployed executable hash from its own package and reports them, and the
verifier requires them to match the locally built package. A stale deployment
therefore fails verification instead of being attributed to the current build.

## Host checks versus hardware evidence

`make check-upstream-cts` runs entirely on the host and needs no console. It
checks the selection manifest against upstream sources and exercises the
verifier and run orchestrator against synthetic `ps5log/1` captures, including
negative cases (unsupported status, missing case, truncated report, stale
binary identity). It is a contract test, not hardware evidence.

Hardware evidence is produced only by a native launch on the console and is
identified as such wherever it is reported. Host tests do not establish GPU
correctness.

## Limitations

### Query-pool object surface (2026-09-13)

`vkCreateQueryPool`, `vkDestroyQueryPool`, `vkCmdResetQueryPool` and the
reset-but-unavailable subset of `vkGetQueryPoolResults`,
`vkGetImageSparseMemoryRequirements` and
`vkGetPhysicalDeviceSparseImageFormatProperties` are implemented and
host-tested, but they add **no upstream case** to this selection: every pinned
`dEQP-VK.query_pool.*` body executes a draw with occlusion query state (the
native occlusion counter is not implemented), and every sparse case requires
the `sparseBinding` feature this implementation reports false. Their evidence is
therefore host-only: `tests/test_query_pool.c` covers object lifecycle, ordered
reset, unavailable result/availability layout, structural fail-closed query
commands and empty sparse reports. `vkCmdBeginQuery`, `vkCmdEndQuery`,
`vkCmdCopyQueryPoolResults` and `vkCmdWriteTimestamp` remain recording-fail-
closed; `vkQueueBindSparse`, `vkCmdNextSubpass` and `vkCmdExecuteCommands`
likewise have no valid invocation in the reported profile. No CTS case or
hardware result is claimed for this slice.

### Pipeline cache (2026-09-13)

One additional original upstream case joins the selection:
`dEQP-VK.pipeline.cache.compute_tests.compute_stage`
(`pipeline/vktPipelineCacheTests.cpp:1893`). Its own body creates compute
pipelines through a `VkPipelineCache`, exports the cache with
`vkGetPipelineCacheData`, re-imports that data into a second cache, compiles
again and repeats the compute comparison, so it exercises creation, export,
re-import and cache-accepted pipeline creation with the upstream oracle.

Two independent launches of the identical final payload completed the 43-case
selection with **43 Pass, 0 Fail, 0 NotSupported**. Both reports passed strict
identity and QPA reconstruction, reached `allocations_bytes=0`, and the title
stopped after system Close Game.

- Executable SHA-256: `9fda99f15ea604987d3124ec40a11d6e8f0dc24310b0163ccf50da1f80f24bd1`
- Selection SHA-256: `00e9eb1902905886e36bfdbe2b288ec4775c6147c9f69026c9f178cf25ca8210`
- QPA SHA-256: `6c5d95aa2dd2b9e8179e759e7fa6e01618e8ff2380ef01c1ca5ffcfaf019528c`
  and `39233c916cba5a72bb36c7e78eac1f8988d72a89f841264c38919ec92644f256`

This case proves the API and lifetime contract only: the implementation exports
the normative 32-byte `VkPipelineCacheHeaderVersionOne` and stores no portable
compiled-code records, so no restored cache hit is claimed or reported. The
graphics-derived cache cases
(`graphics_tests`, `pipeline_from_get_data`, `pipeline_from_incomplete_get_data`,
`merge`, and the four `misc_tests`) remain unselected because the pinned bodies
require a `D16_UNORM` depth attachment this profile does not support; their
oracles are reproduced as host tests in `tests/test_pipeline_cache.c`.
### Binary semaphore and event expansion (2026-09-13)

The synchronization validation selection contained **48 original upstream
cases**. Six new cases retained their pinned factories, bodies and oracles:
host and device event
set/reset, event dependencies inside one submit and across submissions, a
two-record one-queue binary semaphore signal/wait, and the 32,768-link binary
semaphore chain.

Two independent launches of the identical payload completed with **48 Pass,
0 Fail, 0 NotSupported**. Both reports passed strict executable/selection
identity and complete QPA reconstruction, reached exit code zero and
`allocations_bytes=0`, and the title stopped after system Close Game.

- Executable SHA-256: `098087bc16b4363dbf622365b93695acee8697f74e9daeb776a1b297751312d4`
- Selection SHA-256: `c40bce192599fa734b00c8c670dccee43638d01efeb8eb042cc4c037fe2fda88`
- QPA SHA-256: `c8ac547bd58b28d20c65558bbdaae29046aef883543ca91f7d472de875c24c31`
  and `685714b9a80d6fd289ee0cfff206d7e2d04d968b63cd9f9170d9d7ca687a2fe2`

That 48-case receipt predates the independently validated pipeline-cache case.
The combined manifest now contains 49 cases, but no 49-case hardware run is
claimed. The selected synchronization cases establish only the single-queue
Vulkan 1.0 paths they ran.
Multi-queue, secondary command buffers, typed/timeline semaphores and
synchronization2 remain excluded and are not inferred from these results.

### Fixed-function expansion (2026-09-13)

Two independent launches of the identical final payload completed the then-current
42-case selection with **42 Pass, 0 Fail, 0 NotSupported**. Both reports passed
strict identity and QPA reconstruction, included the original upstream triangle
pixel comparison, reached `allocations_bytes=0`, and the title stopped after
system Close Game.

- Executable SHA-256: `2b8ff02a5a0906c4496f8795d9c7eabd8a0af20199b05d9d2261ad1393eb6d2d`
- Selection SHA-256: `b2b74ed43427d2fb0feeab07ab3ad4527643dbc125d41aadc8cee92f7200a515`
- QPA SHA-256: `757efc735d3d77b0b6019535dd8760016ca1f935c02bfa5c34f7579fa486eba5`
  and `88d5d6aff8d7e7fcfd2f4f2ef28acff70914f6d5f0477957ef65c28deb8c85ad`

The native implementation used here is intentionally narrow: one subpass, one
single-sample RGBA8 off-screen attachment, a full-image readback, and the exact
barriers used by the case. It does not broaden the project's Vulkan version or
claim conformance.

### Native acceptance (2026-09-12)

Two independent launches on PS5 FW 12.02 / GFX1013 completed the seven
API/synchronization/memory cases frozen at that time with **7 Pass, 0 Fail,
0 NotSupported**, exit code zero. Both strict verifications matched the
deployed executable and selection, reconstructed the complete QPA, observed
GPU completion and `allocations_bytes=0`, and confirmed the title stopped
after Close Game. This is focused upstream execution, not Vulkan conformance
or broad shared-memory coverage.

- Executable SHA-256: `992e607b6e1ec4d9fb54821796335380a7922b78dcd2c2cc08b7a911db3b9b64`
- Selection SHA-256: `7dc544e694fa40439f49a15417bd56297434bc70381290acb921c371b8ffbfea`
- QPA 1 SHA-256: `601c90bc47e4371e158f1476d6514c159dba688242edda62268469aa5ece2cf8`
- QPA 2 SHA-256: `429836ef94c67a92fca1827600852fc220f5260f26ccf9a4561ce15c45bc5fda`

Raw logs and reports remain private; these are audited result summaries.

### Compute expansion (2026-09-12)

The selection now also contains six `dEQP-VK.compute.basic` cases implemented by
the pinned upstream `vktComputeBasicComputeShaderTests.cpp`. All six execute
their upstream shader generation, runtime GLSL compilation and upstream SSBO
comparison oracles unchanged:

| Case | Upstream shape | What the oracle establishes |
| --- | --- | --- |
| `shared_var_single_invocation` | 1 invocation, 1 workgroup | LDS write, `memoryBarrierShared`/`barrier`, read-back |
| `shared_var_single_group` | 30 invocations, 1 workgroup | the same oracle with more than one invocation per group |
| `shared_var_multiple_invocations` | 1 invocation, 40 workgroups | per-workgroup shared state and workgroup addressing |
| `shared_var_multiple_groups` | 12 invocations, 42 workgroups | both dimensions at once, 504 checked values |
| `shared_atomic_op_single_group` | 30 invocations, 1 workgroup | `atomicAdd` on shared memory must return 30 distinct values |
| `shared_atomic_op_multiple_groups` | 12 invocations, 42 workgroups | the shared counter restarts per workgroup |

Two further independent launches (run 3 and run 4) of the identical final
payload completed the full thirteen-case selection with **13 Pass, 0 Fail,
0 NotSupported**, exit code zero. Both strict verifications matched the
deployed executable and selection, reconstructed a complete QPA whose declared
chunk count and SHA-256 match, observed GPU completion and
`allocations_bytes=0`, and confirmed the title stopped after Close Game. The
reassembled reports contain one `OpControlBarrier`, one `OpMemoryBarrier` and
(for the atomic cases) one `OpAtomicIAdd` per compute shader, so the predicates
above are the ones the hardware actually exercised.

- Executable SHA-256: `ee25e08f8f2aa35a2073b52a934a4197ba19095c6a1dd74b19eb4dfa9fe5380d`
- Selection SHA-256: `4608cfa0d78d37c71d8944bbaced02b108ba1470b294f943a266f64921bdbf4b`
- QPA 3 SHA-256: `782c0a46a35781928390964c10049c746beeeabe40b2d27abdeb80257e9ad0ff`
- QPA 4 SHA-256: `ad98eeb88c3348eadcfddab473d60a0549b7a584f4a202fd57e4fc94c612d62a`

The compute expansion needed no further driver change: the runtime compute
profile established by the earlier fix (compiler LDS sizing, inline dispatch
dimensions, per-dispatch retirement) already covers these shapes. The new
coverage is a statement about which upstream code has now been executed, not a
claim that the driver implements all of Vulkan compute.

The expansion also surfaced two host-side build problems that were fixed in
this change, because the work was done in a separate Git worktree rather than
in the canonical checkout:

- `tools/build_sdk.py` resolved the PS5 toolchain and the sibling lab projects
  through `ROOT.parents[1]`, so an out-of-tree worktree silently staged a
  host-only SDK and then failed to link the payload. It now uses the same
  `tools/lab.py` `lab_root()` resolution as the other native tools, which is
  unchanged in the canonical layout.
- `Makefile` hard-coded `../ps5-agc-gears` and `../logging_server`. They now go
  through `LAB_SIBLINGS`, whose default (`..`) is exactly the previous
  behaviour for in-tree checkouts.

### Resource expansion (2026-09-12)

The selection now also covers the resource contract that the runtime advertises
for compute: uniform buffers, an R32 uniform texel buffer through a
`VkBufferView`, and descriptor binding across more than one set. Five upstream
resource cases are accepted from the compute, binding-model and buffer-view
access modules.

| Case | Upstream shape | What the oracle establishes |
| --- | --- | --- |
| `dEQP-VK.compute.basic.ubo_to_ssbo_single_invocation` | std140 `UNIFORM_BUFFER` + `STORAGE_BUFFER`, set 0, 1x1x1 | the uniform-buffer descriptor returns the 256 source values that the host inverts |
| `dEQP-VK.compute.basic.ubo_to_ssbo_multiple_groups` | the same, local (1,4,2) over 8 workgroups | the same oracle over 1024 values read through the uniform buffer |
| `dEQP-VK.api.buffer_view.access.uniform_texel_buffer.r32_uint` | `VK_FORMAT_R32_UINT` viewed through a uniform texel buffer | all four lanes of each `texelFetch` result match Vulkan one-component completion `(R,0,0,1)` |
| `dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind.storage_buffer.compute.multiple_descriptor_sets.single_descriptor.offset_view_zero` | two descriptor sets bound in one `vkCmdBindDescriptorSets`, resources in set 0 binding 1 and set 1 binding 0 | the quadrant read-back only matches if a nonzero set index is bound and read |
| `dEQP-VK.binding_model.shader_access.primary_cmd_buf.bind.uniform_buffer.compute.multiple_descriptor_sets.single_descriptor.offset_view_zero` | the same two-set shape with uniform buffers and `HOST_WRITE -> UNIFORM_READ` dependencies | command recording completes and the four quadrant values match the original upstream oracle |

The first diagnostic run exposed two independent driver defects. The R32 SRD
used generic `X,Y,Z,W` destination selectors, which made this one-component
format replicate R; it now encodes `X,0,0,1`. The command validator also omitted
`VK_ACCESS_UNIFORM_READ_BIT`, so the binding-model case's valid pre-dispatch
buffer barrier invalidated its command buffer. Exact host regressions cover the
SRD fields and the two-set UBO barrier/dispatch sequence, including a negative
access-mask control.

Two independent launches of the identical corrected payload completed the
strict eighteen-case selection with **18 Pass, 0 Fail, 0 NotSupported**, exit
code zero, matched executable/selection identity, complete QPA reconstruction,
GPU completion, `allocations_bytes=0` and a stopped title after system Close
Game. Both newly promoted cases passed their original upstream oracles.

- Executable SHA-256: `4c869d74ef25f9e1094ef83a334bd725deac6c2bec3a0a2b30664a4216f3182d`
- Selection SHA-256: `4a1d671a7ca64e3b9e0dfa7b26dff8efe2ed54ad3829d1781a610a40a99072e8`
- QPA A SHA-256: `274c9de7cfca7e57b74170a9793ab73731d9351558bd618ef0667dd89d2e5bfe`
- QPA B SHA-256: `0de915028da2a6c7b7b336c62154f208d64dc222c6dcb3343de030b145bda7d2`

### Push and scalar-specialization expansion (2026-09-12)

Two additional genuine upstream Vulkan 1.0 cases now exercise the bounded
push/specialization implementation without changing their bodies or oracles:

| Case | Upstream shape | What the oracle establishes |
| --- | --- | --- |
| `dEQP-VK.pipeline.push_constant.compute_pipeline.simple_test` | one 16-byte compute push range, eight invocations | all eight output `vec4` values byte-compare with `(1,0,0,1)` |
| `dEQP-VK.api.pipeline.pipeline_layout.lifetime.destroy_after_end` | scalar `uint32` specialization value 1, scalar push value 75, 100 invocations; pipeline layout destroyed after recording and before submit | all 100 SSBO words equal `50 + 75 + invocation`, proving the recorded command owns the required state |

Two independent launches of the identical final payload completed the strict
twenty-case selection with **20 Pass, 0 Fail, 0 NotSupported**, exit code zero,
matching executable/selection identities, complete QPA reconstruction and a
stopped title after system Close Game.

- Executable SHA-256: `3cd38c3a7ed6b26384a82151eb9d08618473fd24f1a685299a87b2872fb62c83`
- Selection SHA-256: `8db6098fc129cc43e0238ebfeb1f70b812d8e0ca4a116195b569f33a16318d8b`
- QPA A SHA-256: `91f85420972e2bf40eb3824b7da9feeb7dd14e7f77316c13e06fd397b4049140`
- QPA B SHA-256: `4388f15c334d3e4870cbb3511d27143e72553cabfadb3393cece9376164839ae`

The four nearby `pipeline.spec_constant.compute.basic.*` leaves are deliberately
not selected. This pinned CTS revision requests SPIR-V 1.3 for those shaders,
while ps5vk currently advertises Vulkan 1.0 and no compatible SPIR-V extension;
the upstream framework correctly reports them unsupported before compilation.
The driver version is not inflated to turn that guard into a pass. Cases using
`LocalSizeId` also remain excluded because specialization-dependent workgroup
dimensions are not implemented.

### Extension-negotiated 8/16-bit storage (2026-09-13)

Nine pinned upstream SPIR-V assembly leaves extend strict acceptance without
enabling narrow arithmetic. Four `8bit_storage.storagebuffer_32_to_8` leaves
cover signed/unsigned scalar and vector output. Five 16-bit leaves cover the
same four 32-to-16 shapes plus signed scalar 16-to-32 widening. Each executes
the original upstream SPIR-V assembly and byte-comparison oracle.

The driver continues to report Vulkan 1.0. It advertises
`VK_KHR_get_physical_device_properties2`,
`VK_KHR_storage_buffer_storage_class`, `VK_KHR_8bit_storage` and
`VK_KHR_16bit_storage`, and reports only `storageBuffer8BitAccess` and
`storageBuffer16BitAccess` in their feature structures. The CTS also appends
Vulkan 1.1 protected-memory and shader-draw-parameter feature structures with
false values while creating its session device; ps5vk accepts those neutral
structures but rejects true or invalid booleans and does not advertise either
feature.

Two independent launches of the identical final payload completed the 29-case
selection with **29 Pass, 0 Fail, 0 NotSupported**, exit code zero, matching
executable/selection identities, complete QPA reconstruction and clean Close
Game:

- Executable SHA-256:
  `ee09394647c9bb728f2725f3f3c087fa93fe61840e18c2f1299567576f1d069c`
- Selection SHA-256:
  `5a1448c6d7ea05acf1b1d6e0b8e38881df8fefeb7813386fafb151550377e302`
- QPA A SHA-256:
  `dfaec984b71b5d2eae3c168e4be27b7420d83de4b1f9473ba66d2163b11ba6eb`
- QPA B SHA-256:
  `2c717f5d59c23e121664b64a036e99965d5ed0c5ea4a7d2ae35d1f874c77b862`

The nearby `uniform_8_to_8.stress_test` remains a separate diagnostic. Its
shader requests the supported storage-buffer capability, but its upstream test
also sets `coherentMemory=true`; the pinned allocator therefore requires a
`HOST_COHERENT` memory type. ps5vk truthfully exposes one `HOST_VISIBLE`,
non-coherent type, so the upstream result is `NotSupported` before shader
execution. The driver does not invent coherence to force a pass.

### Physical-device enumeration and queue reporting (2026-09-13)

The focused acceptance set now also registers the original upstream
`dEQP-VK.info.physical_devices` and
`dEQP-VK.info.device_queue_family_properties` factories from
`vktApiFeatureInfo.cpp`. These cases exercise the upstream enumeration and
queue-family reporting oracles; they are not local substitutes with upstream
names.

Two launches of the identical final payload completed the resulting 31-case
selection with **31 Pass, 0 Fail, 0 NotSupported**, exit code zero, complete
QPA reconstruction and clean Close Game:

- Executable SHA-256:
  `3cf502e0855f2d556b56c3284e440eaf85f3fb0ce1a378aadc696fe6792fba80`
- Selection SHA-256:
  `cb59facb407b8c9af78e540edb8dde3f6f6961b43afe9f15260c9f54c9f64d9c`
- QPA A SHA-256:
  `27fc8bbe1cba945158523a17af3a2b7951b279b71ba1f9b64cabb8f4f42d5ba3`
- QPA B SHA-256:
  `a6a12c61eff27ddb203f3ea56283be0873f6f034b0daa7322e57d3a93b5c833e`

The manifest retains `dEQP-VK.info.device_properties` and
`dEQP-VK.info.device_memory_properties` as diagnostics rather than acceptance.
The former is expected to expose the documented Vulkan 1.0 graphics minimum
gaps; the latter requires host-coherent memory, which the current native
profile does not claim. They are traceable to original upstream sources but
were not executed as acceptance cases in these two runs.

### Synchronization, non-coherent ranges and multi-wave LDS (2026-09-13)

The current selection adds ten original upstream leaves to the earlier 31:

- `compute.basic.ssbo_cmd_barrier_single` and `_multiple`;
- `synchronization.basic.fence.multi_waitall_false`, `.one_signaled` and
  `.multiple_signaled`;
- four `memory.mapping.suballocation` flush/invalidate leaves covering full and
  offset subranges;
- `spirv_assembly.instruction.compute.workgroup_memory.uint32`.

The two SSBO cases exercise recorded compute dependencies with their original
sum oracles. The mapping cases exercise upstream mapped-range rules and data
checks but, by themselves, do not prove GPU visibility. The workgroup-memory
case executes 128 invocations (`16x4x2`), four wave32 waves, LDS barriers and
the original exact reverse-copy oracle.

Two independent launches of the identical final payload completed **41 Pass,
0 Fail, 0 NotSupported**, with executable SHA-256
`be494b38483e5a174f40dfe063d967a722267daa4a78affb26143fd0b93335f9`,
selection SHA-256
`c2a630cba690c471bad479425d47d79754cbb82d63133c3a4d5d956c5dbdeca2`
and QPA SHA-256 values
`e613c39592b107d7e915693d3197ff7972ce21e685beafacdd4da53931e4d613`
and `e9106bf707866a9ecf33a4a8384b28b6abcbf129317d9f1d3a52cdaf79c887f7`.
Both runs exited zero and passed system Close Game.

### Heap and driver fixes

The original roughly 13 MiB ceiling belonged to the foundation's **internal
libc heap mode**, not the console's available RAM. That mode ignored the
application's existing expandable-heap parameters. `tools/cts_heap_parameters.py`
selects application mode in this project's freshly linked ELF before signing,
validating the parameter layout and refusing ambiguity or drift. It does not
patch system code or change the shared foundation. Removing that limit allowed
all seven cases to execute and exposed two genuine driver gaps:

- Simultaneous-use command buffers now retain ownership until retirement;
  the conservative backend serializes reuse rather than pretending it is idle.
- Runtime compute preserves compiler LDS sizing and supplies inline dispatch
  dimensions for the pinned PSBC six-user-SGPR ABI. Validated buffer barriers
  use the backend's stronger global completion/cache dependency, retaining
  buffer references and rejecting ownership transfers and invalid ranges.

The optional upstream shader cache remains disabled to avoid its fixed 16 MiB
pool; GLSL compilation still happens natively. No selected test body, shader,
oracle, or selection was replaced to obtain these results.

### Remaining scope limits

* This is a focused selection, not the complete CTS and not conformance.
* The pinned revision is a 1.3-era CTS; it does not establish Vulkan 1.4
  coverage.
* The selected `workgroup_memory.uint32` case establishes one exact multi-wave
  LDS barrier shape at 128 invocations. The selected shared-atomic cases remain
  at most 30 invocations; multi-wave shared atomics are instead covered by the
  independent public-SDK consumer's deterministic permutation/counter oracle.
  This does not imply arbitrary workgroup shapes or a complete memory model.
* The selected smoke triangle supplies one bounded upstream render/pixel oracle;
  it does not establish general rasterization correctness or format coverage.
* Application heap mode removes the measured internal-mode ceiling; this run
  does not establish the maximum safe heap size for arbitrary applications.
* Cases that require API the driver does not implement are reported as failures
  or unsupported results, not silently converted into passes.
* The resource selection is still focused: it does not establish general texel
  formats, descriptor arrays, dynamic buffers, images or arbitrary set layouts.
* The storage-width selection establishes only storage-buffer access and
  conversion. It does not establish `shaderInt8`, `shaderInt16`, float16 or
  uniform/push/input-output narrow storage, all of which remain unadvertised.
* The diagnostic 8-bit stress case requires host-coherent memory and therefore
  does not execute its shader on this non-coherent memory profile.
* `tcuImageIO` (libpng) and the generated EGL wrapper (`gluRenderConfig`) are
  not part of this focused build; no selected case uses them.

## Buffer copy, update and fill (2026-09-13)

The strict selection now contains **55 original upstream cases**. Six additions
exercise the unchanged upstream buffer-transfer bodies and byte-comparison
oracles: four `api.copy_and_blit.core.buffer_to_buffer` leaves (`partial`,
`regions`, `unaligned_regions`, `whole`) and the suballocation
`fill_buffer_whole` / `update_buffer_whole` leaves.

Two launches of the identical payload passed **55/55**, with no Fail,
NotSupported, Skip, missing or unexpected cases. Both passed executable and
selection identity, complete QPA reconstruction, exit code zero and independent
Close Game checks.

- Executable SHA-256:
  `08ef1da0ba1db2ace235944f5c8b14bcc08bce3bdf83ea97bb9db19d2bec69e9`
- Selection SHA-256:
  `6e18c46753ef098174ce05f84a2e0a2cfcd46aa222dc961c7f6711666a6ddcc5`
- QPA SHA-256 values:
  `a78264069e91b86e1c04bd7436a47c9d920cf6b93cf6e661927fb540d7c116da`
  and `3f93179e01caaa3561d5b81e04f216fb44dbecd2084f832375baaaef425ead2d`

The focused copy-module generator changes registration only: the original
`CopyBufferToBuffer::iterate` workload and comparison remain compiled and run.
Two dedicated-allocation leaves are preserved in the diagnostic manifest and
return `NotSupported` because `VK_KHR_dedicated_allocation` is not advertised;
they are not counted as acceptance. No image-copy, blit or resolve claim follows
from this buffer-only increment.

## Indirect compute dispatch (2026-09-13)

The strict selection now contains **57 original upstream cases**. The two new
leaves come directly from
`vktComputeIndirectComputeDispatchTests.cpp`:

- `dEQP-VK.compute.indirect_dispatch.upload_buffer.single_invocation`
- `dEQP-VK.compute.indirect_dispatch.gen_in_compute.single_invocation`

The first uploads one `VkDispatchIndirectCommand` and validates the complete
storage-buffer result. The second writes the command from compute, inserts the
upstream buffer dependency, dispatches indirectly and checks the same final
oracle. The factory and original body are linked; the integration does not
replace either oracle.

Two byte-identical launches passed **57/57**, with no Fail, NotSupported,
Skip, missing or unexpected cases. Both passed executable/selection identity,
complete QPA reconstruction, exit code zero and independent Close Game checks.

- Executable SHA-256:
  `d5e2d3476b4ba241b95ffe62361a9508df596d0305d4c2ef54c0b2e3f6818b13`
- Selection SHA-256:
  `c48990694f33d3deec76d3ed7e32740e4302e09fde047330532f6dcd17d63da9`
- QPA SHA-256 values:
  `637e5a4c1c64ccd1e3f6d7aab2698eeb934117fde1e75de838ff9caf5b14ffbd`
  and `a7f591579691bccfa78f06f322c53720abaffb3bacb1d9bd745264af0d79bbf3`

This establishes the selected single-dispatch compute paths only. The
selection contains no indirect graphics draw oracle, and the implementation
does not advertise multi-draw or `VK_KHR_draw_indirect_count`.

## Dynamic-state compute/transfer non-interference (validated, 2026-09-13)

The focused package now registers the original upstream
`dynamic_state.monolithic.compute_transfer` factory and selects 32 cases: 28
single-state cases covering the seven newly wired Vulkan 1.0 setters across
compute/transfer and before/after placement, plus the factory's four multi-state
cases. The pinned upstream translation unit is compiled directly; its command
recording, buffer readback and comparison oracles are unchanged. The outer
dynamic-state group retains upstream's `cleanupDevice()` lifecycle for its
singleton device helpers.

Selection provenance, manifest shape, factory registration, original-body
anchors and build wiring are host-validated. Two launches of the byte-identical
payload (SELF SHA-256
`e799270e98d5019f2eb8142c2a4a20c3095702dd1ae5f2eaa3180ebc2f449df9`,
selection SHA-256
`32ec25db6d4763451124d7b32afb60091e2b507a921e6856c467c77b20645235`)
passed all 89 focused cases, including all 32 cases in this family, with zero
Fail/NotSupported/Skip, complete QPA reconstruction and clean Close Game:

- `20260913T114816880Z_PPSA99994_upstream-cts_0x3d3341b8a877`
- `20260913T114838666Z_PPSA99994_upstream-cts_0x3d3853beabc2`

This evidence proves the unchanged upstream compute/transfer oracles and that
recording these setters does not interfere with those operations. It does not
exercise their graphical effects and makes no Vulkan conformance claim.

## Image copy and colour clear (2026-09-13)

Four leaves of the pinned `api.copy_and_blit.core.image_to_image.simple_tests`
factory are selected: `partial_image_pot_same_format_clear`,
`partial_image_pot_same_format_noclear`,
`partial_image_npot_same_format_clear` and
`partial_image_npot_same_format_noclear`
(`vktApiCopiesAndBlittingTests.cpp:9235`). They are the leaves whose source and
destination images are `VK_FORMAT_R8G8B8A8_UNORM` with exactly
`TRANSFER_SRC | TRANSFER_DST` usage, which is the only image role this driver
implements byte-exactly. Every variant compares the bit-exact readback of the
destination image, so the selection judges `vkCmdCopyImage` with upstream's own
oracle and not with a substituted one. The clear variants also record
`vkCmdClearColorImage` with `(1, 0, 0, 1)`, but their destination is already
initialized to that same red value. They therefore prove that the extra clear
does not corrupt the copy result; they are not an independent clear-colour
oracle. Deterministic clear-colour coverage remains in the host suite unless a
native consumer uses a distinct pre-clear value and verifies the readback.

The remaining leaves of the same factory are deliberately not selected:
`whole_image`, `whole_image_diff_format` and `partial_image` use
`VK_FORMAT_R8G8B8A8_UINT`, the `diff_format` leaves mix `R32_UINT` with RGBA8,
and the `depth` and `stencil` leaves use `D32_SFLOAT` and `S8_UINT`. None of
those roles is advertised, so selecting them would be a capability claim rather
than evidence.

The focused copy-module generator registers the group through upstream's own
`addImageToImageTestsSimpleOnly` factory from the rewritten
`addCoreCopiesAndBlittingTests`. That keeps the packaged tree bounded (no
all-formats, 3D, cube, array, sparse or blit/resolve registration) while the
selected bodies, support checks and comparison oracles stay byte-for-byte
upstream. Both the rewrite and the derived leaf names are host-checked: the
generator refuses to run if the factory or the pinned registration block drifts,
and `tools/check_upstream_selection.py` derives
`partial_image_<extent>_<format>_<clear>` only from the exact composition
expression and the three table names inside the cited function.

The final hardened implementation was rebuilt and deployed from commit
`002743f`. Two independent launches used the identical SELF SHA-256
`310c662777e6b4ab31a68fd8ae0ad6bb666b09471c2654bd1d909d72fb2754a8`
and selection SHA-256
`4181af6a7032b15fd27d42fcb6eb8aa178d717d3cd66e03607d85e6aecf9c272`:

- `20260913T133316202Z_PPSA99994_upstream-cts_0x42ede9683184`
- `20260913T133335878Z_PPSA99994_upstream-cts_0x42f27e4c8467`

Each reconstructed the complete QPA, passed all 93 selected upstream cases
with zero `Fail`, `NotSupported` or `Skip`, and ended with verified system
Close Game. The image-transfer contribution to those runs is the four bounded
RGBA8 copy leaves described above; the 93-case total also protects the existing
compute, graphics, synchronization, memory and buffer-transfer oracles from
regression. It does not widen the image profile or establish conformance.

## Mandatory Vulkan 1.0 feature reporting expansion

The current manifest contains 94 acceptance cases. The added original upstream
leaf is `dEQP-VK.info.device_mandatory_features`, whose generated oracle requires
`robustBufferAccess` for this Vulkan 1.0 profile. The rebuilt payload used SELF
SHA-256
`b7c485340e03fe66cbba572cdf678c7b7ac2f61643721f63d3eade411298429b`
and selection SHA-256
`5f853eb7d53226b7eda4f758aecaa70be857a80270f213d546d7bcae28015e41`.
Two independent launches produced these ps5log/1 runs:

- `20260913T181536286Z_PPSA99994_upstream-cts_0x52560710bc64`
- `20260913T181557404Z_PPSA99994_upstream-cts_0x525af1f50de2`

Both reconstructed complete QPA reports, passed 94/94 with zero `Fail`,
`NotSupported` or `Skip`, returned exit code zero and stopped through verified
Close Game. This proves the reported mandatory bit and its device-creation
contract on that exact build. At that stage it did not prove out-of-bounds
execution; the following expansion closes the selected scalar buffer subset.

## Executable robust-buffer expansion

The current manifest contains 106 acceptance cases. It adds 12 unchanged
upstream `robustness.buffer_access.compute.scalar_copy.r32_uint` leaves:
out-of-bounds UBO and SSBO reads plus SSBO writes, each at 1-, 3-, 4- and
32-byte descriptor ranges. Registration alone is pruned to this bounded family;
the Khronos shaders, device/resource setup, support checks and result oracles
remain unchanged.

The first diagnostic exposed two independent driver gaps before shader
execution: the native backend rejected the CTS auxiliary logical device, then
descriptor layouts rejected the valid `VK_SHADER_STAGE_ALL` visibility mask.
The backend now reference-counts a serialized process AGC session across
logical devices, and descriptor layouts accept valid core stage masks while
pipeline creation remains responsible for executable-stage support.

Two independent launches of the corrected candidate used SELF SHA-256
`43dd8803a47028ac5086434771d668e119aa662b4483581e72bc3e7ce571a175`
and selection SHA-256
`344a278e325846f6918903e48b3262e2551178aab2caa022c67e8dd3539f5b62`:

- `20260913T183926998Z_PPSA99994_upstream-cts_0x53a3233107e6`
- `20260913T183952333Z_PPSA99994_upstream-cts_0x53a909315ae1`

Both reconstructed all 106 results, reported 106 `Pass` with zero `Fail`,
`NotSupported` or `Skip`, returned exit code zero and stopped through verified
Close Game. Their QPA SHA-256 values are
`8137f2254731850c9b70d59119875df82c12db16815a8b544fe2f1f5847161da`
and `046002260dfa35dd7bf3db02349368eac110a253f44ef8c7a2f587712220b4f0`.
This is executable evidence for the selected scalar buffer family, not a Vulkan
conformance claim or evidence for every robustness permutation.

## Binding-model combined-sampler expansion (2026-09-14)

The current manifest contains 109 acceptance cases. It adds three unchanged
upstream `binding_model.shader_access.primary_cmd_buf.bind.
combined_image_sampler_mutable` leaves, all of them the `single_descriptor.2d`
variant, one per stage combination:

- `...combined_image_sampler_mutable.vertex.single_descriptor.2d`
- `...combined_image_sampler_mutable.fragment.single_descriptor.2d`
- `...combined_image_sampler_mutable.vertex_fragment.single_descriptor.2d`

The factory the manifest cites is
`external/vulkancts/modules/vulkan/binding_model/vktBindingShaderAccessTests.cpp:9698`.
No Khronos test body, shader, support check, reference image or result oracle was
modified, and the integration already registers that factory wholesale and
compiles its module, so this is a selection change only: the packaged case list
grows by exactly three lines. Each case binds one mutable combined image sampler
into a 2D RGBA8 target and compares the rendered quadrants against the upstream
four-quadrant oracle.

Selection is not acceptance on its own. The three leaves were first executed as
a private three-case candidate selection against the unchanged upstream bodies;
only after both runs passed every original oracle were they promoted here.

The promotion was validated with one signed payload deployed through exact FTP
readback, SELF SHA-256
`afc5464712c54f176b52f7e2e00c966ee81cfce1ff7d38e85aa3ece5d61e1c95`
and selection SHA-256
`b74617b2e99d1ebcc9ff3bc1c12f51ece38c82b6bd96db1fdca142b3294eaf1d`:

- run `run-159167454934290`, QPA SHA-256
  `57be15fefc8bd5fd1d8db836225611fdc29cb51ec470b2be148f1778e5993b8d`
- run `run-159192559716276`, QPA SHA-256
  `f81d2a1bf5fc5cc57d64a36be020b174caf48ad6a20618b757d41b7cc0503284`

Each run reconstructed all 109 results, reported exactly 109 `Pass` with zero
`Fail`, `NotSupported` or `Skip`, no missing, unexpected or duplicate cases,
returned exit code zero, completed the `ps5log/1` transport with a
`complete-success` BYE, reported zero tracked allocations at teardown and
stopped through a verified Close Game of the exact title. The run identity is
the deployed package's own selection hash and SELF hash, checked against the
locally built package, and the independent artifact check is the exact FTP
readback of the deployed SELF rather than a hash echoed by the process.

This is executable evidence for those three bounded original oracles. It does
not advertise a descriptor or sampler limit, does not establish general
multi-set sampling support and is not a Vulkan conformance claim; the reported
sampler and sampled-image limits are unchanged, and the separate
96-descriptor stress diagnostic remains backend qualification rather than
portable capability.

The selection gate that guards this list was hardened in the same change: a
manifest that names the same case twice is rejected outright, including an
acceptance/diagnostic conflict, and regression tests fail if a promoted case is
dropped, renamed, duplicated or replaced by a path the pinned sources do not
produce.
