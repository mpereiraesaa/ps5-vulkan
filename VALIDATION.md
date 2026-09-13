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
release/writeback before the next dispatch. This evidence does not establish
range-scoped asynchronous execution, binary semaphores, events, mixed
compute/graphics barriers, queue-family transfers, the Vulkan memory model or
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
  `6c5d95aa2dd2b9e8179e...` and `39233c916cba5a72bb36...`

This establishes the cache object, header, import and merge contract only. No
compiled-code record is serialized and no restored cache hit is claimed; the
graphics-derived pipeline-cache cases remain blocked by the pinned bodies'
`D16_UNORM` depth-attachment prerequisite and are covered by host tests that
reproduce their oracles.

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
- Fails closed on any unexpected drift, asymmetry (e.g. declared in public header but un-dispatched,
  or dispatched without implementation), or regression in fully wired commands (91 fully wired
  commands, with all 46 unimplemented commands cataloged into strict categorical deficit buckets).
- Enforced on host test runs via `make check` and verified by unit tests in
  `tests/test_command_surface.py` (which includes negative test fixtures asserting failure on
  missing dispatch entries, omitted declarations, or bookkeeping regressions).
