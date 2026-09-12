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
  - Device initialization within session context (restricción del backend/harness actual; reinicialización independiente no validada).
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

On 2026-09-12, two independent native launches completed **all seven selected
upstream cases with Pass**, including native GLSL compilation and the original
compute readback oracle. Both passed strict artifact/QPA verification and
Close Game checks, with GPU completion and zero tracked GPU allocation bytes
at teardown. Artifact and report hashes, the heap fix and remaining coverage
limits are recorded in [UPSTREAM_CTS.md](UPSTREAM_CTS.md).

## Independent native SDK consumer validation

The independent native application in `examples/native_consumer/` consumes strictly
public headers (`<ps5vk/ps5vk.h>`, `<ps5vk/ps5vk_present.h>`) and links against the staged
`dist-sdk/lib/libps5vk.a` and `dist-sdk/lib/libpsbc.a`. Zero private project headers or symbols
are included or referenced (enforced by `tests/test_consumer_isolation.py`).

Observed hardware results on PS5 (FW 12.02):

- **Finite verification mode:**
  - Runtime compute pipeline compilation and execution with memory bounds protection (front/tail guard words).
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
