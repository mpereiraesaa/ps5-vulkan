# Vulkan 1.4 conformance requirements inventory

A deterministic, offline, machine-readable inventory of what Vulkan 1.4
conformance requires, plus provenance for every source pin, a static join to a
pinned Vulkan CTS listing, and two consumer requirement overlays.

This directory is deliberately self-contained: it adds no build targets and
touches no existing source, headers or CI. Everything here runs offline with the
Python standard library once the optional caches have been generated.

## Generated figures

Every number quoted below is generated from the data, not typed by hand:

<!-- stats:begin -->
```json
{
  "baseline_dispatched": 144,
  "baseline_entry_points": 144,
  "baseline_implementation_only": 0,
  "baseline_public_header": 144,
  "command_contracts": 12,
  "core_commands": 234,
  "core_conditional_feature_bits": 27,
  "core_mandatory_feature_bits": 58,
  "core_types": 606,
  "cts_cases": 3247552,
  "cts_direct_rows": 6,
  "cts_group_files": 98,
  "cts_mapped_rows": 127,
  "cts_representative_or_family_rows": 121,
  "extension_rules": 21,
  "format_rows": 207,
  "format_tables": 11,
  "limits_raised_in_1_4": 32,
  "limits_rows": 427,
  "requirements": 137,
  "requirements_conditional": 13,
  "requirements_mandatory": 121,
  "requirements_optional": 3
}
```
<!-- stats:end -->

Regenerate with `tools/update_readme_stats.py`; `validate.py` fails (`T018`) when
the block disagrees with the inventory.

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
| `requirements.json` | Requirement rows with stable project IDs, classification, applicability, anchors, feature/limit/format/command/extension references, CTS join and evidence fields. Counts are in the generated block above. |
| `spec_coverage.json` | Completeness ledger: every core chapter and appendix of the pinned specification with a review state and the requirement rows derived from it. |
| `cts_manifest.json` | Identity of the pinned CTS `vk-default` mustpass listing: group files, sizes, SHA-256, Git blob ids and case counts. |
| `core_target.json` | The declared target: cumulative core feature requirements, dependency-resolved API surface, required limits, mandatory format tables and command contracts, all derived from pinned sources. |
| `roadmap_comparison.json` | Khronos roadmap profile sets, recorded as a comparison only. |
| `baseline_surface.json` | Entry points actually present in the baseline implementation, with dispatch scope and whether the public header declares them. |
| `consumers.json` | Independent requirement overlays for ParaLLEl-RDP and DXVK, joined to core rows by id. |
| `dxvk_v262_profile.json` | Exact derivative of DXVK v2.6.2's D3D11 FL11_0 baseline profile. |
| `dxvk_v262_evidence.json` | Reviewed CTS/native evidence overrides; absence is deliberately a blocker. |
| `dxvk_v262_matrix.json` | Fail-closed four-axis join of every profile leaf to API, implementation, CTS and native evidence. |
| `dxvk_v262_backlog.json` | Stable partition of the 61 blockers observed on 2026-09-15 into ordered, dependency-aware implementation tranches. |
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
| DXVK | tag `v2.6.2` | `9d6f54a1ade20d1d27dd421024717a636f3d8c68` |
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
* the conditional core feature bits with their exact trigger text, for
  example `descriptorIndexing` and its update-after-bind family (conditioned on
  descriptor indexing being supported) and the 8/16-bit storage features;
* **301 further feature bits** that the specification conditions on an optional
  extension being advertised;
* the cumulative core API surface (commands and types resolved through the registry dependency chain);
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
  per limit and limit type. Every value is interpreted - integers, decimals,
  powers (`2^30^`), fractions, tuples, ranges, arithmetic, `max()`/`min()` over
  other limits and enumerant expressions - and an expression that cannot be
  interpreted fails generation instead of being approximated by stripping
  characters. `required_for_core_1_4` uses only the `{core}` and `{limit1_4}`
  values; roadmap-only values (`{limit2022}`, `{limit2024}`, `{limit2026}`) are
  recorded separately in `roadmap_only_values`. Every limit raised in 1.4 is
  referenced by a requirement row.
* **Formats** (`formats.tables`): the mandatory format support tables resolved
  to one obligation per format and feature bit. Every cell carries the symbol the
  specification uses (`{sym1}` unconditional, `{sym2}`/`{sym3}`/`{sym4}`
  conditional), the effective scope (`optimalTilingFeatures` or `bufferFeatures`
  from the specification's column definition tables, `linearTilingFeatures` when
  a rule states it, or an explicit `table-defined` marker when nothing states
  one - including for `{sym1}` obligations), the resolved conditions and any
  guard -
  including rows written across several physical lines and markers emitted inline
  by `ifdef::EXT[{symN}]`, which a core row can carry for an
  extension-conditioned column. Table rules are kept as a list and classified as
  `symbol-rule`, `scope-rule`, `negative-scope-rule`, `column-rule`,
  `any-of-formats-rule` or `table-choice-rule`; a rule that cannot be resolved is
  recorded as such and rejected by the validator rather than dropped. Rules keep
  their logical structure (`all-of` over `any-of` groups - depth/stencil requires
  one format from *each* of two groups - and `any-of` over tables, with "this
  table" resolved to the anchor it appears in so BC/ETC/ASTC carry the complete
  alternative set), and every candidate format or table is validated against the
  pinned registry format list and the parsed tables.
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
from documentation. The generated figures above record the current counts;
only `vkResetDescriptorPool` remains implementation-only. Rows partition every
named entry point into present and absent sets and record dispatch/public-header
counts. `tools/update_requirements_baseline.py` regenerates those facts, and the
validator rejects drift.

## CTS mapping

`cts_manifest.json` was produced by reading
`external/vulkancts/mustpass/main/vk-default.txt` and every group file it names,
at the pinned CTS tag; the exact group-file and case-name counts are in the
generated figures above. Each group file is identified by size, SHA-256 and Git
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
`direct` requires at least one named case. The tally per quality label is in the generated figures above. A
representative or family-level mapping is a traceability aid: it is **not** complete coverage of
the requirement, and the coverage notes say so per row.

## Consumer overlays

`consumers.json` describes two consumers with independent requirement sets:

* **ParaLLEl-RDP** (pin above) needs at least three descriptor sets including
  uniform and uniform-texel-buffer bindings, push constants, specialization
  constants, 8/16-bit storage access, and shared-memory/barrier/atomic compute
  kernels. It has a documented fallback when `VK_EXT_external_memory_host` is
  absent, and its timeline-semaphore usage is optional.
* **DXVK 2.6.2** is evaluated against its own machine-readable
  `VP_DXVK_d3d11_level_11_0_baseline` profile. That exact profile declares
  Vulkan 1.3.204 and contains 62 unique leaf requirements: one API-version
  floor, two extensions, 49 features and ten properties. Newer DXVK 3.x and a
  future Vulkan 1.4 target are separate future evaluations; their requirements
  are not mixed into this baseline.

Both overlays are static source reading only. They exist to show why core
conformance alone does not make a consumer work, and they are kept separate so
that a consumer need is never reported as a core obligation.

### DXVK 2.6.2 profile

`tools/derive_dxvk_profile.py` verifies the pinned byte count, SHA-256 and Git
blob identity before deriving the profile. Normal `--check` execution is fully
offline. `tools/check_dxvk_profile.py` then joins every leaf independently to
four evidence axes:

1. the value exposed by the public API;
2. reviewed implementation support;
3. upstream CTS evidence: exact original leaves passing in the frozen
   selection or in a focused run bound to an artifact and case list (every
   named case Pass; NotSupported, Skip and Fail never count); and
4. exact native evidence with run ids and an artifact SHA-256.

A row is satisfied when the API, implementation and native axes are positive
and no applicable CTS leaf was observed failing. CTS is regression evidence: a
missing or unrun leaf does not block, an observed failure does. Unknown or
absent API, implementation or native evidence is a blocker. The checked matrix
records **45/62 satisfied and 17 blockers**. The API axis is the current
native capability probe's observation, which must equal the public host query
of the same row (`core_version_queries` and the route queries in
`reporting_matrix.json`); a probe that disagrees, or one older than an
archived probe, fails the check. The current probe observed device API 1.3.0,
33 device extensions and 46/62 requested values; the Vulkan 1.0 probes are
kept under `historical_capability_probes`.

Of the 46 rows the query meets, 45 are ready. `maintenance4` is not: compound
`LocalSizeId` specialization expressions and wider producer output vectors
are still refused, and no witness executes its creation-description
memory-requirement queries directly. The T04 `geometryShader` and
`tessellationShader` rows now carry their admitted native receipts, and
`maxBufferSize` carries the 1 GiB buffer witness. The 16 query blockers are
`apiVersion` (1.3.0 reported, 1.3.204 required including the patch level;
DXVK's own 1.3.0 filter passes), `shaderSubgroupExtendedTypes`,
`subgroupBroadcastDynamicId`, seven Vulkan 1.3 features and six inline uniform
block limits. The exact per-row evidence is in `dxvk_v262_matrix.json`; its
checked generator, not this prose, is authoritative. This is not a DXVK
compatibility or Vulkan conformance statement.

## Running the tools

Validate the checked-in data and print the coverage report:

```sh
python3 conformance_inventory/validate.py
python3 tools/derive_dxvk_profile.py --check
python3 tools/check_dxvk_profile.py --check
```

Regenerate the derived, checked-in data (all offline except the profile and
listing fetches, which are hash-checked against `sources.json`):

```sh
python3 conformance_inventory/tools/collect_baseline_surface.py
python3 conformance_inventory/tools/update_requirements_baseline.py
python3 conformance_inventory/tools/derive_core_target.py
python3 conformance_inventory/tools/derive_roadmap_comparison.py

# Explicit network refresh; source identity must still match sources.json.
python3 tools/derive_dxvk_profile.py --refresh
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
* Conditional rows carry the exact trigger text per row and per bit in `core_target.json`.
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
6. The format-table parser now resolves multiple annotations per table, cells
   written across several lines, inline `ifdef::EXT[{symN}]` markers and
   annotations that are not symbol-scoped (column, scope, any-of-formats and
   table-choice rules). Each cell obligation carries its symbol, effective scope,
   resolved conditions and guard, with the scope of `{sym1}` cells taken from
   the column definition tables (`optimalTilingFeatures` / `bufferFeatures`) and
   their column guards (`VK_KHR_copy_memory_indirect` for the indirect copy
   destination bit, for example) recorded in the requirement rows. The validator
   rejects an unresolved or generic condition (`T021`), a rule, symbol or guard
   that does not reach the requirement rows (`T020`) and a scope stated by a rule
   or a column but missing on the cell (`T022`).
5. Limit values are now interpreted rather than character-stripped (`2^30^` is
   1073741824, `0.5` stays 0.5, tuples and ranges keep their element-wise
   meaning, symbolic references resolve to the referenced limit) and an
   uninterpretable expression fails generation. Format cells keep their symbol
   (`{sym1}` vs `{sym2}`/`{sym3}`/`{sym4}`), the per-table condition, the
   `linearTilingFeatures`/`optimalTilingFeatures`/`bufferFeatures` scope and any
   `ifdef` guard, so conditional storage support such as R8_UNORM is not
   promoted to mandatory. The figures quoted here are generated by
   `tools/update_readme_stats.py` and checked by the validator.
