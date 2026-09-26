# DXVK v2.6.2 runtime backlog

The objective is to build and run our pinned DXVK v2.6.2 D3D11/DXGI stack on
PS5 through ps5vk, create a feature-level 11_0 device, render and read back a
representative workload, and close and relaunch cleanly. Work follows the next
observed refusal in that path. A tranche number, profile score, Vulkan version
label or missing CTS leaf is **not** permission to stop implementing a needed
dependency. The 15-tranche assignment in
`conformance_inventory/dxvk_v262_backlog.json` remains an inventory of the
original 61 blockers, not a serial work schedule or the acceptance test for
DXVK execution.

## Verified baseline on `main` (2026-09-26)

After the synchronization2, T14 transform feedback, imageless framebuffer,
robustness2 and dynamic rendering promotions, `tools/check_dxvk_profile.py
--check` reports **41/62 ready, 21 blockers**. This is an implementation-evidence
score, not a DXVK runtime result. `tools/check_dxvk_backlog.py --check` reports
40 implementation-ready original blockers and 40 profile-satisfied ones (the 62-row score also
includes the initially satisfied `robustBufferAccess`). The public device
still reports Vulkan **1.0.0**. The matrix counts geometry and tessellation as
blockers because their completed T04 native receipts have not been admitted
to that ledger; this does **not** mean their rendering implementation is
unfinished. Do not infer a missing DXVK dependency solely from those two rows.

The shipping public routes already include the T01–T07 work: draw parameters
via the Vulkan 1.0 KHR route; multiview; indirect/indexed draws; geometry,
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
have since passed their bounded witnesses. The instance accepts DXVK's 1.3
request; the public physical device still reports 1.0.

## Checkpoint: diagnostic render on PS5 (2026-09-25)

Built from `main` at `9c133ef` with
`tools/build_dxvk_ps5_native.py --diagnostic-integration`, the pinned DXVK
renders its FL 11_0 offscreen workload on PS5 (receipts in
[VALIDATION.md](../VALIDATION.md#dxvk-262-d3d11-diagnostic-render-on-ps5-2026-09-25)).
The unmodified variant of the same build stops at DXVK's
`Skipping Vulkan 1.0 adapter` (`src/dxvk/dxvk_device_filter.cpp:39`) after
`vkEnumeratePhysicalDevices`; that is the current first refusal on the truthful
route. The measured refusal sequence that led here was: `VK_KHR_surface`
missing → `vkCreateInstance(apiVersion 1.3)` returning
`VK_ERROR_INCOMPATIBLE_DRIVER` (fixed by the Vulkan 1.1 instance) → the 1.0
adapter filter → FL 11_0 gate (demote, then transform feedback) → required
`VK_EXT_robustness2` → `vk13.synchronization2` → image-format-list/EDS
enabling → DXVK's loose `Position` output → compile stack exhaustion on
DXVK's worker threads → barrier-only initialization submissions → oracle
pass.

**Current status after the subsequent promotions.** The dated render above
does not automatically certify a new combined artifact. Rebuild and measure
the pinned workload from current `main` before claiming a new end-to-end
result. The outstanding boundary and completed dependencies are:

1. **Device API version 1.3.** DXVK filters every device below 1.3.
   `tools/check_core_version_contract.py` blocks raising the reported
   version while any mandatory 1.1/1.2/1.3 item is missing; run it with
   `--assume-version 1.N` to inspect its inventory. That inventory contains
   lagging evidence rows as well as real gaps; verify each against the code
   and native receipts rather than treating its count as an implementation
   queue or requiring general CTS. `maxPerSetDescriptors` ≥ 1024 is
   satisfied: sets are layout-sized and the descriptor capacity witness reads
   full 1024-descriptor sets on hardware. `maxMemoryAllocationSize`/`maxBufferSize` ≥ 2^30 are answered by
   the 1.25 GiB graphics heap (one 1 GiB allocation plus headroom).
2. **Transform feedback — promoted (T14).** The FL 10_0+ gate requires
   `transformFeedback` and `geometryStreams`; both are public through
   `VK_EXT_transform_feedback` with the geometry-stage capture path,
   counters, streams, DrawIndirectByteCount and stream queries, on the
   public-SDK capture witness. A D3D11 stream-output shader with no pixel
   shader bound still needs a pipeline without a fragment stage, which the
   frontend refuses.
3. **What is still behind a default-off switch, and why.** These measured
   routes now ship with their bounded native witnesses:
   extended dynamic state (including dynamic topology and stride),
   synchronization2, format feature flags 2 and image format lists (RGBA8
   UNORM/SRGB only), storage texel buffer views, imageless framebuffer,
   robustness2, descriptor update templates, memory requirements 2, dedicated
   allocation, bind memory 2, dynamic rendering with depth/stencil resolve,
   maintenance1, copy commands 2 and the host-coherent memory type (whose
   control did not observe stale data without cache maintenance, see
   VALIDATION.md). The switches left are either not routes or cannot be
   reported truthfully yet. The current native DXVK builder still enables
   `PS5VK_MAINTENANCE4_DIAGNOSTIC`; the other switches below are not enabled
   by that recipe. Do not generalize this bounded workload to every D3D11
   shader or application:
   - `PS5VK_MAINTENANCE4_DIAGNOSTIC`: `VK_KHR_maintenance4` requires a
     Vulkan 1.1 device and has no Vulkan 1.0 route. Public exposure still
     depends on the device-version boundary (item 1), not a missing CTS run.
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
     `shaderSubgroupExtendedTypes` are Vulkan 1.2 features and wait for the
     device version for their core reporting route as well as completion of
     their operation/type contracts. A version change alone cannot enable them.
   - `PS5VK_SAMPLE_RATE_DIAGNOSTIC`: not a route. `sampleRateShading` ships;
     the switch is the register-override instrument that measured it.
   - `PS5VK_OPTIONAL_STAGE_DIAGNOSTIC`: not a route. Geometry and tessellation
     ship; the switch only lets the optional-stage witness builds skip feature
     negotiation.
   - `PS5VK_GEOMETRY_KEY_DIAG`: pipeline-key refusal logging only.
4. **Core-named commands and the Vulkan 1.1/1.2/1.3 query structures.**
   DXVK uses only core names and the aggregate structures; the diagnostic
   payload translates them onto the extension routes. The driver-side
   version answers become active only when the device version is raised.

The diagnostic build is evidence that its executed Vulkan paths work, not a
claim of unmodified DXVK support. Transform feedback's promotion means its
old feature-level bypass must be re-evaluated, not treated as a missing
implementation or assumed removed from the existing diagnostic executable.

## Active critical path: follow the executable

1. **Refresh the end-to-end measurement on the combined shipping routes.**
   Rebuild the pinned DXVK workload from current `main`, recording every
   remaining patch, translation and diagnostic switch. Re-test the feature
   gate without the old transform-feedback bypass now that both required
   bits ship. Retain the version-filter bypass only as an explicitly
   diagnostic consumer configuration, not a reason to pause implementation.
2. **Implement the next actual consumer dependency.** Capture the exact
   failing Vulkan call and requested shape. Complete it even if outside the
   old tranche labels; add a fast host regression and one bounded native
   witness. A known outstanding shape is stream output without a fragment
   stage. Keep core-name/query translation and maintenance4's version
   boundary explicit; do not silently raise `apiVersion` to pass the filter.
   An unmodified-DXVK claim still requires removing those adaptations.
3. **Connect the rendered workload to presentation.** The offscreen D3D11
   pixel oracle and the native swapchain witnesses are separate results.
   Combine them into acquire/draw/present/readback/teardown with the actual
   DXVK libraries, then bounded relaunch and resource-accounting checks.
   Broaden resources and shaders as the executable requires them. Neither
   the old profile score nor a full core/CTS programme is this goal's gate.

Independent host-only work on WSI, compiler, synchronization and resource
contracts may run in parallel in separate branches/worktrees. Coordinate only
shared mutable state and short console windows. Do not wait for a whole
tranche, another Vulkan minor-version announcement or an unrelated CTS suite
before fixing an independently reproducible DXVK refusal. Integrate small
PRs serially from current `main`; prefer roughly 3–6 files when a slice can
be split honestly. Keep partial support private or default-off until its
public query and native behavior agree.

## What remains in the old profile inventory

The 21 current matrix blockers are useful leads, not the ordered execution
queue. Besides the two T04 receipt-reconciliation rows, they comprise the
Vulkan 1.1 aggregate draw-parameters query, two T08 subgroup features,
the remaining T10–T13 sync/render/shader/descriptor/
robustness families and the API-version row. The exact
row IDs and axis states are generated in
`conformance_inventory/dxvk_v262_matrix.json`; do not copy a row's old
`blocker` verdict into a claim that its implementation is absent. For example,
the Vulkan 1.1 aggregate `shaderDrawParameters` field is blocked while the
equivalent Vulkan 1.0 KHR route already works. Similarly, the T08 subgroup
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
