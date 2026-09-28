# DXVK v2.6.2 runtime backlog

The end goal is to run D3D8, D3D9, D3D10 and D3D11 applications in Prospero Win
through pinned DXVK v2.6.2 and ps5vk. The current native D3D11/DXGI executable
is a checkpoint toward that goal: it creates a feature-level 11_0 device,
renders and reads back a representative offscreen workload, and closes cleanly.
Work follows the next observed refusal in each real application path. A tranche
number, profile score, Vulkan version
label or missing CTS leaf is **not** permission to stop implementing a needed
dependency. The 15-tranche assignment in
`conformance_inventory/dxvk_v262_backlog.json` remains an inventory of the
original 61 blockers, not a serial work schedule or the acceptance test for
DXVK execution.

## End-to-end frontend acceptance

The frontend milestone requires the matching DXVK 2.6.2 PE modules to load in
Prospero Win, reach ps5vk through its Vulkan bridge, render a deterministic
frame, present it, and close and relaunch without leaked ownership. Use an
application or focused consumer for each API, and retain the exact DLL,
Prospero Win, ps5vk and native payload identities with the result.

| Application API | DXVK modules in its path | Current evidence | Next executable proof |
| --- | --- | --- | --- |
| D3D8 | `d3d8.dll`, `d3d9.dll` | No Prospero Win DXVK run | Load both modules and render/present a D3D8 frame |
| D3D9 | `d3d9.dll` | No Prospero Win DXVK run | Load the module and render/present a D3D9 frame |
| D3D10 | `d3d10core.dll`, `d3d11.dll`, `dxgi.dll` | No Prospero Win DXVK run | Resolve the D3D10 API wrapper and render/present a D3D10 frame |
| D3D11 | `d3d11.dll`, `dxgi.dll` | Static native offscreen render only | Load PE modules in Prospero Win and render/present a D3D11 frame |

The module lists identify DXVK's side of each path; Prospero Win also needs a
working PE loader/import path and Vulkan bridge for the selected architecture.
The pinned `d3d10core.dll` exports `D3D10CoreCreateDevice`, which takes a DXGI
factory and adapter; it does not export the application-level
`D3D10CreateDevice`. D3D10 applications therefore also need a working
`d3d10.dll` API wrapper before that DXVK module can be exercised.
Neither a static eboot link nor a host presentation run proves that contract.
The matrix below tracks Vulkan feature evidence, not PE loading or per-API game
compatibility. Focused CTS is useful when it diagnoses a real dependency; full
CTS acceptance and 62/62 are not frontend release gates.

### Offline PE dependency checkpoint (2026-09-28)

All five x64 PE modules built from the pinned, clean DXVK 2.6.2 commit with its
unmodified Windows Meson targets and the local MinGW cross-compiler. The
ignored `build/dxvk-pe-x64/inventory.json` records their full SHA-256 hashes,
sizes, architecture and static import modules. This is a build/import inventory,
not PE execution or proof that Prospero Win can load them. Reproduce the build
with the pinned source checkout (`DXVK_DIR`) and its `build-win64.txt` cross
file:

```sh
meson setup build/dxvk-pe-x64 "$DXVK_DIR" --cross-file "$DXVK_DIR/build-win64.txt" --wrap-mode=nodownload -Dbuildtype=release
ninja -C build/dxvk-pe-x64 -j 8 src/dxgi/dxgi.dll src/d3d11/d3d11.dll src/d3d10/d3d10core.dll src/d3d9/d3d9.dll src/d3d8/d3d8.dll
```

| PE module | SHA-256 of this build |
| --- | --- |
| `dxgi.dll` | `1925aa0196ee108b2bcc3ad07646ef81d178ace60688abb200f6bcf8ec98ccc6` |
| `d3d11.dll` | `ac4e32181df36444ed2fa55c1694876c83adaed620820b7a292c97082abb91fd` |
| `d3d10core.dll` | `57c8557522babc750a97ed5b3ec851e85fc2c207b67f3e8a4f1d46506be69903` |
| `d3d9.dll` | `1e5cee2ff035139dc42a92c161394295f02d13bd0323dba9c90ec201f4be441e` |
| `d3d8.dll` | `3fdf76908e3fd784a84152ca0d4e77a2a7544a72e9df70fe976de23d3634eab6` |

The actual static import chain is `d3d8.dll` → `d3d9.dll`, and
`d3d10core.dll` → `d3d11.dll` → `dxgi.dll`. `d3d9.dll` and `dxgi.dll` have no
other DXVK DLL in their static import table. Across the modules, the external
PE imports are from `ADVAPI32.dll`, `GDI32.dll`, `KERNEL32.dll`, `msvcrt.dll`,
`SETUPAPI.dll` and `USER32.dll`. The pinned DXVK source loads
`winevulkan.dll` or `vulkan-1.dll` dynamically and resolves
`vkGetInstanceProcAddr`; a static import listing alone would miss that bridge.
The immediate integration contract is therefore PE module override/search,
these imports and Win32 WSI, followed by a working Vulkan entrypoint into
ps5vk. The x64 build does not cover 32-bit applications.

The same clean pinned source also built all five **x86 PE32** modules with
`build-win32.txt`. The ignored `build/dxvk-pe-x86/inventory.json` records their
architecture, import tables, sizes and hashes. The DXVK-to-DXVK dependency
chains and the set of external import modules match x64. Both builds export
the expected `Direct3DCreate8`, `Direct3DCreate9`, `D3D10CoreCreateDevice`,
`D3D11CreateDevice` and `CreateDXGIFactory` entrypoints. Build them separately:

```sh
meson setup build/dxvk-pe-x86 "$DXVK_DIR" --cross-file "$DXVK_DIR/build-win32.txt" --wrap-mode=nodownload -Dbuildtype=release
ninja -C build/dxvk-pe-x86 -j 8 src/dxgi/dxgi.dll src/d3d11/d3d11.dll src/d3d10/d3d10core.dll src/d3d9/d3d9.dll src/d3d8/d3d8.dll
```

| x86 PE module | SHA-256 of this build |
| --- | --- |
| `dxgi.dll` | `1d1a7ff491489e70899a8051d1f46b1f8959e47f40227f59f381a674cd2131be` |
| `d3d11.dll` | `98fe2ceeabcdd7f8ea278d4422e5894605b8ad66a4040fe18f6f103c719e0edc` |
| `d3d10core.dll` | `cd08d010eb3037709c1e1ce68c660387a49c388476870073522010c73cafcb47` |
| `d3d9.dll` | `71e5313ada3e8fb69bb5de44e10935def2d6281cd9e6a1909eb77064fe6e72d1` |
| `d3d8.dll` | `00432312177f0a8871be9fbf5c5b11df63b78dfb98281cd3652bbd0a8f56d434` |

PE32 execution still requires the matching x86 calling conventions, loader,
Win32 services and Vulkan bridge inside Prospero Win. These binaries have not
been loaded or executed there; compiling both architectures is only dependency
preparation.

### Four-frontend native host control (2026-09-28)

The same pinned DXVK 2.6.2 source also built its five native Linux libraries
with SDL2 WSI enabled. The four controls in
`examples/dxvk_host_frontends/` each created a 64×64 device/swapchain, cleared
its backbuffer and returned success from `Present` on the **host Vulkan
driver**. D3D11 selected FL 11_0. D3D10 used `D3D10CoreCreateDevice` with a
DXGI factory/adapter, so this control does not test the application-level
`d3d10.dll` wrapper. The ignored
`build/dxvk-host-frontends/receipt.json` binds the successful markers to the
five library hashes and four executable/source hashes; logs remain beside it.
The controls do not compare presented pixels and do not exercise PE loading,
Prospero Win or ps5vk on PS5.

With `DXVK_DIR` pointing at the clean pinned source, reproduce the native
build and controls:

```sh
meson setup build/dxvk-native-all "$DXVK_DIR" --wrap-mode=nodownload -Dbuildtype=release -Dnative_sdl2=enabled -Dnative_sdl3=disabled -Dnative_glfw=disabled -Denable_d3d8=true -Denable_d3d9=true -Denable_d3d10=true -Denable_d3d11=true -Denable_dxgi=true
ninja -C build/dxvk-native-all -j 8 src/dxgi/libdxvk_dxgi.so.0.20602 src/d3d11/libdxvk_d3d11.so.0.20602 src/d3d10/libdxvk_d3d10core.so.0.20602 src/d3d9/libdxvk_d3d9.so.0.20602 src/d3d8/libdxvk_d3d8.so.0.20602
python3 tools/run_dxvk_host_frontends.py --dxvk-dir "$DXVK_DIR"
```

### PS5 ABI cross-link of all five native modules (2026-09-28)

`tools/build_dxvk_ps5_cross_probe.py` cross-compiled 223 units from that same
pinned source for the PS5 toolchain and linked all five native DXVK libraries
with the PS5 WSI adapter. It checked the D3D8 → D3D9 and D3D10 → D3D11 →
DXGI ELF dependencies as well as complete links. The ignored
`build/dxvk-ps5-cross-probe/receipt.json` records these artifact hashes:

| PS5 cross-linked module | SHA-256 |
| --- | --- |
| DXGI | `58d93013638da3e20e4b4ef2cafe882e4b62232f46f67cf343e128ed050b7b76` |
| D3D11 | `7ba72f19613935a504112c2a8f0f2673daf0a08a1d07c80b7274bb563c5957ad` |
| D3D10 core | `8945dce5422dc19e1c3d183a9e2034618277b48698295da0df7b3e464c5238a6` |
| D3D9 | `06949af32de02dc148bb4146f8b16fb847a72d5605b3551b229f2f2245da6a0a` |
| D3D8 | `1dff82848a49d47a88e9767eda9aeedede9cdf48dfaf7fef5291e73575e29748` |

Reproduce with the configured native Meson build above and `DXVK_DIR` at the
pinned source:

```sh
python3 tools/build_dxvk_ps5_cross_probe.py --dxvk-dir "$DXVK_DIR" --build-dir build/dxvk-native-all --jobs 8
```

These are cross-link witnesses, not PE DLLs, deployable payloads or a console
run. The next runtime proof still needs each frontend through Prospero Win's
PE and Vulkan bridge, followed by a presented frame on PS5.

### PS5 WSI variant of the DXVK PE DLLs (2026-09-28)

The unmodified Windows DXVK build asks for `VK_KHR_win32_surface` and resolves
`vkCreateWin32SurfaceKHR`. ps5vk's measured presentation route instead uses
`VK_KHR_display` and `vkCreateDisplayPlaneSurfaceKHR`. The reproducible
`tools/build_dxvk_ps5_pe.py` build starts from the pinned DXVK commit in an
ignored local clone, adds this repository's PS5 WSI adapter to DXVK's WSI
source list, selects that adapter instead of Win32 WSI, and makes it the
Windows build's default. The original pinned checkout stays unchanged. These
DLLs are therefore **DXVK 2.6.2 with a PS5 WSI overlay**, not unmodified DXVK.

All five DLLs built for both PE32+ x64 and PE32 x86. The builder checks the
binary format and each frontend's exported entrypoint; DXGI and D3D9 contain
the `Ps5WSI` bootstrap and no `Win32WSI` bootstrap symbol. The ignored
`build/dxvk-pe-ps5-wsi/receipt.json` binds the base commit, submodule pins,
overlay hashes and output hashes. Its module SHA-256 values are:

| DLL | x64 | x86 |
| --- | --- | --- |
| `dxgi.dll` | `18c3da23192db68fbdb0fc24bd13444bb1e39430ced1a2bbffb75c988ec01e9e` | `964bf6ed1413dc7756d5aa08606cf0759e85ca216807dee2144f653e7e9a74de` |
| `d3d11.dll` | `5416488d0f4edc4a267747579ba23ca5fcd33aa532ab154f2b6162a17fa6653b` | `2fb66f7f14b5999557207bd127e47c71f82539b73309be01c11057bb46ace0ca` |
| `d3d10core.dll` | `5d62ef4ab23f1b0009a7c22c5fc891d30ccbef146277cc7daaad0ff72d858c79` | `fe78abb6d4b8073544436c3a056fae4bde1051ccdabf329ae1b46cc67fcf5524` |
| `d3d9.dll` | `32851711f54c66aa971179a574b020ffd27cb2f883dd85fd593a8a2a84b89326` | `f042cd22ffac7460f83725c7ce7087d0447c1bda82c57d65b6ff8ad477b33864` |
| `d3d8.dll` | `0f8b59d91878b3e4bcbf30d02f13a00f5ee85b6a0ce85583d8dd4268c7ecc04f` | `61b9d35f7411961269d30fa32386e4660ed197404936e8f4893475cef39851d5` |

With `DXVK_DIR` pointing to the clean pinned source and Meson available on
`PATH`, rebuild both architectures offline:

```sh
python3 tools/build_dxvk_ps5_pe.py --dxvk-dir "$DXVK_DIR" --arch both --jobs 8
```

This removes the Win32 surface request from the selected DXVK WSI path. It
does **not** make the DLLs executable in Prospero Win by itself: DXVK still
loads `winevulkan.dll` or `vulkan-1.dll` and needs a working Vulkan entrypoint
bridge to ps5vk, Wine's PE imports and the D3D10 API wrapper. No console
render or presentation is claimed for these DLLs.

### PE application controls for the four APIs (2026-09-28)

`examples/dxvk_pe_frontends/` contains one small PE consumer per API. Each
opens a fixed 1920×1080 window, creates the D3D device and swapchain, clears
two frames to distinct colours, calls `Present` twice, prints a stage/result
marker, and releases its objects. D3D10 enters through the application-facing
`D3D10CreateDeviceAndSwapChain` wrapper; that wrapper must reach DXVK's
`d3d10core.dll` in Prospero Win. The other controls import their matching
DXVK entry DLLs directly.

For a Wine bridge that exposes Win32 WSI to PE clients and maps it to the PS5
display plane internally, use the unmodified DXVK DLL builds and regenerate
all eight x64/x86 controls with a matching receipt:

```sh
python3 tools/build_dxvk_pe_frontends.py --dll-variant unmodified
```

This writes `build/dxvk-pe-frontends-unmodified/receipt.json`; the existing
PS5-display-overlay route remains available with `--dll-variant ps5-wsi` and
its separate `build/dxvk-pe-frontends/receipt.json`. Each receipt records its
DLL variant, each executable's SHA-256, source hash, PE imports, the full
runtime DXVK DLL chain's hashes, and `executed: false`. The builder verifies
all five DLL hashes against the selected build inventory before linking and
never mixes x64 with x86. The controls are
ready as inputs to a Prospero Win run; they do not establish that Wine loads
the DLL chain, reaches ps5vk, or presents correct pixels on PS5. The first
runtime sequence should be x64 D3D11, D3D9, D3D8 and D3D10, followed by x86
once the 32-bit Vulkan bridge is known to work. Capture the first failed stage
and verify the two displayed colours independently of the `Present` return.
All four controls request the same centre RGB sequence: frame 0
`(28, 76, 132)` / `#1C4C84`, then frame 1 `(132, 76, 28)` / `#844C1C`.
The receipt records this nominal pixel oracle; it is not pixel evidence.
Compare RGB only because the D3D8/9 X8 backbuffer does not define alpha.
If using compressed Remote Play video as the independent observation, allow
for compression error and verify both the colour order and a clean relaunch.

### Native display WSI hardware checkpoint (2026-09-28)

The pinned DXVK display adapter, linked into a public-SDK native witness, created
a display-plane surface and presented three bounded BGRA8 swapchain frames on
PS5. The strict verifier accepted frame/image order 0–1–0, matching native
completion events and clean retirement. Eboot, adapter and log hashes plus the
run identity are in [VALIDATION.md](../VALIDATION.md#native-dxvk-display-adapter-wsi-witness-2026-09-28).
The previous eboot was restored and the console released. This closes the
native WSI uncertainty for that artifact; it is not a PE DXVK or Prospero Win
run and does not verify displayed pixel colours independently.

## Current integration target (2026-09-26)

The ordinary instance and device now report **Vulkan 1.3.0**, as an experimental,
non-conformant implementation. Core aggregate queries, feature opt-in and
promoted command dispatch replace the payload's old core/KHR translation.
Maintenance4 is an ordinary driver route, not an SDK diagnostic override.
The acceptance workload uses pinned DXVK's original version and feature-level
checks with no driver capability override. The definitive clean-build result
belongs to the [artifact-bound native receipt](../VALIDATION.md#experimental-vulkan-13-native-dxvk),
not to an assumption based on the version label. This does not claim universal
game support, a presented DXVK frame, or Vulkan conformance.

## Profile ledger on the Vulkan 1.3 probe

`tools/check_dxvk_profile.py --check` reports **45/62 ready, 17 blockers**.
This is an implementation-evidence score, not a DXVK runtime result. Its API
axis is the current native probe (device API 1.3.0, 33 device extensions,
46/62 values met), cross-checked row by row against the public host query.
Before the Vulkan 1.3 integration the ledger read 41/62 on a Vulkan 1.0
probe; that probe is archived, not relabelled. Geometry, tessellation,
draw parameters and `maxBufferSize` now carry admitted native receipts.
`maintenance4` is queried true but stays blocked on native evidence for its
memory-query, specialization and
relaxed graphics-interface routes. Host tests cover scalar/vector expressions,
nested aggregate extraction and insertion, 8/16/32-bit integer-width conversions,
and wider producer vectors; the 33-case original CTS measurement selection is prepared for native execution.
`apiVersion` stays blocked: the pinned profile requires 1.3.204 including the
patch level; the device reports 1.3.0, which DXVK's own device filter accepts.

The integrated inline-uniform diagnostic candidate passed its six SDK stage
witnesses twice, but both complete 881-case CTS runs passed 880 cases and failed
`dEQP-VK.info.device_mandatory_features`. The repeated CTS report names nine
Vulkan 1.3 requirements still reported false: `computeFullSubgroups`,
`pipelineCreationCacheControl`, `privateData`, `robustImageAccess`,
`shaderIntegerDotProduct`, `shaderSubgroupExtendedTypes`,
`shaderZeroInitializeWorkgroupMemory`, `subgroupBroadcastDynamicId`, and
`subgroupSizeControl`. `privateData` is outside the 62-row DXVK profile, so even
a future 62/62 ledger would not by itself clear this CTS failure. The inline
rows remain blocked pending a passing combined acceptance run; the diagnostic
witness results do not change the official 45/62 score.

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

The offline `run_dxvk_ps5vk_host_bootstrap.py` control now reaches ps5vk's
host `vkGetInstanceProcAddr`, then SDL2 WSI cannot obtain the Linux window
surface extensions it expects. That host-only refusal is not a PS5 WSI or
DXVK device-capability failure. The PS5 native acceptance payload below uses
the PS5 WSI adapter instead; its present path still needs validation together
with each frontend in Prospero Win.

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
   witness. A known outstanding shape is stream output without a fragment
   stage. Maintenance4's scalar/vector and aggregate expressions, integer-width
   conversions and relaxed producer/consumer
   vector matching have host coverage; native execution remains pending.
   These are explicit profile limits, not reasons to restore the old 1.0 gate.
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

## Offline presentation candidate (2026-09-27)

The native builder accepts `--present` for a separate `unmodified-present`
artifact. It uses the original pinned DXVK libraries and the ordinary SDK,
creates a two-buffer 1920x1080 DXGI flip-discard swapchain, renders three
frames and calls `Present(1, 0)` for each. The top-left 64x64 region is copied
to staging **before** each Present and checked against the shader oracle.
The clear marker changes on every frame; stale readback, missing or duplicate
frames, a non-S_OK Present (including occlusion), and surviving swapchain,
device or context references fail the presentation verifier.

The host DXVK run on 2026-09-27 returned S_OK for all three frames, with zero
mismatches over 4096 pixels per frame and checksums `6e17a4c5`, `8052d0c5`
and `c0bc44c5`. Swapchain, device and context final reference counts were zero.
The host executable SHA-256 was
`a36abd1891d95a1d025fb6d2c0e68873e30669c7367a841ce00e35285b1f1048`;
the generated host receipt also identifies the linked DXVK libraries.

This is preparation for a native test, not evidence of PS5 presentation or
external scanout. The 64x64 readback does not validate every display pixel or
DXVK's final presentation blit. The historical offscreen artifact and its
checksum remain a separate workload. Missing EDID retains its original DXVK
error log, but the exact documented SDR-default fallback is not classified as
a rendering refusal; other errors remain refusal candidates.

The first native presentation candidate reached D3D11 FL 11_0 device creation
but failed during DXGI swapchain creation at a valid pixel-coordinate sampler
request (`vkCreateSampler`, `unnormalizedCoordinates=1`). The exact artifact,
refusal and run are in [VALIDATION.md](../VALIDATION.md#native-dxvk-presentation-candidate-sampler-refusal-2026-09-28).
The driver now encodes the GFX10 S# unnormalized-coordinate bit for that
bounded sampler form, with a host contract. A second hardware artifact passed
swapchain creation, draw and copy, then refused DXVK's colour-attachment to
shader-read barrier while mapping the readback. The source now records the
measured barrier and provides a tiled 2D sampled-colour descriptor with host
tests. The third native artifact passed the render-to-sample handover and
refused the following shader-read to transfer-source transition. That exact
barrier has a host recorder test and is accepted by the next source slice;
hardware presentation and the pixel oracle remain to be retested.

With the pinned DXVK checkout and its SDL2 native Meson build available:

```sh
python3 tools/build_dxvk_ps5_native.py --host-only --present
python3 tools/build_dxvk_ps5_native.py --variant unmodified --present
```

`--host-only` does not build or run a PS5 payload. It writes the host executable,
stdout/stderr and `receipt.json` under `build/dxvk-ps5-native/host-present`.
The native command only builds; its artifact and package are under
`build/dxvk-ps5-native/unmodified-present`. Neither command deploys anything.
Use `--dxvk-dir` and `--build-dir` to select an existing local pinned build.
The untracked dependency checkout and compiler archives must match their pins.

The native run tool has `--require-presentation`, which requires the matching
presentation artifact, strict Vulkan 1.3 acceptance, all three frame oracles,
successful Present calls and clean title closure. Native acceptance still needs
a clean source build, artifact identity, firmware record and bounded relaunch
on hardware. No native acceptance or profile-matrix promotion is claimed by
this candidate.

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

For `robustImageAccess`, the pinned original image-robustness factory is not
registered in the PS5 CTS package. Registering it alone would not make a
measurement executable: `vktRobustnessExtsTests.cpp` creates its output image
with `STORAGE|TRANSFER_SRC|TRANSFER_DST`, adding `SAMPLED` when the format
reports sampling; it gives the tested image both transfer roles too. The
shipping image-usage gate in `src/texture_format.c` admits the R32_UINT storage
image only as `STORAGE|TRANSFER_SRC|TRANSFER_DST`, while R32_UINT reports a
sampled role, so the original output image asks for an unsupported four-role
combination. Other currently reported storage formats do not provide an
alternative admitted output-image path. Thus the first obstacle is image
creation, before any out-of-bounds shader read can be measured. The local
integer-coordinate witnesses are narrower diagnostics and cannot substitute
for an accepted original CTS leaf. Next: implement and host-test the exact
combined usage and transfer path, register a bounded original selection,
then measure the API query, shader result and repeat acceptance on hardware
before changing this row's verdict.

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
