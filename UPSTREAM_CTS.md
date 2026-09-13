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
* **Selected**: the 41 acceptance cases frozen in `cts/upstream/manifest.json`
  (the previously accepted API, synchronization, memory, compute, resource,
  pipeline and push-constant cases plus nine storage-width cases). Only these
  acceptance leaves are registered by
  `cts/upstream/package_ps5.cpp`
  and shipped in the packaged case list. The manifest also carries a
  `diagnostics` list: upstream cases that are intentionally run separately and
  are known not to satisfy acceptance prerequisites. They are never part of
  strict acceptance.
* **Executed**: what a given report actually contains, which the strict verifier
  checks case by case.

The current selection contains no draw or pixel-comparison case. The graphics
entry is `dEQP-VK.api.smoke.create_shader`, which compiles a vertex shader at
runtime and validates the `vkCreateShaderModule` / `vkDestroyShaderModule`
lifecycle; it does not rasterise, so **no image oracle is executed**. Claiming a
reference-renderer comparison would require adding such a case to the manifest
and executing it, and neither has been done.

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
* No selected case exercises a rendering or pixel-comparison oracle. The
  reference rasterizer is linked but unexecuted, so this integration does not
  demonstrate rasterisation correctness.
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
