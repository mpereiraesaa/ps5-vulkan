# Focused upstream Vulkan CTS on native PS5

This document covers the integration that cross-compiles a focused selection of
**genuine upstream Khronos VK-GL-CTS** code into a native PlayStation 5 payload
and executes it against ps5vk on the console's `gfx1013` GPU.

It is deliberately separate from the two other suites in this repository:

| Suite | Location | What it is |
| --- | --- | --- |
| Upstream VK-GL-CTS (this document) | `cts/upstream/`, `tools/build_upstream_cts.py`, `cts/upstream_runner.py` | Real upstream test bodies, framework and verification oracles |
| Synthetic Vulkan API contracts | `cts/cts_adapter.c`, `cts/runner.py`, `tools/build_cts_native.py` | Local `contract.*` tests modelled on CTS *mustpass* selection; they are not upstream tests |
| Standalone public SDK consumer | `examples/native_consumer/` | Public-header consumer of the staged SDK, not a CTS test |

Nothing here is a claim of Vulkan conformance or of complete CTS coverage. The
selection is intentionally small and is frozen in a committed manifest.

## Integration layout

| File | Role |
| --- | --- |
| `cts/upstream/main_ps5.cpp` | Entry point: ps5log init, run identity, command line, `tcu::App` iteration loop |
| `cts/upstream/platform_ps5.cpp` | `tcu::Platform` / `vk::Platform` adaptation with static dispatch into the ps5vk driver |
| `cts/upstream/package_ps5.cpp` | Focused `vkt::BaseTestPackage` that registers only the selected upstream groups |
| `cts/upstream/log_sink_ps5.cpp` | `qpTestLog` output captured through a pipe and streamed as base64 QPA chunks |
| `cts/upstream/thread_atexit_ps5.cpp` | POSIX thread-exit destructor support required by libc++abi on this SDK |
| `cts/upstream/dladdr_ps5.cpp` | `__dladdr` back-end stub; the SDK stubs do not export it |
| `cts/upstream/manifest.json` | Frozen acceptance selection with per-case upstream source and rationale |
| `cts/upstream_runner.py` | Strict verifier: reassembles the QPA stream and enforces the acceptance policy |
| `tools/build_upstream_cts.py` | Reproducible cross-build and packaging of the payload |
| `tools/run_upstream_cts.py` | Launch, capture and verify one native acceptance run |
| `tools/check_upstream_selection.py` | Host-only check that every selected path traces back to upstream sources |

The integration only supplies platform adaptation (threading, time, assets,
logging, static Vulkan dispatch) and the payload entry point. It does not
replace test bodies or oracles: every selected case runs its upstream
implementation and its upstream result path.

### Linked versus selected versus executed

These are different claims and the selection below must not be read as if they
were the same thing:

* **Linked**: the upstream framework, modules and oracles are compiled into the
  payload, including the reference rasterizer and image-comparison machinery
  (`rrRenderer`, `tcuImageCompare`, `tcuRasterizationVerifier`, ...). The link
  map proves they are present, not that they run.
* **Selected**: the thirteen cases frozen in `cts/upstream/manifest.json`
  (seven API, synchronization and memory cases plus six compute cases). Only
  these are registered by `cts/upstream/package_ps5.cpp` and shipped in the
  packaged case list.
* **Executed**: what a given report actually contains, which the strict verifier
  checks case by case.

The current selection contains no draw or pixel-comparison case. The graphics
entry is `dEQP-VK.api.smoke.create_shader`, which compiles a vertex shader at
runtime and validates the `vkCreateShaderModule` / `vkDestroyShaderModule`
lifecycle; it does not rasterise, so **no image oracle is executed**. Claiming a
reference-renderer comparison would require adding such a case to the manifest
and executing it, and neither has been done.

## Pinned inputs

| Input | Pin |
| --- | --- |
| VK-GL-CTS | tag `vulkan-cts-1.3.8.4`, commit `a0270c1897597e6c77679870e10415398a13001c` |
| glslang, SPIRV-Tools, SPIRV-Headers, Amber | The commits actually compiled are recorded in the generated build manifest under `dependency_commits` |

The CTS expects its external dependencies to be fetched with
`external/fetch_sources.py`, which declares its own pins. Those declared pins
and the checkouts present in the working tree can differ; the build manifest
therefore records the commits that were really compiled rather than assuming
the declared ones. `make check-upstream-cts` does not depend on any of these
checkouts being present.

## Build

Cross-building the payload needs the PS5 payload SDK and the lab tooling layout
described in [BUILDING.md](BUILDING.md):

```sh
export PS5VK_LAB_ROOT=/path/to/homebrew_ps5
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
python3 tools/build_upstream_cts.py   # or: make upstream-cts
```

The build compiles the selected upstream framework, module, shader-toolchain
(glslang), SPIRV-Tools and Amber objects for `x86_64-sie-ps5`, links them
against the staged `libps5vk.a` / `libpsbc.a`, and writes:

```text
build/upstream-cts/upstream_cts.map       # link map: provenance of every object
build/upstream-cts/build_manifest.json    # pins, toolchain, selection hash, hashes
dist-upstream-cts/PPSA99994/              # signed payload and package metadata
```

`upstream_cts.map` is the machine-readable provenance: it lists the upstream
framework and test objects that are actually linked into the executable.

## Selection and acceptance policy

`cts/upstream/manifest.json` is the frozen selection. The packaged case list is
derived from it, so the manifest, the on-device case list and the verifier
cannot drift apart. Each entry records the upstream source file that
implements the case, the required features and the rationale.

The acceptance policy is strict by construction:

* the process must report a completed session (start marker, end marker and an
  explicit completion record), and the exit status must be zero;
* the reassembled QPA stream must match its declared byte count and SHA-256, and
  the chunk sequence must be complete with no duplicates or gaps;
* every declared case must be reported exactly once, with no unexpected cases;
* every declared case must carry a `Pass` status from the upstream oracle.

`NotSupported`, `Fail`, `Skip`, unknown statuses, missing or extra cases,
truncated reports and a stopped/signalled process are all non-accepting.
`tools/check_upstream_selection.py` additionally fails if a selected path cannot
be traced back to the upstream sources it cites.

## Native execution and evidence

Runtime evidence uses the lab's TCP `ps5log/1` transport exclusively. The
payload streams the upstream report as ordered base64 chunks rather than
rewriting verdicts into a private format, so the host verifier reconstructs the
verbatim QPA document and applies the upstream status codes.

```sh
python3 tools/run_upstream_cts.py \
  --host <console> --runs-dir <ps5logd runs directory>
```

The run is tied to a build identity: the payload reads the selection hash and
the deployed executable hash from its own package and reports them, and the
verifier requires them to match the locally built package. A stale deployment
therefore fails verification instead of being attributed to the current build.

## Host checks versus hardware evidence

`make check-upstream-cts` runs entirely on the host and needs no console. It
checks the selection manifest against upstream sources and exercises the
verifier and run orchestrator against synthetic `ps5log/1` captures, including
negative cases (unsupported status, missing case, truncated report, stale
binary identity). It is a contract test, not hardware evidence.

Hardware evidence is produced only by a native launch on the console and is
identified as such wherever it is reported. Host tests do not establish GPU
correctness.

## Limitations

### Native acceptance (2026-09-12)

Two independent launches on PS5 FW 12.02 / GFX1013 completed the seven
API/synchronization/memory cases frozen at that time with **7 Pass, 0 Fail,
0 NotSupported**, exit code zero. Both strict verifications matched the
deployed executable and selection, reconstructed the complete QPA, observed
GPU completion and `allocations_bytes=0`, and confirmed the title stopped
after Close Game. This is focused upstream execution, not Vulkan conformance
or broad shared-memory coverage.

- Executable SHA-256: `992e607b6e1ec4d9fb54821796335380a7922b78dcd2c2cc08b7a911db3b9b64`
- Selection SHA-256: `7dc544e694fa40439f49a15417bd56297434bc70381290acb921c371b8ffbfea`
- QPA 1 SHA-256: `601c90bc47e4371e158f1476d6514c159dba688242edda62268469aa5ece2cf8`
- QPA 2 SHA-256: `429836ef94c67a92fca1827600852fc220f5260f26ccf9a4561ce15c45bc5fda`

Raw logs and reports remain private; these are audited result summaries.

### Compute expansion (2026-09-12)

The selection now also contains six `dEQP-VK.compute.basic` cases implemented by
the pinned upstream `vktComputeBasicComputeShaderTests.cpp`. All six execute
their upstream shader generation, runtime GLSL compilation and upstream SSBO
comparison oracles unchanged:

| Case | Upstream shape | What the oracle establishes |
| --- | --- | --- |
| `shared_var_single_invocation` | 1 invocation, 1 workgroup | LDS write, `memoryBarrierShared`/`barrier`, read-back |
| `shared_var_single_group` | 30 invocations, 1 workgroup | the same oracle with more than one invocation per group |
| `shared_var_multiple_invocations` | 1 invocation, 40 workgroups | per-workgroup shared state and workgroup addressing |
| `shared_var_multiple_groups` | 12 invocations, 42 workgroups | both dimensions at once, 504 checked values |
| `shared_atomic_op_single_group` | 30 invocations, 1 workgroup | `atomicAdd` on shared memory must return 30 distinct values |
| `shared_atomic_op_multiple_groups` | 12 invocations, 42 workgroups | the shared counter restarts per workgroup |

Two further independent launches (run 3 and run 4) of the identical final
payload completed the full thirteen-case selection with **13 Pass, 0 Fail,
0 NotSupported**, exit code zero. Both strict verifications matched the
deployed executable and selection, reconstructed a complete QPA whose declared
chunk count and SHA-256 match, observed GPU completion and
`allocations_bytes=0`, and confirmed the title stopped after Close Game. The
reassembled reports contain one `OpControlBarrier`, one `OpMemoryBarrier` and
(for the atomic cases) one `OpAtomicIAdd` per compute shader, so the predicates
above are the ones the hardware actually exercised.

- Executable SHA-256: `ee25e08f8f2aa35a2073b52a934a4197ba19095c6a1dd74b19eb4dfa9fe5380d`
- Selection SHA-256: `4608cfa0d78d37c71d8944bbaced02b108ba1470b294f943a266f64921bdbf4b`
- QPA 3 SHA-256: `782c0a46a35781928390964c10049c746beeeabe40b2d27abdeb80257e9ad0ff`
- QPA 4 SHA-256: `ad98eeb88c3348eadcfddab473d60a0549b7a584f4a202fd57e4fc94c612d62a`

The compute expansion needed no further driver change: the runtime compute
profile established by the earlier fix (compiler LDS sizing, inline dispatch
dimensions, per-dispatch retirement) already covers these shapes. The new
coverage is a statement about which upstream code has now been executed, not a
claim that the driver implements all of Vulkan compute.

The expansion also surfaced two host-side build problems that were fixed in
this change, because the work was done in a separate Git worktree rather than
in the canonical checkout:

- `tools/build_sdk.py` resolved the PS5 toolchain and the sibling lab projects
  through `ROOT.parents[1]`, so an out-of-tree worktree silently staged a
  host-only SDK and then failed to link the payload. It now uses the same
  `tools/lab.py` `lab_root()` resolution as the other native tools, which is
  unchanged in the canonical layout.
- `Makefile` hard-coded `../ps5-agc-gears` and `../logging_server`. They now go
  through `LAB_SIBLINGS`, whose default (`..`) is exactly the previous
  behaviour for in-tree checkouts.

### Heap and driver fixes

The original roughly 13 MiB ceiling belonged to the foundation's **internal
libc heap mode**, not the console's available RAM. That mode ignored the
application's existing expandable-heap parameters. `tools/cts_heap_parameters.py`
selects application mode in this project's freshly linked ELF before signing,
validating the parameter layout and refusing ambiguity or drift. It does not
patch system code or change the shared foundation. Removing that limit allowed
all seven cases to execute and exposed two genuine driver gaps:

- Simultaneous-use command buffers now retain ownership until retirement;
  the conservative backend serializes reuse rather than pretending it is idle.
- Runtime compute preserves compiler LDS sizing and supplies inline dispatch
  dimensions for the pinned PSBC six-user-SGPR ABI. Validated buffer barriers
  use the backend's stronger global completion/cache dependency, retaining
  buffer references and rejecting ownership transfers and invalid ranges.

The optional upstream shader cache remains disabled to avoid its fixed 16 MiB
pool; GLSL compilation still happens natively. No selected test body, shader,
oracle, or selection was replaced to obtain these results.

### Remaining scope limits

* This is a focused selection, not the complete CTS and not conformance.
* The pinned revision is a 1.3-era CTS; it does not establish Vulkan 1.4
  coverage.
* Every selected shared-memory and atomic case uses at most 30 invocations per
  workgroup, i.e. a single wave32. The selection therefore does not establish
  multi-wave LDS barrier semantics. The upstream cases that would
  (`dEQP-VK.compute.basic.max_local_size_*`) size their workgroups from
  specialization constants through `local_size_x_id`, which this runtime
  compute profile does not implement, so they are outside this change.
* No selected case exercises a rendering or pixel-comparison oracle. The
  reference rasterizer is linked but unexecuted, so this integration does not
  demonstrate rasterisation correctness.
* Application heap mode removes the measured internal-mode ceiling; this run
  does not establish the maximum safe heap size for arbitrary applications.
* Cases that require API the driver does not implement are reported as failures
  or unsupported results, not silently converted into passes.
* `tcuImageIO` (libpng) and the generated EGL wrapper (`gluRenderConfig`) are
  not part of this focused build; no selected case uses them.
