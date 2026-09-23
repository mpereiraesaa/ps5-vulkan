# Runtime graphics validation

The experimental procedural graphics profile was tested on an owned PS5 with
firmware 12.02 on 2026-09-12, using the packaged native SDK and PSBC/ACO gfx1013.

## Merged T04 tessellation validation (2026-09-20)

The merged default native runtime-graphics build passes 403/403 unchanged upstream
CTS cases: 304 regression cases and 99 tessellation, clip/cull and resource
cases. Strict artifact/report verification and clean application closure pass.
The frozen acceptance selection has since grown to 306 cases with the two
fragment `frag_side_effects` leaves (306/306, see
[the fragment promotion](#fragment-stores-and-atomics-promotion-2026-09-20));
the 99 tessellation cases stay outside that frozen selection, so 405 distinct
original upstream cases have passed across the two receipts, not in one run.
Experimental tessellation flags and shader/probe bypasses are off. A separately
built public SDK consumer verifies feature advertisement and the intended limits.
See [tessellation status](TESSELLATION_STATUS.md) for the scope and additional
native witnesses. This is focused validation, not complete CTS or conformance;
T04 was merged to `main` in PR #158. DXVK matrix admission of the 99 passing
tessellation leaves remains separate evidence bookkeeping.

The integrated receipt identifies:

- SELF SHA-256: `2e0fc22b7de6401f7d12f2f6359efc14a223b4917100acad4c98edecf726d45c`.
- QPA SHA-256: `4a81d0f27f374fa3dae7fd4be7ebb15237d08ffbb874325ca4df2b85a49a4218`.
- Selection SHA-256: `6098db45ae68a487ea2fba5438b6d31e9b152217c2c3f6f8eb1eb985702c02e1`.
- Upstream CTS commit: `a0270c1897597e6c77679870e10415398a13001c`.

Dated sections below retain earlier measurements, including failed candidates.
Their unadvertised-feature statements describe those historical artifacts, not
the current runtime. No historical receipt has been relabelled as this run.

## Clip-cull native acceptance

The packed pre-raster distance export is implemented and measured, and
`shaderClipDistance` and `shaderCullDistance` are deliberately **not
advertised**. This section records what the profile does, what was measured on
hardware, and the normative reason the two features stay false.

### Implemented coverage

The pre-raster stage may declare `gl_ClipDistance` and `gl_CullDistance` with
static constant indices inside the ceilings of `src/graphics_stages.h`
(8 clip, 8 cull, 8 combined), and `src/spirv_graphics_interface.c` validates the
`gl_PerVertex` members, the array sizes and the combined ceiling before the
program is compiled. The runtime adapter (`native/runtime_shader.c`) packages a
distance export only when the compiled masks agree with the register state the
pinned compiler emitted (`PA_CL_VS_OUT_CNTL`, `SPI_SHADER_POS_FORMAT`,
`SPI_VS_OUT_CONFIG`), and the resulting unresolved `AGC_LINKAGE` slot is accepted
only for that verified shape. A single-word mutation of any of those registers
is refused.

The private coverage witness (`src/clip_cull_witness.c`, enabled by
`PS5VK_CLIP_CULL_PROBE`, judged by `tools/verify_clip_cull.py`) draws one
bounded scene per case and reads the colour and depth footprint back. Two
deployments of the exact package were run on the console and both verified
strictly, with a clean `ps5log/1` receipt and a confirmed Close Game:

* a single clip half-space produced exactly one cleared 2048-pixel half;
* a two-distance quadrant case produced exactly 1024 cleared pixels;
* a partially clipped primitive left the untouched half unchanged;
* a primitive whose cull half-space was negative at every vertex was discarded,
  and a mixed negative/non-negative primitive was not.

Pulling the distances out of the shader, or corrupting the packed masks,
fails the witness: the pixels are the oracle, so the measured result is the
rasterizer, not the metadata.

### Why the feature is not advertised

The upstream module that owns these built-ins, `vktClippingTests.cpp`, gates
**all** of its user-defined distance leaves on the same two features through
`requireFeatures(FEATURE_SHADER_CLIP_DISTANCE)` and
`requireFeatures(FEATURE_SHADER_CULL_DISTANCE)` in `testClipDistance`, and its
factory registers two variants of every leaf:

* `*_fragmentshader_read` declares the distance arrays as fragment-shader
  inputs and reads them (`fragmentShaderReads`), and
* `*_dynamic_index` writes them through a loop with a non-constant index
  (`indexingMode`).

The measured support is narrower than the family in exactly one place. Dynamic
indexing is **not** refused: a pre-raster module that redeclares
`out float gl_ClipDistance[2]` and writes both elements through a loop is
compiled by the pinned compiler with the full-width mask
(`clip_distance_mask=0x03`, the same value the equivalent static-index module
reports) and is accepted by the native header builder
(`tools/inspect_graphics_compiler.c`, which now prints the masks:
`distances clip_mask=00000003 cull_mask=00000000`, `native_header_result=0`).
That variant now has its own witness case: the probe writes both clip distances
through a loop whose index is not a literal (the loop permutes it by the vertex
index, so each element is written exactly once per vertex), and the verdict is
that the rendered image must be **byte-identical** to the statically indexed
quadrant - both at the oracle, which judges every pixel, and at the native
digest relation, which refuses a run where the two differ. Writing the
distances dynamically and getting the static image back is the whole content of
the variant; the witness's own module is accepted by the host compiler with the
full-width masks (`clip_mask=00000003 cull_mask=0000000c`,
`native_header_result=0`), so the case is not refused before it can run. The
native result is now taken: the nine-case witness run
`20260917T072542029Z_PPSA99994_ps5vk_0x53eadab9451` (eboot
`f02fcf3dadfbe2f19ef360af2964b7ca18060801912bc54d0477577230f69b04`) reports
`cases=9 clip_mask=03 cull_mask=0c strict_verified=1` with every case
`missing=0 foreign=0 wrong_color=0 verified=1`, and the dynamically indexed
write hashes **exactly** like the statically indexed quadrant
(`digest_dynamic_index = digest_clip_quadrant = 85679b0d4edbc725`). So the
compile-time full-width mask the host measured is what the hardware executed,
and the variant's whole content - writing the distances through a non-constant
index and getting the static image back - holds on the console. None of the nine
cases faulted.

The same witness now also carries the **T03 cross-regression**: a tenth case
draws the quadrant program through `vkCmdDrawIndirect`, and the run
`20260917T083050864Z_PPSA99994_ps5vk_0x8ccc37d94b6` (eboot
`27f43ae6ec047dad6823f0ce7ff68505f650034096c5ce590562867a283ebed4`) is
`strict_verified` with `cases=10`. The evidence parser requires the split
explicitly, so the run cannot earn the relation on the direct path: case 9 logs
`indirect=1` and every other case logs `indirect=0`, and case 9's image is
byte-identical to the direct quadrant's
(`digest_indirect_quadrant = digest_clip_quadrant = 85679b0d4edbc725`,
`expected=1024 covered=1024 missing=0 foreign=0 wrong_color=0 verified=1`). Every
other case's digest is unchanged from the direct run, so the indirect path is the
only variable. The packed distance export, the register state the adapter
requires and the clipping result therefore hold through T03's indirect command
path, not only through direct draws.
### What promoted the two features (2026-09-17)

The fragment read that used to be refused is now implemented, measured and part
of the canonical acceptance selection, so `shaderClipDistance` and
`shaderCullDistance` are reported true by the graphics build.

**The compiler describes both ends of the interface.** The merged pre-raster
stage publishes one semantic word per packed distance register it exports (low
byte `PSBC_SEMANTIC_DISTANCE_REGISTER + register`, parameter index above it,
emitted after the described varyings), and the pixel stage publishes the same
key for the register it reads. The register a read lands in comes from the
read's position in the packed builtin space - the linker already places a
`gl_ClipDistance[4]` read in the second slot with component 0, and a cull array
that starts after a one-component clip array in the second slot as well - so the
compiler derives it from that slot and component, not from the attribute index,
which the linker numbers densely. Measured on the pinned fixtures: the vertex
exporting two clip components and one varying reports output semantics
`[15 param0, 48 param1]`, the fragment reading `clip[0]` reports `[48 attr0,
15 attr1]`, a fragment reading only `clip[4]` reports `[49]`, and a combined
1-clip/7-cull pair reports `[48, 49]` - with `PSBC_UNRESOLVED_AGC_LINKAGE` gone
in every case.

**The driver delivers a described read.** `distances_valid` now requires one
described word per exported register and counts the parameter exports from that
description; `ps5vk_runtime_graphics_distance_reads_described()` checks the
pixel stage's read report against the producer's packed masks (clip low bits,
cull immediately after) and requires every register the pixel stage names to
exist once on the export side; and the pipeline gate refuses a pair whose read
is not described end to end. The profile delivers such a pair in the shipping
build, not only in the diagnostic one.

**The limits are reported at the Vulkan floor.** `maxClipDistances` and
`maxCullDistances` are 8 and `maxCombinedClipAndCullDistances` is 8: the two
packed position registers after POS0 hold eight float components, clip
components first and cull continuing immediately after them, so either feature
may use all eight and the combined budget is eight. The stage-interface policy
bounds a declaration against exactly that width (a 4-clip/4-cull module is
accepted, an 8-clip/8-cull module is refused), which is what makes the reported
numbers executable rather than merely the floor.

**Acceptance, frozen and run.** The canonical selection is now 275 acceptance
leaves (211 + 64 clipping leaves: clip counts 1..8 for the combined family with
one cull count each, both indexing modes and the fragment-stage read) with 14
diagnostics. One strict run of
`build/upstream-cts` passed **275/275** (selection hash
`3067f94c99fcfb5047e2ac4f7fa009caa898fbe55c62829ea10ccf6d71cb463d`, eboot sha256
`b58d88149e623adc5cd9d75dcb19d87bd4e6774c43f494093d0ea3c1d5bd3bc7`, run
`20260917T114927552Z_PPSA99994_upstream-cts_0x13a34bfa824d`, Close Game verified and the title
stopped). The clipping family is what validates the reported limits: the leaves
step through every clip count up to eight and the combined clip+cull budget.

**Two honest gaps remain inside the family.** The `complementarity.{1..8}` and
`misc.negative_and_non_negative_cull_distance` leaves stay diagnostics
(`expected_status` `NotSupported`): the pinned binary does not report them when
they are filtered by the name the module's construction implies - they are
absent from both the `ps5log/1` transcript and the QPA in two full runs, while
every other clipping leaf reports - so they are not claimed as covered. The
tessellation and geometry variants of the family are outside this profile's
advertised features and stay unselected.

The native witness behind the fragment read is the eleven-case clip/cull
package: case 10 (the pixel stage reading `gl_ClipDistance[0]`) verifies with
`expected=4096 covered=4096 missing=0 foreign=0 wrong_color=0`, digest
`f50dd9368fee6cc9`, in the acceptance run
`20260917T104552228Z_PPSA99994_ps5vk_0x102afbaa5b65` (eboot
`e1cc2681…`), and the read digest differs from the control's as the verifier
requires; an earlier run on a pre-promotion build measured the same digests,
which is what makes the result reproducible rather than a single sample. The
witness package is the diagnostic profile; the shipping profile is covered by
the upstream run above, whose 16 fragment-read leaves execute the read through
the ordinary pipeline gate.

### What this does and does not establish

It establishes the vertex-stage clip/cull contract, the packed register state
the pinned compiler emits for it, the per-half-space culling rule and the
hardware clipping result for the measured shapes, the compiler and adapter
behaviour of a dynamically indexed declaration and the witness case that
requires it to reproduce the static image, and - since the promotion above - the
fragment stage's read of the interpolated distances, which is what the
promotion section records: the compiler describes the distance registers on both
sides, the driver requires that description before it delivers, and the 64
selected upstream clipping leaves pass inside a 275/275 acceptance run on the
shipping profile. It does not establish the tessellation or geometry variants of
the family (their features are not advertised), the two `complementarity`/`misc`
leaves the pinned binary does not enumerate under their implied names, any
distance width beyond the reported eight components, or Vulkan conformance; it is
not a conformance claim, and the features are advertised only for the graphics
execution path this repository builds.

## Historical optional stage blockers: geometry and tessellation (2026-09-17)

At this historical checkpoint both stages were unadvertised, with the reason measured
rather than inferred. These measurements were taken with the driver built
against an isolated PSBC candidate (the merged-geometry identification plus the
hull-packaging entry point, delivered as dependency pull requests that are
**not** merged), so they are provisional in that sense; only derived facts and
identities are published here, and the raw captures stay private.

**Geometry.** The compiler emits a self-consistent merged program for the
witness's vertex+geometry pair - one ACO program
(`SW (VS+GS+), HW (NEXT_GEN_GEOMETRY_SHADER)`) - and the two halves agree on the
exchange layout it uses: the vertex half stores each vertex's 9-dword (36-byte)
ring item to shared memory and the geometry half reads the same dwords back
(position in dwords 0-3, the varying in 4-6, bookkeeping in 7-8), with the same
per-item stride the package publishes as the ES item size and programs into
`VGT_ESGS_RING_ITEMSIZE`. Positions therefore arrive: the passthrough case covers
exactly the pixels the control covers. **That sentence is withdrawn** - see the
measurement below: it came from reading the oracle's expected-coverage count
(4096, a property of the predicate) as if it were the image's coverage, and the
image is in fact empty. What the varying does is not the only failure either: in
a single diagnostic run the same read path produced the empty image for some
cases and a drawn image for others, and the same case has produced both in
different runs. Concretely, the SUPPRESS case emits nothing by construction and
its own verdict certifies its image as the cleared target (`6927fac75e74a325`);
in the same run PASSTHROUGH and the SENTINEL carry exactly that digest (they drew
nothing) while SHRINK, RECOLOR and AMPLIFY drew something - and in an earlier run
RECOLOR carried the cleared digest. A handoff that sometimes sees the vertex data
and sometimes reads zeros is a *race*, not a constant offset, which is what a
missing visibility or ordering guarantee between the two halves of the merged
program looks like; the host-side A/B already ruled out a fixed wrong base, item
size or stride. The candidate's merged identification and ES-half sizes are
bookkeeping for the driver, not the fix. The
exchange is an LDS ring rather than an SPI parameter-export stream, so the export
configuration is not the path under question. What remains open is the item-index
mapping (the vertex half indexes by its lane id, the geometry half by the
per-vertex offsets the hardware hands it) and whether the ring's LDS region is
allocated and visible between the two phases. The same path also faults: a
geometry program that reads `gl_in[i]` in a loop over the input array loses the
device (the submission never completes) whether it runs before or after the
other modes, so the fault follows that program rather than its surroundings, and
the loss was reproduced again in the table run below. `geometryShader` therefore
stays false.

The table run in question
(`20260917T075403021Z_PPSA99994_ps5vk_0x6cab7462e29`, eboot
`ae239b852a70e0a68516e0fb4e0e947ebd430a51bbf036148e5d88370844ca07`) reports every
case in one log: control and constant emission verify, SUPPRESS verifies once its
input-less fragment stage is present, the sentinel, passthrough and shrink
fail, recolor and amplify draw something other than the cleared target, and
POSITIONS then loses the device (`vkQueueWaitIdle` `rc=-4`). The first attempt at
this run died entering SUPPRESS with `vkCreateGraphicsPipelines` `rc=-8`
(`VK_ERROR_FEATURE_NOT_PRESENT`): this branch never carried the synthetic
input-less fragment stage, so a geometry program that emits nothing was refused
for an input its pre-raster stage cannot export. That is why the witness now has
a table mode (`PS5VK_GEOMETRY_ORDER_PROBE`) as well: the shipping build is
fail-fast and stopped at the sentinel, so it never reached the case that was
broken on this tree.

That reading is not an inference from mixed builds. The same payload
(`ae239b852a70e0a68516e0fb4e0e947ebd430a51bbf036148e5d88370844ca07`, verified by
hash on the console before the pair of runs) was run twice through the table
mode, and the two logs differ exactly on the cases that read the ring:
control (`aa3cf584`), constant emission (`aa7d3f82`) and suppression
(`6927fac7`) are bit-identical between the runs and verified both times, while
the sentinel (`6927fac7` then `38d61b94`), passthrough (`6927fac7` then
`0a4ac999`), shrink (`93df309c` then `02ad379e`), recolor (`38bcd39e` then
`e0e55980`) and amplify (`fc3e50b7` then `45abe76c`) each produce a different
image, and the loop case loses the device in both (`vkQueueWaitIdle` `rc=-4`).
Runs `20260917T075403021Z_PPSA99994_ps5vk_0x6cab7462e29` and
`20260917T080954327Z_PPSA99994_ps5vk_0x7a8352a45b2`. The register state for the
ring is published by the merged program and programmed by the driver
(`VGT_GS_ONCHIP_CNTL`, `VGT_GS_OUT_PRIM_TYPE`, `VGT_ESGS_RING_ITEMSIZE`,
`VGT_GS_MAX_VERT_OUT`, `GE_NGG_SUBGRP_CNTL`), so what remains open is whether the
values describe the launch the hardware performs - not whether the registers are
there.

The witness also carries an observable sentinel for exactly that question: the
input triangle emitted unchanged with the colour computed from the position the
geometry half read (`x*0.5+0.5`, `y*0.5+0.5`, `0.25`), so the verdict asserts the
value the read produced instead of only the coverage. A zero read collapses the
triangle and a shifted item moves or reshapes it, while the control's own image
is refused because the sentinel's blue differs. Its stated limit is that
exchanging the two structurally identical input triangles wholesale maps the
image onto itself, so it separates correct, zero, garbage and shifted reads, not
that exchange. The first run to use it
(`20260917T072649435Z_PPSA99994_ps5vk_0x54e5f74a004`, eboot
`7f1523a9157bec7eeed7ceeaaf8bf25646ef9364cb52d2a4124ed7c9df0f9ed3`) reports
control `verified=1`, constant emission `verified=1`, and the sentinel
`verified=0` with `expected=4096 covered=0 wrong_color=4096` - the image carries
none of the expected per-pixel colour, the first value-level judgement of this
path rather than another coverage check. What image it produced instead is not
stable: that run hashed `436f0a07`, the first table run hashed the cleared target
`6927fac7`, and the second hashed `38d61b94` - three different images for the
same case, which is the non-determinism measured below and not a constant zero.
The shape's own limit is that the render pass clears to opaque black, so a log
alone does not separate "the read returned zero for the colour computation" from
"the read returned zero everywhere and the triangle collapsed"; comparing the
digest against the suppression case's does, and the constant-emission case
passing in the same run shows the stage draws and the fragment path works.
The harness is fail-fast in the shipping profile, so the POSITIONS case did not
execute after the failing verdict in that run and the device did not fault
there - it faults whenever it is reached, as the table runs show.

**Tessellation.** The isolated compiler candidate does produce a two-program
hull buffer for a real vertex+control pair - the control half and the vertex half
in one buffer, with the vertex half's program and resource registers published -
but the same metadata declares the package unresolved because the hull/domain
pipeline state is not part of it. At that checkpoint the driver had no executable
tessellation path: pipeline creation refused it before any
compile. `tessellationShader` therefore stayed false, and the remaining work was
driver-side assembly plus the hull pipeline state - the same class of
vendor-side question as geometry.

## Geometry promotion (2026-09-17, later window)

The two defects the section above was opened for are fixed and measured, and the
feature is advertised on the graphics path.

**The ES->GS read.** Every NGG pre-raster program now has
`VGT_ESGS_RING_ITEMSIZE` programmed to **1** instead of the compiler's legacy
**5**: the hardware scales the per-vertex offsets it hands the geometry half by
that value, so the stage read item `5k` where it had to read item `k` and only
the first vertex of each primitive came back right. The regression in
`tests/test_runtime_shader.c` fails before the change and passes after it. The
device loss in the POSITIONS case was the witness's own loop missing `++i`, so it
emitted past `max_vertices` forever; with the loop fixed the case runs.

**Determinism.** The same payload run twice now produces byte-identical images
for **all nineteen** cases - control `aa3cf584`, constant `aa7d3f82`,
suppression `6927fac7`, sentinel, the three readbacks, the envelope, the
invocations, the components and both input families - where the pre-fix table
above showed the ring-reading cases drifting between runs. Runs
`20260917T180918672Z_PPSA99994_ps5vk_0x285db17d6406` and
`20260917T180931272Z_PPSA99994_ps5vk_0x2860a07ed6b2`, eboot
`14478e545d4ab6cc2bc815f4c0d4987d358f2be211434f263669c5bd607284c8`.

**What the stage is measured to do.** The nineteen-case witness verifies
strictly in both its table and its shipping fail-fast payload
(`20260917T180945535Z_PPSA99994_ps5vk_0x2863f2ad4767`, eboot
`ae9e32761215bb845a81d991033b84d86742a001973958153d92c92929493663`): the two
triangle tiles, the shrink, the suppression, the varying rewrite, the
amplification, the sentinel value, the raw readback of an indexed vertex, the
per-primitive id, the **point** and **line** input families under their own input
assemblies (218/218 with digest `ebb18b9b90df07ca` and 467/467 with
`deab7726a7ac7625`), 32 invocations, 256 emitted vertices, 64 input components
and 64 output components that the pixel half consumes and folds into the colour
(`expected=1024 covered=1024`, centre `808024ff`). The five mandatory limits are
therefore reported at the Vulkan floor they exercised - 256, 32, 64, 64 and
1024 - and `geometryShader` is reported true by the graphics build
(`core_feature_bits` in `src/vk_device.c`, the platform mask in
`native/platform_ps5.c`, the limits in `src/graphics_limits.h`).

**Conformance.** The promotion run measured the pinned geometry module's leaves
with the feature advertised: 294 selected leaves, **286 Pass and 8 Fail**. The
eleven leaves whose own upstream oracles passed are now acceptance cases
(`dEQP-VK.geometry.input.basic_primitive.triangles` and its two conversions, the
six `output_<n>` leaves, `output_vary_by_attribute`, and its instancing variant).
Four of the eight failures were the geometry stage's uniform-buffer, sampled-image
and instancing descriptor variants, and they are now acceptance too: the pinned
module binds those resources to the GEOMETRY stage alone, which the compiler
profile now admits when - and only when - the pipeline carries that stage, and
which the draw path's descriptor plan now carries for the merged pre-raster
program once the native create records the geometry pair on it (a host regression
checks both directions). The four strip-topology leaves that remained were
blocked on one measured fact: the pinned geometry builder enables primitive
restart for strips (`vktGeometryTestsUtil.cpp:153-172`), they are the only leaves
built with `TRIANGLE_STRIP`, and the profile refused that input-assembly state
before the adapter was asked. It now carries it: pipeline creation accepts the
state for `LINE_STRIP`/`TRIANGLE_STRIP` and refuses it for lists, and the draw
path programs the pair RADV programs on gfx10 - the enable in the user-config
`VGT_MULTI_PRIM_IB_RESET_EN` (0x3092c), the index in the context
`VGT_MULTI_PRIM_IB_RESET_INDX` (0x2840c, 0xffff / 0xffffffff by index width) and
the `SQ_NON_EVENT` workaround the GFX10 synchronisation bug needs. The witness
measures it end to end: the indexed strip with a restart index between two quads
draws **foreign=0** with the cut, and 72 foreign pixels without it - the bridging
primitive a missing cut threads across. The frozen
selection re-run is **304/304 Pass, zero Fail, no NotSupported, title closed**
(selection
`7406de91ef883de7c1dd8522926773b72c846834fde37b45f686ff5994c8601b`, eboot
`afe1755291ef231f6f7c3e730abcd062f3a57618e879c758a1f0a89a42c207aa`), with all
twenty-nine geometry leaves the module produces for a device that advertises the
feature passing their own upstream oracles - the input families, the conversions,
the output-count and varying families, the descriptor variants, the strip
restart family and both per-primitive-id leaves. A geometry stage that reads
nothing per-vertex declares no gl_in array at all, so the policy binds it to the
pipeline's topology through the input primitive its execution mode states and
drops a declared attribute no shader input consumes, which Vulkan permits.

**Probe.** The DXVK 2.6.2 capability probe was re-measured against the advertised
profile and verifies strictly: `geometryShader` observed **1**, 10 of 62
requirements satisfied, 52 blockers. The matrix keeps that row as a blocker
because the probe executes capability queries, not geometry draws.

Tessellation was still unadvertised in this geometry-promotion receipt because
that compiler pin had no loadable hull package. The later tessellation candidate
and its separate evidence are documented at the top of this page.

## Fragment stores and atomics promotion (2026-09-20)

`fragmentStoresAndAtomics` is now advertised by the shipping graphics profile.
The promotion rests on two independent hardware oracles rather than on the
feature query itself.

The bounded native witness used one control draw and one candidate draw over a
64x64 target. Its exact SELF SHA-256 was
`e83cac3dad67b48880409a4ba6874954e325881d71cc8ff835b06ca4a1b4b295`;
run `20260920T181405989Z_PPSA99994_ps5vk_0x1145d7afedfdc` reported
`control_counter=0`, `candidate_counter=4096`, all 30 trailing guard words
intact, fence success, zero tracked allocations after teardown and a complete
`ps5log/1` BYE. This distinguishes a real fragment-stage SSBO write from a
CPU-side initialization or a merely negotiated feature.

The two unchanged upstream
`dEQP-VK.rasterization.frag_side_effects.{color_at_beginning,color_at_end}.kill`
oracles then ran with the frozen regression selection. The canonical shipping
run `20260920T213224464Z_PPSA99994_upstream-cts_0x11f2fc5c6a394` passed
**306/306**, with zero `Fail`, zero `NotSupported`, no missing or unexpected
cases, selection SHA-256
`b456e3ca1a93119db27fd58bbf7659932d3d25cbc2e7c7336cf0bfdd58683f06`,
SELF SHA-256
`03b52352f2ae3d073775a88b9734b8effca9538c991ce5335715bf809e79d356`
and QPA SHA-256
`44a0077b1a3c8ed3fa2b104ae406a6a7d2ac4f2ee141a9f01914a58dd2b04e82`.
The supervised invocation confirmed `PPSA99994 stopped=True`; a missing local
output directory prevented only the first QPA copy, so the same finalized log
was reassembled and verified offline without rerunning the console.

Finally, the public-ABI capability probe artifact
`1df87adfa07d0fabe702a685f71b0f2f9ad444f441e06d7d789ef3c55e69fd55`
strictly observed `fragmentStoresAndAtomics=1` in run
`20260920T212913672Z_PPSA99994_ps5vk_0x11f0359d4019e`. It reported 12 of 62
target values and 50 blockers; the DXVK matrix counts 10/62 requirements ready
because every ready row still needs API, implementation, CTS and native
execution evidence, not merely a true query bit.

## Multiview native acceptance

On 2026-09-16 the original 48 multiview leaves (masks, rectangular clears,
vertex ViewIndex and fragment ViewIndex) passed unchanged upstream pixel oracles.
The subsequent combined selection passed **165/165**, with no failures,
NotSupported cases, missing results or transport gaps. Five unrelated diagnostic
cases remain outside acceptance. This is focused execution, not full CTS or
Vulkan conformance.

The combined run `20260916T203823854Z_PPSA99994_upstream-cts_0x145ddaffc8494`
used:

- SELF SHA-256: `deaabb648bf288ab7cd1ec4c8788378785206e62957adab5fbb7e3019d080dd8`.
- Selection SHA-256: `984ab83f7c7586eea2243847f494e4db20784b4590d2772332912ab653aaad8c`.
- QPA SHA-256: `11cef34a5320ec66eac1fc3de5884efb44c7db31b1e35363ccd4f2e1b347648d`
  (11,080,881 bytes; 28,857 reassembled chunks).
- Process exit 0, strict identity/result verification and verified Close Game.
- PSBC commit `c96cb63ba2c3b65b22ae76457e726dde7aeb2ace`, including a real
  fragment user-SGPR for ViewIndex and the full 0–31 compiler range.

The prerequisite input-attachment oracle is separate:
`20260916T192041030Z_PPSA99994_ps5vk_0x141a00db047f9`, SELF
`fccbe810989ec007c490a7f99ab56e9dda306a801e7b930a6fccb0d6c614e7c6`.
It measured an actual fragment subpassLoad across a colour-write/input-read
dependency: 4096/4096 pixels matched the CPU reference (hash `e5c69f45`),
with 1024 guard words intact. Image creation alone is not that evidence.

A fresh public-SDK capability run,
`20260916T204935601Z_PPSA99994_ps5vk_0x1467a16affe93`, SELF
`d5d0e1d9946a7e1deaa0c8ac72e6c9e274614264d9b94b1d5d0d43a52cf6ca82`,
measured `multiview=true`, `maxMultiviewViewCount=6` and
`maxMultiviewInstanceIndex=134217727` using the explicitly recorded
`VK_KHR_multiview` feature/property query route. Strict TCP ps5log/1 verification
and Close Game both passed. The historical six-view and instance-floor
execution witnesses below remain intact and supply the dedicated boundary
evidence; the selected CTS leaves alone do not test the maximum instance index.

The three corresponding profile rows were satisfied by that run; the matrix
was **4/62 ready, 58 blockers** at the time (see
[the DXVK v2.6.2 profile section](#dxvk-262-public-abi-capability-probe) for the current
17/62). This does not advertise the Vulkan 1.2 aggregate query
structures or raise `apiVersion` above 1.0. The equivalent KHR fields and the
separate unmet API-1.3 requirement remain explicit. Raw QPA and transport logs
remain private; sanitized identities are recorded here and in the manifest.

## Indirect and indexed draw native acceptance (2026-09-16)

DXVK262-T03 promotes the three Vulkan 1.0 core features
`drawIndirectFirstInstance`, `multiDrawIndirect` and `fullDrawIndexUint32`,
with `maxDrawIndirectCount = 65535` and `maxDrawIndexedIndexValue = 2^32-1`
reported through the public `VkPhysicalDeviceFeatures` / limits queries. The
executable contract is described in [API.md](API.md#indirect-commands); the
evidence below is one candidate build, strict at every step, and the promotion
carries no `apiVersion` change, no conformance statement and no
`VK_KHR_draw_indirect_count`.

**Upstream CTS 211/211 Pass**, run
`20260916T235354084Z_PPSA99994_upstream-cts_0x15088ceacb458`:

- SELF SHA-256: `fb8836a18de9c2900cf842a88eef6c72e0537bc87e4e57471cb3d8b36865f2d6`.
- Selection SHA-256: `266c95632eb658fa9178d3019b9ff57e4da3e785a5bfc54ff1b36b98298984eb`
  (the previous 165 acceptance leaves plus 46 original indirect leaves: the six
  `shader_draw_parameters` first-instance and `draw_index` leaves, and forty
  `indirect_draw` sequential/indexed leaves with multi-command, first-instance
  and four-instance shapes, including the 16-byte index bind and allocation
  offsets).
- QPA SHA-256: `de55e1c79828b06a9e50ccff5628c546c61e3daf16c518b57576548411c98b17`
  (28,417,868 bytes; 74,005 reassembled chunks); process exit 0; no
  NotSupported, Fail, missing, duplicate or unexpected case; verified Close
  Game. Five unrelated diagnostics remain outside acceptance.

A first candidate run (`20260916T234945696Z_PPSA99994_upstream-cts_0x1504ef9d20ea0`)
had every graphics leaf, the 46 new ones included, pass, while all 57 compute
leaves failed at `vkCreateComputePipelines`: the compute compiler adapter fails
closed on any enabled device feature it does not list and the CTS enables the
three newly reported core features. The adapter now lists the three
graphics-only bits, a real-compiler regression test pins a full graphics
device mask, and the run above is the clean repeat.

## Integrated acceptance on the synced tree (2026-09-17)

After `main` (with T03 and its two review fixes) was merged into `t04-final`,
the same frozen selection was run again on the tree this pull request proposes:
**211/211 `Pass`**, zero `Fail`, zero `NotSupported`, no missing, unexpected or
duplicate case, exit status 0, and the title closed and confirmed stopped.
Deployed SELF SHA-256
`8c3a4a5614703cb4f9bb0abda6cbc923ef797edad227e876b15aead972701983` (read back
through FTP before launch), selection SHA-256
`266c95632eb658fa9178d3019b9ff57e4da3e785a5bfc54ff1b36b98298984eb`, run
`20260917T073014110Z_PPSA99994_upstream-cts_0x57e06d347b9`, reassembled report
28,417,865 bytes with SHA-256
`24af364c39c204f97bd3f5e1a6dd77b044c0be00d2a940c92d9f83153ed80a6f`. This is
the T02 + T03 regression on the integrated tree; nothing optional runs inside
that selection, and no feature is advertised by it.

**Public-SDK indirect witness**, run
`20260916T235643977Z_PPSA99994_ps5vk_0x150b05cf0227a`, SELF
`779e882aac0f4eec5276e3b5149cc2e2633a204ee739ddb7eb558f8a2e906b3a`, log
SHA-256 `932f2237de1887e7e2b1157f5a68cbd03c5109280d8e4c542c7ffb80a5880b47`,
`strict_verified=1`, verified Close Game. A 256x256 grid target receives one
triangle per draw or instance in the cell it selects, coloured with the
delivered built-ins or its 16-bit cell index, and every cell is judged from
the linear staging readback (no wrong cell, no stray coverage):

- indirect `firstInstance = 5` over three instances and indexed indirect
  `firstInstance = 200` over two instances (BaseVertex 0xff from
  `vertexOffset = -1`): instance cells 0..2 / 0..1 carry `gl_InstanceIndex`
  5..7 / 200..201;
- strided (32-byte, junk between) and packed multi-draw of four commands with
  one zero-primitive command: cells 0, 2, 3 carry DrawIndex 0, 2, 3 and their
  own `firstInstance`; cell 1 stays clear
  (`PS5VK_MULTI_DRAW_EXPANDED commands=4 drawing=3 draws=3`);
- GPU-generated arguments: host-written zero-vertex commands overwritten by a
  compute dispatch behind a compute -> indirect barrier, consumed by the next
  submission (`firstInstance` 20..23 observed);
- **65535 packed commands in one call**: 65535 distinct pixels, each carrying
  its own 16-bit DrawIndex, pixel 65535 clear
  (`commands=65535 drawing=65535 draws=65535 arenas=66`,
  `PS5VK_GRAPHICS_BATCHES arenas=66 words=2098325`, 65 chained launches);
- 32-bit indices: `0x80000000..0x8000000b` with `vertexOffset = INT32_MIN`
  (direct and through an indexed indirect command), `0x01000000..0x0100000b`
  with `vertexOffset = -16777216`, and a uint16 control, each landing in cells
  0..3 by `gl_VertexIndex / 3`. This is a bounded legal witness of the fetched
  32-bit index and the signed BaseVertex add at bit 31 and at the 2^24
  boundary; it does not touch out-of-range vertices and does not by itself
  cover primitive restart, which the pipeline path still refuses.

**Public-ABI capability probe**, run
`20260916T235800636Z_PPSA99994_ps5vk_0x150c2361e8369`, SELF
`da7c036e93720a514d377e9911760f333041546bf66a6c975e87472adfad6ca9`, log
SHA-256 `5f3e685c15a2e1613dd451e25a88fc1ddb9602342599d2e6938004bb24cb33d8`:
strict verification derived 7 satisfied / 55 blockers from the pinned profile,
device API 1.0.0, five device extensions, the three core features reported
true and the multiview KHR route unchanged; verified Close Game.

All three runs came from the same source state (t03 promotion candidate; the
committed executable sources equal the tested ones). Raw QPA, transport logs
and receipts stay private under `private-captures/t03/`.

## Layer-addressed target measurement (2026-09-15, DXVK262-T02 slice A)

Multiview's normative core is per-view broadcast into one framebuffer layer per
view, so the first question is whether the pinned GFX1013 AGC/DCB target path
can be pointed at a chosen layer at all. Slice A measures exactly that, and
nothing else: no capability is advertised, no feature or property is flipped.

The public path cannot express the shape - `vkCreateImage` refuses
`COLOR_ATTACHMENT` with `arrayLayers != 1` (`native/image_ps5.c:26`) and
`ps5vk_native_target` refuses a non-zero `baseArrayLayer` - so the probe binds
the ordinary single-layer attachment to the **second of two aligned slots** in
one allocation, seeds the slot no attachment is bound to with a sentinel
(`0x5a5a5a5a`), renders normally, and reads both slots back. A layer-addressed
target is expressible only if the render lands in the bound slot and the
neighbouring slot stays untouched.

Built with `PS5VK_LAYER_PROBE=1` and the audited graphics control
(`build/graphics/control-i83c3zcj`, LLPC control from
`experiments/graphics/scene3d.pipe`, `native_input_version=3`) on top of the
audited compute bootstrap (`build/compute/control-frxdmh8o/first.elf`), the
probe artifact `dist-graphics-api/PPSA99994/eboot.bin` sha256
`733928103cc03780e1d9f23ab502efe590318e0d5b7cca35ae92d114e3f8df57` ran once on
the owned console and reported, in a run that ended with
`BYE reason=graphics-api-end`:

```
PS5VK_LAYER_TARGET_PROBE role=color slot_bytes=8912896 bind_offset=8912896 sentinel=5a5a5a5a untouched_mismatches=0 rendered_changed=295611 rendered_oracle=0 valid=1
PS5VK_LAYER_TARGET_PROBE role=depth slot_bytes=8847360 bind_offset=8847360 sentinel=5a5a5a5a untouched_mismatches=0 rendered_changed=2211840 valid=1
```

* **Colour:** the attachment bound to the second slot rendered there (295611
  words changed) and the first slot is byte-for-byte the seeded sentinel
  (0 mismatches). `rendered_oracle=0` is the probe's triangle oracle, which does
  not apply to the `scene3d` control program: the measured question is *where*
  the render landed, not what was drawn.
* **Depth:** the depth attachment bound to its second slot was written across
  the whole slot (2211840 of 2211840 words changed) and its neighbour is
  untouched, so depth selects by address through the same mechanism.
* Both roles therefore need no distinct routing; the host test in
  `tests/test_targets_ps5.c` pins the arithmetic behind that answer - one
  address-carrying register per layer, advanced by exactly one layer footprint,
  with the depth role requiring 64 KiB alignment and the colour role 128 KiB.

Supporting runs: the triangle-program variant (runtime compiler, no depth
attachment) reported `role=color ... rendered_oracle=1 valid=1` on all three
pipeline iterations, and the offline variant that carries the depth attachment
reported both roles valid. Raw runs are in `projects/logging_server/runs`:
`20260915T232549250Z_PPSA99994_ps5vk_0x1006c3bf20055` (sha256
`08bac6623f255ed886dd4d4c3aa67abce55a3bbadb01d95273e325f52330c19f`),
`20260915T232924537Z_PPSA99994_ps5vk_0x1009e5be3c2e8` (sha256
`a711367b11900af3a77aa61a78f8178583f1359cc99d6e0111dbf1043e889e00`) and the
quoted strict run `20260915T233257773Z_PPSA99994_ps5vk_0x100d0018b47fa` (sha256
`53590a744240319f23ea4adb8ba73e9307f09feb6de742819e46444bb3a05b59`). The
console was released with `running=none` confirmed.

What this does *not* claim: anything about view masks, `ViewIndex` lowering,
per-view query results or the CTS leaf set. It answers the address-selection
question that every later slice depends on; the view-mask and `ViewIndex` work
that followed is recorded below.

## DXVK 262 multiview six-view witness

Slice A above established that a layer is addressable. The slices that followed
landed the pieces that address selection needs and then measured them once, end
to end, on the console. The canonical run is
`20260916T050841017Z_PPSA99994_ps5vk_0x11321e8ad91f2` (log SHA-256
`e49e80220af638b69f20f7b8c7503a05316e518a411f64a1df772a50c64070fe`), from the
native artifact `eboot.bin` SHA-256
`92a4073e227f28028e6a57a4a8d14f8e829bdc25c21e7e3ceb5a3c42f33c3c01`, built with
the private diagnostic gate (`PS5VK_MULTIVIEW_DIAGNOSTIC=1`) and the witness knob
(`PS5VK_MULTIVIEW_VIEW_PROBE=1`), deployed with exact FTP readback and
ShadowMountPlus refreshed and verified before the launch.

```text
PS5VK_MULTIVIEW_VIEW_GATE mask=0000003f views=6 view_index_slot=1 vertex_count=3 color_first=02000200 color_step=00000200 depth_first=02001000 depth_step=00000100
PS5VK_MULTIVIEW_VIEW_SUBMITTED draws=1 views=6 mask=0000003f
PS5VK_MULTIVIEW_VIEW_LAYER layer=0..5 view=0..5 pixels=4096 color_expected=4096 color_other_view=0 color_other=0 depth_expected=4096 depth_other=0 depth_remainder_clear=0 depth_remainder_unknown=12288 color_first_foreign=00000000 color_foreign_views=00 depth_foreign_views=00
PS5VK_MULTIVIEW_VIEW_PROBE views=6 mask=0000003f framebuffer_layers=1 extent=64 layers_per_image=7 color=detiled depth=footprint_count load_op=dont_care depth_words_per_layer=16384 guard_layer=6 guard_words=49152 guard_mismatches=0 strict_verified=1
PS5VK_GRAPHICS_API_CLEANUP_COMPLETE
PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1
BYE seq=36 reason=graphics-api-end
```

One native draw with a real `0x3f` view mask rendered into six 64x64 array layers
of one seven-layer image, the seventh layer being a guard. The compiled runtime
vertex stage declared the `ViewIndex` slot in its metadata (slot 1 of 3), the
draw ABI delivered each view's index to it, and the backend emitted one draw per
view in ascending order into ordered, distinct colour and depth targets. Every
layer's DETILED colour is exactly its own view's colour (4096 of 4096 pixels, no
pixel of another view anywhere) and every layer's depth footprint holds exactly
its own view's 4096 words with none of another view's; the remaining 12288 words
per layer still hold the sentinel the probe seeded, and the complete trailing
seventh layer of both attachments is untouched (49152 guard words, 0 mismatches).
Close Game then reported a verified close and the console returned to
`running=none`.

Two earlier runs of this work are **invalidated, not evidence**: the first one
(`20260916T042417717Z_PPSA99994_ps5vk_0x110b5d18f767e`) failed before any
submission because the probe's command-buffer allocation named no command pool -
a harness error, and its pre-submit gate is the only thing it shows; the second
(`20260916T044546978Z_PPSA99994_ps5vk_0x111e1ff6ec7c9`) failed the verdict on the
oracle's UNORM8 expectation for view 5 (green is `5/8*255 = 159.375`, which
rounds to 159, not 160). Neither run says anything about GPU behaviour beyond
what is written here, and the canonical run above supersedes both.

Historical scope: this witness by itself did not justify advertising multiview.
It measured the floors privately without changing public queries or CTS.
The later [native acceptance](#multiview-native-acceptance) adds original CTS
execution and a fresh public KHR query; it does not rewrite this run's identity.

## DXVK 262 multiview instance floor

The same private scene then measured the other multiview built-in's floor:
`firstInstance = 0x07ffffff` (2^27-1, the lowest value Vulkan 1.1 allows as
`maxMultiviewInstanceIndex`), with exactly one instance. The canonical run is
`20260916T061619722Z_PPSA99994_ps5vk_0x116d2e36312a6` (log SHA-256
`b76940fa824c7a991e7b0d9077185002f8d2ff6e631410b1bc356e2d1ae655b2`), from
artifact `eboot.bin` SHA-256
`0d2654c10fd727a2a63b5e3ae23e42a94b83529d4abc6ffe3dde7653411cc1da`, built with
`PS5VK_MULTIVIEW_VIEW_PROBE=1 PS5VK_MULTIVIEW_INSTANCE_PROBE=1
PS5VK_MULTIVIEW_DIAGNOSTIC=1`, deployed with exact FTP readback and
ShadowMountPlus refreshed and verified.

```text
PS5VK_MULTIVIEW_VIEW_GATE mask=0000003f views=6 view_index_slot=2 start_instance_slot=1 vertex_count=4 color_first=02000200 color_step=00000200 depth_first=02001000 depth_step=00000100
PS5VK_MULTIVIEW_VIEW_DRAW vertices=3 instance_count=1 first_instance=07ffffff
PS5VK_MULTIVIEW_VIEW_LAYER layer=0..5 view=0..5 pixels=4096 color_expected=4096 color_other_view=0 color_other=0 depth_expected=4096 depth_other=0 depth_remainder_clear=0 depth_remainder_unknown=12288 color_first_foreign=00000000 color_foreign_views=00 depth_foreign_views=00 instance_failed=0
PS5VK_MULTIVIEW_VIEW_PROBE views=6 mask=0000003f framebuffer_layers=1 extent=64 layers_per_image=7 color=detiled depth=footprint_count load_op=dont_care depth_words_per_layer=16384 guard_layer=6 guard_words=49152 guard_mismatches=0 instance=07ffffff instance_count=1 instance_witness=1 strict_verified=1
PS5VK_GRAPHICS_API_CLEANUP_COMPLETE
PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1
BYE seq=37 reason=graphics-api-end
```

The gate proves that the compiled vertex stage declared BOTH built-in slots
(`view_index_slot=2`, `start_instance_slot=1` of four user-SGPRs) - the instance
index is the compiler's instance id plus the start-instance slot, so a metadata
regression dropping either one fails before anything is recorded. The vertex
stage tests the instance value as an INTEGER, never through a float (2^27-1 is not
representable and would round to 2^27): when the bit test fails it writes a fixed
colour no view can produce, which the oracle counts as an instance failure rather
than as a foreign view. All six layers report `instance_failed=0` with their own
detiled colour and their own counted depth footprint, the trailing guard layer is
untouched, `strict_verified=1`, and the lifecycle closed cleanly with Close Game
verified and the console back at `running=none`.

This historical run measures the instance floor `0x07ffffff`; it did not itself
change public queries or the CTS selection. The later
[native acceptance](#multiview-native-acceptance) supplies the separate public
query and execution evidence used for promotion. The original floor measurement
and its artifact remain unchanged.

## Shader draw parameters promotion (2026-09-15)

`VK_KHR_shader_draw_parameters` is advertised on the still-apiVersion-1.0
device and the extension's feature is reported through the Vulkan 1.1 features
chain. The promoted contract is the direct and single-indirect
BaseVertex/BaseInstance/DrawIndex=0 shape PSBC exports a slot for;
`multiDrawIndirect` and `drawIndirectFirstInstance` stay false, so the four
`draw_index` leaves and the two `*_first_instance` leaves are excluded rather
than reported as NotSupported.

One strict artifact-bound session produced all of the evidence below.

* **Upstream CTS 117/117 Pass** from payload eboot sha256
  `0d797edebf83c783bda7d2cb073ca7392a7751dd8f760c9c8d94154f5cfa41ec` with
  selection hash `656c2d6e31820084385f8948b952673704930d2882d2417b030e84fab3b3b070`
  (`build/cts-run7.json`): `strict_verified=true`, `fail_count=0`,
  `not_supported_count=0`, `missing=[]`. All eight selected leaves Pass -
  `base_vertex.{draw,draw_indexed,draw_indirect,draw_indexed_indirect}` and
  `base_instance.{draw,draw_indexed,draw_indirect,draw_indexed_indirect}`.
* **Public-SDK/native witness** from consumer eboot sha256
  `69ef5b3fa83641febbaad2edd477664135c108cd336c895cf335d1c4dbe77fab`
  (`build/consumer-run6.json`): `strict_verified=true`, `lifecycle_ok=true`.
  Six draw cases reported the exact pinned triples
  (`list_direct` 7/9/0, `list_indexed` 5/3/0, `strip_indexed_negative`
  254/11/0, `list_indirect` 11/0/0, `strip_indexed_indirect` 17/0/0,
  `strip_direct` 21/23/0), each with 1352 covered pixels and `uniform=1`. The
  same six frames were then copied into the linear staging image and read back
  through `vkGetImageSubresourceLayout` with the same encoded words
  (`ff000907`, `ff000305`, `ff000bfe`, `ff00000b`, `ff000011`, `ff001715`),
  `row_pitch=256` and `staged_bytes=16384`. The transfer-destination witness
  reported `clear_word=ff604020 clear_matched=4032 upload_word=ff1e140a
  upload_matched=64 valid=1`.
* **Refreshed public capability probe** from artifact eboot sha256
  `7ddc91102c3bada3111417937abc1af6668864da3649234bd1ee8731e4b7a593`: two
  runs, both `strict_verified`, reporting `device_extensions=4`,
  `satisfied=1`, `blockers=61`, `total=62` (run ids and log hashes are recorded
  in `conformance_inventory/dxvk_v262_evidence.json`).

The prerequisites that made the leaves runnable are bounded and separately
recorded: the R8G8B8A8_UNORM colour attachment that also declares a transfer
destination (`CAP_DST` with real clear and buffer-to-image execution), the
GENERAL layout shape that role uses (`UNDEFINED` to `GENERAL` for a transfer
write, the clear in `GENERAL`, `GENERAL` render-pass attachments, and the
resource-less barrier into the colour-attachment stages), and the linear
transfer-destination staging image the draw module reads back through (real
`vkGetImageSubresourceLayout`, a real `GENERAL`-to-`GENERAL` colour copy
executed as the existing GPU-completion detile, and the matching capability
queries). `tests/test_cts_draw_case_trace.c` walks the pinned sequence on the
host and executes the readback; every neighbouring shape stays fail-closed in
`tests/test_image_copy_clear.c`, `tests/test_texture_format.c`,
`tests/test_vk_device.c`, `tests/test_upload_commands.c` and the consumer
verifier's fail-closed suite.

The DXVK v2.6.2 row for this feature therefore records `cts-pass` and
`native-evidence` while its `api` axis stays a blocker: the profile still
reports Vulkan 1.0 and the DXVK target is API 1.3.204, so the row is not
`profile_satisfied`.

## Independent texture-upload submissions

On 2026-09-14, the original upstream binding-model cases
`primary_cmd_buf.bind.combined_image_sampler_mutable.{vertex,fragment,vertex_fragment}.single_descriptor.2d`
exposed a backend restriction: an upload submission was rejected unless it
also contained a render pass. These cases upload a two-layer RGBA8 image, use
a single 2D view with a linear/repeat sampler, and eventually compare four
rendered quadrants with the unchanged CTS image reference.

Uploads now use the same DMA/cache emitter in an independent submission or a
render prelude. Staging-buffer ranges are flushed, commands remain job-owned,
and tentative image layouts commit only after the exact GPU completion serial.
The standalone path does not create a color target or perform a CPU pixel copy.
Host tests cover multi-layer DMA encoding, no pixel/layout mutation during
preparation, rollback on span/planner/capacity failures and unsupported commands;
the focused test also passes ASan/UBSan.

At the upload-only revision, the three upstream cases completed their uploads
on GFX1013 but next encountered an initial color-attachment barrier: its
write-only access scope was rejected. The follow-up below resolves that
restriction; the later readback and flat-interface fixes let all three cases
pass their full oracles (see below). They are not yet added to the canonical
106-case selection. No sampler limits or conformance claim change.
Upload completion alone is not verification of the uploaded texture pixels.

Regression validation: two runs of the existing **106-case** upstream selection
passed all original oracles, with complete TCP QPA reconstruction, identity
checks, zero tracked allocations and independently confirmed Close Game.
The exact signed SELF, verified by FTP readback before remount/launch, is
`5446ca4b8ff55d6f66444f13c81ff5fd7c9ffc71fa8739876448527f60c0ec55`;
the selection SHA-256 is
`344a278e325846f6918903e48b3262e2551178aab2caa022c67e8dd3539f5b62`.
Runtime identity fields echo the deployed manifest and do not independently
hash the running SELF. Temporary rejection-location instrumentation was removed
before this regression build.

### Initial color-attachment transitions

The frontend and native emitter now share the initial discard-transition
contract. Empty, read-only, write-only, combined color-access scopes and generic
memory-access aliases are accepted; stage/access compatibility, image usage,
ownership and full-subresource checks still apply. A standalone transition
uses the existing job completion and tentative-layout machinery, without
requiring a render pass. Host regressions check scope preservation, invalid
stage/access/usage rejection, real cache-packet emission and deferred layout
commit; the focused tests also pass ASan/UBSan.

The same three original sampler cases were executed again on 2026-09-14:
all complete the separate color-transition submission. The vertex case also
completes its draw, then fails the independent image-readback submission.
The fragment and combined-stage cases instead fail graphics-pipeline creation.
Those were the remaining implementation gaps at this revision, not CTS passes;
the next section records the readback fix. Upstream sources, oracles and the
strict selection remain unchanged.

The existing 106-case selection above passed with signed SELF SHA-256
`7b74ade0920b1c77d42d867f0d3339c71307c6b84c291dd4b17bd1fd7ccc0b67`,
verified by exact FTP readback before remount. Two independent runs passed
all original oracles, strict TCP QPA/identity checks, zero tracked allocations
and confirmed Close Game. A decoded post-close image showed the home menu
without an error dialog; the automated stream was stopped afterward.
The integrated host and sanitizer gates also passed. As above, runtime
identity is a manifest echo, not an independent running-SELF measurement.

### Independent color readback

The bounded full-image RGBA8 readback can now run separately from the render
pass. Both routes use a shared validator for the color-to-transfer transition,
copy region, destination span and host-read barrier. Preparation records only
tentative layout state. The native job emits a cache-flushing GPU completion
packet and waits for its exact serial before the existing CPU detile publishes
the readback bytes; only then can the submission retire. This is not a GPU
detile implementation or support for arbitrary formats/regions.

On 2026-09-14, two identical-artifact runs of the original three-case sampler
diagnostic each returned **one Pass and two Fail**. The vertex-only case now
passes the unchanged upstream bilinear image comparison, covering its separate
upload, color transition, textured draw and image readback. Fragment and
combined-stage cases still failed graphics-pipeline creation at this revision.
Full diagnostic acceptance was false; no failing case was skipped or counted as supported.
Transport reassembly and expected package identity were checked independently
of that failing acceptance verdict, and both runs confirmed Close Game.

The signed SELF SHA-256 is
`596b34c2c484d4c5930608a3c8615431ef1f71cd5105d720a93d4fd70b922b0f`.
Host tests exercise both submission shapes, deferred layout/pixel mutation,
invalid commands, stages/access scopes, spans and regions, and rollback of an
existing render-layout transaction. They pass ASan/UBSan; the integrated host
and sanitizer gates also pass. Two identical-SELF runs of the unchanged strict
106-case regression selection passed every original oracle, identity/QPA
verification, zero tracked allocations and confirmed Close Game. This bounded
result does not promote advertised sampler counts or establish Vulkan conformance.

### Flat fragment interfaces and independent input bases

The sampler pipeline failures had two separate causes. The frontend rejected
the `Flat` decoration outright. After admitting valid flat interfaces, PSBC
still rejected their linkage metadata: standalone SPIR-V input variables had
no assigned driver locations, so different inputs lowered to attribute base
zero. The compiler now uses Mesa's `nir_recompute_io_bases` for fragment inputs
after IO lowering and before RADV shader-info/ACO processing. Vertex descriptor
locations remain unchanged. PSBC is pinned to
`b30c2e380850adc1d76063d4d5498f2c6305cc39`.

The frontend records `Flat`, checks its instruction shape and requires it for
integer fragment inputs. It preserves location/type matching without requiring
VS and FS interpolation decorations to match. A paired owned shader fixture
tests smooth values and flat float, signed integer and unsigned vector values.
Real PSBC compilation, semantic flags, native-header preservation and VS/FS
linkage are checked; malformed decorations and integer inputs lacking `Flat`
are rejected. The compiler's own test additionally checks sparse locations and
both provoking-vertex modes. These host tests do not prove provoking-vertex
selection on hardware.

On 2026-09-14, two identical-SELF executions of the original three-case sampler
selection each passed **3/3**, including the unchanged bilinear image oracle
for vertex-only, fragment-only and shared-stage sampling. Exact deployment
readback preceded remount; TCP QPA/selection/identity verification and Close Game
passed on both runs. Signed SELF SHA-256:
`ea441cb1b82199ee54e0a0ec65892f3ed24dc46f08fa8c0eedc73d39dde91dcd`.
The diagnostic selection SHA-256 is
`b3355e976a932437d98ec69b49705302ed0e3b1c498065b08eb15262f5e9d882`.
This is genuine upstream pixel evidence for these specific cases, not a
general sampler-limit promotion or complete interpolation qualification.
Two further identical-SELF runs passed the unchanged canonical 106-case
selection, strict QPA/identity verification and Close Game. All four runs
ended with zero tracked allocations and successful TCP BYE. A decoded
post-close image showed Home without an error dialog, and the CLI stream was
stopped. The integrated host gate and sanitizer gate passed with this pinned
compiler. Runtime SELF identity remains a manifest echo, separate from the
exact deployment readback check.

## Shared-stage sampler hardware qualification

On PS5 GFX1013 / firmware 12.02, 2026-09-14, the independent public-SDK
consumer passed two identical-artifact shared-stage runs. Build this finite
diagnostic with `python3 tools/build_consumer.py --shared-stage-samplers`;
without that flag the earlier fragment-only diagnostic remains the default.

- Consumer source: `04c7bb36b6229f6857643db8e29f9542c8ec0c7b`.
- SELF SHA-256:
  `35e0d62f82b90938ead0e6feb012b083ea3912d465e6fd836f28fb592d11790c`.
- Complete TCP log SHA-256:
  `903e7b4b91219bc323b61a35ecccc7722876a89423b34f607348d4392ef5575e`
  and `4e3130e2d86e1a7af29d2efc0a1e5bf5e457feb7a97d47cdc8c03048b70551c7`.
- Four sets, binding 7, 24 combined-sampler references per set, using four
  palette textures. The vertex shader sums reverse weights 96–1; the fragment
  shader sums forward weights 1–96 and combines both stage contributions.
  Upload barriers explicitly target vertex and fragment shader reads.
- Four descriptor-update rounds require independent BGRA references
  `6d3a3b2f`, `6d373d35`, `6d42392a`, `6d2e4135`. Each run matched exactly
  471,744 non-background pixels per round with zero bad pixels, 40 paired GPU
  submissions/completions, earlier compute/synchronization witnesses and
  fixed-function presentation, zero tracked allocations and complete TCP BYE.
- Both runs passed independently confirmed Close Game. A decoded Remote Play
  frame after the shared runs showed the home menu without an error dialog.
- The default fragment-only consumer was freshly rebuilt at the same source
  revision and passed its original pixel references and lifecycle checks:
  SELF `edfa661b4e282b06a346beeda2f9235a86861663a74d638780aafa04d10009b3`.

Package identity was checked by exact signed-file FTP readback, followed by a
verified remount and fresh launch. The verifier's `deployment_self_sha256` is
manifest-supplied, not an independent measurement of the running SELF. Both
shared SPIR-V hashes are additionally emitted by the running consumer and
matched to the manifest. Nine focused verifier tests cover actual C-oracle
compilation in both modes and rejection of mismatched profiles/hashes,
missing/duplicate evidence and wrong pixels. The integrated host gate passed.

**Qualification boundary:** 96 descriptors exceed the still-advertised sampler
counts of one. This is backend qualification through public SDK headers, not
a portable consumer, upstream CTS execution or a general limit promotion.
The witness uses procedural vertices, a nearest sampler and four level-zero
RGBA8 textures. Bindings visible exclusively to nonexecuting stages,
statically unused resources, mixed graphics buffer/image resources and
applicable upstream CTS remain pending. No capability is promoted here.

### Wider core visibility masks

The same shared-stage workload additionally passed two identical-artifact runs
per mask on 2026-09-14, using the native backend at
`529d6b139b567309961d65efdf02a10ff1a21b78` and the public consumer's explicit
visibility option. Only VS and FS execute; wider layout visibility does not
instantiate geometry, tessellation or compute shader stages.

```sh
python3 tools/build_consumer.py --shared-stage-samplers --sampler-visibility all
python3 tools/build_consumer.py --shared-stage-samplers --sampler-visibility all-graphics
```

- `ALL` (`0x7fffffff`), SELF SHA-256:
  `65e560ea1df2a42d20f0ff9f809d040063c5aa5f3365a0a30a8963739da75974`.
  TCP log SHA-256:
  `da9395c3a3ff5dfebf5b6b92b61b8309513086deb1e2c8d732249263125b5707`,
  `13687c02f97330ad10634ed25d5440e36f15145e0ee4e8c0d15f199a4c56ba6c`.
- `ALL_GRAPHICS` (`0x1f`), SELF SHA-256:
  `39dd0c374039e2286deabdb282d0816784c6694935d67ea89d3b780df63f7581`.
  TCP log SHA-256:
  `bf61e44356537519db6d5ff5939abaf9fca68e0366dac3fe79a64cfe0ebd929e`,
  `53ea0e5f78e49043e18e4d5ec3344f23290bbf0bac58d4a95e9a292b2a5b28c6`.

Each run matched the four BGRA references above, 471,744 colored pixels per
round with zero mismatches, matched GPU completion, preceding compute/sync
checks, fixed-function presentation, resource retirement and TCP BYE. All
four runs independently confirmed Close Game; a decoded post-close image
showed the home menu without an error dialog. Deployment and runtime identity
use the same distinct mechanisms described above. The verifier binds the
exact visibility mask in the manifest to the runtime record; it rejects
unknown masks, mismatches, omitted fields in new logs and attempts to relabel
legacy evidence. Eleven focused consumer tests pass. The core implementation
also passed real compiler and native-preparation tests with ASan/UBSan:
wider masks preserve machine code, argument ABI, table offsets and contents.

This remains a 96-descriptor backend diagnostic beyond advertised limits, not
upstream CTS or a general sampler-limit promotion. Static-use elimination and
bindings visible only to nonexecuting stages remain incomplete.

## Shared-stage sampler compilation (host evidence)

The compiler now admits combined-image sampler bindings visible to vertex,
fragment or both stages. Real PSBC/NIR/ACO GFX1013 tests compile owned vertex
and fragment shaders that each read 24 elements from four shared sets, using
different weighted sums. Native shader headers and both user-SGPR banks are
checked: each stage receives its compiler-selected slot, while a shared set
uses the same canonical table address. A vertex-only sampling variant requires
four vertex pointers and no fragment pointers. The pinned PSBC compiler conservatively
ORs option-provided sets into its used-set mask: visibility can reserve a
vertex pointer even without an actual vertex shader access. Eliminating
statically unused resources is not implemented by this change. Invalid
stage masks and unsupported resource types remain rejected.

Compiler/ABI tests alone are **not vertex-sampling hardware evidence**.
The separate shared-stage runs above provide bounded GPU output evidence.
No advertised feature or limit is promoted, and the earlier fragment-only
GPU results do not validate vertex use.

## Shared-stage sampler delivery (host evidence)

Native preparation now builds the union of vertex and fragment descriptor
sets. It allocates each table once, even when both stages use it, and preserves
canonical offsets when a table mixes vertex-only and fragment-only bindings.
The queue's predicted-layout checks cover vertex-only sets as well. The
existing emitter supplies the shared address to each stage's compiler-selected
argument slot; draw-owned backing remains retained until retirement.

The allocation/encoding regression checks the exact table footprint and all
96 descriptor records across shared, vertex-only and fragment-only sets.
Missing or stale vertex sets, undefined array elements and unsupported stage
masks fail before allocation. An injected vertex-descriptor encoding failure
releases the allocation without publishing a prepared draw. The focused
ASan/UBSan test passes. Synthetic inactive-set metadata is tested separately;
it does not prove PSBC eliminates statically unused sets.

These host tests cover delivery and rollback; the independent shared-stage
readback runs above establish the bounded GPU result. Applicable upstream CTS
is still pending. Published sampler limits remain unchanged.

## Multi-set fragment samplers: connected backend and hardware diagnostic

The first shared-stage consumer run stopped at descriptor layout creation,
before graphics submission: combined samplers still had a fragment-only
visibility check. The frontend now applies the same legal core visibility-mask
validation as buffer descriptors and retains the exact mask in the signature.
The regression covers vertex, fragment, both, all graphics, all stages and
compute visibility, plus invalid masks and unchanged immutable-sampler/device
guards. Visibility alone does not enable execution in those stages: pipeline
compiler/backend restrictions remain, and GPU qualification is separate.

Vertex texture barrier recording now accepts transfer-write to shader-read
dependencies scoped to vertex, fragment or both shader stages. The host
regression checks preservation of the recorded stage/access masks and rejects
shader-read access scoped only to vertex input, top/bottom of pipe or color
output. This removes a frontend blocker for vertex texture uploads; it does
not itself prove GPU visibility, vertex sampling or broader synchronization.

The canonical descriptor-table layout is shared by compiler options and
job-owned native table encoding. It preserves offsets across stage filtering,
sparse binding numbers, arrays, 16-byte buffer and 48-byte combined sampler
records. Host compiler tests cover mixed layouts; executable graphics resource
tables accept combined image/samplers; the hardware results in this section
cover fragment use only.

The connected runtime carries up to four fragment-table pointers from PSBC
metadata through command recording, per-set generation/signature validation,
pending ownership, predicted image layouts, native descriptor encoding,
flush and register emission. Draw-owned tables survive until retirement.
Missing pointers, incomplete descriptors, generation mismatch, register
collisions and encoding failures are covered by host rejection/rollback tests.
Procedural vertices do not require a dummy vertex buffer.

On PS5 GFX1013 / firmware 12.02, 2026-09-14, the independently compiled
public-SDK consumer passed two identical-artifact runs:

- SELF SHA-256:
  `be0ad0727d920fbdcf12afa8b4be72b259376bef0fb8b199bbd030e683380dc4`.
- Complete TCP log SHA-256:
  `171fd417f4b90ca66ce85272cf980220bb20682599fdee2fd263556e6d3c48bf`
  and `d04d10c9a70562f936056213139e7b95ca623b04cfef6012f2c326c912101145`.
- Four sets, binding 7, 24 sampler-array elements each, bound in descending
  set order. All 96 elements select from four uploaded RGBA8 textures using
  a different deterministic pattern in each of four rounds.
- An owned fragment shader sums samples with distinct weights 1–96.
  Independent CPU references and strict verifier literals require BGRA words
  `914c503b`, `914b4d4f`, `914a4643`, `913a5449`.
  Each round checked 471,744 non-background pixels with zero mismatches.
- Each run also passed the earlier public consumer's compute/storage-width,
  synchronization, 18-frame fixed-function and VideoOut checks: 40 graphics
  submissions total, clean TCP finalization, zero tracked allocations at
  teardown, and independently confirmed exact-title Close Game.

**Qualification boundary:** this new stress fixture deliberately exceeds the
currently advertised sampler limits. It exercises the backend through public
headers, not a portable application obeying the published limits, and is not
upstream CTS or conformance evidence. The advertised sampler counts remain
one. Broader vertex-stage combinations, non-sampler graphics resources, statically unused
individual bindings, broader sampler/state combinations and applicable CTS
must be addressed before a general limit promotion. The diagnostic uses
procedural vertices, one nearest sampler and four level-0 RGBA8 source images;
it does not validate every format, filter, image dimensionality or vertex path
in combination with these arrays.

The first candidate stopped before GPU work because its old physical-format
assertion still treated newly supported roles as absent. The corrected
consumer uses `VK_FORMAT_UNDEFINED` as the unsupported witness. Its identical
physical-query contract now runs in the host device test against the public
entry points and native reporting configuration; an injected lost format role
must fail. This prevents a second independent copy of those assertions from
drifting. The rejected run is not counted as hardware success.

## Observed results

### Packed sampled images and filtering

On PS5 GFX1013 / firmware 12.02, 2026-09-14, four diagnostic binaries each
passed two consecutive identical-artifact runs. The promotion adds sampled
image/transfer-destination support for `A8B8G8R8_UNORM_PACK32`,
`A8B8G8R8_SNORM_PACK32`, `A8B8G8R8_SRGB_PACK32`, `A8B8G8R8_UINT_PACK32` and
`A8B8G8R8_SINT_PACK32`; only the first three expose linear filtering.

| Diagnostic | Cases per run | SELF SHA-256 |
| --- | ---: | --- |
| normalized/sRGB sampling | 23 | `dbad7e701b4203f7ea19080e8f8e0ca1b316c9a954b56a8347ce12d2f6c46607` |
| nearest/linear discriminator | 46 | `cb7b50aea118b015ef8f83e25ca47b4d0e0b6b7e4a320d2ec6747b387e8e4233` |
| typed unsigned sampling | 10 | `854b60687f3a4754cbeb33ea882776963e2d93c64ac9c1d5b3377e3f9a30bac4` |
| typed signed sampling | 10 | `8cdf6ceaea24a72d5291075c1692d00e4e8f93a8253274b39deb395b64ae00b3` |

Complete TCP log SHA-256 pairs, in the same order:

- Sampling: `ebba816ee063d8a0bf68b6783e2af5cd20a5805840242ea91dfc0cb1538f7bd7`,
  `3e36477c514bd364677d03ff58635e0b7089f6f12747c97185359f188f95589b`.
- Filtering: `810fa91c663c89a9d4297c7975a4969221baab3cb770dd925716163f019dbaa3`,
  `c7eb3e0ff7636add9c0befbf65492cbb789bcab4aa91ac8e7cc3264ac3c4fca7`.
- UINT: `d35793639a624ed5ed0c2d61f8307577b3cc60100e1cada86bf6310f3975c313`,
  `995a17d818c7d968fe7ec809772c0565257d7a624af6500f43e79140bdc34831`.
- SINT: `a3a7091c0f41ce651db18d278d340fdca79414987ece48db0289479832233c9e`,
  `5f139f54dd0700a11b67552da37863a7946b13af7d1493a898740fb329bcad96`.

The 178 GPU trials include the earlier formats as regressions. Each float/filter
trial verified 373,248 interior pixels; each integer trial verified 1,036,800
pixels, with zero unexpected pixels. Asymmetric source bytes distinguish
component order, SNORM/sRGB conversion and signed/unsigned shader interfaces.
All three packed filter pairs produce opaque black with nearest and
`0xff808080` with linear. The UNORM readback word is `0xff4080c0` in this
BGRA8 target, not the byte-reversed word from an RGBA8 target.

Every run has complete `ps5log/1` sequence/identity/hash verification and BYE,
six checked compute rounds before and after each graphics trial, matching
submission/completion serials, VideoOut retirement and zero retained allocation
bytes. `run_format_diagnostic.py` separately confirmed exact-title Close Game
after all eight runs. Exact FTP SELF readback and mount refresh were checked
before each diagnostic pair; those deployment checks are separate from the
log verifier. The final Remote Play capture shows the home screen without an
error dialog. Raw captures, telemetry, deployment receipts and binaries remain
private.

These diagnostics use owned, precompiled scene shaders. They validate texture
roles, not runtime shader compilation, every mip level, all sampling precision,
storage images, new color attachments, blits, or upstream CTS conformance.
Runtime compiler/SDK witnesses are listed separately below. In the scoped
format inventory, eight of the 287 baseline deficit cells are now satisfied;
279 require further backend work. The whole reporting matrix still has 620
documented deficit rows, which is neither a CTS score nor a conformance result.

### Sixteen and sparse runtime vertex bindings

Two consecutive PS5 GFX1013 / firmware 12.02 runs on 2026-09-14 used identical
SELF SHA-256
`c48f75fad391c0072130e23953a4386e290769d5de1bd99cb62f66c831102376`.
Their complete TCP log digests are:

- `f1dd3b7408eaf5665ac6eb9b731835857e7aa982f8bb6627b750946dce69b4cb`
- `204ed9c6245b5e687c7f20b32d2242339bab605811c8283ba9ca1193f8c920cd`

Each 1,784-record run reported 16 available bindings and passed four draws:
all 16 buffers, binding 15 alone, bindings 3/15, and the full layout again from
the runtime cache. The observed optimized masks were `ffff/8000/8008/ffff`.
All buffers had distinct values, reversed location-to-binding order, varying
strides/attribute offsets and odd binding addresses. Exact aligned-copy byte
counts were `7552/457/938/7552`. Unused buffers were not allocated or bound.
Each draw produced exactly 471,744 white pixels and zero unexpected pixels.
Three cold compiled pairs and one warm hit were observed.

Both runs also passed compute controls before and after each draw, GPU completion,
VideoOut presentation, BYE and zero retained allocation accounting. The deployed
SELF was read back byte-for-byte; independent Close Game/status checks confirmed
process termination. These are bounded native diagnostic witnesses, not upstream
vertex-input CTS or a public-header-only consumer claim.

An intermediate reporting-only build aborted before device/GPU initialization:
the diagnostic still required `maxVertexInputBindings == 1`. It is classified
as a stale diagnostic invariant, not a GPU failure. The consumer now checks
whether the workload fits the reported capacity; host regression tests cover
both old one-binding and expanded sixteen-binding reports. The final two runs
above include this correction.

The vertex-layout cache correction and multiple-binding preparation described
in [VERTEX_INPUT.md](VERTEX_INPUT.md) have host contract/compiler tests and the
bounded hardware witnesses documented below.
The cache tests use real PSBC compilation and distinguish stride, offset and
format changes while retaining warm reuse for an unchanged layout. Preparation
tests cover 16 binding spans, sparse compiler masks, optimized-away inputs,
alignment copies and allocation failure. PSBC metadata version 11 connects the
optimized binding-use mask to native descriptor preparation. Older runs do not
establish support for this path; only the explicit multi-binding witnesses do.

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

### Runtime sampled images and explicit mip LOD

The GPL-compatible `ps5-opengl` reference informed the earlier runtime-graphics
adapter for exactly one fragment-stage combined image sampler at set 0,
binding 0. The compiler metadata, user-SGPR slot, 48-byte descriptor table,
descending mip layout and cache identity were validated on the host; that
revision rejected other descriptor shapes. The multi-set extension above is
separately qualified. The public `ps5-opengl` mipmap test was also compiled
and run unchanged on the same console; its explicit LOD and generated-mipmap
oracles passed. A temporary PSBC diagnostic then showed that both projects
lower the fragment operation to the same GFX1013 `image_sample_l` instruction.

The first ps5-vulkan diagnostic run
(`20260914T050039610Z_PPSA99994_ps5vk_0x75893aacacda`) was correctly rejected:
it returned only level 0. Comparing its runtime input to the working GPL test
identified the actual harness defects, not a driver defect: all three probe
vertices had zero UVs, and the scissor covered only half the intended domain.
The fixture now assigns the explicit `(0,0), (1,0), (0.5,1)` domain and uses the
full 1920x1080 scissor. Unit tests prevent either input from silently drifting.

The corrected **public-SDK-linked** payload produced run
`20260914T055702207Z_PPSA99994_ps5vk_0x789cca84a41b`, executable SELF SHA-256
`99844fe54a3a7c6b510fdeb870e13d46a34be098e17100131daca47e89c8c1c9`
and transcript SHA-256
`d4706a2930e0a798123e40e27f5ed84143840ffda16c1f647f8fc1221680a8b6`.
It compiled the owned vertex and fragment SPIR-V at runtime, uploaded three
solid RGBA8 levels, verified their descending backing offsets and read back
red/green/blue counts `103680/62208/20736`, with zero unexpected pixels and
`PS5VK_MIPMAP_READBACK valid=1`. The pre/post compute regression passed,
VideoOut presented the matching completion event, all native allocations were
released, BYE was complete, and exact-title Close Game completed in 100 ms.
`tools/verify_mipmaps.py` binds those claims to the transcript, artifact profile
and SELF identity and fails closed on missing colors, storage drift, partial
descriptor traces, transport gaps or false success.

This establishes the bounded shared mip-chain layout, descriptor and explicit
LOD path. It is not broad shader coverage, generated-mipmap support in Vulkan,
anisotropy, or a claim of Khronos conformance.

The next public-SDK-linked pair qualifies the Vulkan 1.0 sampler LOD-bias
floor at both ends of the advertised interval. It reused the exact three-level
red/green/blue resource and runtime `textureLod` shader, changing only
`VkSamplerCreateInfo::mipLodBias`:

- `+2`: run `20260914T061907811Z_PPSA99994_ps5vk_0x79d16d7ca240`, SELF
  SHA-256 `12e7012d0bd51c9af8ad8967416c43391c4243b68e617a14b55313b5a33cc6d1`,
  transcript SHA-256
  `77d4f0b82fdf64089fc6aea481cc54d7ca06aedde2eba2d0e09920437cfb2443`;
  all 186,624 covered pixels selected the blue level.
- `-2`: run `20260914T062019027Z_PPSA99994_ps5vk_0x79e201f1c3c8`, SELF
  SHA-256 `79f249cdf73fdfe20c784bfca3c3e12b71741f737f10c602b7c2f1697bdea559`,
  transcript SHA-256
  `3fcb376fd9224ddfbe14ff0ac86d24651b23fb1dfab2f988db596512435c5241`;
  all 186,624 covered pixels selected the red level.

Both 256-record streams had zero unexpected pixels, passed the pre/post
compute regression, matched VideoOut completion, ended with BYE and zero live
native allocation bytes, and completed exact-title Close Game. The same strict
verifier accepts the unbiased and both boundary profiles and checks the signed
descriptor word when private descriptor telemetry is present. Values outside
`[-2, 2]` and NaN are rejected at sampler creation. This closes one mandatory
graphics-profile limit blocker without claiming the wider native field range.

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
release/writeback before the next dispatch. Later binary-semaphore and event
validation below covers ordered single-queue execution, but does not establish
range-scoped asynchronous execution, mixed compute/graphics barriers,
queue-family transfers, timeline semaphores, the Vulkan memory model or
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
  `6c5d95aa2dd2b9e8179e759e7fa6e01618e8ff2380ef01c1ca5ffcfaf019528c`
  and `39233c916cba5a72bb36c7e78eac1f8988d72a89f841264c38919ec92644f256`

This establishes the cache object, header, import and merge contract only. No
compiled-code record is serialized and no restored cache hit is claimed; the
graphics-derived pipeline-cache cases remain blocked by the pinned bodies'
`D16_UNORM` depth-attachment prerequisite and are covered by host tests that
reproduce their oracles.
### Vulkan 1.0 binary semaphores and events

The public runtime now separates binary semaphore wait/signal/consumption,
host event state, recorded device event transitions, execution dependencies and
the memory barriers carried by `vkCmdWaitEvents`. Queue submissions are expanded
transactionally into ordered frontend/GPU segments; event operations never
reach the AGC backend, signals publish only on retirement, and the fence belongs
only to the final segment.

Two independent launches of the identical upstream payload passed **48/48**
original cases in the synchronization validation selection, including
`binary_semaphore.one_queue`, the 32,768-link
`binary_semaphore.chain`, all four selected event cases and the previous
42-case selection. Both reconstructed complete QPA reports, reported exit zero
and zero live platform allocations, and passed system Close Game.

- Executable SHA-256:
  `098087bc16b4363dbf622365b93695acee8697f74e9daeb776a1b297751312d4`
- Selection SHA-256:
  `c40bce192599fa734b00c8c670dccee43638d01efeb8eb042cc4c037fe2fda88`
- QPA SHA-256 values:
  `c8ac547bd58b28d20c65558bbdaae29046aef883543ca91f7d472de875c24c31`
  and `685714b9a80d6fd289ee0cfff206d7e2d04d968b63cd9f9170d9d7ca687a2fe2`

The isolated public-SDK consumer independently passed twice with executable
SHA-256
`71940c2a5d4a2d219d56eedf15569818e1be3d2c925576a432e9bec4db3831a9`.
It performs RESET→SET→RESET host event transitions, a device
set→wait→reset chain, a two-record binary signal/wait/consume submission and
the existing deterministic compute/graphics readbacks. Run IDs
`20260913T073253015Z_PPSA99994_ps5vk_0x2f437088c857` and
`20260913T073307544Z_PPSA99994_ps5vk_0x2f46d27bd3a1` passed the strict verifier
and independent Close Game checks. The 48-case receipt predates the separately
validated pipeline-cache case; the combined 49-case manifest has not yet been
run as one hardware selection. This is focused native evidence, not a full
synchronization or Vulkan conformance claim.

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
- Fails closed on any unexpected drift, asymmetry (e.g. declared in public header but un-dispatched),
  or regression from the 137/137 structurally wired commands. This is symbol and
  dispatch parity, not a semantic-support or Vulkan-conformance count; explicitly
  unsupported commands remain present as tested fail-closed entry points.
- Enforced on host test runs via `make check` and verified by unit tests in
  `tests/test_command_surface.py` (which includes negative test fixtures asserting failure on
  missing dispatch entries, omitted declarations, or bookkeeping regressions).

The seven Vulkan 1.0 dynamic-state setters added after the indirect-command
slice have host evidence for public/proc-address identity, strict parameter
gates, retained per-command-buffer values, per-face stencil updates, reset and
zero operation-slot consumption. Two byte-identical native runs also passed all
32 selected original upstream monolithic compute/transfer non-interference
cases, as part of an 89/89 focused run, with exact QPA reconstruction and clean
Close Game. Pipeline creation tests independently prove that none of those
seven states can yet be enabled for drawing, so this establishes structural
recording and compute/transfer non-interference, not dynamic blending, stencil,
depth-bounds or depth-bias effects on rendered pixels.

The following structural slice added all six query commands plus
`vkCmdNextSubpass`, `vkCmdExecuteCommands` and `vkQueueBindSparse`. Host tests
prove ordered query reset, the reset-but-unavailable 32/64-bit availability
layout, preservation of result sentinels, query-pool command lifetime, function
identity, and no queue/fence mutation on rejected sparse calls. Real occlusion,
query-result copying, GPU timestamps and sparse binding remain fail-closed. No
new CTS or hardware claim was attached to the original structural slice.
Secondary command buffers and one bounded two-subpass profile left that list
later through the native evidence recorded below.

## Current capability gap ledger (unsupported, not planned)

The 137/137 figure above is structural. This ledger is the current list of
exported boundaries that do not execute the general Vulkan operation their name
implies, so a reader never has to infer support from an entry point merely
being present. Every entry is recorded as unsupported or as explicitly bounded;
none of them is a planned success.

`tests/test_documentation_facts.py` cross-checks this ledger against the
`REQUIRED_FAIL_CLOSED_COMMANDS` set that `tools/check_command_surface.py`
itself defines, so a bounded boundary command cannot silently drop out of it,
and against the parity audit itself, so the reported command counts cannot
drift away from the documents again.

<!-- capability-gap-ledger:begin -->

- `vkCmdBlitImage` — unsupported. No GPU scaling or filtering path exists, no
  blit feature bit is advertised, and the call records nothing.
- `vkCmdResolveImage` — unsupported. Multisample image creation is not
  implemented, and a single-sample copy is never accepted as a resolve.
- `vkCmdClearAttachments` — unsupported. There is no in-render-pass attachment
  clear path.
- `vkCmdClearDepthStencilImage` — bounded, not general. The whole subresource of
  a one-sample `VK_FORMAT_D32_SFLOAT` 2D target clears to a depth value in
  `[0,1]`, and that single shape is qualified on hardware. Stencil aspects,
  combined depth/stencil formats, partial mip or array ranges, rectangles and
  multisample images remain unsupported.
- `vkCmdNextSubpass` — supported in a bounded profile. Exactly two subpasses
  may execute when both use the same colour and optional D32 attachment with
  identical layouts, with either no dependency or one forward `0` to `1`
  dependency. The boundary emits a full graphics acquire. More subpasses,
  attachment/layout changes, input/resolve/preserve attachments and wider
  dependency graphs remain fail-closed.
- `vkCmdExecuteCommands` — supported in a bounded profile. A primary executes
  the secondaries it names, in call order, outside a render pass and inside a
  render pass begun with `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`. The
  children are never flattened into the primary: outside a pass each is
  expanded into its own submission segment, and inside one the pass and the
  children it names are submitted as a single segment because a render pass is
  a single scope. Both paths are qualified on hardware: execution outside
  a render pass by the consumer's bounded transfer oracle, and execution inside
  one by the in-pass draw oracle recorded at the end of this document.
  Nesting, cross-device children, a child that is neither pending nor
  executable, a repeated child without simultaneous use and every render-pass
  scope mismatch stay fail-closed.
- `vkQueueBindSparse` — unsupported. No queue advertises
  `VK_QUEUE_SPARSE_BINDING_BIT`, and the call fails closed without mutating
  queue, fence or semaphore state.
- Real query results — unsupported. Only bounded occlusion pools are created.
  Reset is ordered and observable, but `vkGetQueryPoolResults` reports
  `VK_NOT_READY` with an untouched destination, and occlusion begin/end,
  `vkCmdCopyQueryPoolResults` and timestamp writes are fail-closed.
  `timestampValidBits` is reported as zero.

<!-- capability-gap-ledger:end -->

## Ordered buffer-transfer slice (2026-09-13)

`vkCmdCopyBuffer`, `vkCmdUpdateBuffer` and `vkCmdFillBuffer` are validated
transactionally, recorded through the owned command-operation model and
executed as ordered frontend segments. Host tests cover byte-granular copies,
record-time update ownership, repeated fills, `VK_WHOLE_SIZE`, aliasing and
overlap rejection, cache ranges, and GPU/transfer/GPU ordering.

The standalone consumer links only the staged public SDK. Two identical native
runs used SELF SHA-256
`8d44d9c9b5d70967b397a63cf1fcda4a7c776a489c5a4cf08aadd064ff9d9312`
and verified the same 67-byte result (`FNV-1a32 9a158222`) with intact guards,
complete `ps5log/1` and clean Close Game:

- `20260913T090542583Z_PPSA99994_ps5vk_0x3454301a9571`
- `20260913T090553746Z_PPSA99994_ps5vk_0x3456c96c63e4`

This closes the three named buffer commands at the bounded profile. It does
not establish the full Vulkan transfer family or conformance.

## Deferred indirect execution (2026-09-13)

The three Vulkan 1.0 indirect entry points are structurally present, and host
tests cover recording valid usage, zero-draw behavior, queue-head resolution,
exact invalidation, compute-producer ordering, resource lifetime and the
device-lost failure path. The public-SDK consumer records its existing
deterministic 64-element compute workload with `vkCmdDispatchIndirect`; its
strict verifier requires the exact recording marker and unchanged result and
guard oracles.

Two independent consumer launches used the identical SELF
`c17bb2f7c389be6bc889401936dc8e8ae86cfc96c1cc705dab8799960ee8eb2a`
and passed the hardened verifier plus independent Close Game checks:

- `20260913T101540775Z_PPSA99994_ps5vk_0x3825a44d1f8c`
- `20260913T101550407Z_PPSA99994_ps5vk_0x3827e255bc2d`

The genuine upstream payload passed 57/57 twice, including both original
indirect compute cases. This is native evidence for indirect dispatch and its
compute-write visibility dependency. It is not indirect-draw pixel evidence,
multi-draw support, or a conformance claim.

## Final Vulkan 1.0 structural command slice (2026-09-13)

The command-surface gate now derives all 137 mandatory Vulkan 1.0 core commands
from the pinned registry and reports 137/137 public, dispatched and implemented
symbols with zero asymmetries. That is a structural symbol and dispatch result,
not a semantic or conformance claim. Unsupported semantics are not counted as
supported: blit, resolve, attachment clear, real query results and sparse
binding retain explicit host-tested fail-closed behavior. Multi-subpass
execution later gained the bounded native profile documented below; wider
forms still fail closed. `vkCmdClearDepthStencilImage` left that list
for one bounded shape only, recorded in the section below.

The final image slice adds bounded RGBA8 transfer-role image copy and colour
clear, pitched buffer/image copies, conservative backing-alias rejection,
transactional payload validation and explicit cache maintenance. Queue tests
prove that every backend segment after a frontend transfer is prepared only
when it reaches queue head; preparation failure device-loses without launching
or signaling. `make check`, the ASan/UBSan gate and the regenerated conformance
inventory pass on commit `002743f`.

Two independent native runs used the same SELF SHA-256
`310c662777e6b4ab31a68fd8ae0ad6bb666b09471c2654bd1d909d72fb2754a8`
and selection SHA-256
`4181af6a7032b15fd27d42fcb6eb8aa178d717d3cd66e03607d85e6aecf9c272`:

- `20260913T133316202Z_PPSA99994_upstream-cts_0x42ede9683184`
- `20260913T133335878Z_PPSA99994_upstream-cts_0x42f27e4c8467`

Both passed 93/93 selected genuine upstream cases, reconstructed a complete QPA
with matching identity, and returned through verified Close Game. The four new
image leaves independently judge bounded `vkCmdCopyImage`. Their clear variant
uses the same red value as the destination initializer, so `vkCmdClearColorImage`
still has deterministic host evidence rather than an independent native pixel
oracle. The 137/137 result remains structural coverage, not Vulkan conformance.

## Whole-subresource depth-only clear (2026-09-14)

`vkCmdClearDepthStencilImage` left the fail-closed list for one exact shape: the
whole subresource of a one-sample `VK_FORMAT_D32_SFLOAT` 2D target, cleared to
a depth value in `[0,1]` through the same uniform-DWORD GPU DMA fill the render
pass already uses for its depth load-op clear. A constant depth value is the
same 32-bit word in every texel, so filling the surface is tiling-invariant and
needs none of the pipe XOR pixel equations `src/depth_layout.h` still refuses to
claim. `VK_FORMAT_D32_SFLOAT` advertises `VK_FORMAT_FEATURE_TRANSFER_DST_BIT`
for that clear and for no other D32 role: no transfer-source, sampled or blit
role is claimed, and stencil aspects, partial mip or array ranges, rectangles,
combined depth/stencil formats and multisample images stay fail-closed and
record nothing.

The witness is built so that only a real clear can satisfy it. The render pass
loads depth, so it contributes nothing to the buffer; the explicit clear runs as
its own submission and must retire before the render pass records, which makes
the committed attachment layout the driver's own proof that the operation
reached its GPU completion label. Geometry at z=0.4 and z=0.8 under
`VK_COMPARE_OP_LESS` means two frames differing in nothing but the clear value
must produce opposite results, and the oracle is the existing GPU colour
readback rather than a host packet check.

Two independent native runs deployed the identical `eboot.bin` SHA-256
`570d711ec90ab604eac8eebb68b1b90c7fcd347ea2da168427629c83bafe1320`, re-read
with FTP SELF conversion disabled and matched exactly before each launch:

- `20260914T213105426Z_PPSA99994_ps5vk_0xab95315aa185`
- `20260914T213136782Z_PPSA99994_ps5vk_0xab9c7e582127`

Both are identical in every witness value: frame 0, cleared to `3f800000`,
reported `changed=139968` of `2228224`; frame 1, cleared to `00000000`, reported
`changed=0`; both with `valid=1` and a deliberately nonzero, ignored stencil
member. `PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0` and a clean
`BYE reason=graphics-api-end` preceded verified Close Game in both runs. This
qualifies that one bounded depth shape; it is not general image-clear, blit,
resolve or stencil support.

## Mandatory core feature reporting

The Vulkan 1.0 profile now reports and accepts its mandatory
`robustBufferAccess` bit while rejecting all other unreported core feature
requests. The exact 94-case upstream payload used SELF SHA-256
`b7c485340e03fe66cbba572cdf678c7b7ac2f61643721f63d3eade411298429b`
and selection SHA-256
`5f853eb7d53226b7eda4f758aecaa70be857a80270f213d546d7bcae28015e41`.
Two independent native runs were captured:

- `20260913T181536286Z_PPSA99994_upstream-cts_0x52560710bc64`, QPA SHA-256
  `a6f94c96dc62537558c970e48d97009bd3c5fca60159bd67769e933156a18b24`
- `20260913T181557404Z_PPSA99994_upstream-cts_0x525af1f50de2`, QPA SHA-256
  `9febc48ffd1618ac5b356ab97cb4a02d14eca4aeddb8a61fb1fac97dc93dad08`

Each passed 94/94 genuine upstream cases with zero failures, unsupported cases
or skips, complete QPA reconstruction, exit code zero and verified Close Game.
That earlier case proves mandatory feature reporting and negotiation. It did
not by itself establish executable semantics; the next subsection records the
selected executable buffer coverage added afterward.

### Executable buffer robustness

The next exact payload added 12 original upstream compute scalar `R32_UINT`
robust-buffer cases, covering UBO/SSBO OOB reads and SSBO OOB writes at 1-, 3-,
4- and 32-byte descriptor ranges. The unchanged upstream tests initially found
two pre-execution defects: only one native logical device could join the AGC
session, and descriptor layouts rejected `VK_SHADER_STAGE_ALL`. Both defects
are now covered by host regressions and native CTS execution.

The corrected candidate used SELF SHA-256
`43dd8803a47028ac5086434771d668e119aa662b4483581e72bc3e7ce571a175`
and selection SHA-256
`344a278e325846f6918903e48b3262e2551178aab2caa022c67e8dd3539f5b62`:

- `20260913T183926998Z_PPSA99994_upstream-cts_0x53a3233107e6`, QPA SHA-256
  `8137f2254731850c9b70d59119875df82c12db16815a8b544fe2f1f5847161da`
- `20260913T183952333Z_PPSA99994_upstream-cts_0x53a909315ae1`, QPA SHA-256
  `046002260dfa35dd7bf3db02349368eac110a253f44ef8c7a2f587712220b4f0`

Each reconstructed a complete report and passed 106/106 with zero failures,
unsupported cases or skips, exit code zero and verified Close Game. This closes
the selected scalar compute buffer semantics only; vector, other-format and
vertex-access permutations remain unclaimed until selected and measured.

## Vulkan 1.0 device-reporting audit (2026-09-13)

The reported device surface was audited against the pinned Khronos core tables
and the pinned CTS consumer rules, with the reported values taken from the real
public query paths rather than from a copied table:

- `tools/dump_device_reporting.c` builds its platform with the same initializer
  as the console platform and prints every `VkPhysicalDeviceLimits` member, all
  1.0 feature bits, the extension feature structs, the format matrix and the
  image-format query results.
- `tools/check_reporting_matrix.py` joins that dump with
  `conformance_inventory/core_target.json`, the pinned registry header and
  `vktApiFeatureInfo.cpp`, and writes
  `conformance_inventory/reporting_matrix.json`. An undocumented below-floor
  report fails the gate; only documented blockers are accepted.

Result on the shipped profiles: 138 mandatory limits satisfied, 60 documented
blockers (real frontend restrictions, not inflated), 656 limits not applicable
to a Vulkan 1.0 `VkPhysicalDeviceLimits`, all 110 feature rows consistent with
the code path that enforces them, 140 mandatory format-feature cells satisfied
with 522 documented per-format blockers, 60 format-query consistency
checks, and twelve shader-capability rows satisfied with two precision rows
recorded as not-audited because the compiler's per-mode behaviour is not
measured.

The dump inventory is also checked against every public image and vertex format
named by the implementation. This exposed five supported sampled formats that
the hand-written dump list had omitted; adding those queries removed eight
false blockers without changing runtime capabilities. A future public format
that is absent from the dumper now fails the host gate instead of silently
appearing unsupported in the generated matrix.

Seven unset values that were below the mandatory floor were corrected to the
minimum the specification allows (`subTexelPrecisionBits`, `mipmapPrecisionBits`,
`maxVertexOutputComponents`, `maxFragmentInputComponents`,
`maxSampleMaskWords`, `pointSizeRange`, `lineWidthRange`); the shared validator
now rejects a profile that drops any of them, and `tests/test_vk_device.c`
checks both the values and the rejections.

This is host-only evidence about the report itself. It does not establish
hardware behaviour behind those limits and it does not make the profile
conformant: the documented blockers include the mandatory image-type, attachment
count, descriptor-count, multisample and format-family gaps. No console run was
performed for this increment, so no new hardware claim is made.

## DXVK 2.6.2 public-ABI capability probe

The pinned `VP_DXVK_d3d11_level_11_0_baseline` profile declares Vulkan 1.3.204
and resolves to 62 unique requirements. `tools/derive_dxvk_profile.py --check`
proves that the checked derivative and generated C header retain the immutable
DXVK v2.6.2 source identity. `tools/check_dxvk_profile.py --check` joins each
leaf to public API reporting, reviewed implementation, exact CTS and exact
native evidence with an AND rule across all four axes.

The current checked result is **18/62 satisfied and 44 blockers**. Core
`robustBufferAccess`, the three multiview requirements, the three indirect and
indexed draw features (`drawIndirectFirstInstance`, `multiDrawIndirect`,
`fullDrawIndexUint32`), the clip/cull pair, `fragmentStoresAndAtomics`,
`dualSrcBlend`, `independentBlend`, `sampleRateShading`,
`uniformBufferStandardLayout` (via `VK_KHR_uniform_buffer_standard_layout`) and the four
rasterization and viewport features (`depthClamp`,
`depthBiasClamp`, `fillModeNonSolid`, `multiViewport`) have all four axes. See
[their indirect acceptance](#indirect-and-indexed-draw-native-acceptance-2026-09-16),
[the fragment promotion](#fragment-stores-and-atomics-promotion-2026-09-20),
[the dual-source promotion](#dual-source-blend-promotion-2026-09-21) and
[the rasterization and viewport promotion](#rasterization-and-viewport-promotion-2026-09-21).
The multiview probe uses explicitly tagged equivalent KHR queries, not the
unimplemented Vulkan 1.2 aggregate structs. The API 1.3.204 floor remains
blocked. See [multiview native acceptance](#multiview-native-acceptance) for the
fresh query artifact and the independent execution evidence. A successful
query alone is never enough to satisfy a row.

The optional native consumer is built with:

```sh
python3 tools/build_consumer.py --dxvk-v262-probe
```

It consumes only staged public headers/libraries, creates no logical device and
submits no GPU work. It emits exactly one `DXVK262_REQUIREMENT` record per leaf;
`tools/verify_dxvk_probe.py` derives every expected value, status and aggregate
from the pinned profile and rejects transport gaps, incomplete termination,
wrong artifact identity, missing/duplicate/reordered rows or payload-reported
false greens. Host isolation, link and mutation tests pass.

Two native runs on PS5 firmware 12.02 used the same exact SELF SHA-256
`379b979aee2341a1926c6fc477ee4cb7a0f4758a73e9ba5728be065e85e8899a`:

- `20260915T142012757Z_PPSA99994_ps5vk_0xe2a6477d4bc5`, log SHA-256
  `fa9c2318b38ae2d5c23b3ffcdacdf4a4d0efd54f7a81ad3ece4aa6c1d874236a`
- `20260915T142026363Z_PPSA99994_ps5vk_0xe2a97268c1b8`, log SHA-256
  `78f5349429318a47b33830ccd555427de355957ffcf733257f17b1daac87600f`

Both strict verifications reconstructed all 62 unique rows, observed device API
1.0.0 and three enumerated device extensions, and independently derived the
same 1 satisfied / 61 blocker result. Both streams ended with a complete BYE
and the title was confirmed absent immediately afterward. This is native
evidence of the current report and its blockers, not execution evidence for the
61 missing capabilities and not a DXVK compatibility claim.

## Core sampler addressing, fixed borders and linear filtering (2026-09-13)

The sampler implementation now encodes core repeat, mirrored-repeat,
clamp-to-edge and clamp-to-border modes independently for U/V/W. Its native
GFX10.3 encodings are adapted under GPL-3.0-or-later from the pinned
`blackbearreloaded/ps5-opengl` revision recorded in `LICENSING.md`.

Two runs used the byte-identical SELF SHA-256
`6e33efe473cf7d150d7fe21132413496203348261d83b95159ad5a95f60b234d`:

- `20260913T195835833Z_PPSA99994_ps5vk_0x57f4cbddf1f2`, log SHA-256
  `4fed9e38cbc37dcf582af1da45ba3754a2d2d911a7829fb43a856062ddcdba25`
- `20260913T195933454Z_PPSA99994_ps5vk_0x5802365bfd81`, log SHA-256
  `333a80f2bea6e81ed18397ec2bf72686d11ff89d04f756fdf1486a0ef1e0d804`

Each ps5log/1 stream contained 1,840 ordered records and cleanly ended after
API teardown with native allocation accounting at zero. Four ordered GPU draws
and detiled readbacks each produced exactly 373,248 expected pixels and zero
other pixels: mirrored repeat at UV -0.25, then transparent black, opaque black
and opaque white with clamp-to-border at UV -2.0. The compute regression also
completed before and after every draw. Close Game was confirmed after each run,
with `PPSA99994` absent from the running-title query.

The follow-up linear-filter payload had exact SELF SHA-256
`cb59949794ab13fefb2381c01faaa8b6188d93e2e9c4b7628d9e2214fabe000c`.
Two byte-identical executions produced complete 3,668-record ps5log/1 streams:

- `20260913T202952199Z_PPSA99994_ps5vk_0x59a9aaa3c9de`, log SHA-256
  `69425e45dbca66a347d782969bad3622e5598aed58d21c9ec302833a654b69d3`
- `20260913T203035331Z_PPSA99994_ps5vk_0x59b3b575cbf6`, log SHA-256
  `48b8040c94ed2561cb7afeddd699827184d3a78bacb6c7ee39306691444d977f`

Each repeated the four addressing/border oracles and added four checkerboard
oracles. At UV 0.5, nearest magnification produced opaque black while linear
magnification produced exact 50% gray (`0xff808080`) across 373,248 pixels.
A one-pixel high-derivative witness then used opposite `magFilter`/`minFilter`
pairs: nearest minification produced opaque black and linear minification
produced the same exact gray, proving that the minification selector—not the
magnification selector—controlled the result. Every case had zero unexpected
pixels, the compute regression completed before and after each draw, both
streams ended cleanly with zero native allocation bytes, and Close Game was
verified after each run.

At that stage this established nearest and linear magnification/minification
for the single-level RGBA8 UNORM sampled-image path. That pair of runs did not
establish mip chains, anisotropy, custom border colors, mirror-clamp extension
support, filtering for the additional formats below or Vulkan conformance; the
later explicit-LOD witness above independently closes only the mip-chain item.

### GPL texture formats promoted by hardware evidence

The sampled-format table derived from the exact GPLv3 `ps5-opengl` revision
pinned in `LICENSING.md` now publicly supports twenty additional formats spanning UNORM,
SNORM, sRGB, packed/shared-exponent and 16/32-bit float texels. Texture layout and
buffer-upload planning use each format's 1/2/4/8/16-byte texel width rather
than assuming four bytes.

Two runs used byte-identical SELF SHA-256
`666e441796ae90c1cb06b3dcbacac121f246356235f1525a77c9ffef99dc7e32`:

- `20260913T212518327Z_PPSA99994_ps5vk_0x5cb014618428`, log SHA-256
  `f7b427cc393add2b802d4d85f15e12484d7fa6e97532a5b2f1ed5c900ce970ce`
- `20260913T212630352Z_PPSA99994_ps5vk_0x5cc0d96c574c`, log SHA-256
  `063be2d9a994e36a92be3823088d181d0796ff8c16fbe3672896dd31db53e2c1`

Each complete 1,029-record ps5log/1 stream performed creation, GPU upload,
layout transitions, descriptor sampling and exact detiled readback for the
three newly enabled formats. Every case produced exactly 373,248 expected
pixels and zero others. R8 yielded `(R,0,0,1)`, RG8 yielded `(R,G,0,1)`, and
the sRGB input `0x80,0x40,0x20` decoded to linear UNORM8 `0x37,0x0d,0x04`.
Thirty-six compute rounds surrounded the three graphics cases, resource
accounting returned to zero, both streams ended with BYE and Close Game stopped
the title in 100 ms. At this tranche the new formats were validated with nearest
sampling only; the later filter matrix below supersedes that boundary. This is bounded format
evidence, not general format coverage or Vulkan conformance.

A second GPL-derived tranche added `R8_SNORM`, `R8G8_SNORM`,
`R8G8B8A8_SNORM`, `E5B9G9R9_UFLOAT_PACK32`, `R16G16B16A16_SFLOAT` and
`R32G32B32A32_SFLOAT`. Two independent launches used byte-identical SELF
SHA-256 `de459dfffebcb6a03337d5054e0e6a8263500364e2252b45bb08b460b497d9e9`:

- `20260914T010455716Z_PPSA99994_ps5vk_0x68ac24913444`, log SHA-256
  `6f4e550061b1432c7b51fb1fd3eadc2e8f4c69587f153dbc086896cddc4d6836`
- `20260914T010520787Z_PPSA99994_ps5vk_0x68b1faf20fa6`, log SHA-256
  `6ccd000fec1bf0041c666bba28fbfbedd736dc45788885ae140350ffb12f0f96`

Each complete 3,063-record ps5log/1 stream executed all nine sampled-format
cases, with 373,248 exact pixels and zero others per case, pre/post compute
regressions, BYE and zero retained allocations. The shared-exponent case also
caught and corrected a Vulkan-specific semantic difference from the source
OpenGL table: a format without alpha must select constant one rather than a
nonexistent W component. Close Game stopped the exact title in 100 ms after
each run. At this tranche linear filtering remained advertised only for RGBA8
UNORM; the later filter matrix below supersedes that boundary.

A third GPL-derived tranche promoted eleven further rows from the pinned Mesa
GFX10 format table: `R16_UNORM`, `R16_SNORM`, `R16_SFLOAT`, `R16G16_UNORM`,
`R16G16_SNORM`, `R16G16_SFLOAT`, `R16G16B16A16_UNORM`,
`R16G16B16A16_SNORM`, `R32_SFLOAT`, `R32G32_SFLOAT` and
`B10G11R11_UFLOAT_PACK32`. Together with the previous nine cases this forms
the twenty-format executable probe. Two launches used byte-identical SELF
SHA-256 `db6983e64ee8a5d016641e8e5d227fbab39421699f7377debd519fd905298b9b`:

- `20260914T013155992Z_PPSA99994_ps5vk_0x6a25639cbe9a`, log SHA-256
  `7299f83bed5adde02dc0c34cfda99f803a32c32ebf0717c3f82fb0540be013aa`
- `20260914T013250320Z_PPSA99994_ps5vk_0x6a3209ca7d20`, log SHA-256
  `a0c7564384c20616b14cc6748ee3d1752a7de007914a7864dad2d79796f1a47d`

Each complete 6,792-record `ps5log/1` stream executed all twenty cases with
exact format identity and texel width, 373,248 expected pixels and zero other
pixels per case, plus pre/post compute regressions. Both runs ended with BYE,
zero retained native allocations and exact-title Close Game in 100 ms. The
new reporting removes four mandatory sampled-image blockers; normalized and
floating-point rows whose sampled-image bit is not independently mandatory do
not inflate that count. The later filter matrix below adds per-format filtering
evidence. Those per-format runs did not exercise mip selection; the later
explicit-LOD section above establishes the shared mip path with RGBA8 only.

### Per-format nearest and linear filtering

The twenty GPL-derived sampled formats were exercised with explicit opaque
black/white checkerboards under both `VK_FILTER_NEAREST` and
`VK_FILTER_LINEAR`; the previously established RGBA8 UNORM discriminator
completes the twenty-one-format public table. The probe uses a gray clear
sentinel distinct from both nearest and linear outputs, and accounts for the
four-wide R8 fixture required to keep one-byte rows DWORD aligned.

Two launches used byte-identical SELF SHA-256
`43bd3115d9896b9708a7d33f4dde6403144718d3d1ab204cb8e89c5f53246be6`:

- `20260914T020040371Z_PPSA99994_ps5vk_0x6bb6ddd97c9d`, log SHA-256
  `482eee7766d80c60963d65fe4d2d6a75aee3e8bf40554add67f97424f9bd9136`
- `20260914T020126402Z_PPSA99994_ps5vk_0x6bc195846c29`, log SHA-256
  `beb82e742fc200c48849833f6c468d0309156cc38cd276f460258835e3f85e95`

Each complete 13,572-record `ps5log/1` stream passed all forty trials with
373,248 exact triangle pixels and zero unexpected pixels per trial. Both runs
also preserved pre/post compute regressions, emitted BYE with zero retained
native allocations and stopped the exact title through Close Game in 100 ms.
The reporting matrix therefore promotes the linear-filter bit on every
validated filterable sampled-image row and removes eight mandatory format
blockers. It does not establish mip filtering, anisotropy, arrays or general
image-format coverage.

### Typed integer sampled images

Eighteen GPL-derived GFX1013 format mappings were promoted only after dedicated
Vulkan integer-shader evidence: R/RG/RGBA signed and unsigned formats at 8, 16
and 32 bits. The two payloads use distinct `isampler2D` and `usampler2D`
fragment interfaces, exact typed texels and a gray clear sentinel. Every case
produced exactly 1,036,800 expected pixels—the owned triangle covers half of a
1920x1080 target—with no other non-background pixels.

The unsigned payload had SELF SHA-256
`6b7af00e6fd80558141e83d71c69ee2bd6ec401e046daf96f4318d92f0b2e4bd`:

- `20260914T023601661Z_PPSA99994_ps5vk_0x6da4c30c934c`, log SHA-256
  `c4a8edd59f9a1e523290ed576d071aa728e7e88541826ed5d4d86c25989724d0`
- `20260914T023630716Z_PPSA99994_ps5vk_0x6dab86bfd458`, log SHA-256
  `229e8540cfbc99bb5c532d4a39aeacbfd9a9b1b75bb20b062afd9b6e1c8a18c1`

The signed payload had SELF SHA-256
`aa7951e029010a7d5b64d4a1c8c9a5374666d78ec253a3d350a346e15ea69297`:

- `20260914T023733094Z_PPSA99994_ps5vk_0x6dba0cb7278c`, log SHA-256
  `0be72bc28270f8dc293ba94255570d21cbd0e11ecd05719522ffe393f9218aff`
- `20260914T023754064Z_PPSA99994_ps5vk_0x6dbeee9493c0`, log SHA-256
  `7e8d3b743f722a64d82e982de6d7ef84475a6165379a0b7a96a05ef84e74cde8`

Each run contained 4,152 ordered `ps5log/1` records, pre/post compute
regressions for all nine cases, BYE, zero retained native allocations and an
exact-title Close Game confirmed in 100 ms. The public table consequently
advertises sampled-image and transfer-destination support for these rows plus
single-sample integer sampling. It deliberately does not advertise linear
filtering, which Vulkan does not define for integer sampled formats. This is
bounded single-level 2D evidence, not general image-format conformance.

### GPL integer and packed UNORM vertex formats promoted by hardware evidence

The vertex-format table adapted from the pinned GPLv3 `ps5-opengl` revision
now exposes the `R32`, `R32G32`, `R32G32B32` and `R32G32B32A32` signed- and
unsigned-integer rows as vertex buffers. Two runs used byte-identical SELF
SHA-256 `2d0ad4667084f3127b38ca0d0e6cf4f9fab1339b6e34f0e2af496f86aa2ffba6`:

- `20260913T225439880Z_PPSA99994_ps5vk_0x619065cfab35`, log SHA-256
  `fb9582f5b10926ce0edb18538698d4568e355805d84b43061b22de2b4506d76c`
- `20260913T225542599Z_PPSA99994_ps5vk_0x619f001c2d7a`, log SHA-256
  `e1e42287e1d169eb93f392222496dde13a609641763ebeb608f53546230b237b`

Each complete 1,534-record `ps5log/1` stream compiled the signed and unsigned
vertex shaders at runtime, created eight independent pipelines, fetched three
vertices per case and verified exactly 471,744 white pixels with zero other
pixels. The scalar, vec2 and vec3 cases additionally prove Vulkan's missing
component completion (`0,0,1`) without conflating the integer input category
with the smooth float output passed to the fragment stage. Ninety-six compute
rounds surrounded the graphics cases, resource accounting returned to zero,
both streams ended with BYE and exact-title Close Game completed in 100 ms.

The probe deliberately uses `vkCmdDraw`: it predates the runtime shader
emitter's indexed support (the combination is emitted now but still has no
native witness), while the separate offline-program path retains the validated
indexed support this document records elsewhere. This evidence therefore
promotes eight vertex-format bits, not general runtime indexed rendering or
broad format conformance.

Two subsequent runs promoted the packed `R8G8B8A8_UNORM` and
`B8G8R8A8_UNORM` vertex rows using byte-identical SELF SHA-256
`f76e366d5d96d9eb5234235764216f7a18df197d9b74cdba2f28a450b9bf9029`:

- `20260913T232331033Z_PPSA99994_ps5vk_0x63237537f92a`, log SHA-256
  `cebb92f009c7b1586cebf36f0cc39108735dcec2fab4d39624001ab0cd628755`
- `20260913T232410710Z_PPSA99994_ps5vk_0x632cb2222841`, log SHA-256
  `408613181e9b90de6c45bcda3d4e645daf1367a0db48334e5396eb080b4d8419`

Each complete 3,104-record `ps5log/1` stream executed all ten vertex cases.
The packed cases used raw word `0xffaa5511`; the RGBA shader expected logical
components `(17,85,170,255)/255`, while the BGRA shader expected
`(170,85,17,255)/255`. Both produced exactly 471,744 white pixels and zero
others, proving normalized conversion and the R/B permutation independently of
the framebuffer result. Resource accounting returned to zero, both streams
ended with BYE and exact-title Close Game completed in 100 ms. This adds two
specific packed rows; it does not imply other normalized vertex formats.

The mandatory 10-bit `A2B10G10R10_UNORM_PACK32` vertex row was then added with
byte-identical SELF SHA-256
`87b30f8dd5026ce5c37a830ae5c514208d9fe5dc000f363647d99184ab38802b`:

- `20260913T234617293Z_PPSA99994_ps5vk_0x64618f4c3bad`, log SHA-256
  `85144d4c84a3745999cca8115fb80b662e64aca48a9170687d528be85b4c2dd0`
- `20260913T234715734Z_PPSA99994_ps5vk_0x646f2aae85da`, log SHA-256
  `e6fa278609b91995e55aca5f8cda91efb2eceb640e76498a87d2ccf00acfa92a`

Each complete 3,413-record stream executed all eleven vertex cases. The new
case used raw word `0xbffaa955` and checked logical RGBA values
`(341/1023,682/1023,1,2/3)`, including the distinct two-bit alpha conversion.
It again produced exactly 471,744 white pixels and zero others; both runs had
zero retained allocations, BYE and exact-title Close Game in 100 ms.

### Core 8/16-bit vertex families and unaligned bindings

The runtime compiler dependency was advanced to the reviewed PSBC merge commit
`75f4066fd98ecc0cd0c6aa394ec8e1cdb6de8a88`. It adds the exact Mesa
`PIPE_FORMAT` mappings needed by the Vulkan 1.0 `R8`/`R8G8`, packed RGBA8 and
`R16`/`R16G16`/`R16G16B16A16` vertex families. Two runs then used the
byte-identical SELF SHA-256
`16f96c2e112044d1689de23bb856b3bb303ed196a0bd829f7228d4e763405e96`:

- `20260914T003253606Z_PPSA99994_ps5vk_0x66ec9e7ca947`, log SHA-256
  `ab3f1da00c2f2db5e596625bcb756f5eb5aa899c44b50945fa8035a70591560f`
- `20260914T003415349Z_PPSA99994_ps5vk_0x66ffa6a47149`, log SHA-256
  `98f6db04d6df59354e3ea6c9e91630356ac4f81ea22fc293d5a66050826f19a3`

Each complete 12,724-record `ps5log/1` stream passed all 41 typed vertex
conversion cases. Every case produced exactly 471,744 white pixels and zero
other pixels, with exact component values supplied independently through
specialization constants. Coverage includes UNORM, SNORM, UINT, SINT and
half-float conversion; missing-component defaults; `A8B8G8R8` packed order;
1- and 2-byte strides; and a deliberately unaligned binding offset of 25.

The first diagnostic run showed that a raw GFX1013 structured SRD discards the
two low base-address bits (`ff03ffff` output instead of the expected white
triangle). The accepted implementation therefore stages an unaligned
accessible buffer span into aligned storage owned by the prepared draw. Every
accepted run recorded one bounded 359-byte bounce per case, preserved compute
regressions before and after each draw, ended with BYE and zero retained native
allocations, and returned to the PS5 menu after exact-title Close Game. This is
evidence for these vertex input combinations, not general format or Vulkan
conformance.

The earlier dynamic-buffer descriptor increment removes four of those
limit blockers across the compute and graphics profiles. It implements distinct
dynamic UBO/SSBO pool accounting, Vulkan-order bind-time offset capture,
alignment validation and native descriptor-address adjustment with checked
ranges. Queue priority reporting subsequently removed two more blockers: both
profiles report the required two discrete priority classes, and device creation
maps every valid normalized priority deterministically to low or high. The
signed sampler-LOD-bias implementation removes another graphics limit blocker.
The reporting matrix now records 133 satisfied mandatory limit rows, 65 limit
blockers and 629 blockers overall.

Two byte-identical public-SDK consumer runs then exercised that path on the
owned PS5. Runs
`20260913T192239874Z_PPSA99994_ps5vk_0x55fed4aef4a8` and
`20260913T192252217Z_PPSA99994_ps5vk_0x5601b45b2478` used executable SELF
SHA-256 `2f90929ff30eb069cc66bfdb86d991b0ebaf08c07d1878be2ee522e02c95e0d7`.
Each strictly verified three descriptor sets, two dynamic storage buffers, one
dynamic uniform buffer, offsets `256,256,256`, an independently non-zero
update-time base plus dynamic offset, 64 deterministic compute results and 192
intact guard words. The existing graphics/readback tail also completed, the
`ps5log/1` transcript was complete, and Close Game was verified after both
runs. Transcript SHA-256 values are
`05296089284398c4377224943904a26e3c466808352a441fd5b8cbd9788be77a` and
`cf7f31e5815b9e03a9bebca3627fc578231130e38ce66bdf5ccd29ec0fe29f3a`.
This evidence validates that exact bounded path; it is not blanket coverage of
every descriptor array, pipeline layout or shader combination.

The core-feature reporting audit also distinguishes feature negotiation from
object validation. For every `VkPhysicalDeviceFeatures` member reported false,
`vkCreateDevice` walks the complete structure and rejects a true request before
opening the backend. `tests/test_vk_device.c` exhaustively sets each member in
turn, proving that only the advertised `robustBufferAccess` bit can enable.
This closes the previous 26 `not-audited` rows (13 features in each profile):
the matrix now has 110/110 satisfied feature-reporting rows. It does not claim
that those optional features are implemented; it proves precisely that they
are reported unavailable and cannot be negotiated accidentally.

## Layered sampled images derived from ps5-opengl (2026-09-14)

The GPL-compatible integration of the pinned `ps5-opengl` GFX1013 texture
descriptor contract now covers distinct single-level 1D, 1D-array, 2D-array,
cube and 3D resource types. The Vulkan frontend adds bounded image creation,
view ranges, multi-slice layout and buffer-upload planning; it does not link
Mesa/Gallium or expose an OpenGL API.

Three independently built RGBA8 payloads exercised the exact paths on firmware
12.02. Each used a 64x64 source with one solid color per layer, face or volume
slice, selected three different coordinates in the fragment shader, preserved
the compute regression before and after the draw, emitted BYE, released all
native allocations and returned cleanly through exact-title Close Game:

- 2D array: run `20260914T033255434Z_PPSA99994_ps5vk_0x70bf955a1933`,
  SELF SHA-256 `46c2aa6de518a7f5642c1631273d073ee6193e0174907f816649cae6dbb7591c`,
  log SHA-256 `448fbab89293fde9ef330114159895a1dfdf9f5e8720cf8963e238315296039a`;
  readback was red/green/blue `82944/207360/82944`.
- Cube: run `20260914T033409959Z_PPSA99994_ps5vk_0x70d0ef5f31b1`,
  SELF SHA-256 `c7715a4555326fcb2e8a846bae0acf06d8fd8259a44c63c09efbcb088536855c`,
  log SHA-256 `d201d846a7fce4ada5e6b9b8e9fd4ffa92103737379e94253d4cb338a12d0c6a`;
  readback was `82944/82944/207360`.
- 3D: run `20260914T033459547Z_PPSA99994_ps5vk_0x70dc7af14826`,
  SELF SHA-256 `f54b6cef2328bdc98f5b11e1d371383344baa1863be8739352f7e390cd781955`,
  log SHA-256 `767a86a15d38d6bb7bb6201c3741679b5ff4f7358d131983fdfd92725c3e4325`;
  readback was `82944/207360/82944`.

All three reported zero unexpected pixels. The reporting matrix can therefore
expose the Vulkan 1.0 floors of 256 array layers, 4096 cube dimension and 512
3D dimension without retaining the previous false “unsupported image type”
blockers. Those values are bounds of the implemented descriptor and allocation
contract; these small witnesses do not claim exhaustive execution at the
maximum dimensions, cube arrays or general descriptor arrays. The later 2D
RGBA8 mip witness is documented above and must not be generalized into layered
mipmap coverage.

Two later independent payloads validated the remaining Vulkan 1D image type
rather than inferring it from the 2D layout. Both emitted 407 ordered records,
preserved the pre/post compute regression, reported zero unexpected pixels,
released all allocations and returned through exact-title Close Game:

- 1D: run `20260914T035618318Z_PPSA99994_ps5vk_0x720636c8b4ca`, SELF
  SHA-256 `3f931e503cf55dee3d5a6efad6fb6353801cb797862f12febe52b383140a2f2f`,
  log SHA-256 `8f0782095b4c8ca7277ab88c379a43eadceb3e9efbb043480ce4965caec30aad`;
  three independently colored regions produced red/green/blue
  `82944/207360/82944`.
- 1D array: run `20260914T035708018Z_PPSA99994_ps5vk_0x7211c91f6e55`, SELF
  SHA-256 `80525965827292498f2ab0c882c5ad701c385ddfe8b34d98cd727c8cc45b301c`,
  log SHA-256 `18c8c8b6a0331dd4eb953462cba83894adb0d10fa03b4184a8225cd909716441`;
  three independently colored layers produced the same exact histogram.

The graphics profile consequently reports `maxImageDimension1D=4096` and
removes that real blocker. This is still a bounded contract, not an exhaustive
maximum-sized allocation test.

## Mandatory uniform buffers beside sampled sets (2026-09-14)

A legal Vulkan graphics layout may carry mandatory uniform buffers next to its
combined image samplers. The runtime graphics profile previously rejected every
non-sampler descriptor type, so such a pipeline failed to create
(`vkCreateGraphicsPipelines -> VK_ERROR_FEATURE_NOT_PRESENT` with the runtime
graphics cache reporting `rc=-8`). The profile now admits
`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` and `UNIFORM_BUFFER_DYNAMIC` bindings and the
`Uniform` storage class, delivers both descriptor kinds from one canonical table
and validates them per element; storage buffers remain outside this bounded
profile.

The public SDK consumer's mixed-resource workload exercises that path: four sets,
each with one uniform buffer at the sparse binding 5 and a 24-element combined
image sampler array at binding 7, 96 sampled descriptors plus 4 uniform buffers
in total. The uniform record is rewritten and re-published as a descriptor write
between the four rounds, and the shader scales each set's whole contribution by
it, so a missing, stale or cross-wired record changes the pixel.

One signed artifact, `dist-consumer/artifact.json` profile `mixed-resources`,
SELF SHA-256
`592ac57858b7da62792ad2638523d59a0d9c986f86f00b3cc49951af3041f2b7`,
fragment shader SHA-256
`44b88cb20c7c11baff96ca4f3b82a8faa3b6449bdaca810e6fbd8a9166d6866a`,
was deployed through exact FTP readback and ShadowMountPlus refresh. Two
identical-artifact runs on the owned PS5:

- `20260914T141642731Z_PPSA99994_ps5vk_0x93e114d27a53`, log SHA-256
  `87377f8cbe9fa7b8653f5d9d57ba5a1dfb04c88cfd3c9be731b9e8b1c009f2e8`
- `20260914T141725539Z_PPSA99994_ps5vk_0x93eb0c34f770`, log SHA-256
  `6a7685224e7c63a5b3774d18c6194fc9afa46a9989fc442b8795917770132226`

Each round reported `changed=471744` with `bad=0` against the in-run model, and
the four expected words `381e1f17`, `4b29272a`, `5d2d2c2c` and `6f2c4038` match a
separately computed reference exactly (the verifier rejected an earlier,
incorrectly mapped attempt at those literals). Both runs ended with
`PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1`, a complete
`ps5log/1` transport (`BYE reason=consumer-finite-end`) and a verified
exact-title Close Game.

This qualifies the mixed delivery path for that bounded workload only. No
descriptor or sampler limit is advertised here, storage buffers and other
descriptor types stay rejected, and the reported sampler and sampled-image
limits are unchanged; the limit derivation remains a separate change.

## Per-set descriptor capacity: 96 sampled descriptors in one set (2026-09-14)

The Vulkan 1.0 floors `maxDescriptorSetSamplers = 96` and
`maxDescriptorSetSampledImages = 96` apply to a **single** descriptor set. The
existing 96-descriptor witness spreads its elements over four sets (24 per set),
which bounds the per-stage count but not the per-set one, so it could not support
those two floors on its own.

The public SDK consumer's single-set profile puts all ninety-six combined image
samplers in set 0, binding 7. Element `i` keeps the weight `1+i` and the palettes
of the globally `i`-th element of the four-set workload, so the exact aggregate is
identical and the frozen reference words are the same as for the four-set shape;
matching them shows the per-set capacity rather than a different workload.

One signed artifact, `dist-consumer/artifact.json` profile `single-set`, SELF
SHA-256
`e4857dd69b6545ebfd7ac82b66551140edcab280655b7e2ed5af7f439535fdcd`,
fragment shader SHA-256
`76225631c9e39e2e64be851ebd78e2ceed3cc0da6af610a963075ab4f8cc05b0`,
was deployed through exact FTP readback and ShadowMountPlus refresh. Two
identical-artifact runs:

- `20260914T145418819Z_PPSA99994_ps5vk_0x95ee5c423c68`, log SHA-256
  `6b16e9bd33748af9da12edb1bdd8907440d9e0d3bd7c7ecc202152122a2d7506`
- `20260914T145426305Z_PPSA99994_ps5vk_0x95f01a62fd68`, log SHA-256
  `7d999a4a850c111b523e77e4f4f530995044e4fb7e76880268f144f1c1f2cb11`

Each round reported `changed=471744` with `bad=0` for the expected words
`914c503b`, `914b4d4f`, `914a4643` and `913a5449`, and both runs ended with
`PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1`, a complete
`ps5log/1` transport and a verified exact-title Close Game.

This is bounded evidence for one set of 96 combined image samplers in a
fragment-only pipeline with four update rounds. It does not advertise a limit by
itself, says nothing about other descriptor types or about other stages, and is
not a conformance claim; the reported limits remain unchanged in this change.

## RGBA8 uniform texel buffer promotion (2026-09-15)

`VK_FORMAT_R8G8B8A8_UNORM`, `VK_FORMAT_R8G8B8A8_SNORM`,
`VK_FORMAT_R8G8B8A8_UINT` and `VK_FORMAT_R8G8B8A8_SINT` now publish
`VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT` in the graphics profile, and
`vkCreateBufferView` accepts them for
`VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT`. This is the enablement half of the
role implemented on `main` by the four-byte-RGBA8 slice; creation and reporting
move together, so the reported set and the creatable set stay identical.

The encoding is not new: a GFX10 buffer descriptor carries the combined
DATA_FORMAT/NUM_FORMAT value that the sampled-image descriptor already stores
(8_8_8_8 UNORM/SNORM/UINT/SINT are 56/57/60/61), and the completion word is the
row's own selector meaning, `(X,Y,Z,W) = 0xfac` for a four-component row
against `(X,0,0,1) = 0x204` for the one-component R32 rows.

### The witness

A new owned compute shader fetches one texel per invocation with
`texelFetch(samplerBuffer)` from an RGBA8 buffer view, rounds every fetched
channel back to its 8-bit value, packs the result as `R | G<<8 | B<<16 | A<<24`
and XORs it with the integer input buffer. The CPU oracle predicts every output
word from the bytes written into the texel buffer, so a wrong element word, a
wrong channel completion, a scalar-replicated `(X,X,X,X)` mapping or an
off-by-one record count each change the expected word. The existing
`R32_UINT`/`R32_SINT`/`R32_SFLOAT` resource-ABI path is unchanged and keeps its
recorded evidence; the new variant is selected by
`tools/build_consumer.py --texel-rgba8` and
`tools/run_consumer.py --texel-rgba8`, whose strict verifier also requires the
device to report the bit it is about to use.

The first attempt at this witness failed at indices 37..63 and the defect was
in the C oracle, not in the driver: the per-channel expressions were not masked
to eight bits, so `i*7+3` spilled from the blue byte into alpha while the GPU
returned exactly the bytes written. The oracle now masks each channel, and the
runs below are the corrected artifact.

### Two identical runs

Deployed `eboot.bin` SHA-256
`848dae57ea7e6e19d1ef608bfea219606e9e7821b8952c1324674b4fd55b8596`, re-read
with FTP SELF conversion disabled and matched exactly before each launch, with
ShadowMountPlus restarted and verified before each launch. The RGBA8
`texelFetch` SPIR-V SHA-256 is
`e14a6bb98ef0a9abf725b1c30bb132cff22b5b2eb149eb7625201561b2e713a4`.

- `20260914T234125783Z_PPSA99994_ps5vk_0xb2b1fcb0ecca`, log SHA-256
  `18cf44694da9a1a52bb69027ed39e26e878bc6156b6facadacebfc968789238c`
- `20260914T234134074Z_PPSA99994_ps5vk_0xb2b3eae2b6ae`, log SHA-256
  `58b85c385daef9b95ece359faad8e43406c027791c1d41fe01634f9c4a8f930a`

Both runs are byte-identical in every witness line and both passed the strict
verifier (`strict_verified=true`, `lifecycle_ok=true`):

- `PS5VK_CONSUMER_TEXEL_RGBA8_FORMAT format=r8g8b8a8_unorm
  buffer_features=0x00000048 uniform_texel_reported=1` — the device reports
  `VERTEX_BUFFER|UNIFORM_TEXEL_BUFFER` for the format being used;
- `PS5VK_CONSUMER_TEXEL_RGBA8_SUCCESS format=r8g8b8a8_unorm texels=64
  channels=4 packed_rgba_order=1 mismatches=0 guard_words=192
  guard_mismatches=0`;
- `PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS ... elements=64 mismatches=0`;
- `PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1` and
  `PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1`;
- `BYE seq=484 reason=consumer-finite-end` in both.

The title was closed and `running=none` was confirmed independently by
`tools/control.py status` and by `tools/night_supervisor.py status`.

This directly qualifies the shared four-component uniform-texel-buffer path
with UNORM. The SNORM/UINT/SINT rows compose that result with their earlier
format-specific conversion and shader-interface witnesses because all four
rows use the same bounded descriptor path and four-component completion. The
new payload did not fetch each of those three formats independently. This is
not a CTS result or conformance claim, and it does not extend to storage texel
buffers, blit, resolve, attachment clears or any other format family.

## GFX10 occlusion-counter measurement (2026-09-15)

The query family could not start honestly without device facts that the desk
research explicitly left NOT MEASURED: how far the render-backend geometry
actually extends, which backends are enabled, and whether a real occlusion
counter can be produced at all. This section records the bounded native
measurement that answered all three for this console and produced a real
counter. It adds **no public query behaviour**:
`vkGetQueryPoolResults` still reports `VK_NOT_READY`, `vkCmdBeginQuery` and
`vkCmdEndQuery` still fail closed, and nothing is advertised.

### The probe

Payload scenario `PS5VK_GRAPHICS_SCISSOR_PROBE=15` runs the existing bounded
scene and, around its draws, emits the GFX10 occlusion event pair through
`ps5vk_graphics_occlusion_event` (`src/graphics_sync.c`, host-asserted in
`tests/test_graphics_sync.c`):

```text
PKT3_EVENT_WRITE (header 0xC0024600)
EVENT_TYPE(ZPASS_DONE=21) | EVENT_INDEX(1)   -> 0x0115
address: slot base for the begin write, slot base + 8 for the end write
```

Each enabled render backend writes a 64-bit start at `16*i` and a 64-bit end at
`16*i + 8`, and sets bit 63 of both words when the dump lands, so the set of
written pairs IS the enabled mask and the result is the sum of the per-backend
deltas. The slot is a separate zeroed, 64 KiB-aligned direct-memory arena sized
for 64 pairs, so a pair no backend owns stays zero and is reported as
unavailable instead of being read as a fabricated result.

Two coherence and evidence rules matter here and are enforced in the payload,
not just asserted in prose. The slot arena is CPU-zeroed and the memory is
non-coherent, so the zeroed lines are flushed before submission and the slot is
invalidated again only after the exact completion label; otherwise a dirty CPU
line could obscure or overwrite what the hardware wrote. And the per-pair and
summary rows are emitted **after** `PS5VK_GRAPHICS_COMPLETED` for the same
serial, so the log itself shows the readout happened after the producing
submission retired.

The evidence is verified by `tools/verify_occlusion_probe.py`, which is bound
to the build manifest and the artifact it names (stage, `submit_enabled`,
`scissor_probe=15`, `termination=shell-close-after-cleanup`, and the
`files/eboot.bin` digest), checks the HELLO identity fields, requires one
serial across begin/end/pairs/slot with its completion row in the right place,
requires exactly one clean `PS5VK_PLATFORM_CLOSE`, and derives validity from
the slot invariants (unique contiguous indices from `first_pair`, mask and
`highest_pair` consistency, availability bit 63 in both words, and the deltas
summing to the reported counter). `tests/test_verify_occlusion_probe.py` holds
19 mutation fixtures that make each of those checks fail, and `make check` runs
them.

### Measured result, two identical runs

Deployed `eboot.bin` SHA-256
`45c096097b9837f786d6d619db4dcd9548de8f89b82ca1c80c097063d37391fd`, re-read
with FTP SELF conversion disabled and matched exactly before each launch, with
ShadowMountPlus restarted and verified before each launch:

- `20260915T061712316Z_PPSA99994_ps5vk_0xc84ad5eb323d`, log SHA-256
  `efc86b5b54aa7639f6664e430cc38cdc86fdfdf6d17e038031150b5936efe7ff`
- `20260915T061721229Z_PPSA99994_ps5vk_0xc84ce9235302`, log SHA-256
  `de93570bf2dc230ecebdad0f14346cdfa35fef952219b7d6cf2379eaf3810d9f`

Both runs report the identical result, and `tools/verify_occlusion_probe.py`
validated each run and required the two to agree exactly:

- `PS5VK_OCCLUSION_PROBE_SLOT serial=7 pairs=64 available=16 first_pair=0
  highest_pair=15 mask_lo=0000ffff mask_hi=00000000 counter=139968`, which
  reports measured facts only; there is no self-certifying validity flag, and
  the verifier derives validity from the invariants it checks itself;
- sixteen `PS5VK_OCCLUSION_PROBE_PAIR` rows, indices 0..15, each with
  `begin=8000000000000000` (start 0 with availability bit 63 set) and end
  values whose deltas sum exactly to 139968;
- `PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0` and
  `BYE seq=263 reason=graphics-api-end`.

So, on this console: the enabled render-backend mask is the contiguous low pair
range 0..15 (`mask 0x0000ffff`, highest written index 15), which makes
`enabled_render_backends = 16` and `max_render_backends` **at least 16**; the
measurement bounds it from below and does not establish the architectural
maximum. A real GPU occlusion counter of 139968 was produced for the bounded
draw at default (non-precise) precision, and this is one bounded measurement
rather than a general claim about counters. The title was closed and
`running=none` was confirmed independently by `tools/control.py status` and
`tools/night_supervisor.py status`.

### What this does and does not establish

It establishes the begin/end packet form, the 16-byte-per-render-backend slot
layout, the bit-63 availability rule and the device geometry: the contract a
real occlusion query has to be built on. It does not implement query results,
does not cover timestamps or pipeline statistics, does not cover the
`WAIT`/`PARTIAL`/64-bit result flags, and is not a CTS result or a conformance
claim. `DB_COUNT_CONTROL` precise counting is not exercised here; the measured
counter is the default-precision form.

Two defects found on the way are worth recording. First, the initial packet
header placed the opcode in the low bits instead of bits 15:8, which is a
different packet entirely: the completion label never arrived and the payload
stalled after `PS5VK_GRAPHICS_SUSPEND_POINT` with no BYE. That is the same
symptom earlier diagnostics attributed to a register-read probe, and it is why
the builder now lives in the host-tested packet helper where `make check`
asserts its exact words. Second, the register-probe block is skipped for this
scenario, because its `COPY_DATA` register reads are unrelated noise for an
occlusion measurement and previously cost a console round trip on their own.

## Secondary execution inside a render pass (2026-09-15)

### The oracle

The consumer renders the SAME triangle twice into the SAME
`VK_FORMAT_B8G8R8A8_UNORM` 1920x1080 attachment, with the same pipeline and the
same dynamic viewport and scissor. The only difference is how the draw reaches
the pass. The first pass is begun with
`VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS` and the primary records **no
draw of its own**: it names one inherited continuation secondary. The second
records the same draw **inline**. The two readbacks must be identical.

A second continuation secondary is recorded against the same scope with a
half-height viewport and **never named**. Equality therefore fails in every
wrong direction: if the named secondary did not execute, the first readback
keeps only its clear; if the driver also executed the unnamed one, its squashed
triangle paints pixels the inline result does not have; if either drew
something else, the images differ. The verifier additionally pins the image, so
two wrong-but-equal results cannot pass either. Each pass is preceded by a
sentinel pre-fill that neither the clear nor the draw produces, so a surface
nothing wrote cannot read as a cleared one, and the second measurement cannot
be the first one's leftovers.

### Measured result, two identical runs

Artifact `dist-consumer/PPSA99994/eboot.bin` sha256
`383484e752f2e511d08ebd0e3326683c13c28018c6a69b274ef9d640c577dc8a`, deployed by
FTP and re-read with SELF conversion disabled with an exact match,
ShadowMountPlus restarted and verified before each launch.

- `20260915T090900132Z_PPSA99994_ps5vk_0xd1aac7365e07`, log sha256
  `00ba815d938a5f377798a614168d2fae851d1ae36ef3f3f9d88c1ebfd531fd36`
- `20260915T090927164Z_PPSA99994_ps5vk_0xd1b112905834`, log sha256
  `a87e4fc27c2671ba1cf269d043081cc294ea3a5fee1c2c2353c06ba9834a1eee`

Both runs are byte-identical in the witness line:

```
PS5VK_CONSUMER_INPASS_SECONDARY_SUCCESS named=1 unnamed_recorded=1
executed_changed=471744 control_changed=471744 bad_alpha=0 bad_sum=0
executed_hash=77abc830 control_hash=77abc830
```

Both ended with `BYE seq=506 reason=consumer-finite-end`,
`zero_tracked_allocations=1` and `resources_retired=1`, and the title was
confirmed stopped independently. 471744 is exactly the triangle area the
eighteen-frame readback contract already pins, `1920 * 1080 * 91 / 400`,
measured independently here.

The log also shows the composition directly: the secondary-executed pass
records `PS5VK_GRAPHICS_PREPARED serial=55 draws=1` for a primary that recorded
no draw at all, so that draw reached the backend through the composed segment.

### What this does and does not establish

It establishes that a secondary recorded for render-pass continuation executes
inside a primary's render pass and produces exactly the image the same draw
produces inline, on the already qualified one-colour-plus-D32 profile. It does
not by itself establish multiple subpasses, `vkCmdNextSubpass`, input or
resolve attachments, multisampling, query inheritance, or any conformance
claim. The separate two-subpass oracle below establishes only its own bounded
profile.

A zero-body render pass cannot be smuggled in through this path either: a pass
whose only content names empty secondaries executes nothing, and recording,
submission and the backend each derive the work that will actually execute
rather than counting commands. Naming an empty secondary beside one that draws
stays legal and keeps its place in the order.

One defect is worth recording, because only the console could find it. The
first version of this scenario rendered the control into the second
presentation image, and `vkQueueSubmit` refused it with `VK_ERROR_UNKNOWN`
because that image was still display-busy from the last presented frame. The
driver was right and the scenario was wrong; both passes now use the same
attachment, which also makes inline versus secondary the only difference
between the two measurements.

## Native two-subpass execution (2026-09-15)

The finite public-SDK consumer records a render pass with two subpasses over
the same `VK_FORMAT_B8G8R8A8_UNORM` colour attachment and layout. Subpass `0`
draws a full-size procedural triangle, `vkCmdNextSubpass` crosses one explicit
forward `0` to `1` dependency, and subpass `1` draws a centred half-size
triangle. A one-subpass control records the same two draws in the same order;
their complete 1920x1080 readbacks must have the same pinned FNV-1a hash.

Three additional controls execute the first draw only, the second draw only,
and the two draws in reverse order. Every incorrect sequence must differ from
the ordered result. The reversed result intentionally equals first-only: its
final full-size draw completely overwrites the smaller draw. The verifier pins
all five hashes independently, requires exactly one boundary marker for
subpass `1` carrying the measured 10-word acquire, requires all 47 graphics
submissions in order, and rejects missing, duplicated or reordered evidence.

Two independent launches used the identical SELF SHA-256
`374e9e59dc23c981f61dbdc62f71eecd5db28090d9eea97b384ee5f2785743ed`:

- `20260915T124637265Z_PPSA99994_ps5vk_0xdd8ad6960977`, log SHA-256
  `806bb9e9086a224bb0b4b8dc0efe752a0159ef9c853ee4770f8c41db26505046`
- `20260915T124759988Z_PPSA99994_ps5vk_0xdd9e18e67fef`, log SHA-256
  `48a45f271e17d62762b51d71db0866f85dbb372d388b7416339e5dc1bec6d5c0`

Both produced the same witness:

```text
PS5VK_SUBPASS_BOUNDARY serial=57 subpass=1 words=10
PS5VK_CONSUMER_TWO_SUBPASS_SUCCESS multi=84cc0cb3 ordered=84cc0cb3
first=77abc830 second=fa6b3a7a reversed=77abc830 changed=471744
negative_distinct=1 bad_alpha=0 bad_sum=0
```

Both retired the two render passes, three pipelines and two framebuffers before
the presentation surface; both then reported
`zero_tracked_allocations=1`, ended with
`BYE seq=560 reason=consumer-finite-end`, and were independently confirmed
stopped. This establishes real GFX1013 execution and attachment ordering for
the exact shared-role two-subpass profile. It does not establish input,
resolve or preserve attachments, attachment/layout changes between subpasses,
more than two subpasses, arbitrary dependency graphs, multisampling or Vulkan
conformance.

## Direct uniform-texel format matrix (2026-09-15)

A finite native consumer directly fetched every enabled uniform-texel-buffer
format through `samplerBuffer`, `usamplerBuffer` or `isamplerBuffer`. The 41
cases span normalized, integer, half/float and packed-float conversions with
1-, 2-, 4-, 8- and 16-byte elements. Each case compared all four returned
components with an exact CPU oracle, including Vulkan component completion;
the packed `VK_FORMAT_B10G11R11_UFLOAT_PACK32` case additionally pins its
hardware decode and alpha completion.

Both runs used the same SELF:
`ef196ce8fbb22ef33f0c15f60dbd9a801d7d7624c0bb87407a1e1e79cfa7e8b1`.

- `20260915T114720138Z_PPSA99994_ps5vk_0xda4ea3a518eb`, log sha256
  `94a07bde50440227191ec5a44f4e47c76a74a5bfa0b985a077ab1bd436174646`
- `20260915T114908594Z_PPSA99994_ps5vk_0xda67e478833a`, log sha256
  `c309d5db753fe0b25b6491f336eb700281ed16fa71af51313f782583f6b709f1`

Each run reported 41/41 exact cases, 164/164 component words, zero guard
mismatches and one submitted/suspended/completed dispatch per case. Both
streams ended with the finite consumer's complete `BYE`, and exact-title Close
Game independently confirmed the process stopped. The qualification applies
only to uniform texel buffers; it does not imply storage-texel-buffer support.

## Rasterization and viewport promotion (2026-09-21)

The four DXVK262-T05 requirements - `depthClamp`, `depthBiasClamp`,
`fillModeNonSolid` and `multiViewport` - are advertised by the shipping profile.
The measurement switch the earlier witnesses needed is gone; `native/platform_ps5.c`
reports the four bits itself.

The frozen acceptance selection grew from 304 to 362 cases with the 58 leaves
these features own, and the shipping build passes all of them:

- run `20260921T165514411Z_PPSA99994_upstream-cts_0x15ea42202ec6c`,
  **362/362 Pass**, zero Fail, zero NotSupported, no missing, unexpected or
  duplicate results, `strict_verified` and `lifecycle_ok` both true, title
  closed and confirmed stopped.
- deployed SELF SHA-256
  `449782bb258a51dc2f72b376c6e88e4b1122efeec8a0a214f532de390c7b0caa`, read back
  exactly through FTP before launch.
- selection SHA-256
  `26862d1a5eb9ca93121797e1e9649b45ad753c319e3ee75624bbec3f0d995cba`;
  reassembled report SHA-256
  `ccfd626cac68cba95a6322408257c17c789fba4972274c4cc425b5c5283a66e7`.

The public-ABI capability probe was rebuilt against the promoted profile and
re-run, so the device's own query route confirms the advertisement rather than
the driver's source doing it: run
`20260921T163032548Z_PPSA99994_ps5vk_0x15d4b1d4b89b8`, artifact
`6c49df2e42461249b9bbfd65530d17381428bdb5f1bdeb1d1b895b7aa68d28c5`,
**15 of the 62 profile requirements satisfied** and 47 blockers, up from 11.
On `main` alone the DXVK matrix therefore reads 13/62 ready with 49 blockers;
this branch also carries T06's `fragmentStoresAndAtomics` and `dualSrcBlend`
promotions, so the merged tree reads **15/62 ready with 47 blockers** and a
frozen acceptance selection of 462 cases (the 362 measured here plus T06's 98
dual-source leaves and its two fragment side-effect leaves).

Per requirement the applicable upstream leaves are: `depthClamp` all eight,
`depthBiasClamp` its only two, `multiViewport` all twenty-two,
`fillModeNonSolid` all twenty-eight it can run. Its twenty-ninth,
`rasterization.line_continuity.polygon-mode-lines`, stays a diagnostic and is
not a defect in the feature: Amber's backend allocates its host-accessible
buffer demanding `HOST_VISIBLE|HOST_COHERENT` with force_flags, this profile
advertises one memory type without `HOST_COHERENT` and deliberately reports
non-coherent memory, so the leaf fails at memory selection before any
rasterization. Advertising that bit would be a false claim; host-coherent
memory is a separate capability question.

This is focused validation of four requirements, not Vulkan conformance, and
release review remains pending.

## Rasterization and viewport witnesses (2026-09-18)

First hardware execution of the DXVK262-T05 rasterization/viewport witnesses,
one bounded console window, two deploys of two payloads built from
`t05-final` `ca70b5b`. Nothing is advertised by this run: it is the evidence the
advertisement would have to rest on, and it is partial, so the platform mask is
untouched.

| | Shipping merge regression | Raster measurement |
|---|---|---|
| package | `consumer-shipping-ca70b5b` | `consumer-diagnostic-ca70b5b` (`PS5VK_RASTER_DIAGNOSTIC=1`) |
| eboot sha256 | `8c6fee5f48a8e31e5a95516dab92704285bc674d1489c895551d33942f60baed` | `9a0056a564567baf9d5aa519e06b80697de2c089af95838e65c962a41286ee8c` |
| run | `20260918T124926937Z_PPSA99994_ps5vk_0x657da0cca63c` | `20260918T125021109Z_PPSA99994_ps5vk_0x658a3dafb094` |

**Shipping regression.** `strict_verified`, `lifecycle_ok`, `clean_tcp`, 63
`PS5VK_GRAPHICS_SUBMIT`, 18 fixed-function frames, 10 indirect draw cases, 6
draw-parameter cases, 4 sampled-graphics rounds and the depth-reject oracle. The
reconciled draw path emits the base registers, the T05 raster block, the
viewport banks and T04's primitive-restart pair on every draw, and the whole
consumer table ran on it without a change. The raster witnesses reported
`depthBiasClamp=0 depthClamp=0 fillModeNonSolid=0 multiViewport=0 maxViewports=1`
and skipped with `reason=features_not_reported` - the required shipping
behaviour while nothing is advertised.

**Raster measurement.** The device reports
`depthBiasClamp=1 depthClamp=1 fillModeNonSolid=1 multiViewport=1 maxViewports=16`
under the measurement build, so the feature and its mandatory limit now come
from the same mask and the witness runs for the first time. Of the 31 cases,
**25 report `valid=1` and 6 do not**: `polygon_fill`,
`polygon_line_cull_front`, `polygon_point_cull_back_cw`,
`polygon_line_cull_back_ccw`, `clamp_disabled_narrow_probe` and
`clamp_enabled_narrow_probe`. The 25 cover all twelve depth-bias cases, six of
eight depth-clamp cases and seven of eight polygon-mode cases, including the
three viewport-bank cases. The six are recorded as measured mismatches and were
**not** reconciled by adjusting the oracle; deciding whether each is a driver
defect or a witness-oracle defect is the next slice.

Two defects of the harness itself were found while reading the transcript and
are recorded rather than quietly fixed: `PS5VK_CONSUMER_RASTER_RESULT` prints
`valid=0` while 25 of its own per-case lines say `valid=1`, so that summary
counter does not accumulate; and `PS5VK_CONSUMER_RASTER_GS_*` counts a skipped
witness as a case in `witnessed=25`.

**The geometry witness first failed here and now passes.** In the run above,
`viewport_index_routing` ended in `vkCreateGraphicsPipelines -> -8`: the pinned
compiler left `PSBC_UNRESOLVED_AGC_LINKAGE` set for a merged vertex+geometry
program that exports `gl_ViewportIndex` (measured `unresolved=0x7`,
`PA_CL_VS_OUT_CNTL=0x01280000`), because RADV counts that export in
`param_exports` while the semantic list only walked user locations.
`ps5vk_runtime_shader_build` refuses an unresolved linkage instead of guessing a
parameter mapping, and the control was exact - the same geometry module with
only the `gl_ViewportIndex = gl_PrimitiveIDIn;` line removed compiled clean
through the identical key.

That gap is closed. The compiler now names the export
(`PSBC_SEMANTIC_VIEWPORT_INDEX`, mpereiraesaa/opengnm-psbc#17, pinned in
`tools/prepare_compiler_deps.py`), and the same window was repeated on
`t05-final` `a888290` with eboot
`436a5c2856eec1b67dca27f6e46475f8c3b206fc73f5b6dc0bfc566940a813a7`, run
`20260918T133046476Z_PPSA99994_ps5vk_0x67beeedc4fef`:

```
PS5VK_CONSUMER_RASTER_GS_FEATURES geometryShader=1 multiViewport=1 maxViewports=16
PS5VK_CONSUMER_RASTER_GS_START cases=1 extent=64 tiles=16 clear_word=ff000000
PS5VK_CONSUMER_RASTER_GS_PIPELINE stages=3 viewports=16 created=1
PS5VK_CONSUMER_RASTER_GS_RESULT cases=1 witnessed=1 valid=1
PS5VK_CONSUMER_RASTER_GS_RETIRED cases=1 witnessed=1
```

So `multiViewport` now has end-to-end evidence: a geometry stage writes
`gl_ViewportIndex`, sixteen viewport banks are programmed, and the sixteen-tile
oracle verifies each tile holds the colour of its own input primitive rather
than a broadcast or a permuted bank. It still stays unadvertised, for a reason
that has nothing to do with the driver: no applicable upstream CTS leaf exists
for it (see UPSTREAM_CTS.md), and the six raster oracle mismatches below are
unresolved. The consumer's own verifier now stops the run on the first of them
(`raster oracle for clamp_disabled_narrow_probe`), which is why the repeat run
ended there rather than at the geometry pipeline.
## Dual-source blend promotion (2026-09-21)

First hardware execution of the DXVK262-T06 dual-source blend witnesses, one
bounded console window, two deploys from `codex/t06-final`. The run is the
evidence `dualSrcBlend` is advertised on: the shipping platform mask now sets
the feature bit, the runtime serves the whole GFX1013 blend contract instead of
the single witnessed shape, partial colour write masks are carried in the
pipeline render-target block for `VK_FORMAT_R8G8B8A8_UNORM`, and that format
reports `COLOR_ATTACHMENT_BLEND`.

| | Native witness | CTS measurement |
|---|---|---|
| package | `dist-graphics-api/PPSA99994` | `dist-upstream-cts/PPSA99994` |
| eboot sha256 | `027a43032361f818e4caf314f55557727eff6bb812a44c2a757fcc4e26779d7b` | `aa1cfae7eaf6570f4bf6618d89f7cad1b532907a6a3c2a26691b66fae9aaa870` |
| run | `20260921T131810348Z_PPSA99994_ps5vk_0x152cbc5ead555` | `20260921T142735727Z_PPSA99994_upstream-cts_0x156959662c9a2` |
| log sha256 | `fa82a55bab8bd419e360ecaec5a3276a49d2eacf6aac89f07eca438c76883db1` | `59673109cd1b1c3829a96ec90c3c5c0c55ef0113ca773230993b2009820bfa23` |

**Native witness.** `PS5VK_DUAL_SOURCE_READBACK extent=64x64 draws=2 pixel=2080
control=4080bfff candidate=333326ff expected_control=4080bfff
expected_candidate=333326ff tolerance=1 distinct=1 fence=success
strict_verified=1`: the control is the module's primary export with blending
disabled, the candidate is `primary.rgb * secondary.rgb` with the equation's
ONE, and the two are required to differ, so a blender that ignored the secondary
export could not pass. Clean lifecycle, title closed, artifact identity checked
against the packaged manifest.

**CTS measurement.** The 404-case selection - the frozen 306-case acceptance set
plus the 98 applicable `blend.dual_source` leaves - reports **404 Pass, zero
Fail, zero NotSupported**, strict identity and lifecycle verified
(`cts_verified`). Two defects were found and fixed by this window: the package
had to compile `vktPipelineBlendTests.cpp` and register the blend factory under
the monolithic construction group (before that dEQP silently dropped the 98
unregistered paths and the run reported 306 cases while `/app0/cases.txt` held
404), and the partial write masks had to be carried in the pipeline's
render-target block - writing `CB_TARGET_MASK` from the per-draw context stream
stalled the queue after 101 of 404 cases, which is why that attempt left only a
comment behind.

**Canonical acceptance on the promoted candidate.** The shipping payload - no measurement switch - built from the merged promotion, eboot sha256 `0bbd6679da721eac7f750b7bbea5041bac974ac8f42a9a94ff5a755caff0b3a7`, ran the frozen 404-case selection in `20260921T160602225Z_PPSA99994_upstream-cts_0x15bf4c85ab360` (log sha256 `30a0a14af266cc4d65943644eaa2c9ca9afe349e4f8c7ebbb7cad22627f57922`) and reported **404 Pass, zero Fail, zero NotSupported**, strict identity and lifecycle verified. The public-ABI capability probe was re-measured on the same promotion: eboot sha256 `c592965a4cc7da3f997f48bcfd350680247cc8248a36c0bd33133d2e8952de04`, run `20260921T160521750Z_PPSA99994_ps5vk_0x15beb5be675ad`, strictly verified with `dualSrcBlend=1` and **13 of 62** profile requirements then satisfied.

**What this does not establish.** The window covers `VK_FORMAT_R8G8B8A8_UNORM`
only: `B8G8R8A8_UNORM` keeps the all-channel write mask and reports no blend
feature, because a BGRA target's export applies a channel swap the mask cannot
express and no leaf measured it. `independentBlend` and `sampleRateShading`
remain blockers with no implementation.

## Main integration (DXVK262-T05 into the T06 line), verified 2026-09-22

`codex/t06-final` merged `main` after the DXVK262-T05 promotion. Both lines had
changed the same colour- and depth-target code, so the merge keeps one
capability set: the per-attachment colour contract (render pass, framebuffer,
pipeline key, native target list) grows the DEPTH-ONLY shape - a subpass with no
colour reference, a pipeline with an undefined colour format and no write mask,
a framebuffer whose only role is depth, and a native draw state that programmes
no colour target while still writing the blend word pair - while the runtime
compiler keeps its per-attachment blend, write-mask and export gates and adds
the depth-only target with the NONE export class. `PS5VK_RASTER_DIAGNOSTIC`
stays retired; `gl_FragCoord` is admitted once, by the rule T05 added.

Merging the two lines surfaced three real defects that the individual lines did
not have, each found by the run below and fixed in this change:

1. `pColorBlendState` is optional in Vulkan and the depth-clamp module omits it
   for its depth-only subpass; the per-attachment pipeline gate treated a
   missing state as malformed and refused `vkCreateGraphicsPipelines`.
2. The begin-render-pass check compared `fb->color_attachments[0]` with the
   subpass's first colour reference even at a colour count of zero, where
   neither side is meaningful.
3. The depth-only draw state passed its caller's *uninitialised* prepared-target
   array to the render-target builder because it tested the pointer rather than
   the count.

The frozen selection is now the union of both lines - **462 acceptance cases**
(the 404 the T06 line carried, including the 98 dual-source leaves and the two
fragment side-effect leaves, plus the 58 T05 leaves) and 98 diagnostics - and
the shipping build passes all of it:

- run `20260922T014510656Z_PPSA99994_upstream-cts_0x17b8f2b94697f`,
  **462/462 Pass**, zero Fail, zero NotSupported, no missing, unexpected or
  duplicate results, `strict_verified`, `cts_verified` and `lifecycle_ok` true,
  title closed and confirmed stopped.
- deployed SELF SHA-256
  `2aefe2ca1b69baa7218883e94668bee689c6d576d37469a8afd2a4bfc508a1c8`, read back
  exactly through FTP before launch; reassembled report SHA-256
  `c93560567795a75610c994d5b7e4241bdbd53d7c0a858862b837d3260e195654`; selection
  SHA-256 `aafc4062b267b88ea1b395aae924fcf0451eff411e9522dd889ce9638073d0ab`.

The public-ABI capability probe was rebuilt against the merged profile and
re-run, so the merged tree's own report - not the sum of two older reports -
is what the evidence records: run
`20260922T014635022Z_PPSA99994_ps5vk_0x17ba2d04ef50c`, artifact
`549184cdd050d5f9349534a65c7ab59ac5d3afaf959293c19fd4b50697a20cc0`,
`strict_verified=1`, **17 of the 62 profile requirements satisfied** and 45
blockers. The DXVK matrix reads **15/62 ready with 47 blockers** on the merged
tree, and `make check` is green on it.

`independentBlend` and `sampleRateShading` remain blockers: the MRT path is
described but not served (the advertised `maxColorAttachments` is still one)
and the multisample contract is still empty.

## independentBlend promotion (2026-09-22)

The two upstream leaves that REQUIRE `independentBlend` pass on hardware, so the
feature is advertised: `native/platform_ps5.c` reports the bit in every build,
`src/graphics_limits.h` advertises `maxColorAttachments` 2 (and the two
four-output fragment limits with it), and the two private measurement switches
are retired - `VK_FORMAT_R8G8B8A8_UINT` is a served colour target now.

Both leaves render two colour attachments in one pass - R8G8B8A8_UINT and
R8G8B8A8_UNORM - clear both through the pass, draw once with a fragment stage
that exports the two attachments' values, copy each attachment into its own
buffer and compare the result with the upstream oracle. The second leaf writes
only the second target; its fragment stage declares Location 1 alone and the
pinned compiler publishes the export enable in the mask's second nibble, so the
driver programmes the attachment it really writes as hardware target zero.

Two strict measurement runs carried the integer colour target, both with the
same payload SELF SHA-256
`a664299c6be988bbe1ff74a5888360d900301793fef39d3e04ee98e51aca30f4` (eboot
`a664299c6be988bbe1ff74a5888360d900301793fef39d3e04ee98e51aca30f4`):

- `20260922T091802564Z_PPSA99994_upstream-cts_0x19445899fd8a1`, log SHA-256
  `9ae18272031fbe09877e223853d081d5cdb4740e8147ac8e522162ce7c84c34b` -
  **464 Pass, 0 Fail, 2 NotSupported**: both
  `dEQP-VK.renderpass.suballocation.attachment_write_mask.attachment_count_2`
  leaves pass, and the two `dedicated_allocation` siblings stay NotSupported
  because `VK_KHR_dedicated_allocation` is not advertised. Title closed and
  confirmed stopped.
- `20260922T083110494Z_PPSA99994_upstream-cts_0x191b6cf7742e3`, log SHA-256
  `34784cf89af8940a917cad59d5f23e328db67ad4b3dbe3c58f6f86f1a3bde8a3` - the
  earlier run of the same pair where `start_index_0` already passed.

Host tests prove the render-target word, the integer export classification, the
renumbering the second-target-only shape needs, the readback plan and the
fail-closed gates around them; only the hardware writes and reads the integer
surface. This is focused validation of one requirement, not Vulkan conformance,
and `sampleRateShading` remains a blocker on the same row set.

**Canonical acceptance on the promoted candidate.** The shipping payload (no
measurement switch; the two switches are gone), eboot sha256
`713150da90f4a30b6f407aba1083958e0783f4cd3781e9f215088b5cb7c1bd03`, ran the
frozen selection - now **464 acceptance cases** (the 462 of the main integration
plus the two `suballocation.attachment_write_mask` leaves the feature owns) and
46 diagnostics - in
`20260922T103049552Z_PPSA99994_upstream-cts_0x1983e4b36fa05` (selection SHA-256
`91c37ed06f78d1048b8fa1e693986ab047a749dd55e9585e2e8565a421603d2a`) and reported
**UPSTREAM ACCEPTANCE PASSED: every selected case matched its upstream oracle
and the title was closed**, with the two `dedicated_allocation` siblings
reported `NotSupported` because `VK_KHR_dedicated_allocation` is not advertised.

**Public-ABI capability probe on the same candidate.** eboot sha256
`c438a98123c86925d62b6627b7b640734930dbcae5b8af6f88a4ed0dc61e8201`, run
`20260922T103414212Z_PPSA99994_ps5vk_0x1986df1aeed66` (log SHA-256
`3fa5f0c0049890f2c2679889616f2d0ccaa303d36a9f62ac9a19e0ba0aba6c62`), strictly
verified by `tools/verify_dxvk_probe.py`: the device itself reports
`independentBlend=1` and the probe ends `valid=1 total=62 satisfied=18
blockers=44`, one requirement more than the merged tip's 17/45. With that
receipt the DXVK matrix row for `feature:VkPhysicalDeviceFeatures:independentBlend`
is **satisfied on all four axes** (api, implementation, cts, native) and the
profile reads **16 of 62 requirements ready with 46 blockers**.
## Focused upstream selection for sampleRateShading (2026-09-22)

The `sampleRateShading` row's CTS axis needs a focused, applicable selection
before anything can be measured. Read from the pinned sources rather than
guessed, `MinSampleShadingTest::checkSupport` is the only
`requireDeviceCoreFeature(DEVICE_CORE_FEATURE_SAMPLE_RATE_SHADING)` call in
`external/vulkancts/modules/vulkan/pipeline/vktPipelineMultisampleTests.cpp`
(`:1344`), and `createMultisampleTests` builds exactly three groups from that
class: `min_sample_shading` (its opaque primitives), `min_sample_shading_enabled`
and `min_sample_shading_disabled` (both a quad). At the counts this profile
serves that is 50 leaves under the new `t06-sample-rate-pending` diagnostic
category: 5 `minSampleShading` values x 2 counts x 3 primitives, plus the
enabled and disabled quads. The sparse variants are left out because this
profile binds no sparse memory, and `primitive_point` is left out because the
measurement below reported it as `NotSupported` for `largePoints`, a feature
this profile does not advertise - which is a scope statement, not a driver
defect. Each selected leaf renders the multisampled colour target, resolves it
into the single-sample image, then reads the multisampled colour back once per
sample through `subpassLoad(imageMS, sampleNdx)`, so a build that shades once
per pixel cannot pass it.

The category's traceability is derived, not transcribed: `tools/
check_upstream_selection.py` grows `_min_sample_shading_leaf_names`, which
composes the names per group from that module's own sample-count array, its
`minSampleShading` value table and the case literals each block registers, plus
`_multisample_generated_segments`, because the `samples_<count>` group segment
is printed by the factory rather than written as a literal. A leaf moved
between the enabled and disabled groups, or a primitive a group does not
register, stops matching; a rewritten factory stops matching too.

Measurement run 1 (`20260922T083940138Z_PPSA99994_upstream-cts_0x1922d783e3ecc`,
log SHA-256 `ae9d271ee5096419caf91151744c853b7a8f6628e89c77e1486001472972b0b9`, 522
cases = 462 frozen acceptance + the first 60 leaves): 522/522 reported, 462
Pass, 50 Fail, 10 `NotSupported`. The ten `NotSupported` leaves are exactly the
`primitive_point` ones, refused as "Requested core feature is not supported:
largePoints" (`vktTestCase.cpp:1227`), which is why they left the category.

Measurement run 2, with the corrected 50-leaf category
(`20260922T084825463Z_PPSA99994_upstream-cts_0x192a7c7884dc5`, log SHA-256
`2993b6e47ffbfa3fb08aa9c3ad70b3785d81ac1cb7eebb53a534b13a107acdba`): payload
eboot SHA-256 `88d9ca738842e9f05021a5f0e429405eee74d480d956d17b0a78414b35f0e2db`,
built with `PS5VK_SAMPLE_RATE_DIAGNOSTIC=1`; measurement selection SHA-256
`73f85a4a290ffe1e5e1b74ad022747af847cd9135e7aeca1274d16269bda88c2` derived from
the frozen selection `aafc4062b267b88ea1b395aae924fcf0451eff411e9522dd889ce9638073d0ab`;
512/512 cases reported, no missing, unexpected or duplicate results, 462 Pass
(the frozen acceptance selection is unchanged and still fully green on this
tree), 50 Fail, zero `NotSupported`, `lifecycle_ok` true and the title closed
and confirmed stopped. All 50 leaves fail at the same first gate:
`vk.createImage(...) VK_ERROR_FEATURE_NOT_PRESENT`, because the multisampled
colour image the oracle builds carries
`COLOR_ATTACHMENT | TRANSFER_SRC` (`| INPUT_ATTACHMENT` for the per-sample
fetch render pass) while this profile serves exactly one observed multisampled
role.

What this establishes: the applicable focused selection exists, it is
traceable to the pinned sources, the packaged payload reports every selected
leaf (the silently-dropped-leaf trap is closed at 512/512), and the first gate
the row has to open is the multisampled image's usage combination. What it does
not establish: no leaf passes, the multisample render pass, the resolve and the
per-sample input read are still unexecuted, the `sampleRateShading` row stays a
blocker, and the shipping platform mask still advertises no sample-rate bit.

## Multisampled colour image role (2026-09-22)

The measurement above named the first gate precisely: the oracle's
multisampled colour image carries `COLOR_ATTACHMENT | TRANSFER_SRC`
(`vktPipelineMultisampleTests.cpp:3368-3371`) and adds `INPUT_ATTACHMENT` for
the render type that reads that image back once per sample, while this profile
admitted exactly one observed multisampled role. This change admits those
combinations and nothing else: `ps5vk_multisampled_color_usage` in
`src/sample_rate_contract.h` is consumed by the two count-aware sites,
`vkCreateImage` and `ps5vk_native_image_requirements`, both of which still
require a 2D one-mip `B8G8R8A8_UNORM`/`R8G8B8A8_UNORM` colour image, no create
flags, and a platform whose served-count mask carries the requested count. A
multisampled image naming the input role without the readback source, the
transfer-destination pair, the sampled role or any other combination keeps the
failure it had. The per-format query stays sample-agnostic and is left for the
promotion slice, which is where it has to be measured through the public ABI;
the shipping mask is untouched, so an unadvertised build still refuses every
multisampled image.

Measured on the console with the same 512-case measurement selection
(`20260922T091910948Z_PPSA99994_upstream-cts_0x1945575a966d5`, log SHA-256
`7cd77372601c2300846e4d092f51b196feb311c03ee16a522eec59f9ceded4c5`, payload
eboot `e46e07c604531c6e29f5907da3c7052c96bfc98fc8785bfcac4706caf931db5a`):
the image gate opened - none of the 50 leaves fails at `vkCreateImage` any
more - and the payload then died without closing its log. The run file records
`bye=false, clean=false, close_reason=eof` after 20.9 s, with no refusal and no
`Fail` verdict anywhere; the last records are the per-sample fetch fragment
shader (`subpassInputMS` + `subpassLoad(imageMS, sampleNdx)`) being assembled,
then two 131072-byte allocations - the resolve and per-sample single-sample
targets - and then EOF. This is a fail-closed defect, not a capability
verdict: the title is not running (the close request found nothing to close),
nothing is hung on the console, and the next slice is a bounded native probe
that walks exactly this CTS shape step by step so the step that kills the
process names itself before it is fixed.

Two provenance notes from the same window, both measured rather than assumed.
First, a payload built while a concurrent `make check` restaged the SDK without
`PS5VK_SAMPLE_RATE_DIAGNOSTIC` reported every selected leaf `NotSupported` for
`sampleRateShading` - a run that would have read as a driver verdict. Second,
`tools/build_upstream_cts.py` did not record that switch in its build profile,
so the two payloads were indistinguishable in the receipt; the switch is now in
the captured switch set and the measurement payload records
`PS5VK_SAMPLE_RATE_DIAGNOSTIC = 1`.

What this establishes: the image-role gate the previous measurement named is
open and bounded to the oracle's own combinations, with the platform mask as
the only switch that reaches it. What it does not establish: no leaf passes,
the render pass with its resolve and per-sample fetch is still unexecuted, the
driver does not yet fail closed on that shape, the row stays a blocker, and
nothing is advertised.

## CTS shape walk (2026-09-22)

The measurement above ended with a payload that died without closing its log,
which names no defect. The walk turns that into steps: `PS5VK_SAMPLE_RATE_PROBE=2`
is the witness scene followed by `ps5vk_sample_rate_shape_probe`, which performs
the CTS oracle's own sequence - the multisampled colour attachment with its
exact usage, the resolve and per-sample single-sample targets, their views, the
input-attachment plus uniform-buffer descriptor set, the pass whose subpass 0
resolves and whose subpasses 1 and 2 fetch per sample and preserve their
sibling's target, both pipelines and one submission - announcing every step
before it runs and stopping at the first refusal with that step and its Vulkan
result. A step that never returns is named by the log it left behind, and a
refusal is fail-closed evidence instead of a dead process.

Measured on the console (run `20260922T095638715Z_PPSA99994_ps5vk_0x19660cd1cb450`,
log SHA-256 `e9b376d80dd3187d87d9d1fe579e0ec525f521aa817dbbfe4270f8029249e317`,
payload eboot `f45e30ac879d8b345398ea98d7cbc18a07e64ea4834364026267641d0c8b4a6c`):

- the multisampled colour attachment with `COLOR_ATTACHMENT | TRANSFER_SRC |
  INPUT_ATTACHMENT` is created on hardware (`vkCreateImage` rc=0), which is the
  image role the previous slice opened;
- the resolve target and both per-sample targets, their memory, their views,
  the UBO, the descriptor set layout, pool and set all succeed;
- `vkCreateRenderPass` returns `VK_ERROR_FEATURE_NOT_PRESENT` (-8) and the walk
  stops there, logging the step and the code, then closes cleanly
  (`PS5VK_PLATFORM_CLOSE rc=0`, `BYE reason=graphics-api-end`).

The first walk iteration had stopped one step earlier - at the resolve target's
`vkCreateImage` - and that was the walk's own mistake, not a driver finding: it
used `B8G8R8A8_UNORM` for the single-sample targets, and this profile carries
the readback-capability colour row only for `R8G8B8A8_UNORM`, so the
attachment-with-its-readback-pair combination exists for the one format. The
walk now uses the oracle's format, and the reason is recorded in the code
beside it.

The same window's first host probe of the stage behind the pass looked like a
compiler gate and was not one, so the correction is recorded here rather than
quietly dropped. That probe handed the adapter the oracle's per-sample fetch
module (`subpassInputMS` plus `subpassLoad(imageMS, sampleNdx)`, compiled from
`vktPipelineMultisampleBaseResolveAndPerSampleFetch.cpp`) with **no
pipeline-layout signature at all**: `ps5vk_runtime_graphics_compile` refused it
with no named site while the pinned compiler logged `Unsupported SPIR-V
capability: SpvCapabilityInputAttachment (40)`, and both facts read as "this
stage cannot be compiled". With the layout the oracle actually uses - an input
attachment at set 0 binding 0 and a uniform buffer at binding 1 - the same
module measures the opposite, and `tests/test_runtime_graphics_compiler.c` now
pins all of it: the interface accepts the module, the descriptor table layout
accepts the signature, `psbc_compile_shader` returns OK on its own (88 bytes of
machine code, measured directly), the adapter compile returns `VK_SUCCESS` with
108 bytes of fragment machine code, the compiled metadata names both bindings as
used (`descriptor_used_binding_mask[0] == 3`), and dropping the layout signature
is what makes the compile fail. The InputAttachment line is a warning from the
NIR front end, not a refusal. The fetch stage is therefore not a compiler-side
gate.

What this establishes: the oracle's attachment and descriptor steps run on
hardware; the first gate after them is the render pass itself, with the
internal attachment bound (two) the obvious candidate; and the per-sample fetch
stage is supported by the pinned compiler and by this profile's adapter once
the pipeline layout declares what it reads.
What it does not establish: no leaf passes, the resolve is not executed, the
per-sample fetch is not executed, the row stays a blocker, and nothing is
advertised.

## The pass bound, and the framebuffer defect it exposed (2026-09-22)

The walk named the render-pass gate, and the arithmetic behind it was one enum:
`PS5VK_MAX_ATTACHMENTS` was `PS5VK_MAX_COLOR_ATTACHMENTS + 1`, which answers how
many colour targets a draw may write but not how many attachments a pass may
name. It is now `PS5VK_SAMPLE_COUNT_MAX_SERVED + 3` (seven), derived from a
named constant in `src/sample_rate_contract.h`, because the pinned oracle's
pass carries the multisampled colour attachment, its resolve target and one
single-sample target per sample it fetches back, plus the optional depth
attachment. `PS5VK_MAX_COLOR_ATTACHMENTS` still bounds what a subpass may render
into, and `tests/test_vk_render_pass.c` now holds both directions: the oracle's
six-attachment pass is accepted and one attachment past the bound is refused.

Moving the pass bound exposed a real defect one layer down, and the walk is what
found it. `struct VkFramebuffer_T` held its attachment slots in fixed arrays of
two (`attachments[2]`, `formats[2]`, `samples[2]`), so a framebuffer built for
the oracle's four attachments wrote past itself. Measured on the console with
the bound raised but this fix not yet in place: run
`20260922T102334060Z_PPSA99994_ps5vk_0x197d8e621b19e`, log SHA-256
`d9d9018e2ca43405970f5dd0c268860f09ffba2d7d95685d81320eb14f5c5916`, payload
eboot `49933aef329b1fb62de3b6f82ae8f3691309944478de973fd1a19279f6689f1c` - the
walk reached `vkCreateRenderPass` rc=0, `vkCreateFramebuffer` rc=0,
`create_pipeline subpass=0` rc=0, `create_pipeline subpass=1` rc=0 (the
per-sample fetch stage compiles on the console build), `vkBeginCommandBuffer`
rc=0, and then `vkEndCommandBuffer` returned `-13`; the payload never closed its
log. Instrumenting the teardown named the killer: run
`20260922T102625871Z_PPSA99994_ps5vk_0x19800e6cf4e94`, log SHA-256
`e96e7988849fbe8a3e5caad2b4a965eb38eed8cbdf765abbb5d184a796480c94`, whose last
record is the announcement of `vkDestroyFramebuffer` - the process died inside
it, after every other teardown step had returned.

The fix is the slots following the same bound the pass does, and the new case in
`tests/test_vk_image.c` proves both directions: with the two-slot arrays, the
sanitizer build reports `index 2 out of bounds for type 'VkImageView_T *[2]'` at
`src/vk_framebuffer.c:88` and the framebuffer's fourth slot reads as garbage;
with the fix the oracle's four-attachment framebuffer round-trips and its
teardown is clean. The record roles are unchanged - `color_attachments` and
`resolve_attachments` stay bounded by the colour-attachment contract, because
that is how many targets a draw may write.

What this establishes: the oracle's pass, framebuffer, pipeline layout, shader
modules and both pipelines exist on hardware, and the framebuffer no longer
corrupts memory for the shape the pass now admits. What it does not establish:
no leaf passes, `vkEndCommandBuffer` still refuses the recording with `-13`
(the executor does not run a resolve or a preserve list yet), the row stays a
blocker, and nothing is advertised.

Confirmed after the fix, one more bounded window (run
`20260922T104105309Z_PPSA99994_ps5vk_0x198cda8a48718`, log SHA-256
`3b8d5b8131567bff1b6590006951f0e2acf430de166137b0ff75942e50b1df67`, payload
eboot `aa398bb707f21667f4211d932966d80674da67a642360653492bbfed2786033e`): 117
records, every step of the walk `ok=1` except one, the teardown running to its
end, `PS5VK_PLATFORM_CLOSE rc=0` and a clean `BYE`. The single refusal is the
recording: `vkEndCommandBuffer` rc=-13. So the object path for the oracle's
shape now stands end to end, and the gate the row has to open next is the
executor - the resolve, the per-sample input read and the preserve list are
described, created and compiled, but not run.

## Recording the oracle's pass, and the gate behind it (2026-09-22)

Two probe defects stood between the walk and the driver, and both are recorded
because both looked like driver gates until they were named:

1. The walk declared three subpasses and recorded work in two of them, and the
   recording refuses to end a pass before its last subpass. Announcing every
   recorded call is what showed it: `vkCmdEndRenderPass` was announced and
   `vkEndCommandBuffer` then refused, i.e. the pass had never been left. The
   walk now creates one pipeline per subpass and records every one of them.
2. The walk handed one clear value to a pass that clears four attachments, and
   `vkCmdBeginRenderPass` requires a value for every attachment it clears - the
   clear-value count is the caller's and the pass's, not a pair. That is the
   first real driver limitation this walk found on the recording side:
   `struct ps5vk_operation` stored two clear values and the begin bound the
   count at two, while the oracle's pass carries four.

This change raises both to the attachment bound the pass itself uses
(`PS5VK_MAX_ATTACHMENTS`), so one begin can carry a value per attachment the
pass names, and `tests/test_vk_command.c` pins both directions: the oracle's
four-attachment pass records with four clear values (and the fourth value is
the one the record carries), while one value short of what the pass clears
still poisons the recording. The executor already indexes the clears per colour
attachment and bounds them by the recorded count, so nothing else moved.

Measured on the console after the change (run
`20260922T111728603Z_PPSA99994_ps5vk_0x19ac9fd1c7523`, log SHA-256
`20dbfad9b6b8049404c139200f41d875f7e442809a8f79b4e1c4ad553e664952`, payload
eboot `76f1f3c653b951e76b68c80d1c303777c3b71299af434ef220b82aff80a4e44c`): the
walk records the whole pass - `vkCmdBeginRenderPass` with four clear values,
both draws, both subpass boundaries, the descriptor binds and
`vkCmdEndRenderPass` - and `vkEndCommandBuffer` returns **rc=0**. The next call,
`vkQueueSubmit`, returns **VK_ERROR_FEATURE_NOT_PRESENT (-8)**: the executor
does not run the resolve, the per-sample input read or the preserve list yet,
and it says so instead of executing a pass whose promise it would drop. The
title closed cleanly (`PS5VK_PLATFORM_CLOSE rc=0`, `BYE`).

What this establishes: the oracle's pass is recordable end to end with its own
clear set, and the gate is now the executor at submission, named by its own
return code. What it does not establish: no leaf passes, the resolve, the
per-sample read and the preserve list are still unexecuted, the row stays a
blocker, and nothing is advertised.

## Executing a subpass that renders elsewhere (2026-09-22)

The executor prepared every draw from the framebuffer's colour and depth role
lists, and those lists are subpass 0's roles, so a pass whose later subpasses
render targets of their own was refused three times over: a resolve target, a
preserve list, and subpasses that do not name the same attachments. This change
lifts the third one. What a draw renders into is its OWN subpass's set - the
seam the draw preparation now takes as `struct ps5vk_target_set` - and the pass
carries the union of the references its subpasses name, with the depth role
single and shared, an attachment that is named twice required to agree with
itself on format and layout, and the resolve and preserve refusals untouched.
The prelude now derives one plan, one clear word and one layout transition per
ATTACHMENT rather than per subpass-0 colour slot.

Measured on the console (run `20260922T113731886Z_PPSA99994_ps5vk_0x19be2253813fd`,
log SHA-256 `d5d48c50ccae7a03f34687e13c4690b63ce43bc338ebb5d81a7f619818d9e16c`,
payload eboot `6ede50ef50cb46838175453dfa4ddad5cbab22ebef259f22cefface956dd4a49`):

- a two-subpass pass, each subpass rendering its own colour attachment, is
  created, recorded (`vkEndCommandBuffer` rc=0), submitted (`vkQueueSubmit`
  rc=0) and completed (`vkWaitForFences` rc=0);
- the SECOND subpass's target is read back through the driver's own span and
  holds the fragment's colour across exactly the plane it covers:
  `PS5VK_SAMPLE_RATE_TARGETS extent=32x32 samples=4 subpasses=2
  target_words=32768 shaded=1024 expected=1024 word=ffbf8040 verdict=1` - 1024
  words is the 32x32 RGBA8 plane, and the tiled padding around it keeps the
  clear, so the draw really landed in that subpass's own attachment;
- the oracle's own pass still refuses at `vkQueueSubmit` rc=-8, because it also
  carries a resolve target, an input attachment and a preserve list, and those
  three are the next refusals to lift;
- the title closed cleanly (`PS5VK_PLATFORM_CLOSE rc=0`, `BYE`).

What this establishes: the executor runs a multi-subpass pass whose subpasses
render their own targets, with hardware-comparable output, which is the
structural half of the oracle's pass. What it does not establish: the resolve,
the per-sample input read and the preserve list are still unexecuted, no CTS
leaf passes, the row stays a blocker, and nothing is advertised.

## A subpass that preserves another subpass's target (2026-09-22)

The executor refused any pass carrying a preserve list, on the grounds that it
did not carry attachment contents across a subpass boundary. With every subpass
rendering its own targets that promise is one this path keeps: a preserved
attachment is one the subpass does not write, the pass's load ops are applied
once at its start, and the boundary between subpasses publishes the previous
subpass's writes without touching anything else. The refusal is therefore
replaced by a bounds check on every preserve entry the pass carries - a record
that reached this backend naming an attachment outside the pass is refused
rather than read out of bounds - and the resolve refusal stays exactly where it
was.

Measured on the console (run `20260922T115140401Z_PPSA99994_ps5vk_0x19ca7b3ec8c32`,
log SHA-256 `4a3a6e4db059d29225ca8ed12a5badfffa33ecdc3a8396ee484164de8ee8daef`,
payload eboot `42d1b5c95dfb8d8048f0cb8c51223b0e2982242751e660fa6ec455caa39d5f74`):
the two-subpass pass now has subpass 1 preserve attachment 0 - the target
subpass 0 renders - and the readback checks both directions in one verdict:
the second subpass's own target holds the fragment's colour across its 32x32
plane (`shaded=1024 expected=1024`), and the PRESERVED target still holds what
subpass 0 drew (`preserved_hits=4096` of 65536 words, which is exactly the
32x32 plane at 4 samples). A subpass that clobbered it would have left the
pass's own clear value there instead. The title closed cleanly
(`PS5VK_PLATFORM_CLOSE rc=0`, `BYE`).

What this establishes: a subpass may preserve another subpass's target and the
executor keeps that promise on hardware, which is the shape the oracle's fetch
subpasses use for their siblings. What it does not establish: the resolve
target and the per-sample input read are still unexecuted, no CTS leaf passes,
the row stays a blocker, and nothing is advertised.

## The multisampled resource read, enabled (2026-09-22)

Reading sample k of a multisampled colour attachment is the enabler the last two
executor gates share: the oracle's fetch subpass reads its multisampled input
attachment once per sample, and a resolve is the same read averaged over every
sample. Two places kept it out, and both are now open in the bounded shape the
profile serves.

The resource record: `ps5vk_image_resource_descriptor` refused any multisampled
image. The pinned gfx6+ texture descriptor carries the sample geometry in the
LEVEL fields - BASE_LEVEL zero and LAST_LEVEL log2(samples) for a multisampled
surface (`ac_descriptors.c`, `ac_build_gfx6_texture_descriptor`) - so a
multisampled attachment is the same RGBA8 2D one-mip record with those two
fields naming the count, single-layer, over the render target's own tiled
storage. A count this profile does not implement is still refused before the
record is written, and `tests/test_texture_descriptor.c` pins the multisampled
record (LAST_LEVEL = log2(4)), the single-sample record carrying no sample
geometry, and the 8x refusal.

The gate: `ps5vk_input_attachment_gate` served exactly one resource - the
promoted multiview attachment, six layers with the colour/transfer-source/
input-attachment/transfer-destination roles. It now serves the multisampled
single-layer attachment as well (colour, readback source and input-attachment
roles), and it no longer requires an explicit forward dependency: the executor
emits the colour-to-texture barrier around every subpass change that reads an
input attachment, and Vulkan gives an attachment read by a later subpass its
implicit dependency, so the barrier the profile already emits is what orders
the read.

What this establishes: the two layers between the oracle's fetch draw and
execution now describe the shape it needs, with the descriptor arithmetic
pinned against the pinned compiler. What it does not establish: the read has
not yet been measured on hardware, the resolve is still refused by the
executor, no CTS leaf passes, the row stays a blocker, and nothing is
advertised.

## The per-sample read executes but does not yet select the sample (2026-09-22)

The multisample resource record from the previous slice was measured on
hardware, and the honest result is a negative one that narrows the search. The
probe's fetch phase is now a SWEEP: subpass 0 draws per-sample values into the
multisampled target (the gl_SampleID module the witness uses, so plane k holds
`0xff0k0000`), and subpass 1 reads a NAMED sample of it through the
resource-only record and writes its own single-sample target, which is then
read back through the driver's own span.

What the shape does: every object and every recording step succeeds, the pass is
submitted and completes, and the read runs - run
`20260922T123034336Z_PPSA99994_ps5vk_0x19ec71b787e8a`, log SHA-256
`851d2b53213e497644a88312efff8c2fc7f22fe3374b6a4fa453fc8a1fc461ca`. What it
returns: for sample index 0 the target holds sample 0's value, and for sample
index 3 it holds sample 0's value again - the index does not select the plane.

Three hypotheses were ruled out by measurement rather than argument:

1. **The uniform block never reached the shader.** Ruled out twice: the flush
   for the non-coherent block is now emitted (a partial mapping cannot be
   flushed at all - the range has to cover a whole non-coherent atom or end at
   the allocation), and the second index runs a module with the sample index
   BAKED IN (`subpassLoad(imageMS, 3)`) which reads the same plane 0.
2. **The compiler drops the index.** Disassembled with the pinned compiler's own
   `PSBC_DEBUG_DISASM`: the baked module lowers to `v_mov_b32 3` into v7 and
   then `image_load ... 2darraymsaa` with that register as the sample operand,
   so the index is carried into the instruction.
3. **The record needs the multisampled type tag.** The pinned header's own tag
   for a multisampled 2D surface is `V_008F1C_SQ_RSRC_IMG_2D_MSAA` (14), not the
   plain 2D tag (9), so the record now carries it together with BASE_LEVEL 0 and
   LAST_LEVEL log2(samples) - and the measurement is unchanged (run
   `20260922T123243995Z_PPSA99994_ps5vk_0x19ee54bac640d`, log SHA-256
   `944289f6b23120b31466be1f917ffdc6c78f5904f74ecb8fb30ecae29a698e97`). The tag
   and the level fields are therefore necessary-but-not-sufficient: the record
   now says what the compiler says, and the hardware still reads plane 0.

What remains, stated as the next question rather than as a guess: how the
surface a colour attachment is backed by is laid out for a sample-indexed read,
and whether the tile mode the record carries describes THAT layout. The witness
measured the storage as one plane per sample and the clear fills all of them;
what a sample-indexed fetch reads is a different question, and the answer is
what the next slice has to measure.

What this establishes: the read executes end to end with the index in the
instruction, and the value always comes from plane 0. What it does not
establish: the per-sample read, the resolve, any passing CTS leaf, the row - all
unchanged, and nothing is advertised.

## The per-sample read, re-measured without a confound (2026-09-22)

The previous measurement concluded that the read "always returns plane zero",
and that conclusion was confounded by the probe's own clear value: the fetch
pass cleared its target to `{0, 0, 0, 1}`, whose RGBA8 word is `0xff000000` -
exactly the value the gl_SampleID module writes for sample 0. A fetch that wrote
nothing looked the same as a fetch that read plane zero.

With a clear no sample can hold (`{0.5, 0.5, 0.5, 1}` = `0xff808080`), the same
sweep (run `20260922T124831879Z_PPSA99994_ps5vk_0x19fc1fd2698b0`, log SHA-256
`c852c65e13c01356f2b7a3c40764064c9224fd5b0e1d1ec868c29a8961341269`) reads:

- `sample_index=0`: `matched=1024 expected=1024`, `seen=ff000000,ff808080` -
  the fetch subpass ran, read a REAL sample of the multisampled attachment and
  wrote it across its own target's plane, with the pass clear left in the tiled
  padding. Verdict 1: the multisampled input-attachment read executes.
- `sample_index=3` (the module with the index baked in, whose ISA carries
  `v_mov_b32 3` into the sample operand): the plane holds `ff000000` again -
  sample 0's value, not sample 3's.

The record now also carries the resource type the pinned compiler's own mapping
implies: `ac_shader_util.c` lowers `GLSL_SAMPLER_DIM_SUBPASS_MS` to
`ac_image_2darraymsaa`, so a `subpassInputMS` is a TWO-DIMENSIONAL ARRAY MSAA
image - `V_008F1C_SQ_RSRC_IMG_2D_MSAA_ARRAY` (15), with the single layer
described by a zero depth field - and not the plain 2D MSAA tag (14) the
previous window carried. Both tags were measured and both return plane 0 for a
non-zero index, so the tag alone is not what selects the sample.

What this establishes, positively: the multisampled input-attachment read runs
on this hardware and delivers a real sample plane into the subpass's own target.
What it does not establish: selecting a sample other than the first, which is
what the oracle's fetch subpass and a resolve both need. The open question is
now narrower than "the read does not work": it is how the sample index selects a
plane in the storage this profile backs a multisampled colour attachment with,
and whether the tile mode the record carries describes that storage - the same
question the previous window raised, now with the confound removed and one
positive result in hand.

## What the multisampled storage actually holds (2026-09-22)

The previous window's "index 3 still reads sample 0" needed one more control:
whether the SOURCE even held another sample to read. It does, and the census is
the interesting part. Run
`20260922T130521123Z_PPSA99994_ps5vk_0x1a0acf7fbf5e9`, log SHA-256
`d8fc7a088f5108237dd34fcad9478a13b19f4311f3a632f1c7475138addd5120`:

```
PS5VK_SAMPLE_RATE_SOURCE extent=32x32 samples=4 words=65536 distinct=4
  value0=ff000000 hits0=1024 first0=0 last0=3327
  value1=ff808080 hits1=61440            <- the pass's own clear (padding)
  value2=ff000001 hits2=1024
  value3=ff000002 hits3=1024
```

So a 32x32 four-sample attachment, dressed per sample by `gl_SampleID`, holds one
1024-word plane PER SAMPLE - `ff000000`, `ff000001`, `ff000002` (and, as the
census' fifth distinct value, `ff000003`) - and they are not stacked at
footprint-sized strides: all of them live inside the first ~4096 words, i.e.
inside the footprint one single-sample 32x32 surface occupies, with the pass
clear in the remaining 61440 words. The hardware packs the samples of a
multisampled colour attachment inside the same tiled block a single-sample
surface would use; the extra storage this profile allocates (count x footprint,
from the pinned `ac_estimate_size` arithmetic) is padding, not a plane stride.

That makes the earlier reading precise rather than mysterious: the fetch reads
the correct surface, the index is in the instruction, per-sample data exists -
and the read still returns sample zero's slot for every pixel, so what does not
line up is how a sample index selects a slot inside that packed block. The
descriptor's level fields and the ARRAY MSAA type were both measured and neither
changed it, which leaves the TILE MODE the record carries as the next thing to
measure against the arrangement the hardware just showed.

What this establishes: the multisampled input-attachment read runs, the source
is genuinely per-sample, and the missing piece is the sample-within-block
addressing rather than anything about the read's execution. What it does not
establish: that addressing, the resolve, any passing CTS leaf, the row - all
unchanged, and nothing is advertised.

## Naming the layout equation the read has to match (2026-09-22)

Two facts from the lab's own sources narrow the sample-addressing question
without another console window, and both belong next to the census.

First, the tiling equation: the lab keeps a full Mesa checkout for this GPU
(`third_party/mesa-gfx1013`), and its addrlib computes a multisampled surface's
micro-tile block by REDUCING it by the sample count -
`gfx10addrlib.cpp`, `GetBlk256SizeLog2(..., numSamplesLog2, ...)` with
`blockBits -= numSamplesLog2` - so a 256-byte micro tile becomes 256/num_samples
bytes per sample and the samples are interleaved inside the block rather than
stacked plane by plane. That is exactly what the census measured on the
hardware, and it is the equation a sample-indexed read has to resolve against.

Second, the record this profile builds already carries the right swizzle: the
tile field of the resource word holds `0x1b` in bits 20..24, which is
`ADDR_SW_64KB_R_X` (27) in the lab Mesa's `addrtypes.h` - the same render-target
swizzle a colour attachment is backed by. So the record is not obviously
describing the wrong surface; what has not been shown yet is that the sample
count reaches the hardware's ADDRESS equation through the fields the record
carries (`BASE_LEVEL` 0 / `LAST_LEVEL` log2(samples) with the ARRAY MSAA type),
which is what the next experiment has to compare against what addrlib implies
for this exact surface.

Also ruled out on the way: the `NUM_SAMPLES` fields the pinned compiler writes
in `ac_descriptors.c` belong to `DB_Z_INFO` (depth/stencil state), not to the
image resource descriptor, so they are not the field a colour-attachment read
would need. The image descriptor's sample geometry really is the level-field
encoding this profile already emits.

## The per-sample read works: MAX_MIP was the missing field (2026-09-22)

The sample-addressing question is answered, and the answer is a field the
pinned compiler writes for exactly this case. A multisampled resource record
needs the sample geometry in **three** places, not two: the ARRAY MSAA type tag
(15), the level fields (BASE_LEVEL 0, LAST_LEVEL log2(samples)) - and
**MAX_MIP = log2(samples)**, the field `ac_descriptors.c`'s
`ac_build_gfx6_texture_descriptor` writes for a multisampled surface on its GFX9
path (`desc[5] MAX_MIP(log2(num_samples))`) and which the GFX10 path of that
same function leaves out. This profile reaches the hardware through AGC rather
than through that builder, and its descriptor does consume it.

Measured on the console (run `20260922T133537885Z_PPSA99994_ps5vk_0x1a253f5f849fd`,
log SHA-256 `449aaac4288736465bdb7886d4344c7a796c245ecbd24aaff34d34d654779154`,
payload eboot `d90ca072307571ea240fdfa92503bd6cccc4d304cb9f820779c524d7d96497ae`):

```
PS5VK_SAMPLE_RATE_FETCH sample_index=0 matched=1024 expected=1024 value=ff000000 verdict=1
PS5VK_SAMPLE_RATE_FETCH sample_index=3 matched=1024 expected=1024 value=ff000003 verdict=1
```

Both indices read the sample they ask for: sample 3's plane holds sample 3's
value and sample 0's holds sample 0's, each across exactly the plane it covers,
with the pass clear left in the tiled padding. The sweep is a sweep over the
uniform-driven pipeline; the baked-index module reads the same planes with the
same values.

Note on the oracle's constant: the previous windows compared against
`0xff030000`, the encoding the 64x64 witness measured, while the 32x32 source
census writes the sample id in the FIRST byte (`ff000001`, `ff000002`,
`ff000003`). With the constant taken from the census the verdict is 1; the
earlier "verdict 0" lines were the wrong expectation, not the wrong read - and
that is why the census mattered.

What this establishes: the multisampled input-attachment read selects and
delivers an arbitrary sample on this hardware, which is the enabler the oracle's
fetch subpass needs and the same read a resolve is built from. What it does not
establish: the resolve itself, the CTS leaves, the row - all unchanged, and
nothing is advertised.

## The resolve arithmetic, measured (2026-09-22)

With the per-sample read working, the arithmetic a resolve is made of is one
more submit of the same pass: subpass 0 dresses every sample of the
multisampled attachment, and subpass 1 runs a stage that reads EVERY sample and
writes their average into its own single-sample target. Two probe defects were
paid for on the way and both are ordinary ones: the resolve pipeline was first
created with the multisampled state of the attachment it READS rather than the
single-sample state of the target it renders into (the front end refuses a
pipeline whose sample count does not match its subpass), and the oracle
compared against the 64x64 witness's encoding instead of the 32x32 census'.

Measured on the console (run `20260922T135608051Z_PPSA99994_ps5vk_0x1a37260b0a121`,
log SHA-256 `8cdd8405708b1170e09010f1ee316967d062ecc079c90fe7be1062be31bd4425`,
payload eboot `72470aa67baa609ef691b7f98410c951710fe0c36e2e9ea26f254286f5b6afb6`):

```
PS5VK_SAMPLE_RATE_FETCH   sample_index=0 matched=1024 expected=1024 value=ff000000 verdict=1
PS5VK_SAMPLE_RATE_FETCH   sample_index=3 matched=1024 expected=1024 value=ff000003 verdict=1
PS5VK_SAMPLE_RATE_RESOLVE samples=4 words=32768 distinct=2 value=ff000002 second=ff808080 averaged=1
```

The four samples hold R = 0, 1, 2 and 3; their average is 1.5, and the target
holds R = 2 - a value NO sample had, with the pass clear in the tiled padding.
So the resolve result is computed by reading the samples this profile can now
address, and the value it produces is the average rather than any one plane.
Both facts come from one run, in one pass, on the same attachment.

What this establishes: the two mechanisms the oracle's pass needs - selecting a
sample of a multisampled colour attachment, and averaging them into a
single-sample target - are measured on hardware, with the exact fields the
resource record has to carry (ARRAY MSAA type, BASE_LEVEL 0 / LAST_LEVEL log2,
MAX_MIP log2). What it does not establish: the executor does not yet EMIT that
resolve for a subpass that declares a resolve target - it still refuses such a
pass - and no CTS leaf passes, the row stays a blocker, and nothing is
advertised.

## The driver's own resolve program (2026-09-22)

A resolve cannot be produced by a probe's payload: the draw that averages the
samples has to be emitted by the DRIVER, so its stages have to live in the
driver. `tools/build_resolve_shaders.py` compiles one averaging fragment per
served sample count (2x and 4x; the reads are written out rather than looped,
because this profile's fragment interface has only been measured on
straight-line subpass reads) plus the oversized-triangle vertex stage they pair
with, into a header the SDK build compiles in, and `native/resolve_program.c`
pairs each with the descriptor contract an averaging stage needs - set 0 binding
0 is the input attachment, fragment-visible, and nothing else - before compiling
the pair through the same runtime compiler the pipeline objects use.

`tests/test_runtime_graphics_compiler.c` pins it: both served counts compile into
a program whose fragment has machine code, whose descriptor set is valid and
whose used-binding mask names exactly the input attachment; a count with no
generated stage (8x, and 1x, which has no samples to average) is refused rather
than averaged by a stage that reads the wrong samples.

What this establishes: the driver can build the resolve draw's program with the
descriptor contract the measured read used. What it does not establish: the
executor still refuses a subpass that declares a resolve target, so the program
is not yet emitted; that wiring - the target, the barrier and the draw at the
subpass boundary - is the next slice. The row stays a blocker and nothing is
advertised.

## The executor emits the resolve, and the next gate is a layout (2026-09-22)

The resolve draw is now emitted by the driver. `native/graphics_queue_ps5.c`
gained `resolve_draw_emit`, which builds the draw on the stack: a one-subpass
synthetic pass whose colour reference is the resolve attachment, a framebuffer
carrying both views, a descriptor set whose single element is the multisampled
attachment as an input attachment, and a pipeline carrying a compiled-then-
LOADED resolve program (`ps5vk_native_resolve_program_acquire` plus
`ps5vk_native_runtime_graphics_create`, the same loader every app pipeline
uses). It is called where a subpass that declares a resolve target ENDS - at the
next subpass boundary and, for the last subpass, before the postlude. The stale
blanket refusal of resolve subpasses is gone; the per-subpass validation admits
exactly the shape the emission describes and keeps the refusal for everything
else.

The path was named step by step on hardware, and each step is now reported
rather than guessed (run `20260922T145043495Z`, log
`830121e53f979d4fe17436f04bf8d0e2c3f66412a69b843cbf12f6251c2c54ee`, payload
eboot `d00844385e55341b0ab0e9adeda8d6e90650c2b14166eacc60fb9a6a2dc9bb12`):
`PS5VK_RESOLVE_STEP target_set/native_target/descriptor` (0 for all three), the
produced draw (`PS5VK_RESOLVE_DRAW serial=7 subpass=0 samples=4 colour=0
resolve=1 targets=1 words=8`), and then the refusal that is left -
`PS5VK_INPUT_ATTACHMENT_REFUSED serial=7 subpass=1 inner=123 rc=-8 defined=1
layout=5 view_is_fb=1 ref=0`. In that same run the walk's own phases still pass
- `PS5VK_SAMPLE_RATE_FETCH ... sample_index=3 ... value=ff000003 ... verdict=1`,
`PS5VK_SAMPLE_RATE_RESOLVE ... value=ff000002 ... averaged=1` and
`PS5VK_SAMPLE_RATE_TARGETS ... subpasses=2 ... preserved_hits=4096 ... verdict=1`
- so the emission did not trade away any measured phase. Layout 5 is
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`,
which is the layout the pinned CTS gives its fetch subpass's input attachment,
while this profile's input-attachment gate and its boundary transition assume
`GENERAL` (the layout the multiview witness declared). That is the next gate,
and it is a real one: the boundary barrier has to leave the attachment in the
layout the SUBPASS declares, and the gate has to accept `SHADER_READ_ONLY_OPTIMAL`
for an input read.

Two probe defects and one driver defect were paid for on the way to the
emission, all recorded rather than hidden: the stale blanket refusal above, a
synthetic descriptor set with no pool (refused at `descriptor_plan`), a
synthetic pipeline state that had never been through the native loader, and a
legacy prepare entry that passes a zero shader address (the descriptor table's
high-address check needs the loaded pair's aperture, so the runtime entry is
used). Both host tests and the console runs remain green before this window's
last refusal: the walk's own phases - per-sample fetch, resolve arithmetic,
two-subpass targets with a preserve list - all still pass.

## The layout equation, and the resolve that lands (2026-09-22)

The last window left two things open: the layout the pinned oracle reads its
colour attachment in, and whether the resolve draw the executor emits actually
reaches its target. Both are closed here. The second one cost five console runs,
because each run removed exactly one layer and named the next.

What changed:

- The input-attachment gate accepts the two READ layouts the render-pass
  frontend (`src/vk_render_pass.c`) already admits: `GENERAL`, which the
  multiview witness declares, and `SHADER_READ_ONLY_OPTIMAL`, which the pinned
  multisample oracle declares for its fetch subpasses
  (`external/vulkancts/modules/vulkan/pipeline/vktPipelineMultisampleTests.cpp`:
  `pInputAttachments[0].layout` and the descriptor's `imageLayout` are both
  `SHADER_READ_ONLY_OPTIMAL`). Each side is admitted by its own pinned rule -
  the reference by the layouts an input reference may name, the record by the
  input-attachment layout list - and a layout that is not a read layout is still
  refused on either side.
- The prelude walks each attachment's LAYOUT SEQUENCE in subpass order, through
  the new `ps5vk_render_pass_attachment_layout`: the attachment's initial
  layout, then the layout each subpass declares for it as the pass reaches that
  subpass, then the attachment's final layout. The boundary transition therefore
  leaves the attachment in the layout the READING subpass declares instead of
  assuming `GENERAL`, which is what the gate then checks the recorded descriptor
  against.
- A resolve target now honours the pass's `LOAD_OP_CLEAR`: the whole surface is
  filled before the pass begins (`PS5VK_RESOLVE_CLEAR_PREPARED`), as Vulkan
  requires for an attachment whose load operation is clear. The driver used to
  leave the target holding whatever its allocation contained, which is why a
  readback of a resolve target could show an earlier phase's pattern.
- Three defects in the emission itself, each measured:
  the synthetic operation carried no vertex or instance count, and a zero count
  is Vulkan's "no rasterization side effects" draw - the emitter returned
  success without writing a word (`words=8`, i.e. only the barrier);
  the synthetic pipeline set `color_write_mask` but not the blend block
  `CB_TARGET_MASK` is actually carried from, so the hardware mask was zero;
  and the prepared draw's AGC context block and the loaded shader pair were
  released INSIDE the walk, while the command stream references both by address
  and the GPU reads them at submit. The last one is the defect that kept the
  target untouched: the draw went out, ran, and read freed memory.
- The probe's resolve oracle draws a SPREAD pattern now
  (`experiments/graphics/runtime_subpass_write_spread.frag`, R = 4*(gl_SampleID+1)/255
  with per-sample shading enabled), so the samples hold 4, 8, 12 and 16 and
  their average is 10: a value NO sample holds, and one no 0..3 pattern an
  earlier phase of the same payload leaves in reused memory can counterfeit. It
  also censuses all four targets of the oracle's pass, not just the resolve one.

Measured, the decisive run (`20260922T160651821Z`, log
`1737a9bb1f187ec2c3f7d381be33469e7f166872541f39bab4bf3bac159e6d9`, payload eboot
`fb7c519df9339de207f5b9605c1cfa69cb4edb639d3788c8d579670f55dd0f4e`):

```text
PS5VK_RESOLVE_CLEAR_PREPARED serial=7 target=1 word=ffbf8040 bytes=131072
PS5VK_RESOLVE_DRAW serial=7 subpass=0 samples=4 colour=0 resolve=1 targets=1 words=37
PS5VK_GRAPHICS_PREPARED serial=7 draws=3 words=228
PS5VK_SAMPLE_RATE_TARGET_CENSUS marker=source_target_map words=65536 distinct=5 clear_hits=61440 sample_hits=4096 average_hits=0 verdict=0 value0=ff000004 hits0=1024 first0=0 last0=3327 value1=ffbf8040 hits1=61440 value2=ff000008 hits2=1024 value3=ff00000c hits3=1024 value4=ff000010 hits4=1024
PS5VK_SAMPLE_RATE_RESOLVED extent=32x32 words=32768 distinct=2 clear_hits=31744 sample_hits=0 average_hits=1024 verdict=1 value0=ff00000a hits0=1024 first0=0 last0=3327 value1=ffbf8040 hits1=31744
```

The resolve target holds the AVERAGE (`ff00000a`, R = 10) across exactly its
32x32 plane and no sample's value anywhere in its span, with the pass clear in
the tiled padding; the source holds the four distinct sample planes the SPREAD
pattern wrote (4, 8, 12, 16). Every other phase of the walk passes in the same
run: `PS5VK_SAMPLE_RATE_SHADED ... verdict=1`, `PS5VK_SAMPLE_RATE_FETCH ...
sample_index=0 ... verdict=1` and `sample_index=3 ... value=ff000003 ... verdict=1`,
`PS5VK_SAMPLE_RATE_TARGETS ... subpasses=2 ... preserved_hits=4096 verdict=1`, and
the lifecycle closes clean (`PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0`,
`PS5VK_GRAPHICS_API_CLEANUP_COMPLETE`, `BYE seq=367`).

The runs that named each layer, every one of them a clean lifecycle (each entry
is run id, log sha256 prefix, payload eboot sha256 prefix, and what it showed):

- `20260922T155027168Z` / `7b8c12468891fd39` / `d0084438` - the oracle's pass
  executes end to end for the first time (submit `rc=0`, `draws=3`), and the
  resolve target reads back as four sample-looking values.
- `20260922T155421552Z` / `a7dae8bd647c5349` / `63dcc5a8` - the sharpened oracle
  is in place, and the TWO-SUBPASS verdict collapses to 0: the two phases of the
  walk had been sharing one subpass-0 module, so the spread pattern had replaced
  the flat one the two-subpass variant measures. Separated into their own
  modules.
- `20260922T155629250Z` / `3e8f8a3b352e0008` / `c1c37579` - modules separated,
  two-subpass verdict back to 1, all four targets censused. The resolve target's
  content is byte-for-byte the 0..3 pattern and clear of the witness phase
  earlier in the same payload: it is STALE MEMORY, not a resolve result.
- `20260922T155759226Z` / `a0ba629adae5d43f` / `23380172` - per-sample shading
  enabled, so the source shows four distinct planes (4, 8, 12, 16) and the
  resolve's expected average (10) is distinguishable from every sample. The
  target is still stale, and the target registers differ from the source's in
  their address word only.
- `20260922T155923914Z` / `78fbeaff61953ce2` / `f004d02b` - the missing vertex
  and instance counts: the emission grows from 8 words to 37 and the submission
  from 199 to 228, and the target is STILL untouched.
- `20260922T160123400Z` / `6f80a485a1a7b47b` / `0cb256b2` - the resolve target's
  own `LOAD_OP_CLEAR` is honoured, so the target reads back as one value across
  the whole span (`clear_hits=32768`): the draw writes nothing into it, rather
  than writing something that looked like stale data. The image spans logged in
  the same run (`200020000` for the resolve target) match the target register's
  address word exactly, so the draw IS aimed at the right memory.
- `20260922T160301551Z` / `e1b4bcbd732d0d7f` / `752b67de` - the pipeline's blend
  block carries `CB_TARGET_MASK`; a synthetic pipeline that sets only
  `color_write_mask` hands the hardware a zero mask. Set both, as
  `vkCreateGraphicsPipelines` does. The target is still untouched.
- `20260922T160423999Z` / `72a8f71398b587bd` / `400a7339` - the emitted words are
  logged: the barrier, the AGC context and link packets, and a
  `PKT3_DRAW_INDEX_AUTO` of three vertices are all there, so the draw leaves the
  queue as a draw. The context address it references is the one the procedure
  prepared.
- `20260922T160651821Z` / `1737a9bb1f187ec2` / `fb7c519d` - the last change:
  the prepared draw and the loaded shader pair are handed to the JOB instead of
  being released inside the walk. `PS5VK_SAMPLE_RATE_RESOLVED ... verdict=1`.

What this establishes: the pinned oracle's pass runs end to end through this
driver, its per-sample reads deliver the plane each index names, and its resolve
attachment receives the AVERAGE of the samples - written by a draw the driver
owns, on hardware, with no sample's value anywhere in the target. What it does
not establish yet: no upstream CTS leaf has been run against this path, the row
stays a blocker, and nothing is advertised. `make check` is green; the console
window that produced this run left the acceptance payload restored and
`running=none`.

## The focused CTS selection, and the compiler assertion behind it (2026-09-22)

The row's last axis is CTS, and this window both measured it and named the
blocker - in the compiler, not in the driver's execution path.

Two runs of the same measurement selection (512 cases: the frozen 462-case
acceptance set plus the 50 `t06-sample-rate-pending` leaves moved into `cases`)
say the whole story:

- **Without the feature advertised** (`PS5VK_SAMPLE_RATE_DIAGNOSTIC` unset, so
  the shipping platform does not set the bit): run `20260922T173812017Z`, log
  `77ff13e863d48a86fdf9c889502ce4d4a8e9abe75cbd8349c6db80f3a2ae4cf8`, payload
  eboot `bea19ce65db1c2dc328e6134d211c3e1acc6e3c9f9b9d0b1c678cacec5f9c99c` -
  `Test execution complete: pass=462 fail=0 notSupported=50 total=512`,
  `UPSTREAM_CTS_COMPLETE status=0`, clean `BYE`. dEQP skips the leaves at
  feature-query time, so no leaf is judged at all.
- **With it advertised** (the measurement switch on; the shipping bit is still
  off): run `20260922T175103500Z`, log
  `d4ca09dbce54431b3b863a887ed84122e4e50f83124f941da23f3da791cbd631`, payload
  eboot `9735d96702e8ab94e159972660e8660c0896cab80ddec68ea4e779f4b62af31f` - the
  process dies inside the first sample-rate leaf that actually runs,
  `dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_0.samples_2.primitive_triangle`,
  and the log ends without `BYE` (`clean=false`).

The payload now logs the case each run is in (`UPSTREAM_CTS_CASE`, added here
because an unclean close leaves no result to name), the queue preparation phase
(`PS5VK_GRAPHICS_PHASE`), and the front-end calls that build a case
(`PS5VK_IMAGE_CREATE`, `PS5VK_IMAGE_BIND`, `PS5VK_IMAGE_VIEW`,
`PS5VK_RENDER_PASS_CREATE`, `PS5VK_FRAMEBUFFER_CREATE`, `PS5VK_PIPELINE_CREATE`,
`PS5VK_NATIVE_PIPELINE_CREATE`). The last records of the failing run are:

```text
UPSTREAM_CTS_CASE name=dEQP-VK.pipeline.monolithic.multisample.min_sample_shading.min_0_0.samples_2.primitive_triangle
PS5VK_IMAGE_CREATE format=37 samples=2 usage=00000011 extent=32x32
PS5VK_IMAGE_BIND samples=2 usage=00000011 extent=32x32
PS5VK_IMAGE_CREATE format=37 samples=1 usage=00000013 extent=32x32
PS5VK_IMAGE_BIND samples=1 usage=00000013 extent=32x32
PS5VK_IMAGE_VIEW format=37 type=1
PS5VK_IMAGE_VIEW format=37 type=1
PS5VK_RENDER_PASS_CREATE attachments=2 subpasses=1 dependencies=0
PS5VK_FRAMEBUFFER_CREATE attachments=2 extent=32x32 layers=1
PS5VK_PIPELINE_CREATE subpass=0 samples=2 stages=2 topology=3 vb=1 va=2 colors=1 dyn=0 ds=1 rp=1
```

and then nothing: the native pipeline entry is never reached, so the death is
inside `vkCreateGraphicsPipelines`' own front-end/compile phase for that one
call.

The cause was then reproduced on the host, outside the console, with the
sources the run itself logged: the case's vertex and fragment GLSL were
extracted from the reassembled QPA, compiled with `glslangValidator -V
--target-env vulkan1.0`, and handed to the same runtime compiler the console
uses (`ps5vk_runtime_graphics_compile`), under AddressSanitizer. The pair
aborts:

```text
SPIR-V WARNING: Unsupported SPIR-V capability: SpvCapabilitySampleRateShading (35)
src/amd/common/nir/ac_nir.c:185: ac_nir_load_arg_at_offset: Assertion `arg.used' failed.
  #0 ac_nir_load_arg_at_offset
  #7 ac_nir_unpack_arg (rshift=24, bitwidth=1)
  #9 lower_abi_instr            src/amd/vulkan/nir/radv_nir_lower_abi.c:455
  #13 radv_nir_lower_abi
  #14 radv_postprocess_nir      src/amd/vulkan/radv_postprocess_nir_standalone.c:283
  #15 psbc_compile_impl         libpsbc/psbc_compile.c:3623
  #17 ps5vk_runtime_graphics_compile
```

`radv_nir_lower_abi.c:455` is the `load_ps_iter_mask_amd` case, which unpacks the
`PS_STATE_PS_ITER_MASK` bit out of the `ps_state` argument - and that argument is
not marked used for this compile.

Bisected by hand with the same harness, one shader at a time:

| fragment stage | result |
| -------------- | ------ |
| reads `gl_SampleID`, never `gl_FragCoord` (the sample-rate probe's spread module) | compiles |
| reads `gl_FragCoord`, never `gl_SampleID` | compiles |
| reads `gl_FragCoord` and declares `gl_SampleID` (the CTS leaf's own module - its `sampleId` is even dead code) | aborts with the assertion above |

The compiler skips its fragment-coordinate lowering whenever the shader
declares sample shading (`libpsbc/psbc_compile.c`: the
`radv_nir_lower_opt_fs_frag_pos` call is guarded by
`!gfx_state.ms.sample_shading_enable && !nir->info.fs.uses_sample_shading`), and
`gfx_state.ms.sample_shading_enable` is never set from the compile options at
all, so the two facts cannot agree: the shader keeps a built-in whose argument
the ABI never marks used. A driver-side attempt was measured and REJECTED:
compiling the pixel stage with `rasterization_samples=1` whenever the pipeline
does not enable sample shading does not avoid the abort, so the fix belongs in
the compiler's fragment-coordinate/sample-shading path. That change was
reverted; no rendering behaviour changed in this window.

What this establishes: the CTS blocker is a pinned-compiler abort on one shape
(a fragment stage that reads `gl_FragCoord` and declares `gl_SampleID`), with a
host reproduction that needs no console. What it does not establish: the
remaining 49 leaves have not been reached, the row stays a blocker, and nothing
is advertised.

## The compiler abort fixed, and the leaves judged at last (2026-09-22)

The abort above was fixed where it lives - in the pinned compiler - and with it
the focused selection stopped dying and started judging the leaves.

The compiler is a PINNED dependency: `tools/build_sdk.py` validates its archive
against the identity stamp `tools/build_psbc.py` writes (source commit +
archive sha256), so a loose edit is not shippable. The fix is therefore a commit
in the compiler's own repository:

- `opengnm-psbc` branch `codex/fragment-coord-sample-shading`, commit
  `992ba13385a77fbc49683da2124f5a9a12f09fd8`, PR
  https://github.com/mpereiraesaa/opengnm-psbc/pull/23. It runs
  `radv_nir_lower_opt_fs_frag_pos` unconditionally for fragment stages in
  standalone compiles, as `radv_pipeline_graphics.c` already does, instead of
  skipping it whenever the pipeline or the shader asks for sample shading. The
  skip is what let the pass run later, inside `radv_postprocess_nir`, after
  `radv_nir_shader_info_pass` had decided the stage arguments - so the shader
  kept `load_use_float_frag_coord_xy_amd` with no `ps_state` argument, and the
  compiler aborted on `assert(arg.used)`.
- `build/libpsbc.ps5.a` rebuilt with that identity (`source_commit
  992ba13385a77fbc49683da2124f5a9a12f09fd8`).

Measured, run `20260922T183509764Z`, log
`6cb7d0d655e2c20c20cc35bed9f4a7bc5901eddc817a5f7b94a4bd7d0098824d`, payload
eboot `33b84bc0da289e726ef5c006c8fb993ecbb2b15ddc1ae0651c8c0db203b1fea5`: 512
expected, 512 reported, **462 pass, 50 fail, 0 notSupported**, clean lifecycle.
No case dies any more, and every sample-rate leaf is judged for the first time.

Two driver-side refusals then had to agree with what recording had let through,
both of them measured on the console in the same window:

1. The prelude refused the pinned leaves' own first-use barrier
   (`UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL`, no source access, the
   colour-attachment write as destination, `TOP_OF_PIPE -> COLOR_ATTACHMENT_OUTPUT`)
   for the multisampled colour image and the single-sample attachments the oracle
   reads back - `PS5VK_GRAPHICS_PREPARE_FAILED ... phase=prelude site=701`. The
   executor performs exactly this transition itself in the pass prelude, and the
   recorder already accepted it, so `native/upload_commands_ps5.h` now accepts it
   for those roles (run `20260922T185428184Z`, log
   `7f46b94674cd2233f35252e16ec969c09493f83899b2a1a95f57931fd1a58021`, payload
   `18b0efd3dc3b7c9cc2141c370c8f9ede556d982fd7e433255c23c31f6568d1d4`: the
   prelude refusal is gone).
2. The driver's own resolve draw then failed building the multisampled resource
   record, because `ps5vk_image_resource_descriptor` required the
   INPUT_ATTACHMENT usage bit - and the pinned CTS resolves an image it creates
   with COLOR_ATTACHMENT and TRANSFER_SRC alone (`RENDER_TYPE_RESOLVE`). A
   subpass resolve is implementation work, not an application's descriptor read,
   so `src/texture_descriptor.c` now requires the colour-attachment role for a
   multisampled record while the app-facing input-attachment gate keeps its
   strict usage rule (`tests/test_texture_descriptor.c` pins both directions).

Measured, run `20260922T185644808Z`, log
`d895f7c050562780e6572c5db46b9eb8fd2055f7fab23bec75afd110620d01d6`, payload
eboot `3b9bc090122363b64b783625463d80a550e8fa31cfe63dbc30d2c70dbafaee33`:
**475 pass, 37 fail**, clean lifecycle, and thirteen sample-rate leaves PASS -
the first upstream CTS leaves this row has ever had green, including
`min_sample_shading.min_0_0.samples_2.primitive_triangle` and its `.samples_4`
sibling.

The remaining 37 failures are named, and they are three different problems:

| count | deqp result | what it means |
| ----- | ----------- | ------------- |
| 20 | `VK_ERROR_FEATURE_NOT_PRESENT at vkPipelineConstructionUtil.cpp:178` | plain VS+FS POINT and LINE topologies, which this profile has never served (the same limitation T05 recorded as a diagnostic) |
| 7 | `Got less unique colors than requested through minSampleShading` | the pixel-iteration count our state asks for is not the one the oracle expects for a fractional `minSampleShading` |
| 8 | `Did not get any covered pixel` | a rendering/coverage defect on the leaves that reach the comparison with no covered pixel |
| 2 | `Invalid color` | a rendered value outside the expected set |

What this establishes: the CTS axis is no longer blocked by the compiler, the
payload runs all 512 cases to a clean close, and thirteen focused leaves pass.
What it does not establish: the row is not complete - 37 leaves still fail for
the three reasons above, the compiler fix still needs merging in its own
repository, and nothing is advertised.

## The sample-shading state the compiler was never told (2026-09-22)

The unique-colours family was expected to be the per-sample fragment coordinate:
`radv_nir_lower_opt_fs_frag_pos` chooses between the per-sample position path
and the pixel-centre one from the pipeline's sample-shading state, and the
standalone compiler had no way to be told that state - it emitted a runtime
selection reading the PS state user SGPR, which this ABI does not supply. The
state was therefore published (`PsbcCompileOptions::sample_shading_enable`,
opengnm-psbc branch `codex/fragment-coord-sample-shading`, commit
`883747bf04af7746b6ec49e00d2a247a5327e326`, on top of the abort fix) and set
from the pipeline key in `native/runtime_graphics_compiler.c`.

Measured twice with the rebuilt compiler archive (identity
`d9fa65080bf5761beaf0032f9a20ab5a1afd957c59814c07ff5d54f4cb4b64e1`), same
selection, same payload eboot
`e03fb14b98567c4ac4b5d7d8b8b4583e31e49f893c886c6fadf0b361a85e5b88`:

- run `20260922T191325185Z`, log
  `fd8b931eece17c1984d91a2cbca98cf0126ace48e1b1413a2f7d55c39556bc16`: 474 pass / 38 fail
- run `20260922T191416917Z`, log
  `01a3a3a10950fb681286870c38dd927338621b89ee08bce952bb13ad9543394c`: 477 pass / 35 fail

The honest reading of the two runs together: **the hypothesis was not
confirmed**. The stable unique-colour failures are unchanged by it - the five
`min_sample_shading` triangle leaves at min 0.5, 0.75 and 1.0 at both served
counts - and the leaves that move between runs are the `quad` families, which
are FLAKY in this driver: across the two runs the coverage and invalid-colour
sets differ by several leaves each (only one leaf is stable in each of those
two groups), which is a defect of its own and would fail an acceptance run
whatever the score. The change is kept because the state it publishes is one
the compiler legitimately needs and the previously-working shapes still
compile, but it is NOT the fix for the unique-colour family, and that family
still needs the hardware per-sample position state to be understood and
measured - the same way the per-sample sample-id path was.

Current best measurement: 477 pass / 35 fail of 512, of which 20 are the
plain point/line pipeline refusals, 5 are the stable unique-colour leaves, and
the rest are the flaky `quad` families. Nothing is advertised.

### What the failing leaves actually contain (2026-09-22)

The unique-colour failure was then read directly out of the run's own report
instead of inferred: the QPA of run `20260922T191416917Z` carries the three
images the oracle compares for
`min_sample_shading.min_1_0.samples_2.primitive_triangle` - the render without
sample shading and one image per sample of the sample-shaded render - and
decoding them says exactly what the driver produced:

```text
noSampleshadingImage : 1024 pixels, 976 zero, 48 of 808000ff
sampleShadedImage[0] : identical histogram
sampleShadedImage[1] : identical histogram
sample image 0 vs 1    : 0 differing pixels
covered pixel         : sample0 808000ff  sample1 808000ff  noShading 808000ff
```

`80 80 00 ff` is RGBA8 (0.5, 0.5, 0, 1): the shader's `fract(gl_FragCoord.xy)`
is the PIXEL CENTRE for every sample, and the two per-sample images are
byte-identical - so the fragment coordinate does not vary per sample, which is
precisely what the oracle's `expectedUniqueSamplesCount =
round(minSampleShading * samples)` then rejects at min 0.5/0.75/1.0.

Two host experiments then placed the defect:

- Compiling the leaf's own fragment module with the pipeline's sample-shading
  state set and clear produces *byte-identical* machine code (104 bytes, same
  checksum), so the state published in this window is not what the
  fragment-coordinate lowering keys on - the shader's own
  `nir->info.fs.uses_sample_shading` already decides it, which is why the
  measurement above could not move.
- The per-sample position itself is not in the shader's own arithmetic:
  `radv_nir_lower_opt_fs_frag_pos` lowers per-sample `gl_FragCoord` to
  `pixel_coord + sample_pos`, and the pinned tree resolves `sample_pos` through
  `nir_load_sample_positions_amd` into a fetch from the RING at
  `RING_PS_SAMPLE_POSITIONS` (`radv_nir_lower_abi.c`). This driver supplies no
  sample-position table and no ring offset for the fragment stage, so the read
  cannot return the sample's location.

That is the next slice, and it is now stated as a driver-side gap with two
concrete halves: supply the sample positions the fragment path reads, and make
the compile decision follow the pipeline's state rather than only the shader's
declaration.

### Correction to the paragraph above (2026-09-22)

The claim that this shader's per-sample coordinate needs the ring's
sample-positions table is WRONG, and the compiled ISA says so. Disassembling the
leaf's own fragment module (compiled through the same runtime compiler with
`PSBC_DEBUG_DISASM=1`) shows it never reads a sample position:

```text
v_cvt_f32_u32                     ; the integer pixel coordinate
v_add_f32 0.5, ...                ; pixel_coord + 0.5, the pixel centre
v_cndmask_b32 <+0.5 value>, <interpolated coord>, <condition>
v_fract_f32 ...
```

The condition comes from `load_use_float_frag_coord_xy_amd`, i.e. the
`PS_STATE_USE_FLOAT_FRAG_COORD_XY` bit of the PS-state user SGPR - a slot this
driver never writes - so it reads as zero, the shader takes the pixel-centre
branch on every iteration, and every sample gets (0.5, 0.5). That is the
failure; no sample-position table is involved for this shader at all.

The next slice is therefore: (1) make the pipeline say which fragment
coordinate it really delivers - the SPI input-address register's
`POS_FIXED_PT_ENA` (`ac_shader_util.c` reads it, and
`ac_nir_lower_intrinsics_to_args.c` unpacks the `pos_fixed_pt` argument), since
per-sample positions are only reachable with it disabled and the float path
selected - and (2) publish and program the PS-state user SGPR
(`ps_state_user_data_dword`, alongside the existing base-vertex and
push-constant slots) with that bit plus `NUM_SAMPLES`, `PS_ITER_MASK`
(`ac_get_ps_iter_mask`), `USE_QUAD_POS` and `USE_SAMPLE_MASK_IN`, so the branch
and the hardware agree. Nothing about the rendered images above changes: they
still show the pixel centre for every sample.

## The sample positions the raster stage never had (2026-09-23)

The paragraph above ends by naming the next slice as "the PS-state user SGPR
plus `POS_FIXED_PT_ENA`". **That reading was wrong, and the measurement says
so.** The compiler already publishes the interpolated-coordinate shape for a
sample-shaded standalone compile, and the register that decides the answer is
neither of the two it named. What the device was missing is on the RASTER side,
and the pinned PAL source (`third_party/amd-pal`, `gfx9MsaaState.cpp`) names
both halves:

* `PA_SC_MODE_CNTL_0.MSAA_ENABLE` - PAL sets it whenever the stage's coverage
  samples are more than one. This driver published the single-sample word
  (`0x22`) for every draw, so the rasteriser had no sample locations to work
  with.
* the sixteen `PA_SC_AA_SAMPLE_LOCS_PIXEL_*` context words (`0x2fe..0x30d`),
  four samples each, X in the low nibble of a byte and Y in the high one as a
  signed offset from the pixel centre in 1/16 pixel units, all four pixels of
  the quad sharing the pattern. They had never been written, so all sixteen read
  zero and every sample of a pixel sat ON the pixel centre.

The pixel stage's own half comes from LLPC
(`third_party/amd-llpc/lgc/lowering/RegisterMetadataBuilder.cpp`):
`SPI_BARYC_CNTL.POS_FLOAT_LOCATION = 2` ("calculate per-pixel floating point
position at iterated sample number") whenever the wave iterates per sample, and
0 otherwise. This compiler published 0.

### The measurement that places it

A diagnostic payload publishes those registers per draw instead of taking them
from the compiler (`native/sample_rate_diagnostic.h`, reachable only under
`PS5VK_SAMPLE_RATE_DIAGNOSTIC`) and draws the `fract(gl_FragCoord.xy)` witness
into a 4x target with per-sample shading. Payload eboot
`bdebc7259a0bffb650e72728e7c2526e25c4f5b3253dd581b0d5f7c4b18dd7c4`, run
`20260922T225844981Z_PPSA99994_ps5vk_0x1c10e85bc0773`, log
`6ea767abc3ef017908d874e0dcd9e5091185393841b7a1bf5d5cad9706ea689d`, clean
lifecycle. The census is the distinct-word count of the whole surface:

```text
default                           values=ff602000,ffdf6000,ff209f00,ff9fdf00 oracle=coordinate verdict=1
without-msaa-enable               values=ff808000                          oracle=coordinate verdict=0
without-sample-locations          values=ff808000                          oracle=coordinate verdict=0
without-sample-distance           values=ff602000,ffdf6000,ff209f00,ff9fdf00 oracle=coordinate verdict=1
without-position-location         values=ff808000                          oracle=coordinate verdict=0
col-format-zero (control)         shaded_values=0                          oracle=coordinate verdict=0
```

`ff602000`, `ffdf6000`, `ff209f00` and `ff9fdf00` are RGBA8
`fract(gl_FragCoord.xy)` for (0.375,0.125), (0.875,0.375), (0.125,0.625) and
(0.625,0.875) - Vulkan's standard 4x sample locations, which is also what makes
the 1/16-offset encoding above self-checking: the four values the hardware
delivers are the four values the pattern asks for. `ff808000` is (0.5,0.5), the
pixel centre, and is what every configuration produced before this fix.

The three necessary elements are therefore `MSAA_ENABLE`, the sample-location
words, and the position location; each one withdrawn collapses the census back
to the pixel centre. Withdrawing `MAX_SAMPLE_DIST` (kept at the pattern's own
extent, 6/16 at 4x, because PAL derives it from the pattern) does not change the
position, and the `col-format-zero` control proves the overrides reach the
pipeline at all. `PA_SC_MODE_CNTL_0`, the sample words and `SPI_BARYC_CNTL` are
now published by `native/draw_state_ps5.c` for exactly this shape, only when the
pipeline carries more than one sample, and the position location only when the
wave really iterates per sample.

### The focused selection after the fix

The same 514-case measurement (the frozen acceptance selection plus the 50
`t06-sample-rate-pending` leaves) on payload eboot
`445c894191c4914b15119c33075af3efe9c3a3eb9e4322da6a323b0da20e500e`, run
`20260922T230226909Z_PPSA99994_upstream-cts_0x1c142318ff862`, log
`4706ca168f5f574f6dfb517fbecdf56dc503c754bb7106764d242594e66ebdac`, clean
lifecycle, reports **489 Pass, 25 Fail, 0 NotSupported**. Every
`min_sample_shading.*.primitive_triangle` leaf passes now; before the fix the
unique-colour family failed 35 of 512 (five stable `min_sample_shading` triangle
leaves at min 0.5, 0.75 and 1.0 at both served counts, plus the flaky `quad`
families).

What still fails, in full:

* twenty leaves - `min_sample_shading.*.samples_2|samples_4.primitive_line` and
  `...primitive_point_1px` - are the plain vertex+fragment POINT/LINE pipelines
  this profile does not serve at all: `VK_ERROR_FEATURE_NOT_PRESENT` raised by
  the CTS itself at `vkPipelineConstructionUtil.cpp:178`, with the driver's own
  `PS5VK_PIPELINE_CREATE` line as the last driver record and no refusal marker.
  That is the same scope decision the earlier runs recorded, not a
  sample-position defect.
* five leaves - `min_sample_shading_disabled.*.samples_4.quad` - fail inside the
  oracle as "Invalid color" or "Did not get any covered pixel, cannot test
  minSampleShadingDisabled". These are the `quad` family the earlier runs
  already measured as run-to-run flaky; they are a defect of their own and would
  fail an acceptance run whatever the score, so they are the next item, not part
  of this fix.

### The remaining flakiness was the colour-to-texture barrier (2026-09-23)

That last paragraph was a prediction, not a diagnosis, and the next two runs
replaced it with one. A repeat of the SAME payload (run
`20260922T230750349Z`, log
`edc244cc866c4aa6e6474c46f12852b90c7626c7b01881af39f57c21d25ea08e`, 488 Pass /
26 Fail) failed a different set:
one `min_sample_shading_enabled` quad and two `samples_2` quads joined the
failing five, so no property of the *disabled* group explains it.

The run's own QPA images name the defect instead. In
`min_sample_shading_disabled.min_0_0.samples_4.quad` the oracle compares the
resolved image against the four per-sample images:

* the four per-sample images held the correct uniform `808000ff` -
  `fract(gl_FragCoord.xy)` at the pixel centre, the value a per-pixel
  invocation writes - over the whole 32x32, so the multisampled attachment
  really did hold what the draw wrote;
* the RESOLVED image held the drawn value in only a few 8x8 tiles
  (`20200040` in one tile and `40400080` in others: one and two of four samples
  covered) and the clear word everywhere else, while the previous run of the
  same case resolved almost the whole target. Different tiles, different runs.

That is the signature of the colour-to-texture barrier being ASYNCHRONOUS. The
driver's `ps5vk_graphics_color_to_texture` emits the reference RELEASE_MEM
packet (event 0x2d `FLUSH_AND_INV_CB_DATA_TS` with the GCR writeback/
invalidation bits, `DST_SEL=TC_L2`, no completion token), and a RELEASE_MEM
retires when the event is accepted: the writeback it starts continues behind
it, so a draw that reads the attachment through the texture path can see
whatever the caches had not written back yet. The pinned RADV emitter
(`gfx10_cs_emit_cache_flush` in the gfx10 Mesa tree) uses the same event with
the write CONFIRMED - `DST_SEL=MEM`, `INT_SEL=SEND_DATA_AFTER_WR_CONFIRM`,
`DATA_SEL=VALUE_32BIT` towards a token - and then a `WAIT_REG_MEM` for it.

`ps5vk_graphics_color_to_texture_wait` (`src/graphics_sync.c`) is that packet:
the same CB data-flush event with the token selected (word 2 becomes
`0x23000000`), the 32-bit token stored at the address the caller names, and a
`PKT3_WAIT_REG_MEM` equality wait for it. Both barrier sites use it - the
resolve draw's own boundary and the subpass boundary that publishes colour to
the texture path - with the private token word of the arena that executes the
wait, zeroed before the release so the equality wait cannot pass on a value a
previous submission left behind.

Measured on the same 514-case selection, payload eboot
`7eed073a5e49b52ed9df75932b3e3b22504106e1a5d5a824b99765f89d22f27d`, three
consecutive runs, all with a clean lifecycle:

| run | log sha256 | Pass | Fail | quad failures |
| --- | --- | ---: | ---: | ---: |
| `20260922T231948101Z` | `ce28818943d639bca3305f2703d0aae8c4e77deda85f0196eed0052c72930027` | 494 | 20 | 0 |
| `20260922T232046070Z` | `b078a112854bf6af369a472e50b75e7ba7e0e5a5afa7a8120d8644fb5996127f` | 494 | 20 | 0 |
| `20260922T232129201Z` | `5c9a5863317461d4d0de756ad1efb6f9ba0e65602922f7678e3f618c1123d81c` | 494 | 20 | 0 |

The twenty are the POINT/LINE refusals alone, so the 30 applicable
`min_sample_shading*` leaves - the five min-fractions at both served counts for
the triangle and quad geometries - pass deterministically, which is what the row
needs and what the frozen selection cannot carry as a flaky member.

## DXVK262-T06 sampleRateShading promotion (2026-09-23)

T06 is complete: the four requirements of the tranche -
`fragmentStoresAndAtomics`, `dualSrcBlend`, `independentBlend` and
`sampleRateShading` - are satisfied on all four axes, so the live matrix reads
**17/62 ready with 45 blockers** (from 16/62).

The promotion moved the row's own oracle into the frozen selection. The
multisample module has exactly one class whose `checkSupport` requires
`DEVICE_CORE_FEATURE_SAMPLE_RATE_SHADING`; it registers five min-fractions at
two served counts over five primitives. Thirty of those fifty leaves - the
triangle and quad shapes - are the group `sample-rate-shading`, measured Pass;
the twenty line and `primitive_point_1px` shapes moved to
`plain-point-line-pipeline-refused`, because what refuses them is this
profile's pipeline resolver, not this feature: `vkCreateGraphicsPipelines`
returns `VK_ERROR_FEATURE_NOT_PRESENT` and the CTS reports it at
`vkPipelineConstructionUtil.cpp:178` with the driver's own
`PS5VK_PIPELINE_CREATE` line as the last record. The frozen selection is now
494 acceptance cases and 66 diagnostics.

The four axes, each with its own artifact:

* **API** - the public-ABI capability probe on the promoted profile, artifact
  SELF SHA-256
  `439de5c96579b634bb4698adf439631dd545f9298bdccd1c5e1129f750184a07`, run
  `20260922T234500151Z_PPSA99994_ps5vk_0x1c394a882c7bf`, log SHA-256
  `2b04497f8ae6a8d01feb3961f2a7b724bb763c611b69b78d9f3c7b497ece8d20`
  (rebuilt against the promoted matrix, and a first run on the same artifact,
  `20260922T233053671Z` log
  `93268e3d01eadd0a8d922bd7459cc3618010b2e7f03844eb50d6473ac059b13b`,
  verified against the pre-promotion snapshot). Both strict verifications
  reconstructed all 62 rows, observed `sampleRateShading = 1`, and derived the
  same **19 satisfied / 43 blockers** result;
  `framebufferColorSampleCounts` moved to 1x|2x|4x with it.
* **CTS** - three consecutive runs of the 514-case measurement selection
  (frozen 464 + the 50 pending leaves) reported 494 Pass / 20 Fail / 0
  NotSupported each time, the twenty being the POINT/LINE refusals and every
  one of the thirty applicable sample-rate leaves Pass. Payload eboot
  `7eed073a5e49b52ed9df75932b3e3b22504106e1a5d5a824b99765f89d22f27d`
  (measurement selection, register survey available), runs
  `20260922T231948101Z`, `20260922T232046070Z` and `20260922T232129201Z`.
* **Native** - the frozen acceptance selection itself, on the shipping profile
  and without the measurement switch: payload eboot SELF SHA-256
  `e1ed40fb0089c5a39eb0b6c333b74b9aa430d524c10d4f74220452263d40b4cb`,
  selection SHA-256 `f6924b34930a837f88635e47cf388bf9a88fd72e1082a0f3ee9ab6bba5a540ed`,
  run `20260922T232928853Z_PPSA99994_upstream-cts_0x1c2bbd396dece`, log SHA-256
  `287f98ec1e81ccb09642bbf12b75062ed60bb95501d3451849d8f3d5f6633b54`: **494
  reported, 494 Pass, 0 Fail, 0 NotSupported**, clean lifecycle. The
  shipping witness payload (no measurement switch, eboot
  `1ce2fe9cf7d0647535f044c69f48316272a860c2dead603ae9b2135e12a60613`, run
  `20260922T233238023Z_PPSA99994_ps5vk_0x1c2e7dee46277`, log SHA-256
  `0b82307d624e9adeb405e9ce93a782e577246dba50c54be894556f4f6293a570`) is the
  row's own oracle executed directly:

```text
PS5VK_SAMPLE_RATE_SHADED extent=64x64 samples=4 words=65536 shaded_values=4 expected_values=4 matched=4 covered_words=16384 values=ff000000,ff010000,ff020000,ff030000 oracle=sample-id verdict=1
PS5VK_SAMPLE_RATE_SHADED extent=64x64 samples=4 words=65536 shaded_values=4 expected_values=4 matched=4 covered_words=16384 values=ff602000,ffdf6000,ff209f00,ff9fdf00 oracle=coordinate verdict=1
```

One limit the promotion made applicable is NOT satisfied and is recorded rather
than claimed. While `sampleRateShading` is unreported the CTS leaves the
interpolation-offset limits out and this profile reports them relaxed; reporting
the feature brings the core table's own floors (`maxInterpolationOffset >= 0.5`,
`minInterpolationOffset <= -0.5`, `subPixelInterpolationOffsetBits >= 4`) into
scope. This driver reports 0 for all three: no path lowers an interpolation
offset, so they are documented as blockers in the reporting matrix
(`tools/check_reporting_matrix.py`, `KNOWN_BLOCKERS`) instead of being raised to
values nothing measured.

The compiler half of this window is `mpereiraesaa/opengnm-psbc` PR #23
(`codex/fragment-coord-sample-shading`, head
`a33305201385947cb49d74b68f6311a0c2f4add7`): the unconditional fragment-coordinate
lowering that stops the standalone compile from aborting in
`ac_nir.c` (`assert(arg.used)`), the pipeline sample-shading state the
standalone compile needs, and the single compile-time decision about which
fragment coordinate the shader reads. Every payload above was built and run
with that revision - the driver passes `sample_shading_enable` into
`PsbcCompileOptions`, which does not exist before it - so
`tools/prepare_compiler_deps.py` has to pin the merged commit before this
promotion reproduces from a fresh clone. The pin still names `be4d043`.

## Standard uniform buffer layout (2026-09-23)

The public Vulkan 1.0 device advertises `VK_KHR_uniform_buffer_standard_layout`
through the `VK_KHR_get_physical_device_properties2` query route. Device
creation requires an explicit `VkPhysicalDeviceUniformBufferStandardLayoutFeatures`
opt-in. The shipping compiler validates uniform-buffer offsets, array and
matrix strides, and nested members before lowering; invalid layouts remain
rejected. The Vulkan 1.2 aggregate feature structure and API 1.3 remain
unadvertised. `shaderSubgroupExtendedTypes` and `subgroupBroadcastDynamicId`
remain false: the former has a Vulkan 1.1 KHR dependency and incomplete
operation/type coverage, while the latter has no Vulkan 1.0 extension alias.

The subgroup boundary is explicit. The shipping Vulkan 1.0 device exposes no
subgroup stage or operation properties and rejects subgroup SPIR-V at shader
module creation. A separate, default-off diagnostic build used Vulkan 1.2
SPIR-V and the local PSBC candidate `bf2e00b` to measure compute behavior.
It selected each broadcast source ID from GPU memory, used two workgroups and
even-lane activity, checked exact outputs and untouched inactive slots, and
completed a bounded fence with zero guard mismatches. This is compiler/GPU
evidence, not a legal public feature or original CTS result.

| Operand and operation | Stage tested | Verified state | T08 feature bit |
| --- | --- | --- | --- |
| 32-bit unsigned `subgroupBroadcast` with a runtime buffer source ID | Compute | Diagnostic GPU readback: 64/64 active values, 64 inactive slots untouched, guards zero; repeated | False |
| Unsigned 8-bit, signed 16-bit, unsigned 64-bit and 16-bit float scalar `subgroupBroadcast` | Compute | Each diagnostic GPU readback: 64/64 active values, 64 inactive untouched, guards zero | False |
| The same four operand types as two-component vectors | Compute | Both components checked on GPU: 64/64 active values per type, 64 inactive untouched, guards zero | False |
| The same four operand types as three- and four-component vectors | Compute | PSBC host compile and wave32 NIR only; no GPU result | False |
| Any subgroup operation in graphics stages | None | No reviewed stage exposure or GPU oracle | False |

The diagnostic narrow-integer runs enabled compiler options and SPIR-V
capabilities only in the private build. The shipping guards remain in place.
The vec2 run IDs are `20260923T114013349Z`, `20260923T114102324Z`,
`20260923T114235184Z`, and `20260923T114324202Z` for 8-bit, 16-bit,
64-bit, and 16-bit float respectively; their signed eboot SHA-256 values are
recorded with the private strict receipts. The pinned original dynamic
broadcast CTS factory requires Vulkan 1.2, so no original subgroup leaf is
eligible on this Vulkan 1.0 profile. Wider vector GPU behavior, other
operations, and graphics stages remain unproven.

On firmware 12.02, the public SDK shipping witness compiled a Vulkan 1.0 SPIR-V
compute shader that reads compact scalar arrays, a row-major matrix and a
nested struct. Two workgroups produced 64 exact values with zero mismatches and
zero guard mismatches. Its bounded fence completed, the TCP receipt verified,
and the title closed. Signed eboot SHA-256:
`44eb76ca162e3c15ab35abc4bc48b0466a9144a0f28d53a5f4255a5fa337a408`;
run `20260923T094543254Z_PPSA99994_ps5vk-ubo_0x1e45c7b386581`;
log SHA-256 `db0771d2ec2b4aba7b56772f475bd698c8d3d095485814d43b1de7905f78c357`.

The public ABI capability probe independently observed the extension and a true
KHR feature field. It verified all 62 requirement records, with 20 reported
satisfied and 42 reported blockers at the query layer. Signed eboot SHA-256:
`9075c100fab8326c73b7bb458be1b560a153ebfb2eb2f8e85389303d5c431a62`;
run `20260923T094657975Z_PPSA99994_ps5vk_0x1e46de0b656f8`;
log SHA-256 `733e047d9014d7b5cd8481520e4a422df5bd6b6081f8182b85b156e7e9fbe4b6`.
The four-axis DXVK matrix is 18/62 ready with 44 blockers.

The unchanged original
`dEQP-VK.ubo.single_basic_array.std430.uint.vertex` oracle passed twice in a
495-case measurement selection and once in the promoted canonical selection.
The canonical shipping run reported 495/495 Pass, zero missing, foreign or
duplicate cases, zero Fail or NotSupported, verified payload identity and clean
title closure. Signed eboot SHA-256:
`b2dbc3f47eaabe6890e1e6a9d603c38344ad18dc011d2f5404913bbc3e9ed6f3`;
selection SHA-256:
`1a8f7ea9c33873abf40b7352c8ddbf3e533fd0048ebb3edc8c0bdbbc9ae695b3`;
run `20260923T095014926Z_PPSA99994_upstream-cts_0x1e49bbbc79bac`;
log SHA-256 `7845a522bf5040707b550a30c77d10b5e14a8868d0d888c8c83eb45613176ed1`.

Three additional unchanged original `std430` UBO oracles cover a matrix array,
a nested struct, and a two-level struct array in the vertex stage. They passed
twice in the 498-case measurement selection (runs `20260923T115937942Z` and
`20260923T120049216Z`) and then passed in the promoted frozen selection. The
strict canonical receipt reports 498/498 Pass, zero missing, foreign or
duplicate cases, and clean title closure. Signed eboot SHA-256:
`38a8b6e2367a45f6eeebd3043637d6d01624e71bd46f5f95a53da898058a1ec1`;
selection SHA-256:
`c5b81c9814f1a993f74ebd2a19ce580d3022f26853a4f3e35216afad07dfe1ab`;
run `20260923T120459357Z_PPSA99994_upstream-cts_0x1ebf603188bae`;
log SHA-256 `94669451880f9a692ca7c6b07a4e93066f100c209d8cc6ca1cd7191f095a867b`.
