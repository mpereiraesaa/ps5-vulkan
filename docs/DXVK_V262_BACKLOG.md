# DXVK v2.6.2 runtime backlog

The objective is to run pinned DXVK v2.6.2 D3D8, D3D9, D3D10 and D3D11 through
Prospero Win and ps5vk, render and verify pixels, present, and close and
relaunch cleanly. Work follows the next observed refusal in those paths. A
tranche number, profile score, Vulkan version
label or missing CTS leaf is **not** permission to stop implementing a needed
dependency. The 15-tranche assignment in
`conformance_inventory/dxvk_v262_backlog.json` remains an inventory of the
original 61 blockers, not a serial work schedule or the acceptance test for
DXVK execution.

## Current integration target (2026-09-29)

The ordinary instance and device now report **Vulkan 1.3.0**, as an experimental,
non-conformant implementation. Core aggregate queries, feature opt-in and
promoted command dispatch replace the payload's old core/KHR translation.
Maintenance4 is an ordinary driver route, not an SDK diagnostic override.
The acceptance workload uses pinned DXVK's original version and feature-level
checks with no driver capability override. The current clean-build result is
the [artifact-bound native presentation oracle](../VALIDATION.md#pinned-dxvk-262-native-presentation-oracle-on-the-current-sdk-2026-09-29):
three frames with 4096 verified pixels each, repeated twice on PS5. This
establishes that bounded native DXVK path, not universal game support, PE DLL
execution through Prospero Win, or Vulkan conformance.

The backbuffer path covers bounded tiled colour handovers, BGRA8 readback and
display release. Its SDK BGRA8 GPU oracle, pinned native DXVK D3D11 offscreen
oracle and three-frame public-SDK WSI control passed on the same source;
their artifact-bound receipts are in
[VALIDATION.md](../VALIDATION.md#dxvk-backbuffer-and-bgra8-runtime-path-2026-09-29).
The native DXVK presentation and pixel oracle then passed on a clean build.
DXVK's D3D11 stream-output pipeline with rasterizer discard and no fragment
stage now passes a separate two-stage native capture oracle twice, with the
ordinary three-stage capture witness still passing on the same source. Its
artifact-bound result is in
[VALIDATION.md](../VALIDATION.md#dxvk-stream-output-without-a-fragment-stage-2026-09-29).
The remaining integration tests are PE D3D8/9/10/11 backbuffer readback and
presentation on the combined current SDK, plus an x86 guest `vkMapMemory`
call with a GPU-copy oracle. These execution changes do not alter the 45/62
requirement ledger.

An independent 1920x1080 BGRA8 public-SDK witness now passed two complete
image-to-buffer GPU readbacks with exact texel and guard-byte oracles. Its
artifact-bound run is in
[VALIDATION.md](../VALIDATION.md#full-size-bgra8-gpu-readback-witness-for-dxvk-d3d89-2026-09-29).
It narrows the native D3D8/9 readback risk; PE pixel output still needs its
own Prospero Win run.
The full-size copy to a nonzero staging-buffer offset initially refused at
the native postlude: the strict whole-surface executor was selected for a
region it cannot address. The selector now routes that shape to the general
region executor. Its 4,096-byte-offset, two-frame GPU oracle passed twice with
zero pixel, prefix or guard mismatches; the exact failure and accepted runs
are in [VALIDATION.md](../VALIDATION.md#dxvk-full-size-bgra8-readback-at-a-staging-offset-2026-09-29).
This still does not establish a PE D3D8/9 readback or alter the 45/62 ledger.
The current frozen 879-case CTS selection has 49 failures on both this change
and the exact main baseline, with no fixed, broken or moved cases in the
side-by-side receipt comparison. That neutral result is recorded in the same
validation entry; it is not a clean CTS acceptance claim.

The standalone PE map control at
[`examples/dxvk_pe_vkmap_probe/main.c`](../examples/dxvk_pe_vkmap_probe/main.c)
can be built for x86 and x64 with `python3 tools/build_dxvk_pe_vkmap_probe.py`.
It loads Wine's `vulkan-1.dll` dynamically, prints each `vkMapMemory` result
and guest pointer before dereferencing it, then uses a 300 ms fence to check a
1024-byte GPU buffer copy through two mapped allocations. The offline receipt
records both PE architectures, imports and hashes with `executed=false`.
On Prospero Win, success requires two `PS5VK_PE_VKMAP_MAP` lines with result
zero and usable pointers, a `PS5VK_PE_VKMAP_RESULT` at `gpu-copy-oracle` with
zero mismatches, and corresponding native low-alias markers for x86. A build
receipt alone is not guest or GPU evidence.

The PE frontends at `examples/dxvk_pe_frontends/` have ordinary two-frame
presentation controls and separate `-pixels` variants for D3D8/9/10/11 in
both x64 and x86. Build against the pinned, unmodified DXVK PE DLLs with
`python3 tools/build_dxvk_pe_frontends.py --dll-variant unmodified`; the
builder checks each executable's PE architecture and imports and writes a
hash receipt with `executed=false`. The pixel variants read the centre pixel
through their own D3D API **before** each Present: D3D8 `CopyRects`, D3D9
`GetRenderTargetData`, and D3D10/11 staging-texture copy and map. They compare
both fixed RGB values and log the copy/map stage plus two Present results.
`DXVK_PE_D3D*_PIXEL_RESULT ... mismatches=0` together with both successful
Presents and native GPU/presentation completion is the bounded PE acceptance
target. A readback failure is a useful next driver frontier; it must not be
silently counted as a presentation failure. This oracle checks the rendered
backbuffer, not the physical display scanout, and no executable build receipt
is hardware evidence.
The D3D9 pixel variant flushes a `DXVK_PE_D3D9_PIXEL_STAGE` marker immediately
before and after each copy, lock and Present call. If a guest run terminates
before its final result line, the last stage marker and the first native
driver refusal identify which part of the readback/presentation path stopped.

## Profile ledger on the Vulkan 1.3 probe

`tools/check_dxvk_profile.py --check` reports **45/62 ready, 17 blockers**.
This is an implementation-evidence score, not a DXVK runtime result. Its API
axis is the current native probe (device API 1.3.0, 33 device extensions,
46/62 values met), cross-checked row by row against the public host query.
Before the Vulkan 1.3 integration the ledger read 41/62 on a Vulkan 1.0
probe; that probe is archived, not relabelled. Geometry, tessellation,
draw parameters and `maxBufferSize` now carry admitted native receipts.
`maintenance4` is queried true but stays blocked on its two refused shapes
(below) and on a direct witness of its memory-requirement queries.
`apiVersion` stays blocked: the pinned profile requires 1.3.204 including the
patch level; the device reports 1.3.0, which DXVK's own device filter accepts.

The shipping public routes already include the T01–T07 work: draw parameters;
multiview; indirect/indexed draws; geometry,
tessellation and clip/cull distances; raster, blend and multisample features;
and cube arrays, BC textures, extended gather and precise occlusion. T08 has
buffer device address, uniform-buffer standard layout and the base/DeviceScope
memory model through KHR routes. T11 has demote to helper invocation and
terminate invocation through EXT/KHR routes. T09 has host query reset, mirror-clamp
samplers, timeline semaphores and their limit, and separate depth/stencil
layouts and the imageless framebuffer through EXT/KHR routes; the two T08
subgroup bits remain off. The public API details and restrictions
are in [API.md](../API.md); exact hardware receipts are in
[VALIDATION.md](../VALIDATION.md). The frozen 879-case upstream selection
passed on the shipping build, but that historical regression result is not a
prerequisite for the next DXVK implementation step.

The DXVK source is pinned to
`9d6f54a1ade20d1d27dd421024717a636f3d8c68`. Earlier host bootstrap
stopped at missing `VK_KHR_surface`; that is historical, not the current
refusal. Native surface/swapchain acquisition, submission and presentation
have since passed their bounded witnesses. Both instance and device now use
the experimental Vulkan 1.3 negotiation path described above.

## Checkpoint: diagnostic render on PS5 (2026-09-25)

Built from `main` at `9c133ef` with
`tools/build_dxvk_ps5_native.py --diagnostic-integration`, the pinned DXVK
renders its FL 11_0 offscreen workload on PS5 (receipts in
[VALIDATION.md](../VALIDATION.md#dxvk-262-d3d11-diagnostic-render-on-ps5-2026-09-25)).
The unmodified variant of the same build stops at DXVK's
`Skipping Vulkan 1.0 adapter` (`src/dxvk/dxvk_device_filter.cpp:39`) after
`vkEnumeratePhysicalDevices`; that was the first refusal for that historical
artifact. The measured refusal sequence that led there was: `VK_KHR_surface`
missing → `vkCreateInstance(apiVersion 1.3)` returning
`VK_ERROR_INCOMPATIBLE_DRIVER` (fixed by the Vulkan 1.1 instance) → the 1.0
adapter filter → FL 11_0 gate (demote, then transform feedback) → required
`VK_EXT_robustness2` → `vk13.synchronization2` → image-format-list/EDS
enabling → DXVK's loose `Position` output → compile stack exhaustion on
DXVK's worker threads → barrier-only initialization submissions → oracle
pass.

**Status after the subsequent promotions and core negotiation work.** The
dated render above does not automatically certify a new combined artifact.
Use the new clean-build receipt linked above for current execution claims.
The completed negotiation work and remaining bounded contracts are:

1. **Device API version 1.3 — experimental route implemented.** DXVK's
   original version filter remains intact. The explicit experimental policy
   permits reporting 1.3 without treating the general core inventory as a
   conformance gate; unsupported capabilities must remain false and concrete
   resource limitations documented. The inventory contains lagging evidence
   rows as well as real gaps; verify each against code and native receipts.
   `maxPerSetDescriptors` ≥ 1024 is
   satisfied: sets are layout-sized and the descriptor capacity witness reads
   full 1024-descriptor sets on hardware. `maxMemoryAllocationSize`/`maxBufferSize` ≥ 2^30 are answered by
   the 1.25 GiB graphics heap (one 1 GiB allocation plus headroom).
2. **Transform feedback — promoted (T14).** The FL 10_0+ gate requires
   `transformFeedback` and `geometryStreams`; both are public through
   `VK_EXT_transform_feedback` with the geometry-stage capture path,
   counters, streams, DrawIndirectByteCount and stream queries, on the
   public-SDK capture witness. The D3D11 stream-output shape with no pixel
   shader and rasterizer discard also passed its two-stage native capture
   oracle twice; other no-fragment pipeline shapes remain outside that proof.
3. **What is still behind a default-off switch, and why.** These measured
   routes now ship with their bounded native witnesses:
   extended dynamic state (including dynamic topology and stride),
   synchronization2, format feature flags 2 and image format lists (RGBA8
   UNORM/SRGB only), storage texel buffer views, imageless framebuffer,
   robustness2, descriptor update templates, memory requirements 2, dedicated
   allocation, bind memory 2, dynamic rendering with depth/stencil resolve,
   maintenance1, copy commands 2 and the host-coherent memory type (whose
   control did not observe stale data without cache maintenance, see
   VALIDATION.md). Maintenance4 also ships through core and KHR routes;
   `PS5VK_MAINTENANCE4_DIAGNOSTIC` has been retired. The native DXVK acceptance
   recipe needs no driver diagnostic switches. The switches below are not
   prerequisites for its workload; do not generalize that workload to every
   D3D11 shader or application:
   - `PS5VK_SHADER_INT16_DIAGNOSTIC`: core `shaderInt16` allows the `Int16`
     capability in every stage. Only the compute adapter forwards the
     compiler's Int16 option, and the evidence is one original compute CTS
     leaf. Missing: the graphics adapter's Int16 option and a vertex/fragment
     Int16 witness. Applicable CTS may diagnose defects, but is not a
     prerequisite for this runtime goal.
   - `PS5VK_SHADER_INT8_DIAGNOSTIC`: a compute-only compiler probe. Public
     `shaderInt8` (Vulkan 1.2, or `VK_KHR_shader_float16_int8`) covers every
     stage. Missing: the graphics path and the KHR extension route. It is not
     enabled by the current native DXVK measurement recipe.
   - `PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC` and `PS5VK_SUBGROUP_IADD_DIAGNOSTIC`:
     compute-only broadcast and integer-add measurements (bounded typed
     witnesses and original CTS in VALIDATION.md). The public
     `supportedOperations` bits are whole sets: BALLOT needs every ballot
     operation and ARITHMETIC every operation over every supported type,
     including floats. The DXVK rows `subgroupBroadcastDynamicId` and
     `shaderSubgroupExtendedTypes` have a core reporting route now, but still
     need completion of their operation/type contracts. A version change alone
     cannot enable them.
   - `PS5VK_SAMPLE_RATE_DIAGNOSTIC`: not a route. `sampleRateShading` ships;
     the switch is the register-override instrument that measured it.
   - `PS5VK_OPTIONAL_STAGE_DIAGNOSTIC`: not a route. Geometry and tessellation
     ship; the switch only lets the optional-stage witness builds skip feature
     negotiation.
   - `PS5VK_GEOMETRY_KEY_DIAG`: pipeline-key refusal logging only.
4. **Core-named commands and the Vulkan 1.1/1.2/1.3 query structures.**
   DXVK uses core names and aggregate structures. The driver now handles
   those requests directly, including synchronization2, dynamic rendering
   and maintenance4. No payload translation is needed in the acceptance path.

The older diagnostic executable retains its historical adaptations; it must
not be relabelled as an unmodified result. New acceptance builds preserve
DXVK's version and feature-level checks. Platform/WSI build adaptations remain
identified separately from changes to rendering or feature negotiation.

## Active critical path: follow the executable

1. **Keep the clean Vulkan 1.3 runtime receipt current.** Rebuild the pinned
   DXVK workload on the combined source, with the original version/feature
   gates, no compatibility translation and no SDK diagnostic switch. Record
   the exact source and executable identity, pixel oracle and clean lifecycle.
   A successful preliminary candidate does not replace that final receipt.
2. **Implement the next actual consumer dependency.** Capture the exact
   failing Vulkan call and requested shape. Complete it even if outside the
   old tranche labels; add a fast host regression and one bounded native
   witness. Stream output without a fragment stage has a bounded native
   receipt; PE execution of that shape remains unverified. Maintenance4 still
   rejects compound LocalSizeId specialization expressions and wider
   producer/narrower consumer varying vectors. These
   are explicit profile limits, not reasons to restore the old 1.0 gate.
3. **Validate the PE path on the combined SDK.** Native DXVK now passes
   acquire/draw/present/readback/teardown with pixel and resource-accounting
   checks. Run the x86 memory-map/GPU-copy probe and the D3D8/9/10/11 PE pixel
   controls through Prospero Win against one current SDK identity, then bounded
   relaunch and closure checks. Broaden resources and shaders as those
   executables require them. Neither the old profile score nor a full
   core/CTS programme is this goal's gate.

Independent host-only work on WSI, compiler, synchronization and resource
contracts may run in parallel in separate branches/worktrees. Coordinate only
shared mutable state and short console windows. Do not wait for a whole
tranche, another Vulkan minor-version announcement or an unrelated CTS suite
before fixing an independently reproducible DXVK refusal. Integrate small
PRs serially from current `main`; prefer roughly 3–6 files when a slice can
be split honestly. Keep partial support private or default-off until its
public query and native behavior agree.

## What remains in the old profile inventory

The 17 current blockers are useful leads, not the ordered execution queue.
They are the API-version row, `maintenance4`, two T08 subgroup features and
the remaining Vulkan 1.3 feature and inline-uniform-block families. The exact
row IDs and axis states are generated in
`conformance_inventory/dxvk_v262_matrix.json`; do not copy a row's
`blocker` verdict into a claim that its implementation is absent. The T08 subgroup
diagnostics demonstrate only bounded broadcast/arithmetic cases. The shipping
profile now reports wave32 compute BASIC (Elect and subgroup barriers), not
zero stages/operations. Neither extended-types nor dynamic-broadcast feature
bit is advertised. A future DXVK request for those capabilities needs the exact
requested type, operation, stage and reporting contract, not a speculative
blanket bit flip.

The machine-readable tranche dependencies and final 1.3.204 promotion row
remain as historical profile bookkeeping. They do not gate parallel
implementation or the DXVK workload. The matrix's four-axis verdict still
requires a public route, implementation and artifact-bound native evidence,
and treats an observed applicable CTS failure as a blocker **for that row's
profile score**. We do not use that score or a CTS selection as a release
criterion for the DXVK runtime path. A failing CTS leaf that exposes a real
DXVK-used defect should be diagnosed and fixed; a missing, unmapped or unrun
leaf is not a reason to pause. Do not erase or relabel existing CTS failures.
General Vulkan conformance, whole-suite CTS and certification are outside the
current goal.

## Validation and closure for each runtime slice

* Reproduce the exact DXVK call sequence or resource shape in a fast host
  contract test. Assert both accepted and rejected cases, then run
  `make check` before the PR. A test harness or a mock alone is not native
  execution evidence.
* For GPU, presentation or lifecycle behavior, use one bounded native PS5
  run with artifact hash, structured `ps5log/1` events, deterministic
  readback/presentation evidence and confirmed cleanup. Repeat when the
  result is flaky, timing-sensitive, or the change affects ownership; do not
  run identical console probes by habit. Keep private captures out of the
  public repository.
* Record **old refusal → new behavior or next refusal** and the exact
  supported query/format/limit. A success on a diagnostic build is not a
  shipping capability. An observed crash or wrong pixel keeps that path
  open even if a host test passes. Update API/README claims only after the
  shipping route is proven, and never silently raise `apiVersion`.
* A DXVK milestone is closed by the actual pinned DXVK libraries reaching its
  named outcome on PS5, not by 62/62, a synthetic consumer, a green CTS
  selection or a cross-link receipt. Capture build identity, first/last
  Vulkan call, output, and clean shutdown in the final native receipt.

Historical T01–T09 experiments, case lists, artifact hashes and receipts
remain in [VALIDATION.md](../VALIDATION.md). Keep the live blocker above
current; add a dated checkpoint only when a measured DXVK run moves it.
