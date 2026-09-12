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
replace test bodies or oracles: the selected cases run their upstream
implementations, including the reference rasterizer comparison used by the
graphics smoke test.

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

* This is a focused selection, not the complete CTS and not conformance.
* The pinned revision is a 1.3-era CTS; it does not establish Vulkan 1.4
  coverage.
* Cases that require API the driver does not implement are reported as failures
  or unsupported results, not silently converted into passes.
* `tcuImageIO` (libpng) and the generated EGL wrapper (`gluRenderConfig`) are
  not part of this focused build; no selected case uses them.
