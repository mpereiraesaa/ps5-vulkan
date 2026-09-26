# Tessellation status: integrated and merged

## Current status

The merged default native runtime passes **403/403 original upstream CTS
cases on one PS5 executable**, including the existing 304-case regression and
99 tessellation, tessellation clip/cull and TCS/TES resource cases. Strict
artifact/report verification and application closure pass. Experimental
tessellation negotiation and all shader/probe bypasses are off.
This is focused validation, not complete Vulkan CTS or a conformance claim.
T04 was merged to `main` in PR #158 (merge commit
`2dea243e50fadacc666e0d00aea6be54f5a767e4`). The native runtime-graphics
build enables tessellation by default; its public-profile query and integrated
default-profile execution pass strict identity and clean closure. The default
403-case run is `20260920T130122695Z_PPSA99994_upstream-cts_0x1034cdef319e6`
(SELF SHA-256 `2e0fc22b7de6401f7d12f2f6359efc14a223b4917100acad4c98edecf726d45c`).
Compute-only and offline graphics profiles do not gain the feature. Historical
experimental receipts below must not be relabelled as default-profile runs.

The DXVK profile matrix admits this default-profile run as the
`tessellationShader` native execution evidence. The 99 passing leaves are
still outside the frozen CTS selection, so its CTS axis records no evidence;
under the current policy an unrun or unselected leaf does not block a row.

A standalone consumer of staged public SDK headers also verifies that the
default build reports tessellationShader and the eight intended limits
(64, 32, 128, 128, 120, 4096, 128, 128), with strict artifact verification and
clean closure. This verifies default native runtime-graphics profile
advertisement separately from the execution witnesses. It does not establish
DXVK compatibility or a published release.

Additional native witnesses pass all nine domain/spacing patch-discard cases,
stage execution modes distributed between TCS and TES, combined indexed and
instanced direct/indirect draws, and disjoint vertex/control push-constant
ranges with runtime uniform array indexing. Push-constant array indices must
be dynamically uniform: an earlier invocation-indexed diagnostic violated that
requirement and is not acceptance evidence. The corrected witness verifies
1378 pixels and clean resource retirement.

The compiler now preserves push-constant member ranges during explicit IO
lowering. An unused block prefix no longer imposes unrelated stage visibility;
the effective load address is preserved. Actual-compiler regressions exercise
bounded and runtime uniform indices. The following sections retain the earlier
per-slice results; their historical counts are not the latest aggregate.

Native diagnostic builds now execute tessellation and verify GPU pixels for
triangles, quads and isolines, per-vertex and per-patch delivery, 32 control
points with cross-invocation communication, and a three-input/32-output patch.
An additional vertex output not consumed by the control shader is also tested.
These are bounded diagnostic results, not general feature or conformance claims.
These earlier runs used an experimental profile; they predate the current
default-profile release candidate.

Own point-mode native tests pass all nine domain/spacing combinations:
triangles, quads and isolines with equal/even level2 and odd level3. An
independent coordinate/color oracle judges every pixel, including absence of
extra points. Each artifact passes strict verification and clean closure.
These integral-level cases do not establish every fractional non-integer
placement rule; other upstream spacing evidence remains separately attributed.

An own native TES-output envelope now checks128 declared components: Position
plus31 vec4 user outputs. Fragment consumes all124 user scalars, checks their
distinct affine values, and the independent CPU pixel oracle verifies1378/1378
interior pixels. Completion, ring restoration and clean closure pass. This
also fixes reflection of TES output arrays, which must retain their array
dimension (unlike TES per-vertex inputs). Normal and sanitizer host regressions
accept the31-location match and reject a shifted producer. This is one envelope
layout, not all legal interface layouts or final integrated feature acceptance.

A further unchanged original upstream resource selection passes16/16 on a
fresh native artifact: TCS/TES uniform buffers, dynamic offsets, two descriptor
sets and sampled images. Earlier runs were10/16 and15/16. Graphics command
recording now snapshots offsets separately per set, and genuinely merged LS/HS
programs use their merged argument ABI instead of the separately compiled ABI.
The final report passes strict artifact identity, image oracles and clean
lifecycle. Own host regressions cover offset snapshots and two-set hull
compilation. The later integrated regression is recorded in Current status.

A further original upstream selection passes16/16 clip/cull cases on a fresh
native artifact: TES and TES+GS, static/dynamic indexing, with and without
fragment distance reads. The preceding artifact failed six of these cases.
The fixes preserve packed distance identity when fragment inputs are compacted
and use actual parameter-export indices (including implicit PrimitiveID) for
the producer metadata. Own-source host regressions cover both contracts.
Strict report/identity validation and clean closure pass. This does not cover
every clip/cull interface; neither the full feature nor conformance is claimed.
An own-shader cull-only TES→FS witness additionally passes1378/1378 expected
interior pixels after correcting the combined packed-width validation. The
fragment color comes only from CullDistance[4..6], exercising sparse second-
register input linkage. Mixed producer clip+cull with a consumer declaring only
cull now has explicit packing/linkage support: an additional own native witness
with3clip+4cull from TES and cull-only FS verifies1378/1378 pixels, including a
read across parameter registers. A separate dynamic-index artifact now also
verifies1378/1378 pixels: an interpolated distance selects among four cull
components across the register boundary. Compiler regressions cover dynamic
prefixes0..4. The fix preserves declared widths through frontend and IO
lowering; the static receipt does not stand in for this new native result.
An initial dynamic fixture using FragCoord was rejected before drawing by
the current reflection profile and remains recorded as a failed attempt.

An additional own-shader quad point-mode witness now verifies generation level64
on hardware. Its bottom edge is mapped to a fixed65-pixel atlas; every pixel is
judged, with zero missing, foreign or wrong-color pixels. Host negative controls
show that every smaller integer level misses required points. The canonical
offscreen artifact passes strict identity/transport validation, GPU completion,
shared-ring restoration and clean closure. This establishes this level64 case,
not all domains/spacing modes at maximum level or the total component ceiling.
An earlier presentation-profile run produced the same pixels but was not
accepted by the offscreen verifier; its manifest and evidence remain unchanged.

Variant30 now transports and checks32*(121 user scalars +4 Position scalars)
+90 per-patch user scalars +6 tessellation-factor scalars:4096 declared output
components in its generated SPIR-V. Both TCS and TES pass the pinned host
SPIRV-Tools validator after relocating only Patch locations into their separate
namespace. The TES checks every vertex's values and Position, all90 patch values
and all six factors before producing the independently expected affine image.
Native readback verifies1378/1378 interior pixels with no missing/foreign/wrong
colors, completion, ring restoration and clean lifecycle. This is one concrete
large-interface arrangement, not all legal type/location/interface combinations.

An earlier aggregate executed67/67 original upstream cases successfully on
one experimental-feature artifact with normal native ring lifecycle. All ring,
offchip, shader-trace and optional-stage-bypass diagnostic switches are off;
only experimental public feature negotiation is enabled. This revalidates the
families below together after lifecycle integration. It remains a focused
selection, not complete feature validation or Vulkan conformance.

Three further own-shader artifacts validate larger interface payloads. Variant26
checks31 output control points carrying31 vec4 per-vertex user outputs (3844
user components). Variant27 checks30 vec4 per-patch user outputs (120 user
components). Variant28 checks31 control points carrying28 vec4 each together
with two patch vec4 (3480 user components). Each verifies1378 independently
expected interior pixels, zero missing/foreign/wrong-color pixels, GPU completion,
checked ring restoration and clean exit. These are separate artifacts, not a
single combined-maxima test or proof of the4096-component ceiling. Position and
tessellation-factor built-ins are separate from these user-component counts.

Patch and non-Patch SPIR-V locations are independent namespaces. A reflection
regression accepts a shared numeric location and rejects a missing patch
producer; normal and sanitizer tests pass. This is not hardware proof of shared
locations. The native mixed witness uses distinct locations because the GLSL
frontend rejected the initial overlapping-location source spelling. That build
failure must not be generalized into a restriction on valid SPIR-V.

A direct indexed own-shader patch test also passes with UINT16 indices,
nonzero index-buffer binding offset, firstIndex and baseVertex. The expected
positions depend on the actual fetched indices, and 1378 interior pixels are
checked. A second case passes with UINT32 indices above65535 and a negative
baseVertex, producing the same independently expected image. A separate direct
instanced test verifies two distinct patches using instance IDs3 and4, with956
correct interior pixels. An SDK-linked indirect test produces the same956
pixels using a nonzero argument-buffer offset and firstInstance3. Arguments
are resolved on CPU at submission, not by a GPU indirect packet. Initial
non-SDK feature negotiation correctly failed; it is not passing evidence.
Indexed-instanced combinations and GPU-written indirect arguments remain
unvalidated by these cases.

Additional experimental native results now include six original `misc_draw`
fill-cover cases (18 reference-image comparisons), three isoline cases across
all spacing modes (nine reference-image comparisons), eight primitive/spacing
transition cases with nonempty readback, and the original four-stage overlapping
push-constant case. These retain the upstream test bodies and image oracles.
These families, all28 shader-input/output cases, four winding cases and five
geometry-interaction cases now also pass together:55/55 original upstream cases
on one experimental artifact, with strict report identity and clean closure.
This is a focused selection, not the complete upstream tessellation suite.
On a later experimental artifact, all twelve original `common_edge` cases also
pass: indexed adjacent triangles/quads with two vertex attributes, three spacing
modes and basic/precise variants. Their unchanged image oracle checks for cracks
inside the rendered grid. This does not prove maximum generation level64 or
revalidate the earlier55 cases on that individual12-case artifact. The subsequent
67-case aggregate described above does revalidate both selections together.
Own-shader tests additionally verify exactly nine quad-domain points and
independent VS/TCS/TES specialization using the same ID with different values
in two pipelines. Those own-shader results belong to their separate measured
artifacts; the55-case run does not implicitly revalidate them.

The primitive-discard selection is **not accepted**: all six selected cases
stop at the optional `vertexPipelineStoresAndAtomics` requirement before drawing.
The own point-mode test does not replace these upstream tests.

Linked compiler stages now have separate specialization maps and entrypoints;
an explicitly empty map retains shader defaults. A new cache payload stores
hull/domain/fragment ownership and passes actual-compiler host tests plus
ASan/UBSan, including warm reuse, stage-key separation and live-lease lifetime.
An own-shader native warm-cache test now verifies a hit without recompilation,
destroys the original pipeline before drawing with the replacement, and checks
the expected stage-specific colors and clean resource retirement. This covers
that cache/lifetime case; earlier uncached runs remain distinct evidence.

Five original upstream tessellation/geometry passthrough cases now pass in one
experimental run (triangles, quads and isolines, including geometry shading).
Four previously failed image comparison because the blending path used an
incompatible color-export/conversion combination. Blended RGBA8 pipelines now
compile FP16 exports and emit matching conversion state; unblended pipelines
explicitly reset it. An independent dense own-shader oracle checks all 2916
interior pixels, including pixel parity classes missed by a sparse point test.
This is not a general blending or full tessellation conformance claim.

The experimental native offchip-binding profile now passes **28/28 original
upstream shader-input/output cases**, including all twelve cross-invocation
variants. It initializes the driver's offchip parameters from the owned
allocation in 32 KiB blocks, checks readback, and restores the previous state
only after GPU completion. The run used an 8 MiB allocation, completed with
zero outstanding allocation bytes, and closed cleanly. This validates the
selected family, not the entire tessellation feature. A second, leaner artifact
also passes all 28 with the hull-entry trace, GE_CNTL override and additional VS
flush disabled. Existing ring-query telemetry remains. Broader integration and
coverage remain pending.

The earlier empty-draw investigation below is historical. Its conclusions that
the tessellator never runs, that no tessellation-specific driver call exists,
and that only emitted register state matters are superseded: native TF-ring
binding has now been identified and exercised. A checked lease binds the ring
around GPU work and restores the prior binding after confirmed completion.
Compiler IO-layout and native input/output patch-count defects were also fixed.

Shared-ring lifetime and two pipelines with complementary scissors have native
evidence. Four original upstream CTS winding cases now also pass on hardware:
default-domain GLSL triangles and quads, both CW and CCW. Each runs both front-face
settings and compares pixels using the unchanged upstream oracle. The experimental
public-API profile produced eight completed draws with ring restoration and clean
closure. A missing readback-to-color transition initially rejected the second
recording; that defect is fixed and covered by host regressions.

Before native offchip initialization, a full 28-case original shader-input/output
run produced **16 Pass, 12 Fail**.
The passing cases are asymmetric patches 5→10 and 10→5, PrimitiveId and
PatchVertices in TCS/TES, all six TES tessellation-level reads, all three
GLPosition delivery routes, and the cross-invocation barrier case. Device closure
reported zero allocated bytes and Close Game confirmed the title stopped.
After a further aggregate-interface fix, all 28 cases reach their image oracle.
That run's twelve failures were cross-invocation image comparisons, each with
one incorrect stripe out of eight. However, a subsequent redeployment and run of
the **identical SELF** produced **1 Pass, 27 Fail**, with all cases reaching the
oracle and clean closure again. The 16/28 result is historical evidence, not a
reproducible baseline. State-dependent or otherwise uncontrolled execution is
unresolved; its cause has not been established. Both runs fail acceptance.
No skip is a pass.

Private observation subsequently found that the launch environment uses offchip
offsets outside the original 144-workgroup allocation. A bounded diagnostic
expanded owned capacity to 256 workgroups without changing the emitted buffering
parameter; valid hull data was observed beyond the old extent. That run completed
all 28 cases with 16 Pass/12 Fail and clean closure. This establishes an allocation
coverage defect, not native setter semantics or a complete tessellation fix.
The capacity switch remains experimental; the twelve cross-invocation image
failures and the earlier reproducibility discrepancy were unresolved at that
point. The newer checked offchip-binding profile above passes this family.

A private non-acceptance diagnostic isolated the patch whose `.75` attribute was
absent from a grouped draw's post-completion offchip observation. Drawing only
that patch produced the correct white stripe in all twelve cross-invocation
variants (including vector and matrix payloads), with correct observed hull data
and clean closure. This narrows investigation toward grouped execution/delivery;
it does not establish a missing invocation or an address collision. An explicit
VS completion event did not improve the original 28-case result. Neither
diagnostic promotes a capability or replaces the unchanged upstream oracles.

A subsequent bounded own-shader entry trace recorded all eight distinct patch
input keys in one failing grouped draw. Two patches received the same offchip
offset with different factor offsets. This rules out a missing hull launch for
that draw, but did not alone establish harmful overlap. After checked native
offchip initialization, the trace shows two separate regions and all eight
contributions, while the original pixel oracles pass. The wider supported
configuration envelope remains under analysis.

Earlier attempts stalled or failed before submit. Follow-up found missing LS
vertex-fetch metadata, a missing independent HS user-data block, a residual hull
loader prohibition and a runtime-emitter wrapper that ignored LS-only fetch.
The fixes have compiler, loader and actual runtime-wrapper packet regressions.
The follow-up built-in and aggregate interface changes are now measured natively.
Arrays/matrices expand into their full location spans, with negative tests for
overlap, overflow, invalid column counts, zero lengths, cycles and mismatches.

These earlier bounded results alone did not validate the full tessellation
envelope, all limits, or external hull descriptor/push delivery. Later envelope,
resource and default-profile measurements are summarized in Current status.

## Historical investigation — do not use as current capability status

The following preserves the predecessor's observations and hypotheses at that
time. Statements such as "still unfixed", "does not execute", "only remaining"
and "would justify using the console again" describe that snapshot only.

Recorded 2026-09-19, at the end of a bounded native investigation. `tessellationShader`
is **false** and stays false: nothing rasterises from a patch draw, so no advertised
contract would have hardware evidence behind it.

This document exists because the work stopped at a boundary rather than at a fix, and a
boundary is only useful if it is stated precisely. It separates what was demonstrated
from what was inferred, names the instruments and their controls, and states the one
thing that would justify spending console time here again.

## What is demonstrated

**The patch draw completes.** `PS5VK_GRAPHICS_COMPLETED`, clean BYE, ~0.168 s, with the
19 geometry cases still passing in the same process. It hung for 22 consecutive runs
before that. The cause was a driver defect, not a hardware limit: the hull's ring-table
pointer was written into the block that builds the *context* bank, which runs before the
shader bank exists, and the runtime path then overwrote both entries and the count. The
register, slot, address and delivery model had all been correct the whole time; the write
simply never reached the engine. A hull that cannot load its first descriptor cannot
write tessellation factors at any level.

**The control (hull) half executes and stores correct factors.** It writes outer
2.0/2.0/2.0 and inner 1.0 as four contiguous dwords at the offset the geometry engine
itself hands the wave — the hardware's triangle layout, at the hardware's own address.
This is a positive measurement of the stage running, not an absence of errors.

**The evaluation (domain) half does not execute.** Two witnesses with no shared failure
mode agree:

- a *write* witness — an evaluation half that also writes a storage buffer; zero
  invocations recorded, every coordinate zero, in a run where the hull stored correct
  factors and the draw retired;
- a *time* witness — an evaluation half that spins 200,000,000 dependent iterations
  feeding `gl_Position`, with no descriptor, no buffer and no readback at all. Execution
  would blow past the bounded fence and report a stall. It does not.

**The time witness is validated by a positive control.** The same spin placed in the
*control* half — the stage proven to execute, because its factors are in the ring — does
report the stall and takes 0.470 s against the usual 0.169 s. So "no stall" genuinely
means "did not execute" on this hardware, rather than "the loop was optimised away" or
"200 million iterations are not slow enough here". The control was run *after* three
results had already been taken with the instrument; that ordering was a mistake and is
recorded as one.

**The failure is not domain-specific.** The same witness on **isolines** — two outer
levels instead of three plus an inner, a two-dword factor layout instead of four, line
output, a different `VGT_TF_PARAM.TYPE` read back as `0x00060060` — behaves identically.
The tessellator emits nothing for either domain, which makes the failure structural
rather than configurational and retires every triangle-specific hypothesis at once.

**No factor value anywhere can reach the failure.** The entire tessellation factor ring —
all 27,648 words — was pre-filled with a legal level before the draw, so wherever the
engine reads, it finds one. No change.

So: the geometry engine dispatches the merged LS/HS workgroup, the hull runs and stores
correct factors, and the VGT never enters the tessellation path.

## Five driver defects found and fixed on the way

These are real, independent of whether tessellation ever works, and are the part of this
work with lasting value.

1. **The hull's ring-table pointer was never emitted** (above). Moved into the shader-bank
   block, after the hull's own registers.
2. **The hull stored quad-shaped factors for a triangle domain.** The lowering pass
   branches on the primitive mode and falls through to *quads* for anything unspecified,
   and the mode is declared in the evaluation shader — which the compiler was not given.
   Linking the evaluation half in (as a link-only third module) and applying the
   cross-stage copy fixed the layout; hull code shrank 324 → 188 bytes and the inner
   factor moved one dword.
3. **The domain half received no ring descriptor table.** Delivered through the same
   user-data window as the hull's.
4. **The hardware tessellation-level clamps were never written.** The pinned tree writes
   both in three separate init paths; this driver wrote neither and relied on whatever the
   platform had left in the context registers.
5. **The draw path's descriptor visibility mask had no tessellation stages.** A
   tessellation pipeline with any descriptor set was therefore created successfully and
   then refused at queue submit — a defect in the draw path, not in the tessellation work.

## What was closed by measurement

Each of the following was programmed, verified to have reached the hardware by reading it
back out of the pipeline the draw actually used, and changed nothing:

- **`VGT_SHADER_STAGES_EN`, every field, in both states** — `ES_EN` (corrected from
  `REAL` to `DS`, a genuine defect), `PRIMGEN_PASSTHRU_EN` (forced off *in the compiler*,
  so the generated code moves with the register), `GS_EN`, `DYNAMIC_HS`, and `VS_EN` set
  to `VS_STAGE_DS`.
- **`VGT_TF_PARAM` omitted entirely** — which closes its donut, detect-one, detect-zero,
  memory-type and read-request-policy fields in a single run — plus those fields set
  individually.
- **`VGT_LS_HS_CONFIG` patches-per-workgroup**, `VGT_TESS_DISTRIBUTION`,
  `VGT_GS_MAX_VERT_OUT`, `GE_PC_ALLOC`, and the merged workgroup's LDS allocation.
- **The command stream**, decoded packet by packet, including emitting the idle-and-
  pointer-reset events this driver otherwise never emits.
- **The platform's register-defaults library**, enumerated read-only: 137 keyed entries
  across context, shader and user-config tables, of which this driver uses exactly one.
  Four keys land on precisely the four tessellation registers, all of which this driver
  already writes.
- **A work threshold** (drawn at tessellation levels 16 rather than 2), **process
  contamination** (the patch draw made the first and only draw in the process), and the
  **primitive argument** passed to the platform's shader-link call.

## What is *not* claimed

The register space is **not** exhausted, and earlier notes that said so have been
corrected. The honest statement is weaker and truer:

> With what this lab currently knows, no justifiable candidate remains.

That is a statement about the available evidence, not about the hardware. Three times in
this investigation a claim of completeness dissolved when it was audited instead of
repeated — most recently a twice-stated claim that every field of `VGT_SHADER_STAGES_EN`
had been measured, when one two-bit field never had been. Each audit produced a run worth
doing. Treat any completeness claim here, including this one, as something to re-check.

Two specific inferences have been **withdrawn**:

- *"Reverse-engineer the platform's draw path to find hidden tessellation state."* Twice
  recommended, and wrong: the lab's own research establishes that the shader-link entry
  point is a pure CPU transform with no hidden state. The whole GPU configuration is the
  register banks and command words already dumped.
- *"The register space is closed, so the answer is not a register."* Withdrawn in the
  weaker form above.

## Instruments, and their controls

All default-off, all inside the tessellation probe build; none affects a shipping build.

| Instrument | What it shows | Control |
| --- | --- | --- |
| Bank dump | every context / shader / user-config register in **emission order**, for the patch draw *and* one passing draw in the same process | the passing draw is the control — it is a free diff |
| Command-word dump | every word of the submission | decoded against the pinned packet definitions |
| Factor-ring scan | the ring's **whole** extent | replaced a fixed 8-word window at the ring base that produced three false negatives |
| Bounded fence + stall report | whether a stage executed, by time | the same spin in the hull half, which **does** stall |
| Domain-execution counter | whether the evaluation half ran, by memory write | *unvalidated* — see below |

**Method lesson, worth more than any single result:** reading a register out of the object
that holds it proves its *value*, never that it was *emitted*. Eleven single-register
arguments preceded the two fixes that changed the failure class, and neither fix came from
one; both came from dumping the whole stream in emission order.

**One instrument remains unvalidated.** The write witness's own control — a vertex+fragment
draw doing the same storage-buffer write — cannot be built, because graphics storage-buffer
support does not exist in this driver. That is a feature gap, not a gate to open. The
conclusion it supports is carried by the *time* witness, which is validated; the write
witness corroborates rather than establishes.

## The blockage, stated exactly

```
geometry engine  ──dispatches──▶  merged LS/HS workgroup     [PROVEN to run]
      │                                    │
      │                                    └──stores factors──▶ TF ring  [PROVEN correct,
      │                                                                    at the engine's
      │                                                                    own offset]
      ▼
  tessellator                                                 [NEVER RUNS — no domain,
      │                                                         any factor value, both
      ▼                                                         instruments agree]
  TES / domain half                                           [PROVEN not to execute]
```

Everything to the left of the tessellator is measured correct. Every register this driver
programs on the way is measured correct. The gap is the platform's own tessellation
behaviour, and the lab's research — which covers shader creation, shader linking and queue
submission — does not touch tessellation anywhere, nor does the identified export set
contain a tessellation-specific call.

## Recorded and still unfixed

- `VGT_LS_HS_CONFIG.HS_NUM_OUTPUT_CP` is taken from the pipeline's patch-control-point
  count rather than from the control shader's declared output vertex count. Equal by luck
  in the current fixtures; wrong in general.
- The vertex half of a tessellation pipeline has no descriptor delivery path at all — it
  is the LS end of the merged hull, and the pre-raster ABI slots come from the domain.
- The driver writes `VGT_TF_PARAM` whole, from a value derived from the control half's
  declared interface alone, so several of its fields are zeroed with nothing restoring a
  platform default.
- Graphics storage-buffer support is absent, which is what blocks the write witness's
  control.

## What would justify using the console again

Not another field in a register that has already been read back correct, and not a re-run
of anything above. One of these:

1. A **primary source** — the pinned driver tree, the pinned hardware headers, or an
   existing lab research note — naming a register, packet or ABI element on the
   tessellation path that this driver does not currently emit. The diff against the
   emitted banks is a host-side exercise and must be done *first*; the console run is only
   to confirm it.
2. **A working tessellation pipeline from an authorised title**, read as a command stream,
   showing state this driver does not program. This is a different question from the
   platform-API one that was already settled, and it is the only remaining source of
   ground truth about what this hardware wants.
3. **Graphics storage-buffer support**, which would let the write witness's control be
   built and would turn a corroborating instrument into an establishing one.

Any of those justifies exactly one bounded run, at an exact build identity, with the
console and title claimed before and released after.
