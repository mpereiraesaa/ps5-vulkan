# DXVK v2.6.2 implementation backlog

This backlog turns the 61 original blockers for
`VP_DXVK_d3d11_level_11_0_baseline` into 15 ordered implementation tranches.
The exact baseline membership lives in
`conformance_inventory/dxvk_v262_backlog.json`; this document explains how to
execute it.

Current checkpoint: multiview (T02) has all three requirements satisfied by
public KHR queries, dedicated floor witnesses and 48 passing original CTS
leaves within a 165/165 regression; the indirect and indexed draws (T03)
have all three requirements satisfied by core feature reports, the
per-command multi-draw expansion, 46 passing original indirect/draw-index CTS
leaves within a 211/211 regression and the public-SDK indirect witness; the
the four DXVK262-T05 rasterization and viewport features were promoted on
2026-09-21; and DXVK262-T06 is complete, with all four of its requirements
satisfied: `fragmentStoresAndAtomics` and `dualSrcBlend` with their four axes,
`independentBlend` promoted on 2026-09-22, and `sampleRateShading` promoted on
2026-09-23 - the pixel stage publishes Vulkan's standard sample positions and
interpolates the position at the iterated sample, the colour-to-texture barrier
waits for a confirmed writeback, and the feature's own oracle, thirty leaves at
both served counts, passes inside the frozen acceptance selection, which now
carries them and passes 494/494. **T04 is implemented, hardware-validated and
merged into `main`** (PR #158): the default graphics profile exposes geometry,
tessellation and clip/cull distances, and its integrated native run passed
403/403 focused upstream cases, including 99 tessellation-related cases. The
live matrix is 28/62 ready with 34 blockers after T07's four resource and
precise-query features passed the ordinary 829-case selection twice, T08
DeviceScope passed its ordinary SDK KHR witness and public query, and T09
promoted `timelineSemaphore`, `maxTimelineSemaphoreValueDifference` and
`separateDepthStencilLayouts` through their Vulkan 1.0 KHR routes. The ordered table preserves the
original tranche membership.

Tranche delivery and DXVK profile scoring are different gates. The current
matrix still leaves `geometryShader` and `tessellationShader` as blockers:
their native witness receipts have not been admitted to the DXVK evidence
ledger, so their native axis is still `reported-not-executed`. This is
**evidence reconciliation**, not unfinished T04 rendering work. Admit the exact
receipts (and, optionally, the 99 passing tessellation cases as a
`cts-focused-pass` record), then regenerate the matrix and rerun its gates
before changing its count. T07 may proceed using the merged
T04 implementation; it need not wait for that accounting change. The current
score is derived from `conformance_inventory/dxvk_v262_matrix.json`.

The target is deliberately narrow: the pinned DXVK v2.6.2 D3D11 feature-level
11_0 baseline, and the goal is running our DXVK v2.6.2 build on PS5. Reaching
62/62 means that this profile has complete API, implementation and native
evidence with no observed applicable CTS failure. It is **not** Vulkan 1.3 conformance: whole-suite CTS
and the official conformance process are separate goals that no tranche waits
for and this project does not claim.

## Ordered tranches

| Order | Tranche | Requirements | Purpose |
| ---: | --- | ---: | --- |
| 1 | T01 — Vulkan 1.1 draw-parameter foundation | 1 | BaseVertex, BaseInstance and DrawIndex shader semantics. |
| 2 | T02 — Vulkan 1.1 multiview | 3 | View masks, ViewIndex and measured multiview limits. |
| 3 | T03 — Indirect and indexed draws | 3 | First-instance, multi-draw and full 32-bit indices. |
| 4 | T04 — Programmable graphics stages | 4 | Geometry, tessellation and clip/cull distances. |
| 5 | T05 — Rasterization and viewport state | 4 | Clamp, non-solid fill and multiple viewports. |
| 6 | T06 — Fragment output and multisampling | 4 | Blend, fragment stores/atomics and sample-rate behavior. |
| 7 | T07 — Resources and precise queries | 4 | Cube arrays, BC, gather and precise occlusion. |
| 8 | T08 — Vulkan 1.2 shader/memory model | 6 | Addresses, layouts, subgroups and memory semantics. |
| 9 | T09 — Vulkan 1.2 lifecycle/render primitives | 6 | Query reset, framebuffer/layout, sampler and timeline behavior. |
| 10 | T10 — Vulkan 1.3 sync/dynamic rendering | 5 | synchronization2, dynamic rendering, maintenance4 and cache control. |
| 11 | T11 — Vulkan 1.3 shader/subgroup behavior | 7 | Required compiler and subgroup semantics. |
| 12 | T12 — Inline uniform blocks | 7 | Descriptor implementation plus six measured limits. |
| 13 | T13 — VK_EXT_robustness2 | 3 | Null descriptors and robust resource access. |
| 14 | T14 — VK_EXT_transform_feedback | 3 | Transform-feedback capture, counters and streams. |
| 15 | T15 — API 1.3.204 promotion gate | 1 | Final advertisement only after all earlier work, the mandatory 1.0–1.3 core surface and a native run of our DXVK build. |

### T07 combined diagnostic status before public reporting (2026-09-24)

The BC diagnostic route passed three focused original selections: 48/48
compressed-texture sampling, 128/128 compatible blits and 74/74 copies,
including mip and array-layer readback. Separate SDK witnesses checked
interior multi-layer copies and sampled output. A 757-case selection combining
these 250 BC leaves with the 507 frozen cases is prepared but has not run.
The cube-array image-view CTS case passed twice; a SDK witness sampled two
cubes, all six faces and a
nonzero view base twice each. The original precise-occlusion CTS case passed
twice and in a 508/508 combined selection, and a native API witness checked
zero, one and three covered samples with result retrieval and reset. The D32
comparison-gather leaf passed twice. Exact receipts and artifact hashes are
recorded in [VALIDATION.md](../VALIDATION.md).

These are bounded diagnostic results. All four T07 feature bits remain off in
the shipping profile. The cube-compatible sampled colour-attachment role has
a host-tested tiled descriptor and a packaged render-to-sample witness, but
that witness has not run on hardware. A 577-case measurement selection now
includes all 70 applicable original extended-gather leaves alongside the 507
frozen cases, but it has not run. The complete public feature, frozen CTS and
combined acceptance audit remain pending.

### T07 public reporting candidate (2026-09-24)

The ordinary build in the T07 candidate now reports `imageCubeArray`,
`textureCompressionBC`, `shaderImageGatherExtended` and
`occlusionQueryPrecise` without diagnostic switches. Its public SDK capability
probe passed with eboot SHA-256
`c3857792f29221da1e0452c5971ad29c084cd4cf89ed6ec6974104c9699ed3ae`
in run `20260924T165734913Z_PPSA99994_ps5vk_0x24a81b7608ca2`.
That probe observed 26/62 requested values; it checks reporting, not execution.

The frozen selection now includes 829 original upstream cases. The ordinary
CTS package has eboot SHA-256
`95befcf38c164d38dec0748a9aa88608329fbc5e7681cc36ef70edff4f0f13f1`
and case-list SHA-256
`81f656f1b0559f212bcc7b802c572d59c23919272f9109aa37f8774e78b849c4`.
Its strict PS5 acceptance remains pending. The four T07 rows therefore remain
blocked on the CTS axis, and the checked DXVK matrix stays at 20/62 ready with
42 blockers. [VALIDATION.md](../VALIDATION.md#t07-public-reporting-candidate-2026-09-24)
separates the candidate from the earlier diagnostic runs.

### T07 public CTS promotion (2026-09-24)

On owner-reported firmware 12.02, the ordinary package above completed two
strict 829/829 Pass runs, `run-3158572987094` and `run-3231483363368`, with
zero Fail, NotSupported, missing, unexpected or duplicate cases. The four
T07 groups each passed twice: cube arrays 1/1, BC 250/250, extended gather
70/70 and precise occlusion 1/1. Both reports verified the exact eboot and
selection hashes, complete QPA, clean log transport and normal title closure.
The four rows now satisfy API, implementation, original CTS and native-evidence
axes; after the separate T08 DeviceScope KHR witness and public query, the
checked matrix is **25/62 ready with 37 blockers**. DeviceScope has no
applicable original message-passing CTS leaf under API 1.0. The earlier
candidate paragraph records its status before these runs. Full receipt and
artifact hashes are in [VALIDATION.md](../VALIDATION.md#t07-public-upstream-cts-promotion-2026-09-24).

For T08's extended subgroup Int16 operands, the pinned original Vulkan 1.0
compute indexing factory is now packaged as an independent `shaderInt16`
prerequisite. Its `opaccesschain_u16` leaf reaches the original CTS support
check, which reports `NotSupported` because the shipping core bit is false.
The one-leaf diagnostic receipt and artifact identity are in `VALIDATION.md`.
The current frozen acceptance selection remains 829 cases; the Int16 leaf is
only in a separate diagnostic selection. A build combining that factory with
the default-off Int16 diagnostic route passed the original 128-value GPU
oracle twice after forwarding `shaderInt16` to PSBC. Exact artifact and QPA
identities are in `VALIDATION.md`. Public `shaderInt16`, subgroup features and
`apiVersion` stay off; the broader Int16 operand matrix, subgroup properties,
original subgroup CTS and combined profile still need evidence.

The table totals the 61 blockers observed at backlog creation: one API-version
requirement, two extensions, 48 features and ten properties. Their membership
is stable while readiness and completion are derived from the live matrix, so implemented
requirements remain auditable in their original tranche. The checker fails if
a baseline requirement is missing, duplicated or unknown, or if the initially
satisfied `robustBufferAccess` regresses. It also enforces dependency order and
the final API promotion gate.

### T09 timeline semaphores and separate depth/stencil layouts (2026-09-25)

Three T09 rows are now ready on all four axes, through Vulkan 1.0 KHR
extensions. `apiVersion` stays 1.0.

* `timelineSemaphore` and `maxTimelineSemaphoreValueDifference` use
  `VK_KHR_timeline_semaphore`. The reported difference is `UINT64_MAX`, because
  payloads are compared at full width.
* `separateDepthStencilLayouts` uses `VK_KHR_separate_depth_stencil_layouts`
  over `VK_KHR_create_renderpass2`, `VK_KHR_maintenance2` and
  `VK_KHR_multiview`.

The public `D32_SFLOAT_S8_UINT` format carries only its witnessed attachment
and readback roles. Evidence on the promoted shipping build:

* two strict public-SDK witness runs per capability;
* 50/50 focused original CTS leaves: timeline, renderpass2, and D32S8
  stencil/depth with and without separate layouts;
* 829/829 frozen acceptance;
* a re-measured capability probe (30/62 query values, 13 extensions).

Run IDs and hashes are in
[VALIDATION.md](../VALIDATION.md#t09-timeline-semaphores-and-separate-depthstencil-layouts-promotion-2026-09-25).
DXVK 2.6.2 renders through dynamic rendering, not `vkCreateRenderPass*`. The
render pass 2 route exists because the registry requires it for the separate
layouts. It is not a claim that dynamic rendering is available.

The following are still missing for D32S8:

* sampled or single-aspect views;
* `vkCmdClearDepthStencilImage`;
* transfer-destination uploads;
* mip levels and array layers;
* HTILE compression.

`D24_UNORM_S8_UINT` is not reported.

**Open blocker: fragment discard does not suppress depth/stencil writes.** On
gfx1013, through this driver's path, a fragment shader that executes
`discard`/kill still lets the DB write depth and stencil. The compiled shader
enables kill (`DB_SHADER_CONTROL=0x50`), yet the T09 depth/stencil witness saw
all 4096 texels written. The root cause is open. This breaks alpha-tested
depth in DXVK. It needs an owner and a native witness in a later tranche. It
does not change the three T09 rows above, which do not depend on discard.

### T08 subgroup profile prerequisites

The pinned registry and original CTS impose different routes for the two T08
subgroup bits. The actionable format and stage matrix is in
`conformance_inventory/subgroup_profile_contract.json`; its host checker keeps
the current public reporting closed while these prerequisites are incomplete.

| Requirement | Legal route and original CTS gate | Current state |
| --- | --- | --- |
| `shaderSubgroupExtendedTypes` | `VK_KHR_shader_subgroup_extended_types` requires Vulkan 1.1; the feature is core in 1.2. The original CTS also requires `VK_KHR_shader_float16_int8`, which independently depends on Vulkan 1.1 at this registry pin. Broadcast and arithmetic cases need subgroup `supportedStages` for the tested stage and `supportedOperations` with `BALLOT` and `ARITHMETIC` respectively. The 8/16/64-bit integer and 16-bit float formats need their respective shader features, with storage access for 8/16-bit CTS buffers. | Public bit off; API 1.0; no accepted subgroup CTS or complete native witness. |
| `subgroupBroadcastDynamicId` | Only `VkPhysicalDeviceVulkan12Features` at this registry pin; the original nonconstant broadcast cases require Vulkan 1.2, SPIR-V 1.5, the feature bit, a supported stage and `BALLOT`. | Public bit off; API 1.0; no extension alias or accepted CTS route. |

`VkPhysicalDeviceSubgroupProperties` supplies `subgroupSize`, `supportedStages`,
`supportedOperations` and `quadOperationsInAllStages` through properties2.
The current Vulkan 1.0 KHR query explicitly returns zero for all four fields.
Compute subgroup support is required by the original CTS;
other stages depend on their reported masks. The quad field does not establish
broadcast support. Neither subgroup bit nor `apiVersion` changes in this slice.

## Readiness versus profile completion

For Vulkan 1.1–1.3 rows, implementation can be ready before the device is
allowed to advertise the target API version. The backlog therefore reports two
different facts:

* **implementation ready** means real implementation and native evidence are
  green and no applicable CTS leaf was observed failing; it does not assert
  that the capability is advertised;
* **profile satisfied** additionally requires the public API axis and final
  fail-closed verdict to be green.

This distinction lets the implementation counter advance without weakening the
final Vulkan 1.3.204 gate or creating a circular dependency.

## Definition of done for a requirement

A row is counted complete only when all three gating axes in the live matrix
are green and its CTS record shows no observed failure:

1. the public query reports the exact supported value and device creation
   accepts it;
2. the implementation is real and fail-closed for unsupported combinations,
   with host tests for its contract;
3. an artifact-bound native PS5 run proves execution, output and clean
   lifecycle through `ps5log/1`; `native-evidence` must name its run ids,
   artifact SHA-256 and references.

Upstream CTS is regression evidence, not a readiness gate. The matrix records
it per capability as `cts-pass` (original leaves in the frozen regression
selection), `cts-focused-pass` (original leaves outside that selection passing
in a focused run whose run ids, artifact SHA-256 and case-list SHA-256 are
recorded, with every named case `Pass`), `mapped-not-run` or `not-mapped`.
A missing, unmapped or unrun leaf does not block an implemented,
native-witnessed capability. An observed applicable failure, `cts-fail`, stays
in the ledger and blocks the row until it is explained or fixed. NotSupported,
Skip and Fail never count as a pass, and no historical report is rewritten.
Use focused CTS where it validates a concrete contract or helps debugging;
whole-suite CTS and general Vulkan conformance are not gates for a row or for
the final API promotion, which instead requires our DXVK v2.6.2 build to create
its device and run natively.

Properties require measured, defensible values rather than copied profile
floors. Extensions are enumerated only after every extension capability used by
the profile meets the same gate. Host tests and mocked packet checks never
promote a row alone.

## Delivery discipline

Use a fresh branch from current `main` for each PR and wait for merge before
starting dependent work; stacked PRs make review and evidence attribution
ambiguous. Prefer 3–6 files per PR. A large tranche may therefore need several
sequential PRs: contract/tests first, implementation next, and CTS/native
promotion last. Intermediate PRs must keep reporting false and reject device
requests until the complete behavior is proven.

Run `make check` before publication. Console work additionally requires the
shared `console:PS5` and `title:PPSA99994` leases, exact artifact identity,
structured TCP telemetry and a confirmed clean Close Game/self-exit. No PR may
raise the reported Vulkan API version merely to let a consumer proceed.
