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
| `requirements.json` | 136 requirement rows with stable project IDs, classification, applicability, anchors, feature/limit/format/command/extension references, CTS join and evidence fields. |
| `spec_coverage.json` | Completeness ledger: every core chapter and appendix of the pinned specification with a review state and the requirement rows derived from it. |
| `cts_manifest.json` | Identity of the pinned CTS `vk-default` mustpass listing: group files, sizes, SHA-256, Git blob ids and case counts. |
| `core_target.json` | The declared target: cumulative core feature requirements, dependency-resolved API surface, required limits, mandatory format tables and command contracts, all derived from pinned sources. |
| `roadmap_comparison.json` | Khronos roadmap profile sets, recorded as a comparison only. |
| `baseline_surface.json` | Entry points actually present in the baseline implementation, with dispatch scope and whether the public header declares them. |
| `consumers.json` | Independent requirement overlays for ParaLLEl-RDP and DXVK, joined to core rows by id. |
| `validate.py` | Offline validator and report generator (standard library only). |
| `tools/collect_baseline_surface.py` | Regenerates `baseline_surface.json` from the baseline sources. |
| `tools/derive_core_target.py` | Regenerates `core_target.json` from the pinned specification section and `vk.xml`. |
| `tools/derive_roadmap_comparison.py` | Regenerates `roadmap_comparison.json` from the pinned profile file. |
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
| Khronos roadmap profiles, comparison only (`registry/profiles/VP_KHR_roadmap.json`, Apache-2.0) | tag `v1.4.362` | `ee2ec5fd83dafce291024683b50dc89219333076` |

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

## Declared target and how classification is decided

The declared target is **Vulkan 1.4 core for a graphics implementation**, defined
as the cumulative core obligations of Vulkan 1.0 through 1.4. It is not a
roadmap profile and not a consumer requirement set.

The requirement sets come from two pinned primary sources:

* the specification section **Feature Requirements** (anchor
  `features-requirements`), which states that *all Vulkan graphics
  implementations must support* specific features per core version, including
  the conditional ones and the "at least one of" alternatives. This is what the
  CTS `dEQP-VK.info.device_mandatory_features` case is generated to enforce;
* the registry `vk.xml` per-version blocks: `VK_VERSION_1_x` lists the mandatory
  feature bits and `VK_BASE_VERSION_1_x` / `VK_GRAPHICS_VERSION_1_x` list the
  required commands, types and enums.

`core_target.json` is produced by `tools/derive_core_target.py` from both, with
every input hash-checked against `sources.json`. The derivation records:

* **58 unconditional core feature bits** across 1.0-1.4, including
  `timelineSemaphore` (1.2), `synchronization2`, `dynamicRendering` and
  `bufferDeviceAddress` (1.3);
* **27 conditional core feature bits** with their exact trigger text, for
  example `descriptorIndexing` and its update-after-bind family (conditioned on
  descriptor indexing being supported) and the 8/16-bit storage features;
* **301 further feature bits** that the specification conditions on an optional
  extension being advertised;
* the cumulative core API surface: **182 commands and 476 types**;
* the divergences between the specification's conditional phrasing and the
  registry's unconditional list, recorded per version rather than silently
  resolved.

`roadmap_comparison.json` records the Khronos roadmap profiles (`VP_KHR_roadmap_2022`,
`2024` and `2026`, the last declaring api-version 1.4.328) **for comparison
only**. Roadmap 2026 adds requirements that core does not have, such as
cooperative matrices, maintenance7/8/9 and presentation extensions; declaring
api-version 1.4 does not make a profile the definition of core conformance. The
validator rejects any classification derived from it.

Each row then carries:

* `applicability.core` — `core-mandatory`, `core-feature-gated`,
  `extension-optional` or `outside-core`: what the specification requires;
* `applicability.target` — `required`, `conditional` (with the exact trigger) or
  `not-required`: what the declared core target requires;
* `classification` — `mandatory`, `conditional` or `optional`, which must follow
  the derived core sets.

`validate.py` enforces this against `core_target.json`, so the misclassifications
this inventory has been corrected for cannot recur:

* a core-mandatory capability may not be classified optional or conditional
  (`T002`, `T005`);
* a capability whose core obligation is conditional must be classified
  conditional, with the exact trigger (`T003`, `T004`);
* every core-mandatory and core-conditional capability must be referenced by at
  least one requirement row (`T007`, `T008`), so coverage cannot silently shrink;
* no classification may be derived from a roadmap profile (`T009`), and the
  roadmap file must stay marked `comparison-only` (`T010`).

The umbrella index rows have been replaced by decomposed capability rows:
`VK14-CORE-001` to `VK14-CORE-012` carry the per-version mandatory sets within
them, separated wherever the condition, the implementation area or the covering
tests differ (per-version mandatory, per-version conditional, the
optional-extension trigger table, the core API surface, and the 1.4 host-image-copy
either/or rule).

## Core requirement tables

Three core families are carried as machine-readable tables inside
`core_target.json`, each with an explicit resolution rule, and each represented
by requirement rows so that a table cannot exist without a tracked obligation:

* **Limits** (`limits.rows`): the specification's *Required Limits* table resolved
  per limit and limit type, 434 rows. Values keep their version tags, and
  `required_for_core_1_4` only ever uses the `{core}` and `{limit1_4}` values;
  roadmap-only values (`{limit2022}`, `{limit2024}`, `{limit2026}`) are recorded
  separately in `roadmap_only_values`. 24 limits are raised in 1.4 and each is
  referenced by a requirement row.
* **Formats** (`formats.tables`): the mandatory format support tables as
  format x required-feature-bit rows: 10 tables covering 179 formats, each table
  represented by a row.
* **Command contracts** (`command_contracts.contracts`): the resolved core
  command surface grouped into 12 differentiable API areas (instance/device,
  memory, buffers, images, samplers, descriptors, pipelines and shaders, command
  buffers, recording, synchronization, queries, other), with one requirement row
  per area instead of a single "all commands" row.
* **Extension rules** (`mandatory_feature_bits.extension_rules`): the residual
  rules from the Feature Requirements section now carry a taxonomy -
  `extension-required-by-feature` (20) and `either-or` (1, the 1.4
  host-image-copy / transfer-queue rule) - instead of being unclassified prose.

`validate.py` enforces (`T011`-`T016`) that each table declares its resolution
rule, that every 1.4-raised limit and every mandatory format is referenced by a
requirement row, that every contract is covered by a row and contains only
commands from the resolved core surface, and that no extension rule stays
unclassified.

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

A third rule was added after review: baseline claims are checked against
`baseline_surface.json`, which is derived from the implementation rather than
from the public header. At the pinned baseline the implementation defines 81
entry points, all 81 are in the dispatch table, only 54 are declared in
`include/ps5vk/`, and 27 are implementation-only. For example
`vkEnumerateInstanceExtensionProperties`, `vkEnumerateInstanceLayerProperties`
and `vkEnumerateDeviceExtensionProperties` all exist and succeed with empty
lists, and `vkCreateImage`, `vkCreateSampler`, `vkCreateRenderPass`,
`vkCreateGraphicsPipelines` and `vkCmdDraw` are implemented but not declared in
the public header. Rows must list the entry points that exist, the ones that do
not, and say explicitly when an entry point exists only outside the public
header.

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

Every mapped row also states how much of its requirement the mapping covers:

* `direct` — the named case is specifically about this obligation;
* `representative-case` — a real case that exercises part of the requirement;
* `family-level` — the row is joined to a whole group, with no single case
  claimed;
* `not-mapped` — no CTS mapping is claimed.

Anything other than `direct` requires a `coverage_note` describing the gap, and
`direct` requires at least one named case. The current tally is 4 direct, 74
representative-case, 15 family-level and 10 not-mapped rows. A representative or
family-level mapping is a traceability aid: it is **not** complete coverage of
the requirement, and the coverage notes say so per row.

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

Regenerate the derived, checked-in data (all offline except the profile and
listing fetches, which are hash-checked against `sources.json`):

```sh
python3 conformance_inventory/tools/collect_baseline_surface.py
python3 conformance_inventory/tools/derive_core_target.py
python3 conformance_inventory/tools/derive_roadmap_comparison.py
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

* 13 core chapters and 483 extension appendices were enumerated but not read;
  requirements that live only there are missing rather than recorded as absent.
* Several rows are explicitly `unmapped` in CTS terms; the report lists them.
* Push descriptors (1.4) and external host memory import have no mustpass case
  identified at this revision, which is itself a finding.
* Implementation states are intentionally conservative: a fresh read of the
  baseline public surface shows which entry points exist, and nothing more.
* CTS mappings are traceability, not coverage: most mapped rows are
  representative-case or family-level, each with an explicit gap note.
* The core rows are still index rows: they carry complete obligation sets and
  point at `core_target.json` for per-bit triggers, per-limit values and
  per-format feature bits, and they do not by themselves constitute
  per-capability test evidence. Per-command and per-format detail is expected to
  be attached as each contract is audited.
* 12 rows are conditional on a capability or extension being supported; the
  exact trigger text is carried per row and per bit in `core_target.json`.
* Three rows are optional because core does not require them: sparse resources,
  window-system integration, and external host memory import. WSI is required by
  roadmap 2026, which is recorded as a comparison, not as a core obligation.

## Correction history

Changes made to this inventory after external review, recorded so that the
reasoning stays auditable:

1. `timelineSemaphore`, `synchronization2`, `dynamicRendering` and
   `bufferDeviceAddress` were first misclassified as optional, then (wrongly)
   justified with a roadmap profile. The specification's Feature Requirements
   section shows they are **core** obligations: 1.2 for timeline semaphores and
   1.3 for the other three. Classification is now derived from the cumulative
   core sets, and roadmap profiles are demoted to a comparison.
2. Baseline statements were re-derived from the implementation instead of the
   public header, after extension and layer enumeration was wrongly reported as
   absent. Those entry points exist and return empty lists.
3. CTS mappings were given an explicit coverage-quality label and a per-row gap
   note, because several mappings are close references rather than complete
   coverage of the requirement.
4. The core surface was corrected to a transitive walk of the registry
   `depends` chain (the compute surface had been dropped), "at least one of"
   obligations were kept as single disjunctive obligations instead of being
   expanded, and the limits, format and command-contract tables were added with
   rows per differentiable contract.
