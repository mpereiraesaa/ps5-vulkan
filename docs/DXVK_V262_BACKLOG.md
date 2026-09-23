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

### T07 Part2 diagnostic status (2026-09-23)

The T07 Part2 diagnostics have strict source-derived hardware passes for the
`query_pool.occlusion_query.basic_precise` D16 diagnostic and the bounded D32
depth-gather case, each repeated twice on the same diagnostic eboot. The
default-off frozen selection also passed 362/362 before the BC Part1 integration.
These results qualify narrow diagnostic paths only: the public query, gather
and format bits remain off, and the runs do not satisfy the four-axis DXVK
promotion gate. Part1 has since reported a separate successful public-SDK
cube-array witness; that result is not combined acceptance and does not promote
the feature.

A second isolated Part2 port is based on Part1's fixed commit `722973c`, which
includes T08 #406. It preserves T08 feature bits 20/21/22, UBO layout bit 23,
BC bit 24, cube-array bit 25, gather bit 26 and precise-query bit 27. PSBC host
and PS5 archives were rebuilt at `ee8959186cfb1f5c0a574d1e2ee329aad1fe2747`.
Full `make check` passed on exact pre-integration HEAD
`0364726c3fbc20db932789be4ae61cc50ecfd3b4`: 652 Python tests in 103 modules
(48 skipped), C contracts, reporting audit, SDK consumers and runtime compiler.
This is host validation only; no combined hardware/CTS run or public feature
promotion is claimed.

#### Review status (2026-09-24)

The T08 baseline was published to `codex/t07-part1-final` at `722973c`. Five
review PRs are now staged in order: #425 (CTS factory registration), #426
(diagnostic selection), #427 (gather compiler gate), #428 (host query state),
and #429 (host ZPASS control encoding). They remain open and unmerged. Exact
`make check` passes on #428 HEAD `b69dde1` and #429 HEAD `01d167d` (643 Python
tests in 102 modules, one skip, plus C, SDK, reporting, upstream-selection and
compiler checks). These results are host-only. The BC work and combined
hardware/CTS acceptance remain outstanding; no public feature bit is promoted.
After serial integration onto the final branch, rerun combined checks and
canonical acceptance before reassessing CTS eligibility or public reporting.

The table totals the 61 blockers observed at backlog creation: one API-version
requirement, two extensions, 48 features and ten properties. Their membership
is stable while readiness and completion are derived from the live matrix, so implemented
requirements remain auditable in their original tranche. The checker fails if
a baseline requirement is missing, duplicated or unknown, or if the initially
satisfied `robustBufferAccess` regresses. It also enforces dependency order and
the final API promotion gate.

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
