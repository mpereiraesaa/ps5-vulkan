# Tessellation status: the hull launches, the tessellator does not run

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
