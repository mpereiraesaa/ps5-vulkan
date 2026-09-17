# Runtime graphics validation

The experimental procedural graphics profile was tested on an owned PS5 with
firmware 12.02 on 2026-09-12, using the packaged native SDK and PSBC/ACO gfx1013.

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
The fragment read is refused: `native/runtime_shader.c` returns `-2` for any
pixel-stage clip or cull mask, because this profile does not deliver the
distances to the pixel stage.

Because the feature flag is the only gate the upstream oracle applies,
advertising either feature today would assert the whole family, including the
fragment-read variants this profile cannot run, and it would also present the
dynamic-index variants as measured when only their compile-and-package path has
been checked. The honest report is therefore `false` for both features, the
limits stay at the gated-off value, and the reporting matrix cites the
fragment-stage refusal as the effective gate.

The 25 leaves this profile's measured subset would cover are recorded in
`cts/upstream/manifest.json` as diagnostics with `expected_status`
`NotSupported`, next to the reason above, so a later slice that implements the
fragment-stage read - the one genuinely missing mode - can promote them by
changing the acceptance list and the device report together. The canonical
acceptance selection is unchanged at 165 leaves.

### What this does and does not establish

It establishes the vertex-stage clip/cull contract, the packed register state
the pinned compiler emits for it, the per-half-space culling rule and the
hardware clipping result for the measured shapes, plus the compiler and adapter
behaviour of a dynamically indexed declaration (full-width mask, accepted
package). It does not establish fragment-shader reads of the distances, any
hardware result for a dynamically indexed write, any tessellation/geometry
variant of the family, or the two core features themselves, and it is not a
Vulkan conformance claim.

## Optional stage blockers: geometry and tessellation (2026-09-17)

Both remaining T04 stages stay unadvertised, and the reason is now measured
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
exactly the pixels the control covers. The varying does not: every covered pixel
is uniform black, which is what a zero read looks like, and it is identical with
the pinned compiler and with the candidate - the candidate's merged
identification and ES-half sizes are bookkeeping for the driver, not the fix. The
exchange is an LDS ring rather than an SPI parameter-export stream, so the export
configuration is not the path under question. What remains open is the item-index
mapping (the vertex half indexes by its lane id, the geometry half by the
per-vertex offsets the hardware hands it) and whether the ring's LDS region is
allocated and visible between the two phases. The same path also faults: a
geometry program that reads `gl_in[i]` in a loop over the input array loses the
device (the submission never completes) whether it runs before or after the
other modes, so the fault follows that program rather than its surroundings,
while the constant-emission and suppression modes pass. `geometryShader`
therefore stays false.

**Tessellation.** The isolated compiler candidate does produce a two-program
hull buffer for a real vertex+control pair - the control half and the vertex half
in one buffer, with the vertex half's program and resource registers published -
but the same metadata declares the package unresolved because the hull/domain
pipeline state is not part of it. The driver has no tessellation path at all
today: pipeline creation validates the contract and refuses it before any
compile. `tessellationShader` therefore stays false, and the remaining work is
driver-side assembly plus the hull pipeline state - the same class of
vendor-side question as geometry.

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
was **4/62 ready, 58 blockers** at the time (see the indirect and indexed draw
acceptance below for the current 7/62). This does not advertise the Vulkan 1.2 aggregate query
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
the code path that enforces them, 138 mandatory format-feature cells satisfied
with 524 documented per-format blockers, 60 format-query consistency
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

The current checked result is 7/62 satisfied and 55 blockers. Core
`robustBufferAccess`, the three multiview requirements and the three indirect
and indexed draw features (`drawIndirectFirstInstance`, `multiDrawIndirect`,
`fullDrawIndexUint32`, see [their acceptance](#indirect-and-indexed-draw-native-acceptance-2026-09-16))
have all four axes.
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
