# Runtime graphics validation

The experimental procedural graphics profile was tested on an owned PS5 with
firmware 12.02 on 2026-09-12, using the packaged native SDK and PSBC/ACO gfx1013.

## Observed results

Two consecutive launches of the same executable each completed:

- Runtime compilation of owned vertex and fragment SPIR-V absent from the
  diagnostic's offline graphics library.
- One compiled pair followed by two cache hits, with one retained cache entry
  using 9,316 bytes.
- Eighteen GPU-completed and presented frames across three viewport sizes,
  checked for triangle coverage, alpha and interpolated color invariants.
- Cache destruction and zero tracked GPU allocations after retirement.
- System Close Game returning success, confirmed title termination and a
  subsequent Home view without an error dialog.

Remote Play confirmed the triangle visually. Structured TCP telemetry provided
the GPU/VideoOut completion and ownership evidence; the screenshot alone did
not establish correctness.

## Reproduce and interpret

See [BUILDING.md](BUILDING.md) for the SDK-linked diagnostic and the
`verify_graphics_runtime.py` receipt verifier. Local validation includes
`make check`, `make check-sanitize` and the runtime graphics compiler/cache
test with ASan/UBSan enabled. The PSBC static archive itself is not instrumented.

### Runtime sampled images and explicit mip LOD

The GPL-compatible `ps5-opengl` reference informed a bounded runtime-graphics
adapter for exactly one fragment-stage combined image sampler at set 0,
binding 0. The compiler metadata, user-SGPR slot, 48-byte descriptor table,
descending mip layout and cache identity are validated on the host and reject
other descriptor shapes. The public `ps5-opengl` mipmap test was also compiled
and run unchanged on the same console; its explicit LOD and generated-mipmap
oracles passed. A temporary PSBC diagnostic then showed that both projects
lower the fragment operation to the same GFX1013 `image_sample_l` instruction.

The first ps5-vulkan diagnostic run
(`20260914T050039610Z_PPSA99994_ps5vk_0x75893aacacda`) was correctly rejected:
it returned only level 0. Comparing its runtime input to the working GPL test
identified the actual harness defects, not a driver defect: all three probe
vertices had zero UVs, and the scissor covered only half the intended domain.
The fixture now assigns the explicit `(0,0), (1,0), (0.5,1)` domain and uses the
full 1920x1080 scissor. Unit tests prevent either input from silently drifting.

The corrected **public-SDK-linked** payload produced run
`20260914T055702207Z_PPSA99994_ps5vk_0x789cca84a41b`, executable SELF SHA-256
`99844fe54a3a7c6b510fdeb870e13d46a34be098e17100131daca47e89c8c1c9`
and transcript SHA-256
`d4706a2930e0a798123e40e27f5ed84143840ffda16c1f647f8fc1221680a8b6`.
It compiled the owned vertex and fragment SPIR-V at runtime, uploaded three
solid RGBA8 levels, verified their descending backing offsets and read back
red/green/blue counts `103680/62208/20736`, with zero unexpected pixels and
`PS5VK_MIPMAP_READBACK valid=1`. The pre/post compute regression passed,
VideoOut presented the matching completion event, all native allocations were
released, BYE was complete, and exact-title Close Game completed in 100 ms.
`tools/verify_mipmaps.py` binds those claims to the transcript, artifact profile
and SELF identity and fails closed on missing colors, storage drift, partial
descriptor traces, transport gaps or false success.

This establishes the bounded shared mip-chain layout, descriptor and explicit
LOD path. It is not broad shader coverage, generated-mipmap support in Vulkan,
anisotropy, or a claim of Khronos conformance.

## Vulkan API contract suite validation (CTS-modeled)

A suite of 26 synthetic Vulkan API contract tests modeled after the Khronos `VK-GL-CTS`
mustpass selection (`vulkan-cts-1.3.8.4`, commit `a0270c1897597e6c77679870e10415398a13001c`,
Apache-2.0) was evaluated on both the host mock harness and real hardware. Full upstream
Khronos VK-GL-CTS coverage remains incomplete; the separate focused upstream
execution is described below.

- **Host mock suite:** `make check` builds `build/tests/test_cts_host` against `dist-sdk/lib/libps5vk_host.a`.
  Observed result: `total=26 pass=22 not_supported=4 fail=0 skip=0`. The 4 `NotSupported` cases
  faithfully reflect the host mock environment's lack of native PSBC shader compilation and AGC hardware queues.
- **PS5 hardware execution (FW 12.02, GFX1013):** The native package executes the 26 contract cases
  in a single session using the statically linked driver and runtime compiler.
  Observed result: **26 / 26 PASS** within the single session device context, 0 failures.
  Structured `ps5log/1` telemetry confirmed:
  - Core API build, platform and device introspection matching driver caps.
  - Device initialization within session context (a restriction of the current
    backend/harness; independent reinitialization is not validated).
  - Device limits, non-coherent atom size (64B) and storage alignment (256B).
  - Memory allocation, suballocated memory mapping (257 bytes), cache flush and invalidate ranges.
  - Sampler and shader module creation and destruction.
  - Runtime triangle graphics pipeline compilation and cache insertion (9,316 bytes; pipeline creation only, draw/rasterization verified in native consumer below).
  - Runtime SSBO compute dispatch (`vkCmdDispatch` 16 workgroups x 64 threads = 1,024 elements) with exact arithmetic verification.
  - Empty compute pipeline compilation and retirement.
  - Signaled and unsignaled fence status polling, reset, and queue empty submission.
  - Clean graphics cache destruction and zero tracked GPU memory allocations at retirement.

See [`cts/gap_matrix.md`](cts/gap_matrix.md) for the complete case list, Vulkan API mapping and rationales.

## Genuine upstream VK-GL-CTS (separate from the synthetic contracts)

The `contract.*` suite above is local and modelled on the Khronos *mustpass*
selection; it is **not** upstream CTS code. A separate integration compiles a
focused selection of real upstream VK-GL-CTS tests, the upstream framework and
their original verification oracles into a native payload. Its build inputs,
selection manifest, strict acceptance policy and evidence rules are documented
in [UPSTREAM_CTS.md](UPSTREAM_CTS.md). Results from that integration are
reported separately and are not merged into the counts above.

On 2026-09-13, two independent native launches completed **all 29 selected
upstream cases with Pass**. The prior API, synchronization, memory, compute,
resource, push-constant and specialization cases remain present. Nine original
SPIR-V assembly oracles now additionally cover storage-buffer-only 8/16-bit
scalar/vector conversions under extension-negotiated feature bits. Both runs
matched executable and selection identity, reconstructed complete QPA reports,
returned exit code zero and passed system Close Game checks.

The 29-case executable and selection hashes are respectively
`ee09394647c9bb728f2725f3f3c087fa93fe61840e18c2f1299567576f1d069c`
and `5a1448c6d7ea05acf1b1d6e0b8e38881df8fefeb7813386fafb151550377e302`.
The QPA hashes are
`dfaec984b71b5d2eae3c168e4be27b7420d83de4b1f9473ba66d2163b11ba6eb`
and `2c717f5d59c23e121664b64a036e99965d5ed0c5ea4a7d2ae35d1f874c77b862`.
One separate upstream stress case remains diagnostic because it requires a
`HOST_COHERENT` memory type that ps5vk does not advertise; it is not counted as
acceptance. Details and remaining limits are recorded in
[UPSTREAM_CTS.md](UPSTREAM_CTS.md).
The four nearby specialization cases that demand SPIR-V 1.3 and every
`LocalSizeId` workgroup-size case remain outside the selection; no advertised
API or SPIR-V version was widened to bypass their upstream support checks.

### Physical-device reporting foundation

The current selection subsequently grew from 29 to **31 acceptance cases** by
adding the original upstream `dEQP-VK.info.physical_devices` and
`dEQP-VK.info.device_queue_family_properties` cases. Two independent launches
of the identical final payload passed all 31 cases with zero failures or
unsupported results, complete QPA reconstruction, exit code zero and clean
system Close Game:

- Executable SHA-256:
  `3cf502e0855f2d556b56c3284e440eaf85f3fb0ce1a378aadc696fe6792fba80`
- Selection SHA-256:
  `cb59facb407b8c9af78e540edb8dde3f6f6961b43afe9f15260c9f54c9f64d9c`
- QPA A SHA-256:
  `27fc8bbe1cba945158523a17af3a2b7951b279b71ba1f9b64cabb8f4f42d5ba3`
- QPA B SHA-256:
  `a6a12c61eff27ddb203f3ea56283be0873f6f034b0daa7322e57d3a93b5c833e`

The standalone public-SDK consumer independently queried enumeration,
properties, limits, memory, queue families, format properties and image-format
support twice. Both runs used executable SHA-256
`89849de59f76e97be574323c617e88d01f086482010c0cdcea790dfefaea5a54`,
produced the same canonical physical-report FNV-1a value `be169e1b`, verified
the 256 MiB graphics heap and non-coherent host-visible memory contract, and
closed cleanly. Their `ps5log/1` receipt hashes were
`06cadb6dab4cd63bf9fba5d21d4f40a175fe3f889b44720d58af0dbba9be6d8e`
and
`44321d1d8c11363a4760d61d800d14364666a57496d606d3d353e2d2053fcff6`.

This evidence validates the implemented reporting slice, not every mandatory
Vulkan 1.0 limit or format. The original upstream `device_properties` and
`device_memory_properties` cases remain diagnostics: the first exposes known
minimum-limit gaps and the second requires a host-coherent type that this
backend truthfully does not report. See
[PHYSICAL_DEVICE_REPORTING.md](PHYSICAL_DEVICE_REPORTING.md).

### Vulkan 1.0 synchronization and non-coherent visibility slice

The synchronization milestone acceptance selection contained **41 original upstream cases**.
Ten additions exercise two compute command-buffer barrier cases, three further
fence states, four non-coherent mapping ranges and
`dEQP-VK.spirv_assembly.instruction.compute.workgroup_memory.uint32`. The last
case uses `LocalSize 16x4x2` (128 invocations, four wave32 waves) and its original
exact reverse-copy oracle.

Two independent launches of the identical payload passed **41/41** with zero
failures or unsupported results, complete QPA reconstruction, exit code zero
and clean system Close Game:

- Executable SHA-256:
  `be494b38483e5a174f40dfe063d967a722267daa4a78affb26143fd0b93335f9`
- Selection SHA-256:
  `c2a630cba690c471bad479425d47d79754cbb82d63133c3a4d5d956c5dbdeca2`
- QPA SHA-256 values:
  `e613c39592b107d7e915693d3197ff7972ce21e685beafacdd4da53931e4d613`
  and `e9106bf707866a9ecf33a4a8384b28b6abcbf129317d9f1d3a52cdaf79c887f7`

The consumer that includes only public SDK headers independently passed twice
with executable SHA-256
`18feb46fc7e4a8972949541ece619270b891c77711f9d172aadef4f72036b9d6`.
It verifies a host-write/compute-read-write/host-read chain, an explicit
shader-write to shader-read buffer barrier, non-coherent flush/invalidate,
64 exact output words and guards, and a 128-lane LDS atomic permutation with a
final counter of 128. Its ps5log/1 receipt hashes are
`ff4094fed0ab44d70c49e08d5f16d63cc9cce511c4b3518ba215a4301b28ac98`
and `f0ec40048f397a3425b7b3a87ff454b1ad67752b4e69d8d1bff9242ed2c00d19`.

The native queue implements a conservative dependency stronger than the
recorded buffer range: every compute dispatch reaches completion and performs
release/writeback before the next dispatch. Later binary-semaphore and event
validation below covers ordered single-queue execution, but does not establish
range-scoped asynchronous execution, mixed compute/graphics barriers,
queue-family transfers, timeline semaphores, the Vulkan memory model or
`synchronization2`.

### Fixed-function and original upstream pixel oracle

The next bounded graphics slice adds one static or dynamic viewport/scissor
pair, compatible one-subpass render-pass/framebuffer ownership, bounded color
and depth attachment load/store/clear behavior, and RGBA8 off-screen readback.
The selection now contains **43 original upstream cases** (the 42 below plus the
pipeline-cache compute case described afterwards). The added
`dEQP-VK.api.smoke.triangle` body records a real draw, copies the rendered image
to a buffer and compares its pixels with the unchanged upstream reference
renderer.

Two launches of the identical payload passed **42/42** with zero failures,
unsupported or skipped cases, complete QPA reconstruction, zero live platform
allocations at teardown and clean system Close Game:

- Executable SHA-256:
  `2b8ff02a5a0906c4496f8795d9c7eabd8a0af20199b05d9d2261ad1393eb6d2d`
- Selection SHA-256:
  `b2b74ed43427d2fb0feeab07ab3ad4527643dbc125d41aadc8cee92f7200a515`
- QPA SHA-256 values:
  `757efc735d3d77b0b6019535dd8760016ca1f935c02bfa5c34f7579fa486eba5`
  and `88d5d6aff8d7e7fcfd2f4f2ef28acff70914f6d5f0477957ef65c28deb8c85ad`

The public-header-only consumer independently passed twice with executable
SHA-256
`7209dd80a3a3431dd6f26d54420d3f91bba15b5d98773050916f91f13395c500`.
Each run verified 18 fixed-function frames and 36 graphics submissions,
including exact color/depth results, dynamic viewport/scissor state,
attachment `LOAD` preservation, negative depth controls, strict `ps5log/1`
validation and clean Close Game. Transcript SHA-256 values were
`5fba873ebe85d478ab8caf279c2e15b156771a43062a3054da8d965b2750dfbf`
and `dc638572e4785c9c2dad9a4654f2bfb6e0a959f7c0a1be6f1e2a438023cb7528`.

This evidence covers only the enumerated profile. It does not establish general
rasterization, blending, multisampling, stencil, secondary command buffers,
WSI or Vulkan conformance.

### Pipeline cache (2026-09-13)

The pipeline-cache slice makes the four mandatory Vulkan 1.0 cache commands part
of the public surface again: `vkCreatePipelineCache`,
`vkDestroyPipelineCache`, `vkGetPipelineCacheData` and `vkMergePipelineCaches`.
Both pipeline-creation entry points now accept a live same-device cache again;
an invalid or foreign handle still fails closed. The exported blob is exactly
the normative 32-byte `VkPipelineCacheHeaderVersionOne`, externally supplied
data is treated as untrusted (short, corrupt, mismatched or oversized blobs are
ignored and creation still succeeds), and merge is a validated no-op.
`pipelineCacheUUID` is derived deterministically from public compatibility
inputs instead of being zero.

The selection gained `dEQP-VK.pipeline.cache.compute_tests.compute_stage`. Two
launches of the identical payload passed **43/43** with zero failures,
unsupported or skipped cases, complete QPA reconstruction, zero live platform
allocations at teardown and clean system Close Game:

- Executable SHA-256:
  `9fda99f15ea604987d3124ec40a11d6e8f0dc24310b0163ccf50da1f80f24bd1`
- Selection SHA-256:
  `00e9eb1902905886e36bfdbe2b288ec4775c6147c9f69026c9f178cf25ca8210`
- QPA SHA-256 values:
  `6c5d95aa2dd2b9e8179e759e7fa6e01618e8ff2380ef01c1ca5ffcfaf019528c`
  and `39233c916cba5a72bb36c7e78eac1f8988d72a89f841264c38919ec92644f256`

This establishes the cache object, header, import and merge contract only. No
compiled-code record is serialized and no restored cache hit is claimed; the
graphics-derived pipeline-cache cases remain blocked by the pinned bodies'
`D16_UNORM` depth-attachment prerequisite and are covered by host tests that
reproduce their oracles.
### Vulkan 1.0 binary semaphores and events

The public runtime now separates binary semaphore wait/signal/consumption,
host event state, recorded device event transitions, execution dependencies and
the memory barriers carried by `vkCmdWaitEvents`. Queue submissions are expanded
transactionally into ordered frontend/GPU segments; event operations never
reach the AGC backend, signals publish only on retirement, and the fence belongs
only to the final segment.

Two independent launches of the identical upstream payload passed **48/48**
original cases in the synchronization validation selection, including
`binary_semaphore.one_queue`, the 32,768-link
`binary_semaphore.chain`, all four selected event cases and the previous
42-case selection. Both reconstructed complete QPA reports, reported exit zero
and zero live platform allocations, and passed system Close Game.

- Executable SHA-256:
  `098087bc16b4363dbf622365b93695acee8697f74e9daeb776a1b297751312d4`
- Selection SHA-256:
  `c40bce192599fa734b00c8c670dccee43638d01efeb8eb042cc4c037fe2fda88`
- QPA SHA-256 values:
  `c8ac547bd58b28d20c65558bbdaae29046aef883543ca91f7d472de875c24c31`
  and `685714b9a80d6fd289ee0cfff206d7e2d04d968b63cd9f9170d9d7ca687a2fe2`

The isolated public-SDK consumer independently passed twice with executable
SHA-256
`71940c2a5d4a2d219d56eedf15569818e1be3d2c925576a432e9bec4db3831a9`.
It performs RESET→SET→RESET host event transitions, a device
set→wait→reset chain, a two-record binary signal/wait/consume submission and
the existing deterministic compute/graphics readbacks. Run IDs
`20260913T073253015Z_PPSA99994_ps5vk_0x2f437088c857` and
`20260913T073307544Z_PPSA99994_ps5vk_0x2f46d27bd3a1` passed the strict verifier
and independent Close Game checks. The 48-case receipt predates the separately
validated pipeline-cache case; the combined 49-case manifest has not yet been
run as one hardware selection. This is focused native evidence, not a full
synchronization or Vulkan conformance claim.

## Independent native SDK consumer validation

The independent native application in `examples/native_consumer/` consumes strictly
public headers (`<ps5vk/ps5vk.h>`, `<ps5vk/ps5vk_present.h>`) and links against the staged
`dist-sdk/lib/libps5vk.a` and `dist-sdk/lib/libpsbc.a`. Zero private project headers or symbols
are included or referenced (enforced by `tests/test_consumer_isolation.py`).

Observed hardware results on PS5 (FW 12.02):

- **Finite verification mode:**
  - Runtime compute pipeline compilation and execution with three simultaneously
    bound resource sets: two storage buffers, one std140 uniform buffer and one
    `VK_FORMAT_R32_UINT` uniform texel buffer.
  - Exact verification of 64 output words and 128 front/tail guard words.
  - Runtime procedural graphics pipeline compilation with 1 cold compile (9,316 bytes cache entry) and 1 warm cache hit.
  - 18 frames presented to 1080p VideoOut across 3 distinct viewports (1920x1080, 1280x720, 640x480).
  - Deterministic GPU framebuffer readbacks on all 18 frames: valid pixel coverage, alpha channel = 255, and color gradient invariants verified.
  - Clean presentation retirement and zero leaked memory allocations upon completion.
  - OS-level clean termination via system Close Game in ~100 ms.
- **Continuous rendering mode:**
  - Sustained continuous rendering executed for over 5 minutes (>17,850 consecutive frames at 60 FPS) without degradation, memory growth, or queue faults.
  - 1080p visual output confirmed via Remote Play stream.
- **Clean recovery and relaunch cycling:**
  - 3 consecutive launch, run, and system Close Game cycles completed successfully, proving prompt resource reclamation and zero driver or GPU lockups.

Raw console logs, captures, deployment details and internal planning are kept out of
the public repository. No proprietary shader or module data is required by the consumer fixture.

The expanded resource ABI was accepted in two independent launches on 2026-09-12
using the same deployed SELF (`c1433cc564eb4035aba391c89d6c4d0d03b78ffd8c2510c09a95fbd708d9b6fc`).
Run IDs `20260912T204550691Z_PPSA99994_ps5vk_0xbf48764b48e` and
`20260912T204616652Z_PPSA99994_ps5vk_0xbfa92d1a5c7` passed the strict
`verify_consumer_resource_abi.py` oracle and independent Close Game checks.
Their TCP transcript hashes were respectively
`72d545af3092b6b6d828e813cd0b3b27dd0c6b0a14543d6f2f6f473906a8407b`
and `c58300ed0cbaa4170ef7c27cf42fa890a49ff98d0084b728166f88ebb7bfecee`.
These results establish only the exact bounded resource configuration above;
they are not a Vulkan conformance claim.

## Push and specialization constants

On 2026-09-13, two independent launches of the same public-consumer SELF
(`387555789fdcfdee19b35985128211eeefb6a30c8e6298c10d58130494682741`)
passed an expanded compute oracle. The shader was compiled at runtime with two
non-default scalar specialization values (`multiplier=5`, `extra_bias=11`) and
consumed one four-byte push constant (`addend=19`). Each launch verified all 64
output words and 128 guards, completed the existing 18-frame graphics/readback
sequence, emitted a complete `ps5log/1` BYE and was then closed through the
system in about 100 ms with the title confirmed absent.

Run IDs were
`20260912T221917075Z_PPSA99994_ps5vk_0x110dd9b55e3c` and
`20260912T221948593Z_PPSA99994_ps5vk_0x11153046507d`; their TCP transcript
hashes were respectively
`43085eb90866eb55aedfaa02c03c946cc52c4f691fb79a3e35c6a5fd67e97b17`
and `ba1f956cca47ab6feca758d53ddf5c7cf8531e46b0dd13c530429e3d60ff0ed0`.
This validates the exact compute path. Runtime vertex/fragment compilation with
push and specialization metadata has host/compiler regression coverage but no
separate native draw oracle yet. `LocalSizeId` specialization remains outside
the supported profile.

## Extension-negotiated 8/16-bit storage

The public SDK consumer was expanded on 2026-09-13 without including private
driver headers or symbols. It enables the four Vulkan 1.0 extension contracts
listed in [API.md](API.md), requests only `storageBuffer8BitAccess` and
`storageBuffer16BitAccess`, and runs two storage-buffer dispatches in one
command buffer. Each launch checks 64 byte results, 64 16-bit results and 8,000
surrounding guard bytes. FNV-1a checksums were stable at `9575e8c5` and
`603ddade`. Both owned modules use SPIR-V 1.0 extension forms; the 8-bit module
declares `SPV_KHR_storage_buffer_storage_class` rather than relying on the
Vulkan 1.1 SPIR-V environment.

Two independent runs used the identical deployed SELF
`e6c267c44e757ffe2a4f4fd72a49f853273d5f2eeb8c84fe4a809012c65ffc7d`:

- `20260913T013455340Z_PPSA99994_ps5vk_0x1bbad7796637`, transcript
  `8e54ea92d9fdaac875e0bfacdf484a077eaeea4b875db748370c66b71fb8d99c`;
- `20260913T013501565Z_PPSA99994_ps5vk_0x1bbc4a86b154`, transcript
  `da3574aa6cad0a702965cf1fbe84de1c3bf3ce456fcf0b092244c19fb97f2047`.

Both passed `verify_consumer_resource_abi.py`, emitted complete `ps5log/1`
streams and were confirmed absent after Close Game. This evidence covers only
the two advertised storage-buffer feature bits. It does not establish
`shaderInt8`, `shaderInt16`, float16, uniform/push/input-output narrow storage,
general Vulkan 1.1 support or Vulkan conformance.

## Core Vulkan 1.0 command surface parity gate

To prevent regressions and enforce parity with the Khronos Vulkan 1.0 core specification,
`tools/check_command_surface.py` validates the public driver surface against the pinned
Khronos registry (`third_party/vulkan-headers/registry/vk.xml`):

- Derives the 137 mandatory Vulkan 1.0 core commands.
- Audits 1:1 symmetry across public headers (`include/ps5vk/ps5vk.h`), static dispatch
  tables (`src/vk_dispatch.c`), and implementation symbols (`src/*.c`).
- Fails closed on any unexpected drift, asymmetry (e.g. declared in public header but un-dispatched),
  or regression from the 137/137 structurally wired commands. This is symbol and
  dispatch parity, not a semantic-support or Vulkan-conformance count; explicitly
  unsupported commands remain present as tested fail-closed entry points.
- Enforced on host test runs via `make check` and verified by unit tests in
  `tests/test_command_surface.py` (which includes negative test fixtures asserting failure on
  missing dispatch entries, omitted declarations, or bookkeeping regressions).

The seven Vulkan 1.0 dynamic-state setters added after the indirect-command
slice have host evidence for public/proc-address identity, strict parameter
gates, retained per-command-buffer values, per-face stencil updates, reset and
zero operation-slot consumption. Two byte-identical native runs also passed all
32 selected original upstream monolithic compute/transfer non-interference
cases, as part of an 89/89 focused run, with exact QPA reconstruction and clean
Close Game. Pipeline creation tests independently prove that none of those
seven states can yet be enabled for drawing, so this establishes structural
recording and compute/transfer non-interference, not dynamic blending, stencil,
depth-bounds or depth-bias effects on rendered pixels.

The following structural slice added all six query commands plus
`vkCmdNextSubpass`, `vkCmdExecuteCommands` and `vkQueueBindSparse`. Host tests
prove ordered query reset, the reset-but-unavailable 32/64-bit availability
layout, preservation of result sentinels, query-pool command lifetime, function
identity, and no queue/fence mutation on rejected sparse calls. Real occlusion,
query-result copying, GPU timestamps, multi-subpass execution, secondary
command buffers and sparse binding remain fail-closed. No new CTS or hardware
claim is attached to these structural boundaries.

## Ordered buffer-transfer slice (2026-09-13)

`vkCmdCopyBuffer`, `vkCmdUpdateBuffer` and `vkCmdFillBuffer` are validated
transactionally, recorded through the owned command-operation model and
executed as ordered frontend segments. Host tests cover byte-granular copies,
record-time update ownership, repeated fills, `VK_WHOLE_SIZE`, aliasing and
overlap rejection, cache ranges, and GPU/transfer/GPU ordering.

The standalone consumer links only the staged public SDK. Two identical native
runs used SELF SHA-256
`8d44d9c9b5d70967b397a63cf1fcda4a7c776a489c5a4cf08aadd064ff9d9312`
and verified the same 67-byte result (`FNV-1a32 9a158222`) with intact guards,
complete `ps5log/1` and clean Close Game:

- `20260913T090542583Z_PPSA99994_ps5vk_0x3454301a9571`
- `20260913T090553746Z_PPSA99994_ps5vk_0x3456c96c63e4`

This closes the three named buffer commands at the bounded profile. It does
not establish the full Vulkan transfer family or conformance.

## Deferred indirect execution (2026-09-13)

The three Vulkan 1.0 indirect entry points are structurally present, and host
tests cover recording valid usage, zero-draw behavior, queue-head resolution,
exact invalidation, compute-producer ordering, resource lifetime and the
device-lost failure path. The public-SDK consumer records its existing
deterministic 64-element compute workload with `vkCmdDispatchIndirect`; its
strict verifier requires the exact recording marker and unchanged result and
guard oracles.

Two independent consumer launches used the identical SELF
`c17bb2f7c389be6bc889401936dc8e8ae86cfc96c1cc705dab8799960ee8eb2a`
and passed the hardened verifier plus independent Close Game checks:

- `20260913T101540775Z_PPSA99994_ps5vk_0x3825a44d1f8c`
- `20260913T101550407Z_PPSA99994_ps5vk_0x3827e255bc2d`

The genuine upstream payload passed 57/57 twice, including both original
indirect compute cases. This is native evidence for indirect dispatch and its
compute-write visibility dependency. It is not indirect-draw pixel evidence,
multi-draw support, or a conformance claim.

## Final Vulkan 1.0 structural command slice (2026-09-13)

The command-surface gate now derives all 137 mandatory Vulkan 1.0 core commands
from the pinned registry and reports 137/137 public, dispatched and implemented
symbols with zero asymmetries. Unsupported semantics are not counted as
supported: blit, resolve, depth/stencil clear, attachment clear, real queries,
multi-subpass execution, secondary command buffers and sparse binding retain
explicit host-tested fail-closed behavior.

The final image slice adds bounded RGBA8 transfer-role image copy and colour
clear, pitched buffer/image copies, conservative backing-alias rejection,
transactional payload validation and explicit cache maintenance. Queue tests
prove that every backend segment after a frontend transfer is prepared only
when it reaches queue head; preparation failure device-loses without launching
or signaling. `make check`, the ASan/UBSan gate and the regenerated conformance
inventory pass on commit `002743f`.

Two independent native runs used the same SELF SHA-256
`310c662777e6b4ab31a68fd8ae0ad6bb666b09471c2654bd1d909d72fb2754a8`
and selection SHA-256
`4181af6a7032b15fd27d42fcb6eb8aa178d717d3cd66e03607d85e6aecf9c272`:

- `20260913T133316202Z_PPSA99994_upstream-cts_0x42ede9683184`
- `20260913T133335878Z_PPSA99994_upstream-cts_0x42f27e4c8467`

Both passed 93/93 selected genuine upstream cases, reconstructed a complete QPA
with matching identity, and returned through verified Close Game. The four new
image leaves independently judge bounded `vkCmdCopyImage`. Their clear variant
uses the same red value as the destination initializer, so `vkCmdClearColorImage`
still has deterministic host evidence rather than an independent native pixel
oracle. The 137/137 result remains structural coverage, not Vulkan conformance.

## Mandatory core feature reporting

The Vulkan 1.0 profile now reports and accepts its mandatory
`robustBufferAccess` bit while rejecting all other unreported core feature
requests. The exact 94-case upstream payload used SELF SHA-256
`b7c485340e03fe66cbba572cdf678c7b7ac2f61643721f63d3eade411298429b`
and selection SHA-256
`5f853eb7d53226b7eda4f758aecaa70be857a80270f213d546d7bcae28015e41`.
Two independent native runs were captured:

- `20260913T181536286Z_PPSA99994_upstream-cts_0x52560710bc64`, QPA SHA-256
  `a6f94c96dc62537558c970e48d97009bd3c5fca60159bd67769e933156a18b24`
- `20260913T181557404Z_PPSA99994_upstream-cts_0x525af1f50de2`, QPA SHA-256
  `9febc48ffd1618ac5b356ab97cb4a02d14eca4aeddb8a61fb1fac97dc93dad08`

Each passed 94/94 genuine upstream cases with zero failures, unsupported cases
or skips, complete QPA reconstruction, exit code zero and verified Close Game.
That earlier case proves mandatory feature reporting and negotiation. It did
not by itself establish executable semantics; the next subsection records the
selected executable buffer coverage added afterward.

### Executable buffer robustness

The next exact payload added 12 original upstream compute scalar `R32_UINT`
robust-buffer cases, covering UBO/SSBO OOB reads and SSBO OOB writes at 1-, 3-,
4- and 32-byte descriptor ranges. The unchanged upstream tests initially found
two pre-execution defects: only one native logical device could join the AGC
session, and descriptor layouts rejected `VK_SHADER_STAGE_ALL`. Both defects
are now covered by host regressions and native CTS execution.

The corrected candidate used SELF SHA-256
`43dd8803a47028ac5086434771d668e119aa662b4483581e72bc3e7ce571a175`
and selection SHA-256
`344a278e325846f6918903e48b3262e2551178aab2caa022c67e8dd3539f5b62`:

- `20260913T183926998Z_PPSA99994_upstream-cts_0x53a3233107e6`, QPA SHA-256
  `8137f2254731850c9b70d59119875df82c12db16815a8b544fe2f1f5847161da`
- `20260913T183952333Z_PPSA99994_upstream-cts_0x53a909315ae1`, QPA SHA-256
  `046002260dfa35dd7bf3db02349368eac110a253f44ef8c7a2f587712220b4f0`

Each reconstructed a complete report and passed 106/106 with zero failures,
unsupported cases or skips, exit code zero and verified Close Game. This closes
the selected scalar compute buffer semantics only; vector, other-format and
vertex-access permutations remain unclaimed until selected and measured.

## Vulkan 1.0 device-reporting audit (2026-09-13)

The reported device surface was audited against the pinned Khronos core tables
and the pinned CTS consumer rules, with the reported values taken from the real
public query paths rather than from a copied table:

- `tools/dump_device_reporting.c` builds its platform with the same initializer
  as the console platform and prints every `VkPhysicalDeviceLimits` member, all
  1.0 feature bits, the extension feature structs, the format matrix and the
  image-format query results.
- `tools/check_reporting_matrix.py` joins that dump with
  `conformance_inventory/core_target.json`, the pinned registry header and
  `vktApiFeatureInfo.cpp`, and writes
  `conformance_inventory/reporting_matrix.json`. An undocumented below-floor
  report fails the gate; only documented blockers are accepted.

Result on the shipped profiles: 132 mandatory limits satisfied, 66 documented
blockers (real frontend restrictions, not inflated), 656 limits not applicable
to a Vulkan 1.0 `VkPhysicalDeviceLimits`, all 110 feature rows consistent with
the code path that enforces them, 98 mandatory format-feature cells satisfied
with 564 documented per-format blockers, 60 format-query consistency
checks, and twelve shader-capability rows satisfied with two precision rows
recorded as not-audited because the compiler's per-mode behaviour is not
measured.

The dump inventory is also checked against every public image and vertex format
named by the implementation. This exposed five supported sampled formats that
the hand-written dump list had omitted; adding those queries removed eight
false blockers without changing runtime capabilities. A future public format
that is absent from the dumper now fails the host gate instead of silently
appearing unsupported in the generated matrix.

Seven unset values that were below the mandatory floor were corrected to the
minimum the specification allows (`subTexelPrecisionBits`, `mipmapPrecisionBits`,
`maxVertexOutputComponents`, `maxFragmentInputComponents`,
`maxSampleMaskWords`, `pointSizeRange`, `lineWidthRange`); the shared validator
now rejects a profile that drops any of them, and `tests/test_vk_device.c`
checks both the values and the rejections.

This is host-only evidence about the report itself. It does not establish
hardware behaviour behind those limits and it does not make the profile
conformant: the documented blockers include the mandatory image-type, attachment
count, descriptor-count, multisample and format-family gaps. No console run was
performed for this increment, so no new hardware claim is made.

## Core sampler addressing, fixed borders and linear filtering (2026-09-13)

The sampler implementation now encodes core repeat, mirrored-repeat,
clamp-to-edge and clamp-to-border modes independently for U/V/W. Its native
GFX10.3 encodings are adapted under GPL-3.0-or-later from the pinned
`blackbearreloaded/ps5-opengl` revision recorded in `LICENSING.md`.

Two runs used the byte-identical SELF SHA-256
`6e33efe473cf7d150d7fe21132413496203348261d83b95159ad5a95f60b234d`:

- `20260913T195835833Z_PPSA99994_ps5vk_0x57f4cbddf1f2`, log SHA-256
  `4fed9e38cbc37dcf582af1da45ba3754a2d2d911a7829fb43a856062ddcdba25`
- `20260913T195933454Z_PPSA99994_ps5vk_0x5802365bfd81`, log SHA-256
  `333a80f2bea6e81ed18397ec2bf72686d11ff89d04f756fdf1486a0ef1e0d804`

Each ps5log/1 stream contained 1,840 ordered records and cleanly ended after
API teardown with native allocation accounting at zero. Four ordered GPU draws
and detiled readbacks each produced exactly 373,248 expected pixels and zero
other pixels: mirrored repeat at UV -0.25, then transparent black, opaque black
and opaque white with clamp-to-border at UV -2.0. The compute regression also
completed before and after every draw. Close Game was confirmed after each run,
with `PPSA99994` absent from the running-title query.

The follow-up linear-filter payload had exact SELF SHA-256
`cb59949794ab13fefb2381c01faaa8b6188d93e2e9c4b7628d9e2214fabe000c`.
Two byte-identical executions produced complete 3,668-record ps5log/1 streams:

- `20260913T202952199Z_PPSA99994_ps5vk_0x59a9aaa3c9de`, log SHA-256
  `69425e45dbca66a347d782969bad3622e5598aed58d21c9ec302833a654b69d3`
- `20260913T203035331Z_PPSA99994_ps5vk_0x59b3b575cbf6`, log SHA-256
  `48b8040c94ed2561cb7afeddd699827184d3a78bacb6c7ee39306691444d977f`

Each repeated the four addressing/border oracles and added four checkerboard
oracles. At UV 0.5, nearest magnification produced opaque black while linear
magnification produced exact 50% gray (`0xff808080`) across 373,248 pixels.
A one-pixel high-derivative witness then used opposite `magFilter`/`minFilter`
pairs: nearest minification produced opaque black and linear minification
produced the same exact gray, proving that the minification selector—not the
magnification selector—controlled the result. Every case had zero unexpected
pixels, the compute regression completed before and after each draw, both
streams ended cleanly with zero native allocation bytes, and Close Game was
verified after each run.

At that stage this established nearest and linear magnification/minification
for the single-level RGBA8 UNORM sampled-image path. That pair of runs did not
establish mip chains, anisotropy, custom border colors, mirror-clamp extension
support, filtering for the additional formats below or Vulkan conformance; the
later explicit-LOD witness above independently closes only the mip-chain item.

### GPL texture formats promoted by hardware evidence

The sampled-format table derived from the exact GPLv3 `ps5-opengl` revision
pinned in `LICENSING.md` now publicly supports twenty additional formats spanning UNORM,
SNORM, sRGB, packed/shared-exponent and 16/32-bit float texels. Texture layout and
buffer-upload planning use each format's 1/2/4/8/16-byte texel width rather
than assuming four bytes.

Two runs used byte-identical SELF SHA-256
`666e441796ae90c1cb06b3dcbacac121f246356235f1525a77c9ffef99dc7e32`:

- `20260913T212518327Z_PPSA99994_ps5vk_0x5cb014618428`, log SHA-256
  `f7b427cc393add2b802d4d85f15e12484d7fa6e97532a5b2f1ed5c900ce970ce`
- `20260913T212630352Z_PPSA99994_ps5vk_0x5cc0d96c574c`, log SHA-256
  `063be2d9a994e36a92be3823088d181d0796ff8c16fbe3672896dd31db53e2c1`

Each complete 1,029-record ps5log/1 stream performed creation, GPU upload,
layout transitions, descriptor sampling and exact detiled readback for the
three newly enabled formats. Every case produced exactly 373,248 expected
pixels and zero others. R8 yielded `(R,0,0,1)`, RG8 yielded `(R,G,0,1)`, and
the sRGB input `0x80,0x40,0x20` decoded to linear UNORM8 `0x37,0x0d,0x04`.
Thirty-six compute rounds surrounded the three graphics cases, resource
accounting returned to zero, both streams ended with BYE and Close Game stopped
the title in 100 ms. At this tranche the new formats were validated with nearest
sampling only; the later filter matrix below supersedes that boundary. This is bounded format
evidence, not general format coverage or Vulkan conformance.

A second GPL-derived tranche added `R8_SNORM`, `R8G8_SNORM`,
`R8G8B8A8_SNORM`, `E5B9G9R9_UFLOAT_PACK32`, `R16G16B16A16_SFLOAT` and
`R32G32B32A32_SFLOAT`. Two independent launches used byte-identical SELF
SHA-256 `de459dfffebcb6a03337d5054e0e6a8263500364e2252b45bb08b460b497d9e9`:

- `20260914T010455716Z_PPSA99994_ps5vk_0x68ac24913444`, log SHA-256
  `6f4e550061b1432c7b51fb1fd3eadc2e8f4c69587f153dbc086896cddc4d6836`
- `20260914T010520787Z_PPSA99994_ps5vk_0x68b1faf20fa6`, log SHA-256
  `6ccd000fec1bf0041c666bba28fbfbedd736dc45788885ae140350ffb12f0f96`

Each complete 3,063-record ps5log/1 stream executed all nine sampled-format
cases, with 373,248 exact pixels and zero others per case, pre/post compute
regressions, BYE and zero retained allocations. The shared-exponent case also
caught and corrected a Vulkan-specific semantic difference from the source
OpenGL table: a format without alpha must select constant one rather than a
nonexistent W component. Close Game stopped the exact title in 100 ms after
each run. At this tranche linear filtering remained advertised only for RGBA8
UNORM; the later filter matrix below supersedes that boundary.

A third GPL-derived tranche promoted eleven further rows from the pinned Mesa
GFX10 format table: `R16_UNORM`, `R16_SNORM`, `R16_SFLOAT`, `R16G16_UNORM`,
`R16G16_SNORM`, `R16G16_SFLOAT`, `R16G16B16A16_UNORM`,
`R16G16B16A16_SNORM`, `R32_SFLOAT`, `R32G32_SFLOAT` and
`B10G11R11_UFLOAT_PACK32`. Together with the previous nine cases this forms
the twenty-format executable probe. Two launches used byte-identical SELF
SHA-256 `db6983e64ee8a5d016641e8e5d227fbab39421699f7377debd519fd905298b9b`:

- `20260914T013155992Z_PPSA99994_ps5vk_0x6a25639cbe9a`, log SHA-256
  `7299f83bed5adde02dc0c34cfda99f803a32c32ebf0717c3f82fb0540be013aa`
- `20260914T013250320Z_PPSA99994_ps5vk_0x6a3209ca7d20`, log SHA-256
  `a0c7564384c20616b14cc6748ee3d1752a7de007914a7864dad2d79796f1a47d`

Each complete 6,792-record `ps5log/1` stream executed all twenty cases with
exact format identity and texel width, 373,248 expected pixels and zero other
pixels per case, plus pre/post compute regressions. Both runs ended with BYE,
zero retained native allocations and exact-title Close Game in 100 ms. The
new reporting removes four mandatory sampled-image blockers; normalized and
floating-point rows whose sampled-image bit is not independently mandatory do
not inflate that count. The later filter matrix below adds per-format filtering
evidence. Those per-format runs did not exercise mip selection; the later
explicit-LOD section above establishes the shared mip path with RGBA8 only.

### Per-format nearest and linear filtering

The twenty GPL-derived sampled formats were exercised with explicit opaque
black/white checkerboards under both `VK_FILTER_NEAREST` and
`VK_FILTER_LINEAR`; the previously established RGBA8 UNORM discriminator
completes the twenty-one-format public table. The probe uses a gray clear
sentinel distinct from both nearest and linear outputs, and accounts for the
four-wide R8 fixture required to keep one-byte rows DWORD aligned.

Two launches used byte-identical SELF SHA-256
`43bd3115d9896b9708a7d33f4dde6403144718d3d1ab204cb8e89c5f53246be6`:

- `20260914T020040371Z_PPSA99994_ps5vk_0x6bb6ddd97c9d`, log SHA-256
  `482eee7766d80c60963d65fe4d2d6a75aee3e8bf40554add67f97424f9bd9136`
- `20260914T020126402Z_PPSA99994_ps5vk_0x6bc195846c29`, log SHA-256
  `beb82e742fc200c48849833f6c468d0309156cc38cd276f460258835e3f85e95`

Each complete 13,572-record `ps5log/1` stream passed all forty trials with
373,248 exact triangle pixels and zero unexpected pixels per trial. Both runs
also preserved pre/post compute regressions, emitted BYE with zero retained
native allocations and stopped the exact title through Close Game in 100 ms.
The reporting matrix therefore promotes the linear-filter bit on every
validated filterable sampled-image row and removes eight mandatory format
blockers. It does not establish mip filtering, anisotropy, arrays or general
image-format coverage.

### Typed integer sampled images

Eighteen GPL-derived GFX1013 format mappings were promoted only after dedicated
Vulkan integer-shader evidence: R/RG/RGBA signed and unsigned formats at 8, 16
and 32 bits. The two payloads use distinct `isampler2D` and `usampler2D`
fragment interfaces, exact typed texels and a gray clear sentinel. Every case
produced exactly 1,036,800 expected pixels—the owned triangle covers half of a
1920x1080 target—with no other non-background pixels.

The unsigned payload had SELF SHA-256
`6b7af00e6fd80558141e83d71c69ee2bd6ec401e046daf96f4318d92f0b2e4bd`:

- `20260914T023601661Z_PPSA99994_ps5vk_0x6da4c30c934c`, log SHA-256
  `c4a8edd59f9a1e523290ed576d071aa728e7e88541826ed5d4d86c25989724d0`
- `20260914T023630716Z_PPSA99994_ps5vk_0x6dab86bfd458`, log SHA-256
  `229e8540cfbc99bb5c532d4a39aeacbfd9a9b1b75bb20b062afd9b6e1c8a18c1`

The signed payload had SELF SHA-256
`aa7951e029010a7d5b64d4a1c8c9a5374666d78ec253a3d350a346e15ea69297`:

- `20260914T023733094Z_PPSA99994_ps5vk_0x6dba0cb7278c`, log SHA-256
  `0be72bc28270f8dc293ba94255570d21cbd0e11ecd05719522ffe393f9218aff`
- `20260914T023754064Z_PPSA99994_ps5vk_0x6dbeee9493c0`, log SHA-256
  `7e8d3b743f722a64d82e982de6d7ef84475a6165379a0b7a96a05ef84e74cde8`

Each run contained 4,152 ordered `ps5log/1` records, pre/post compute
regressions for all nine cases, BYE, zero retained native allocations and an
exact-title Close Game confirmed in 100 ms. The public table consequently
advertises sampled-image and transfer-destination support for these rows plus
single-sample integer sampling. It deliberately does not advertise linear
filtering, which Vulkan does not define for integer sampled formats. This is
bounded single-level 2D evidence, not general image-format conformance.

### GPL integer and packed UNORM vertex formats promoted by hardware evidence

The vertex-format table adapted from the pinned GPLv3 `ps5-opengl` revision
now exposes the `R32`, `R32G32`, `R32G32B32` and `R32G32B32A32` signed- and
unsigned-integer rows as vertex buffers. Two runs used byte-identical SELF
SHA-256 `2d0ad4667084f3127b38ca0d0e6cf4f9fab1339b6e34f0e2af496f86aa2ffba6`:

- `20260913T225439880Z_PPSA99994_ps5vk_0x619065cfab35`, log SHA-256
  `fb9582f5b10926ce0edb18538698d4568e355805d84b43061b22de2b4506d76c`
- `20260913T225542599Z_PPSA99994_ps5vk_0x619f001c2d7a`, log SHA-256
  `e1e42287e1d169eb93f392222496dde13a609641763ebeb608f53546230b237b`

Each complete 1,534-record `ps5log/1` stream compiled the signed and unsigned
vertex shaders at runtime, created eight independent pipelines, fetched three
vertices per case and verified exactly 471,744 white pixels with zero other
pixels. The scalar, vec2 and vec3 cases additionally prove Vulkan's missing
component completion (`0,0,1`) without conflating the integer input category
with the smooth float output passed to the fragment stage. Ninety-six compute
rounds surrounded the graphics cases, resource accounting returned to zero,
both streams ended with BYE and exact-title Close Game completed in 100 ms.

The probe deliberately uses `vkCmdDraw`: the runtime shader emitter still
rejects indexed draws, while the separate offline-program path retains its
validated indexed support. This evidence therefore promotes eight vertex-format
bits, not general runtime indexed rendering or broad format conformance.

Two subsequent runs promoted the packed `R8G8B8A8_UNORM` and
`B8G8R8A8_UNORM` vertex rows using byte-identical SELF SHA-256
`f76e366d5d96d9eb5234235764216f7a18df197d9b74cdba2f28a450b9bf9029`:

- `20260913T232331033Z_PPSA99994_ps5vk_0x63237537f92a`, log SHA-256
  `cebb92f009c7b1586cebf36f0cc39108735dcec2fab4d39624001ab0cd628755`
- `20260913T232410710Z_PPSA99994_ps5vk_0x632cb2222841`, log SHA-256
  `408613181e9b90de6c45bcda3d4e645daf1367a0db48334e5396eb080b4d8419`

Each complete 3,104-record `ps5log/1` stream executed all ten vertex cases.
The packed cases used raw word `0xffaa5511`; the RGBA shader expected logical
components `(17,85,170,255)/255`, while the BGRA shader expected
`(170,85,17,255)/255`. Both produced exactly 471,744 white pixels and zero
others, proving normalized conversion and the R/B permutation independently of
the framebuffer result. Resource accounting returned to zero, both streams
ended with BYE and exact-title Close Game completed in 100 ms. This adds two
specific packed rows; it does not imply other normalized vertex formats.

The mandatory 10-bit `A2B10G10R10_UNORM_PACK32` vertex row was then added with
byte-identical SELF SHA-256
`87b30f8dd5026ce5c37a830ae5c514208d9fe5dc000f363647d99184ab38802b`:

- `20260913T234617293Z_PPSA99994_ps5vk_0x64618f4c3bad`, log SHA-256
  `85144d4c84a3745999cca8115fb80b662e64aca48a9170687d528be85b4c2dd0`
- `20260913T234715734Z_PPSA99994_ps5vk_0x646f2aae85da`, log SHA-256
  `e6fa278609b91995e55aca5f8cda91efb2eceb640e76498a87d2ccf00acfa92a`

Each complete 3,413-record stream executed all eleven vertex cases. The new
case used raw word `0xbffaa955` and checked logical RGBA values
`(341/1023,682/1023,1,2/3)`, including the distinct two-bit alpha conversion.
It again produced exactly 471,744 white pixels and zero others; both runs had
zero retained allocations, BYE and exact-title Close Game in 100 ms.

### Core 8/16-bit vertex families and unaligned bindings

The runtime compiler dependency was advanced to the reviewed PSBC merge commit
`75f4066fd98ecc0cd0c6aa394ec8e1cdb6de8a88`. It adds the exact Mesa
`PIPE_FORMAT` mappings needed by the Vulkan 1.0 `R8`/`R8G8`, packed RGBA8 and
`R16`/`R16G16`/`R16G16B16A16` vertex families. Two runs then used the
byte-identical SELF SHA-256
`16f96c2e112044d1689de23bb856b3bb303ed196a0bd829f7228d4e763405e96`:

- `20260914T003253606Z_PPSA99994_ps5vk_0x66ec9e7ca947`, log SHA-256
  `ab3f1da00c2f2db5e596625bcb756f5eb5aa899c44b50945fa8035a70591560f`
- `20260914T003415349Z_PPSA99994_ps5vk_0x66ffa6a47149`, log SHA-256
  `98f6db04d6df59354e3ea6c9e91630356ac4f81ea22fc293d5a66050826f19a3`

Each complete 12,724-record `ps5log/1` stream passed all 41 typed vertex
conversion cases. Every case produced exactly 471,744 white pixels and zero
other pixels, with exact component values supplied independently through
specialization constants. Coverage includes UNORM, SNORM, UINT, SINT and
half-float conversion; missing-component defaults; `A8B8G8R8` packed order;
1- and 2-byte strides; and a deliberately unaligned binding offset of 25.

The first diagnostic run showed that a raw GFX1013 structured SRD discards the
two low base-address bits (`ff03ffff` output instead of the expected white
triangle). The accepted implementation therefore stages an unaligned
accessible buffer span into aligned storage owned by the prepared draw. Every
accepted run recorded one bounded 359-byte bounce per case, preserved compute
regressions before and after each draw, ended with BYE and zero retained native
allocations, and returned to the PS5 menu after exact-title Close Game. This is
evidence for these vertex input combinations, not general format or Vulkan
conformance.

The earlier dynamic-buffer descriptor increment removes four of those
limit blockers across the compute and graphics profiles. It implements distinct
dynamic UBO/SSBO pool accounting, Vulkan-order bind-time offset capture,
alignment validation and native descriptor-address adjustment with checked
ranges. Queue priority reporting subsequently removed two more blockers: both
profiles report the required two discrete priority classes, and device creation
maps every valid normalized priority deterministically to low or high. The
reporting matrix now records 132 satisfied mandatory limit rows, 66 limit
blockers and 630 blockers overall.

Two byte-identical public-SDK consumer runs then exercised that path on the
owned PS5. Runs
`20260913T192239874Z_PPSA99994_ps5vk_0x55fed4aef4a8` and
`20260913T192252217Z_PPSA99994_ps5vk_0x5601b45b2478` used executable SELF
SHA-256 `2f90929ff30eb069cc66bfdb86d991b0ebaf08c07d1878be2ee522e02c95e0d7`.
Each strictly verified three descriptor sets, two dynamic storage buffers, one
dynamic uniform buffer, offsets `256,256,256`, an independently non-zero
update-time base plus dynamic offset, 64 deterministic compute results and 192
intact guard words. The existing graphics/readback tail also completed, the
`ps5log/1` transcript was complete, and Close Game was verified after both
runs. Transcript SHA-256 values are
`05296089284398c4377224943904a26e3c466808352a441fd5b8cbd9788be77a` and
`cf7f31e5815b9e03a9bebca3627fc578231130e38ce66bdf5ccd29ec0fe29f3a`.
This evidence validates that exact bounded path; it is not blanket coverage of
every descriptor array, pipeline layout or shader combination.

The core-feature reporting audit also distinguishes feature negotiation from
object validation. For every `VkPhysicalDeviceFeatures` member reported false,
`vkCreateDevice` walks the complete structure and rejects a true request before
opening the backend. `tests/test_vk_device.c` exhaustively sets each member in
turn, proving that only the advertised `robustBufferAccess` bit can enable.
This closes the previous 26 `not-audited` rows (13 features in each profile):
the matrix now has 110/110 satisfied feature-reporting rows. It does not claim
that those optional features are implemented; it proves precisely that they
are reported unavailable and cannot be negotiated accidentally.

## Layered sampled images derived from ps5-opengl (2026-09-14)

The GPL-compatible integration of the pinned `ps5-opengl` GFX1013 texture
descriptor contract now covers distinct single-level 1D, 1D-array, 2D-array,
cube and 3D resource types. The Vulkan frontend adds bounded image creation,
view ranges, multi-slice layout and buffer-upload planning; it does not link
Mesa/Gallium or expose an OpenGL API.

Three independently built RGBA8 payloads exercised the exact paths on firmware
12.02. Each used a 64x64 source with one solid color per layer, face or volume
slice, selected three different coordinates in the fragment shader, preserved
the compute regression before and after the draw, emitted BYE, released all
native allocations and returned cleanly through exact-title Close Game:

- 2D array: run `20260914T033255434Z_PPSA99994_ps5vk_0x70bf955a1933`,
  SELF SHA-256 `46c2aa6de518a7f5642c1631273d073ee6193e0174907f816649cae6dbb7591c`,
  log SHA-256 `448fbab89293fde9ef330114159895a1dfdf9f5e8720cf8963e238315296039a`;
  readback was red/green/blue `82944/207360/82944`.
- Cube: run `20260914T033409959Z_PPSA99994_ps5vk_0x70d0ef5f31b1`,
  SELF SHA-256 `c7715a4555326fcb2e8a846bae0acf06d8fd8259a44c63c09efbcb088536855c`,
  log SHA-256 `d201d846a7fce4ada5e6b9b8e9fd4ffa92103737379e94253d4cb338a12d0c6a`;
  readback was `82944/82944/207360`.
- 3D: run `20260914T033459547Z_PPSA99994_ps5vk_0x70dc7af14826`,
  SELF SHA-256 `f54b6cef2328bdc98f5b11e1d371383344baa1863be8739352f7e390cd781955`,
  log SHA-256 `767a86a15d38d6bb7bb6201c3741679b5ff4f7358d131983fdfd92725c3e4325`;
  readback was `82944/207360/82944`.

All three reported zero unexpected pixels. The reporting matrix can therefore
expose the Vulkan 1.0 floors of 256 array layers, 4096 cube dimension and 512
3D dimension without retaining the previous false “unsupported image type”
blockers. Those values are bounds of the implemented descriptor and allocation
contract; these small witnesses do not claim exhaustive execution at the
maximum dimensions, cube arrays or general descriptor arrays. The later 2D
RGBA8 mip witness is documented above and must not be generalized into layered
mipmap coverage.

Two later independent payloads validated the remaining Vulkan 1D image type
rather than inferring it from the 2D layout. Both emitted 407 ordered records,
preserved the pre/post compute regression, reported zero unexpected pixels,
released all allocations and returned through exact-title Close Game:

- 1D: run `20260914T035618318Z_PPSA99994_ps5vk_0x720636c8b4ca`, SELF
  SHA-256 `3f931e503cf55dee3d5a6efad6fb6353801cb797862f12febe52b383140a2f2f`,
  log SHA-256 `8f0782095b4c8ca7277ab88c379a43eadceb3e9efbb043480ce4965caec30aad`;
  three independently colored regions produced red/green/blue
  `82944/207360/82944`.
- 1D array: run `20260914T035708018Z_PPSA99994_ps5vk_0x7211c91f6e55`, SELF
  SHA-256 `80525965827292498f2ab0c882c5ad701c385ddfe8b34d98cd727c8cc45b301c`,
  log SHA-256 `18c8c8b6a0331dd4eb953462cba83894adb0d10fa03b4184a8225cd909716441`;
  three independently colored layers produced the same exact histogram.

The graphics profile consequently reports `maxImageDimension1D=4096` and
removes that real blocker. This is still a bounded contract, not an exhaustive
maximum-sized allocation test.
