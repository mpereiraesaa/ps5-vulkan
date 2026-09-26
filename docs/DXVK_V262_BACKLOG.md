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

## Verified baseline on `main` (2026-09-25)

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
`9d6f54a1ade20d1d27dd421024717a636f3d8c68`. On a host Vulkan driver,
its real `D3D11CreateDevice` entry point created a feature-level 11_0 device
and context. Against the ps5vk host loader, the same DXVK libraries stopped
at **`Required Vulkan extension VK_KHR_surface not supported`**: no DXVK
device or context was created. That run did not reach DXVK's later version or
feature checks. PS5 cross-compilation and linking of the D3D11/DXGI libraries
also succeeded. The PS5 display adapter has a host-tested call sequence for a
fixed 1080p60 monitor using `VK_KHR_display` and
`vkCreateDisplayPlaneSurfaceKHR`; it is **not** a Vulkan WSI implementation in
ps5vk. That was the state before the checkpoint below. Reproduce these
boundaries with `tools/run_dxvk_host_smoke.py`,
`tools/run_dxvk_ps5vk_host_bootstrap.py` and
`tools/build_dxvk_ps5_cross_probe.py`, retaining their source and artifact
identities. Do not call the cross-link result `runtime_ready`.

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

**What the diagnostic configuration still supplies, i.e. the contracts missing
for an unmodified, truthful route:**

1. **Device API version 1.3.** DXVK filters every device below 1.3.
   `tools/check_core_version_contract.py` blocks raising the reported
   version while any mandatory 1.1/1.2/1.3 item is missing; run it with
   `--assume-version 1.N` to list them. `maxPerSetDescriptors` ≥ 1024 is
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
3. **Measured routes still behind a default-off switch:** extended dynamic
   state (dynamic topology and vertex stride are still refused) and
   maintenance4, which cannot be exposed on a 1.0 device. Every other
   measured route ships with a passing native witness: synchronization2,
   format feature flags 2 and image format lists (RGBA8 UNORM/SRGB only),
   storage texel buffer views, imageless framebuffer, robustness2,
   descriptor update templates, memory requirements 2, dedicated allocation,
   bind memory 2, dynamic rendering with depth/stencil resolve, maintenance1,
   copy commands 2 and the host-coherent memory type (whose control did not
   observe stale data without cache maintenance, see VALIDATION.md).
4. **Core-named commands and the Vulkan 1.1/1.2/1.3 query structures.**
   DXVK uses only core names and the aggregate structures; the diagnostic
   payload translates them onto the extension routes. The driver-side
   version answers become active only when the device version is raised.

Items 1 and 2 need no DXVK change once implemented; item 3 is a per-route
promotion through the capability probe. The diagnostic build is evidence that
the executed Vulkan paths work, not a claim of unmodified DXVK support.

## Active critical path: follow the executable

1. **Instance and presentation bootstrap.** Implement the ps5vk side of the
   surface/display/swapchain route actually consumed by the existing DXVK PS5
   adapter. Start with DXVK's exact instance-extension query and creation
   sequence, then display mode/plane, surface capabilities and formats,
   swapchain image acquisition, present and teardown. Match advertised
   capabilities to implemented operations; fail closed for unsupported
   present modes, image usages and surface combinations. A host mock is useful
   for the call contract, but a native present/readback and clean Close Game
   prove the path. The first newly observed DXVK refusal replaces the current
   `VK_KHR_surface` refusal in this backlog.
2. **Device bootstrap and version boundary.** The pinned DXVK source requests
   Vulkan 1.3 at instance creation and filters physical devices reporting
   less than 1.3. ps5vk currently accepts only a 1.0 instance request and
   reports a 1.0 device. Inspect the exact calls and required feature/extension
   checks after WSI progresses; implement the contracts DXVK actually uses.
   An instrumented local DXVK build may bypass a version filter to discover
   later refusals, but it must be labeled diagnostic. Never make ps5vk report
   1.3 merely to pass the filter, or claim an unmodified DXVK 2.6.2 run from
   such a build. A truthful public version change needs the corresponding
   mandatory API contracts, independently of whether we run full CTS or seek
   formal conformance.
3. **First D3D11 workload.** Advance from `D3D11CreateDevice` to a real device
   and context, texture/render-target creation, shaders, draw/dispatch,
   synchronization, presentation and deterministic readback. Capture the first
   failing Vulkan call and its requested shape. Implement that dependency even
   when it falls outside the old T08–T15 grouping; repeat until the workload
   renders. Keep feature queries, accepted device-create chains and actual
   execution aligned. The fragment-discard defect (kill did not suppress
   depth/stencil writes) is fixed: a removing pixel program that exports
   nothing now gets MRT0 export memory, measured by the public-SDK
   pixel-removal witness (see
   [VALIDATION.md](../VALIDATION.md#t11-demote-and-terminate-public-extension-promotion-2026-09-25)).
4. **Broaden real use, then stabilize.** Exercise the D3D11 resources and
   shader forms selected by the running workload, especially the remaining
   subgroup operations/types, dynamic rendering, synchronization2, descriptor
   and robustness paths, and transform feedback where DXVK actually requests
   them. A listed profile feature is a candidate, not automatic proof that
   this workload needs it. Conversely, a missing format, limit or WSI call
   outside the 62-row profile must enter the active backlog as soon as DXVK
   hits it. Finish with repeated launch/render/present/readback/Close Game
   cycles and bounded resource accounting.

Independent host-only work on WSI, compiler, synchronization and resource
contracts may run in parallel in separate branches/worktrees. Coordinate only
shared mutable state and short console windows. Do not wait for a whole
tranche, another Vulkan minor-version announcement or an unrelated CTS suite
before fixing an independently reproducible DXVK refusal. Integrate small
PRs serially from current `main`; prefer roughly 3–6 files when a slice can
be split honestly. Keep partial support private or default-off until its
public query and native behavior agree.

## What remains in the old profile inventory

The 27 current matrix blockers are useful leads, not the ordered execution
queue. Besides the two T04 receipt-reconciliation rows, they comprise the
Vulkan 1.1 aggregate draw-parameters query, two T08 subgroup features,
the remaining T10–T13 sync/render/shader/descriptor/
robustness families and the API-version row. The exact
row IDs and axis states are generated in
`conformance_inventory/dxvk_v262_matrix.json`; do not copy a row's old
`blocker` verdict into a claim that its implementation is absent. For example,
the Vulkan 1.1 aggregate `shaderDrawParameters` field is blocked while the
equivalent Vulkan 1.0 KHR route already works. Similarly, the T08 subgroup
diagnostics demonstrate only bounded 32-bit and Int8 cases: public subgroup
properties still report zero stages/operations, and neither feature bit is
advertised. A future DXVK request for those capabilities needs the exact
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
