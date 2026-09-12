# Vulkan 1.4 conformance requirements inventory

A deterministic, offline, machine-readable inventory of what Vulkan 1.4
conformance requires, plus provenance for every source pin, a static join to a
pinned Vulkan CTS listing, and two consumer requirement overlays.

This directory is deliberately self-contained: it adds no build targets and
touches no existing source, headers or CI. Everything here runs offline with the
Python standard library once the optional caches have been generated.

## What this is not

* Not a conformance claim, a conformance submission, or a statement that any
  feature is implemented.
* Not a CTS result set. Nothing here has been executed on any device.
* Not a substitute for the normative specification. Rows contain project-owned
  summaries and anchors only; no specification text is copied.
* Not an exhaustive normative database. `spec_coverage.json` records exactly
  which specification sections were read and which were merely enumerated.

## Contents

| File | Purpose |
| --- | --- |
| `sources.json` | Pinned Khronos and consumer sources: tag, tag object, commit, retrieval date, license, artifact hashes, and cross-source compatibility rules. |
| `schema.json` | JSON Schema 2020-12 contract for the data documents. |
| `requirements.json` | 100 requirement rows with stable project IDs, classification, applicability, anchors, feature/limit/format/command/extension references, CTS join and evidence fields. |
| `spec_coverage.json` | Completeness ledger: every core chapter and appendix of the pinned specification with a review state and the requirement rows derived from it. |
| `cts_manifest.json` | Identity of the pinned CTS `vk-default` mustpass listing: group files, sizes, SHA-256, Git blob ids and case counts. |
| `consumers.json` | Independent requirement overlays for ParaLLEl-RDP and DXVK, joined to core rows by id. |
| `validate.py` | Offline validator and report generator (standard library only). |
| `tools/collect_cts_listing.py` | Regenerates `cts_manifest.json` from the pinned CTS revision. |
| `tools/extract_spec_anchors.py` | Extracts the anchor index used to prove that every row anchor exists in the pinned specification. |
| `tests/` | Unit tests for every validator failure mode, plus integration tests over the checked-in data. |

Generated caches (`.cache/`, hundreds of MB of third-party listing text) are
ignored and are never committed.

## Source pins

All revisions were resolved with `git ls-remote` on 2026-09-12 and file hashes
were computed from the exact bytes fetched at the pinned revision
(`git_blob_sha1` in `sources.json` is re-derivable with `git hash-object`).

| Source | Revision | Commit |
| --- | --- | --- |
| Vulkan specification (Vulkan-Docs) | tag `v1.4.362` | `f84d432d5b8912362f96f581f29bbc4f3c8c7843` |
| Vulkan registry and headers (Vulkan-Headers) | tag `v1.4.362` | `ee2ec5fd83dafce291024683b50dc89219333076` |
| Vulkan CTS (VK-GL-CTS) | tag `vulkan-cts-1.4.6.2` | `f6a29701220f34dd1407513bfe80d74ca7b392ce` |
| ParaLLEl-RDP | commit | `1cecd042b2619bc505c12bfdc713808386f2b54d` |
| Granite (RDP dependency) | commit | `cf71dee71fb00110749a9a3fcbd87b0297e49b6a` |
| DXVK | tag `v3.1` | `70d7508c01201ed3d4bfb33da42ba834eafe3857` |

The specification and registry pins are the same release train, and the
registry declares `VK_HEADER_VERSION 362` with `VK_API_VERSION_1_4`. The README
of the pinned specification appendix is the authority for what 1.4 changed;
the registry alone is metadata, not the normative text.

The registry also serves a moving convenience copy of the specification HTML
(`.../specs/latest/html/vkspec.html`). At retrieval time its title reported
Vulkan 1.4.362, matching the tag, and its hash is recorded in `sources.json`.
Because that URL is not immutable it is treated only as an anchor source, and
`tools/extract_spec_anchors.py` refuses to run if the served version stops
matching the pin.

## Requirement rows

Every row carries: category, a short summary, the core version in which the
obligation entered core, 1.4 applicability, mandatory/conditional/optional
classification with an exact condition where conditional, a normative anchor,
the feature bits, limits, formats, commands and extensions involved, a CTS
mapping, an implementation state, separate source/runtime/conformance evidence
fields, and provenance for inferred mappings.

Categories (in canonical order): `instance_device`, `formats_limits`,
`resources_memory`, `descriptors`, `shaders`, `pipelines`, `renderpass`,
`commands_transfers`, `synchronization`, `queries`, `resource_lifetime`,
`promoted_features`, `extensions`, `platform_wsi`.

Result vocabulary, used across rows:

`not-audited`, `missing`, `implemented-unvalidated`, `host-only-evidence`,
`native-evidence`, `cts-pass`, `cts-fail`, `allowed-not-supported`, `not-run`,
`harness-blocked`.

Two rules are enforced by the validator and by tests:

1. A public symbol existing is recorded only in `baseline.surface_state`. It is
   never treated as evidence of contract compliance, so
   `implementation_state` stays `not-audited` unless a whole-row audit links
   exact evidence.
2. Skipped, unexecuted, unmapped and unimplemented states are never counted as
   passes, and no success percentage is produced.

## CTS mapping

`cts_manifest.json` was produced by reading
`external/vulkancts/mustpass/main/vk-default.txt` and every group file it names,
at the pinned CTS tag. For the pinned revision that is 98 group files and
3,247,552 case names. Each group file is identified by size, SHA-256 and Git
blob id, and the blob id is cross-checked against the GitHub tree API for the
same commit before the manifest is written.

This is a **static source listing**. It is not an executable-generated case
list, and it does not account for platform, capability or extension conditions
that decide which cases are generated or runnable on a given device. A case
name being present says nothing about whether it can run, and nothing about
results. The join is stored per row in `cts.groups` and `cts.cases`; the
validator checks each name against the pinned listing when the cache is
available, so an invented or stale case name is a hard error.

## Consumer overlays

`consumers.json` describes two consumers with independent requirement sets:

* **ParaLLEl-RDP** (pin above) needs at least three descriptor sets including
  uniform and uniform-texel-buffer bindings, push constants, specialization
  constants, 8/16-bit storage access, and shared-memory/barrier/atomic compute
  kernels. It has a documented fallback when `VK_EXT_external_memory_host` is
  absent, and its timeline-semaphore usage is optional.
* **DXVK** (pin above) declares `VK_API_VERSION_1_3`, ships machine-readable
  Vulkan Profiles at api-version 1.3.204 / 1.3.224, and requires a named set of
  extensions and features — including `VK_KHR_maintenance5`,
  `VK_KHR_maintenance6`, `VK_KHR_load_store_op_none`,
  `VK_EXT_depth_clip_enable`, `VK_EXT_robustness2`, descriptor indexing and a
  256-byte push constant limit. The claim that current DXVK 3.x guidance targets
  Vulkan 1.4 was **not** reproducible from the pinned revision or from the
  default branch at retrieval time; what is reproducible is that 1.4 promotes
  several extensions DXVK still requires by name.

Both overlays are static source reading only. They exist to show why core
conformance alone does not make a consumer work, and they are kept separate so
that a consumer need is never reported as a core obligation.

## Running the tools

Validate the checked-in data and print the coverage report:

```sh
python3 conformance_inventory/validate.py
```

Add the optional strict checks (fail on a stale anchor or an unknown case):

```sh
python3 conformance_inventory/tools/extract_spec_anchors.py --strict-hash
python3 conformance_inventory/tools/collect_cts_listing.py    # ~75 s, ~440 MB of ignored cache
python3 conformance_inventory/validate.py \
  --anchor-index conformance_inventory/.cache/spec_anchors.txt \
  --cts-cache conformance_inventory/.cache/cts
```

Run the tests:

```sh
python3 -m unittest discover -s conformance_inventory/tests -t conformance_inventory/tests
```

`validate.py` exits non-zero on any error and prints warnings separately. It
fails clearly when pins become stale or incompatible (specification/registry
release-train mismatch, moving-branch pins, missing artifact hashes, a CTS
manifest pinned to a different commit than `sources.json`, or a served
specification whose version no longer matches the pin).

## Licenses and provenance of referenced material

Only URLs, revisions, anchors, sizes and hashes are reproduced here; no
third-party source text or listing content is redistributed.

| Material | License |
| --- | --- |
| Vulkan specification sources (Vulkan-Docs) | CC-BY-4.0; `vk.xml`/`video.xml` are `Apache-2.0 OR MIT` |
| Vulkan registry and headers (Vulkan-Headers) | Apache-2.0 |
| VK-GL-CTS | Apache-2.0 |
| ParaLLEl-RDP and Granite | MIT |
| DXVK | zlib/libpng |

Formal Vulkan conformance submission requires Khronos Adopter status, an
approved CTS revision and the associated Adopter Fee. This inventory records a
development/test revision only, makes no submission attempt and no conformity
claim, and incurs no fee or legal agreement.

## Known gaps

* 16 core chapters and 483 extension appendices were enumerated but not read;
  requirements that live only there are missing rather than recorded as absent.
* Several rows are explicitly `unmapped` in CTS terms; the report lists them.
* Push descriptors (1.4) and external host memory import have no mustpass case
  identified at this revision, which is itself a finding.
* Implementation states are intentionally conservative: a fresh read of the
  baseline public surface shows which entry points exist, and nothing more.
