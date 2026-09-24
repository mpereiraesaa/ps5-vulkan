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
live matrix is 20/62 ready with 42 blockers. The ordered table preserves the
original tranche membership.

Tranche delivery and DXVK profile scoring are different gates. The current
matrix still leaves `geometryShader` and `tessellationShader` as blockers:
their native witness receipts have not been admitted to the DXVK evidence
ledger, and the 99 passing tessellation cases are outside its frozen CTS
selection. This is **evidence reconciliation**, not unfinished T04 rendering
work. Promote the exact cases and receipts, then regenerate the matrix and
rerun its gates before changing its count. T07 may proceed using the merged
T04 implementation; it need not wait for that accounting change. The current
score is derived from `conformance_inventory/dxvk_v262_matrix.json`.

The target is deliberately narrow: the pinned DXVK v2.6.2 D3D11 feature-level
11_0 baseline. Reaching 62/62 means that this profile has complete API,
implementation, applicable CTS and native evidence. It does **not** by itself
constitute Vulkan 1.3 conformance: the wider cumulative core contract and the
official conformance process remain separate obligations.

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
| 15 | T15 — API 1.3.204 promotion gate | 1 | Final advertisement only after all earlier work and wider core validation. |

The table totals the 61 blockers observed at backlog creation: one API-version
requirement, two extensions, 48 features and ten properties. Their membership
is stable while readiness and completion are derived from the live matrix, so implemented
requirements remain auditable in their original tranche. The checker fails if
a baseline requirement is missing, duplicated or unknown, or if the initially
satisfied `robustBufferAccess` regresses. It also enforces dependency order and
the final API promotion gate.

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
`supportedOperations` and `quadOperationsInAllStages` through the 1.1
properties query. Compute subgroup support is required by the original CTS;
other stages depend on their reported masks. The quad field does not establish
broadcast support. Neither subgroup bit nor `apiVersion` changes in this slice.

## Readiness versus profile completion

For Vulkan 1.1–1.3 rows, implementation can be ready before the device is
allowed to advertise the target API version. The backlog therefore reports two
different facts:

* **implementation ready** means real implementation, applicable CTS and native
  evidence are green; it does not assert that the capability is advertised;
* **profile satisfied** additionally requires the public API axis and final
  fail-closed verdict to be green.

This distinction lets the implementation counter advance without weakening the
final Vulkan 1.3.204 gate or creating a circular dependency.

## Definition of done for a requirement

A row is counted complete only when all four evidence axes in the live matrix
are green:

1. the public query reports the exact supported value and device creation
   accepts it;
2. the implementation is real and fail-closed for unsupported combinations;
3. applicable focused upstream CTS passes without weakening its oracle;
4. an artifact-bound native PS5 run proves execution, output and clean
   lifecycle through `ps5log/1`.

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
