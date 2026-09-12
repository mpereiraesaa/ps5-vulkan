#!/usr/bin/env python3
"""Offline validator and report generator for the Vulkan 1.4 conformance inventory.

Standard library only. It validates the four data documents against schema.json's
rules, checks that they agree with each other, and prints a coverage report.

Guarantees that matter for review:

* Never reports a success percentage from incomplete coverage. The report counts
  reviewed, unmapped, unexecuted and unknown rows explicitly.
* Never treats not-audited, not-run, harness-blocked, missing, implemented-
  unvalidated, allowed-not-supported or cts-fail as a pass.
* Fails clearly when source pins are stale or mutually incompatible, including
  when the optional specification anchor index or CTS listing cache does not
  match the pinned revisions.

Usage
-----
    python3 conformance_inventory/validate.py
    python3 conformance_inventory/validate.py --anchor-index .cache/spec_anchors.txt
    python3 conformance_inventory/validate.py --cts-cache .cache/cts --json

Exit status is 1 when any error is found, otherwise 0. Warnings never fail the
run but are always printed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))

ID_RE = re.compile(r"^VK14-[A-Z0-9]+(-[A-Z0-9]+)*-[0-9]{3}$")
CASE_RE = re.compile(r"^dEQP-VK\.[A-Za-z0-9._-]+$")
GROUP_RE = re.compile(r"^vk-default/[A-Za-z0-9._/-]+\.txt$")
ANCHOR_RE = re.compile(r"^[A-Za-z0-9._-]+$")
CHAPTER_RE = re.compile(r"^(chapters|appendices)/[A-Za-z0-9_.-]+\.adoc$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
EXPECTED_CASE_STATE_VOCAB = {
    "not-audited",
    "missing",
    "implemented-unvalidated",
    "host-only-evidence",
    "native-evidence",
    "cts-pass",
    "cts-fail",
    "allowed-not-supported",
    "not-run",
    "harness-blocked",
}
CORE_STATUS_VOCAB = {"core-mandatory", "core-feature-gated", "extension-optional", "outside-core"}
TARGET_STATUS_VOCAB = {"required", "conditional", "not-required"}
TARGET_TO_CLASSIFICATION = {"required": "mandatory", "conditional": "conditional", "not-required": "optional"}
CLASSIFICATION_BASIS_VOCAB = {
    "core-mandatory",
    "core-cumulative-required",
    "core-conditional",
    "project-conditional",
    "not-required-by-core",
    "consumer-required",
}
CTS_COVERAGE_QUALITY_VOCAB = {"direct", "representative-case", "family-level", "not-mapped"}
NEVER_A_PASS = {
    "not-audited",
    "not-run",
    "harness-blocked",
    "missing",
    "implemented-unvalidated",
    "allowed-not-supported",
    "cts-fail",
}


@dataclass
class Problem:
    level: str  # "error" | "warning"
    code: str
    location: str
    message: str

    def render(self) -> str:
        return "%-7s %-6s %-28s %s" % (self.level.upper(), self.code, self.location, self.message)


@dataclass
class Bundle:
    sources: dict | None = None
    requirements: dict | None = None
    coverage: dict | None = None
    consumers: dict | None = None
    manifest: dict | None = None
    target: dict | None = None
    roadmap: dict | None = None
    surface: dict | None = None
    readme_text: str | None = None
    anchor_index: set[str] | None = None
    cts_cache_dir: str | None = None

    @classmethod
    def load(cls, inventory_dir: str = HERE, **overrides) -> "Bundle":
        def read(name: str):
            path = os.path.join(inventory_dir, name)
            if not os.path.exists(path):
                return None
            with open(path, "r", encoding="utf-8") as handle:
                return json.load(handle)

        bundle = cls(
            sources=read("sources.json"),
            requirements=read("requirements.json"),
            coverage=read("spec_coverage.json"),
            consumers=read("consumers.json"),
            manifest=read("cts_manifest.json"),
            target=read("core_target.json"),
            roadmap=read("roadmap_comparison.json"),
            surface=read("baseline_surface.json"),
        )
        readme_path = os.path.join(inventory_dir, "README.md")
        if os.path.exists(readme_path):
            with open(readme_path, "r", encoding="utf-8") as handle:
                bundle.readme_text = handle.read()
        for key, value in overrides.items():
            setattr(bundle, key, value)
        return bundle


def _require(problems: list[Problem], obj: dict, keys, location: str, code: str) -> None:
    for key in keys:
        if key not in obj:
            problems.append(Problem("error", code, location, "missing required field %r" % key))


def _string_list(problems: list[Problem], obj: dict, key: str, location: str) -> None:
    value = obj.get(key)
    if value is None:
        return
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        problems.append(Problem("error", "R019", "%s.%s" % (location, key), "must be a list of strings"))


def _field(source: dict, dotted: str):
    node = source
    for part in dotted.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def validate_sources(bundle: Bundle, problems: list[Problem]) -> dict:
    doc = bundle.sources
    if doc is None:
        problems.append(Problem("error", "S000", "sources.json", "document is missing"))
        return {}
    _require(problems, doc, ["schema_version", "retrieved_date", "compatibility_rules", "sources"], "sources.json", "S002")
    sources = {}
    for index, source in enumerate(doc.get("sources", [])):
        location = "sources[%d]" % index
        sid = source.get("id")
        if not sid:
            problems.append(Problem("error", "S002", location, "source has no id"))
            continue
        if sid in sources:
            problems.append(Problem("error", "S001", location, "duplicate source id %r" % sid))
        sources[sid] = source
        _require(problems, source, ["kind", "role", "title", "publisher", "license", "retrieved_date"], location, "S002")
        if source.get("revision_kind") == "moving-branch":
            problems.append(Problem("error", "S102", location, "source %r is pinned to a moving branch" % sid))
        for kind in ("specification", "registry", "conformance-test-suite", "consumer", "consumer-dependency"):
            if source.get("kind") == kind and not COMMIT_RE.match(str(source.get("commit", ""))):
                problems.append(Problem("error", "S103", location, "source %r must declare a 40-hex commit" % sid))
        for art in source.get("artifacts", []):
            if art.get("kind") == "file":
                if not SHA256_RE.match(str(art.get("sha256", ""))):
                    problems.append(Problem("error", "S003", location, "artifact %r has no sha256" % art.get("path")))
                if not COMMIT_RE.match(str(art.get("git_blob_sha1", ""))):
                    problems.append(Problem("error", "S003", location, "artifact %r has no git blob id" % art.get("path")))
        for web in source.get("documents", []):
            if not SHA256_RE.match(str(web.get("sha256", ""))):
                problems.append(Problem("error", "S003", location, "document %r has no sha256" % web.get("url")))
    return sources


def validate_compatibility_rules(bundle: Bundle, sources: dict, problems: list[Problem]) -> None:
    doc = bundle.sources or {}
    for index, rule in enumerate(doc.get("compatibility_rules", [])):
        location = "compatibility_rules[%d] %s" % (index, rule.get("id"))
        check = rule.get("check") or {}
        op = check.get("op")
        if op == "field_equals_field":
            left = _field(sources, check.get("left", ""))
            right = _field(sources, check.get("right", ""))
            if left is None or right is None:
                problems.append(Problem("error", "S100", location, "unresolved field reference %r / %r" % (check.get("left"), check.get("right"))))
            elif left != right:
                problems.append(
                    Problem("error", "S101", location, "incompatible source pins: %s=%r but %s=%r" % (check.get("left"), left, check.get("right"), right))
                )
        elif op == "field_equals":
            left = _field(sources, check.get("left", ""))
            if left != check.get("right"):
                problems.append(Problem("error", "S101", location, "%s=%r, expected %r" % (check.get("left"), left, check.get("right"))))
        elif op == "field_matches":
            left = _field(sources, check.get("left", ""))
            if not isinstance(left, str) or not re.match(check.get("pattern", ".*"), left):
                problems.append(Problem("error", "S101", location, "%s=%r does not match %r" % (check.get("left"), left, check.get("pattern"))))
        elif op == "all_of_kind_have_field":
            wanted = set(check.get("kinds", []))
            field_name = check.get("field")
            for sid, source in sources.items():
                if source.get("kind") in wanted and not source.get(field_name):
                    problems.append(Problem("error", "S103", location, "source %r (%s) has no %r" % (sid, source.get("kind"), field_name)))
        elif op == "every_hashable_artifact_has":
            for sid, source in sources.items():
                for art in source.get("artifacts", []):
                    if art.get("kind") != "file":
                        continue
                    for field_name in check.get("fields", []):
                        if not art.get(field_name):
                            problems.append(Problem("error", "S003", location, "artifact %s/%s has no %s" % (sid, art.get("path"), field_name)))
        else:
            problems.append(Problem("error", "S104", location, "unknown compatibility check op %r" % op))


def _coverage_index(bundle: Bundle, problems: list[Problem]) -> dict:
    doc = bundle.coverage
    if doc is None:
        problems.append(Problem("error", "V000", "spec_coverage.json", "document is missing"))
        return {}
    index = {}
    entries = list(doc.get("core_chapters", [])) + list(doc.get("appendices", []))
    entries += list((doc.get("extension_appendices") or {}).get("reviewed_exceptions", []))
    for entry in entries:
        path = entry.get("path")
        if not path:
            problems.append(Problem("error", "V001", "spec_coverage.json", "coverage entry without path"))
            continue
        if not CHAPTER_RE.match(path):
            problems.append(Problem("error", "V005", path, "coverage path must look like chapters/*.adoc or appendices/*.adoc"))
        if entry.get("review_state") not in ("reviewed", "partially-reviewed", "not-reviewed"):
            problems.append(Problem("error", "V004", path, "unknown review_state %r" % entry.get("review_state")))
        if path in index:
            problems.append(Problem("error", "V001", path, "duplicate coverage path"))
        index[path] = entry
    return index


def validate_coverage(index: dict, row_ids: set[str], problems: list[Problem]) -> None:
    for path, entry in index.items():
        for row_id in entry.get("rows", []):
            if row_id not in row_ids:
                problems.append(Problem("error", "V002", path, "coverage entry names unknown requirement %r" % row_id))


def validate_requirements(bundle: Bundle, sources: dict, coverage_index: dict, problems: list[Problem]) -> list[dict]:
    doc = bundle.requirements
    if doc is None:
        problems.append(Problem("error", "R000", "requirements.json", "document is missing"))
        return []
    rows = doc.get("requirements", [])
    required = [
        "id",
        "category",
        "summary",
        "core_introduction",
        "applicability",
        "classification",
        "classification_basis",
        "condition",
        "coverage_chapter",
        "source",
        "features",
        "limits",
        "formats",
        "commands",
        "extensions",
        "cts",
        "implementation_state",
        "baseline",
        "evidence",
        "owner",
        "provenance",
        "manual_review",
        "review_notes",
    ]
    versions = set(doc.get("core_introduction_versions", []))
    classifications = set(doc.get("classification_vocabulary", []))
    applicability = set(doc.get("applicability_vocabulary", []))
    mappings = set(doc.get("cts_mapping_vocabulary", []))
    state_vocab = set(doc.get("state_vocabulary", []))
    evidence_vocab = doc.get("evidence_state_vocabulary", {})

    seen = {}
    for index, row in enumerate(rows):
        row_id = row.get("id") or "requirements[%d]" % index
        location = row_id
        _require(problems, row, required, location, "R001")
        if not ID_RE.match(str(row.get("id", ""))):
            problems.append(Problem("error", "R003", location, "id does not match the project id pattern"))
        if row_id in seen:
            problems.append(Problem("error", "R002", location, "duplicate requirement id (first at index %d)" % seen[row_id]))
        seen[row_id] = index
        if row.get("core_introduction") not in versions:
            problems.append(Problem("error", "R004", location, "unknown core_introduction %r" % row.get("core_introduction")))
        applicability_obj = row.get("applicability") or {}
        if applicability_obj.get("core") not in CORE_STATUS_VOCAB:
            problems.append(Problem("error", "R005", location, "unknown applicability.core %r" % applicability_obj.get("core")))
        if applicability_obj.get("target") not in TARGET_STATUS_VOCAB:
            problems.append(Problem("error", "R005", location, "unknown applicability.target %r" % applicability_obj.get("target")))
        if row.get("classification_basis") not in CLASSIFICATION_BASIS_VOCAB:
            problems.append(Problem("error", "R020", location, "unknown classification_basis %r" % row.get("classification_basis")))
        if row.get("classification") not in classifications:
            problems.append(Problem("error", "R006", location, "unknown classification %r" % row.get("classification")))
        if row.get("classification") == "conditional" and not (row.get("condition") or "").strip():
            problems.append(Problem("error", "R007", location, "conditional requirement without an exact condition"))
        if row.get("implementation_state") not in state_vocab:
            problems.append(Problem("error", "R009", location, "implementation_state %r is outside the state vocabulary" % row.get("implementation_state")))
        source = row.get("source") or {}
        if source.get("source_id") not in sources:
            problems.append(Problem("error", "R010", location, "unknown source_id %r" % source.get("source_id")))
        if not ANCHOR_RE.match(str(source.get("anchor", ""))):
            problems.append(Problem("error", "R010", location, "anchor %r is not a valid anchor token" % source.get("anchor")))
        chapter = row.get("coverage_chapter")
        if chapter not in coverage_index:
            problems.append(Problem("error", "R011", location, "coverage_chapter %r is not in spec_coverage.json" % chapter))
        elif row_id not in coverage_index[chapter].get("rows", []):
            problems.append(Problem("error", "V002", location, "spec_coverage.json entry %r does not list this requirement" % chapter))
        for key in ("features", "limits", "formats", "commands", "extensions"):
            _string_list(problems, row, key, location)
        cts = row.get("cts") or {}
        if cts.get("mapping") not in mappings:
            problems.append(Problem("error", "R012", location, "unknown cts.mapping %r" % cts.get("mapping")))
        if cts.get("basis") not in set(doc.get("cts_mapping_basis", [])):
            problems.append(Problem("error", "R012", location, "unknown cts.basis %r" % cts.get("basis")))
        groups = cts.get("groups", [])
        cases = cts.get("cases", [])
        if cts.get("mapping") == "mapped" and not groups and not cases:
            problems.append(Problem("error", "R013", location, "cts.mapping is 'mapped' but no group or case is named"))
        if cts.get("mapping") == "not-applicable" and (groups or cases):
            problems.append(Problem("error", "R017", location, "cts.mapping 'not-applicable' must not name groups or cases"))
        if cts.get("mapping") == "unmapped" and not (cts.get("note") or "").strip():
            problems.append(Problem("error", "R013", location, "unmapped CTS mapping must explain the gap in cts.note"))
        for group in groups:
            if not GROUP_RE.match(str(group)):
                problems.append(Problem("error", "R014", location, "group %r is not a mustpass group path" % group))
        for case in cases:
            if not CASE_RE.match(str(case)):
                problems.append(Problem("error", "R014", location, "case %r is not a dEQP-VK case name" % case))
        evidence = row.get("evidence") or {}
        for field_name, allowed in evidence_vocab.items():
            state = (evidence.get(field_name) or {}).get("state")
            if state not in allowed:
                problems.append(Problem("error", "R008", location, "evidence.%s.state %r is outside its vocabulary" % (field_name, state)))
        if (row.get("baseline") or {}).get("surface_state") not in ("none", "symbol-present", "documented", "host-tested", "native-demonstrated"):
            problems.append(Problem("error", "R018", location, "unknown baseline.surface_state %r" % (row.get("baseline") or {}).get("surface_state")))
    return rows


def validate_ordering(doc: dict, rows: list[dict], problems: list[Problem]) -> None:
    order = doc.get("category_order", [])
    group_of = {}
    for position, group in enumerate(order):
        for category in group:
            group_of[category] = position
    missing = sorted({row.get("category") for row in rows} - set(group_of))
    if missing:
        problems.append(Problem("error", "R015", "requirements.json", "categories missing from category_order: %s" % ", ".join(missing)))
    keys = [(group_of.get(row.get("category"), 999), row.get("id", "")) for row in rows]
    for index in range(1, len(keys)):
        if keys[index] < keys[index - 1]:
            problems.append(
                Problem(
                    "error",
                    "R015",
                    "requirements.json",
                    "rows are not in canonical (category_order, id) order at index %d (%s after %s)" % (index, keys[index][1], keys[index - 1][1]),
                )
            )
            break


def validate_cts_mapping(bundle: Bundle, rows: list[dict], problems: list[Problem]) -> dict:
    manifest = bundle.manifest
    stats = {"mapped": 0, "unmapped": 0, "not-applicable": 0}
    if manifest is None:
        return stats
    if manifest.get("listing_kind") != "static-source-listing":
        problems.append(Problem("error", "C002", "cts_manifest.json", "listing_kind must be 'static-source-listing'"))
    cts_source = None
    for source in (bundle.sources or {}).get("sources", []):
        if source.get("id") == manifest.get("source_id"):
            cts_source = source
    if cts_source is None:
        problems.append(Problem("error", "C001", "cts_manifest.json", "source_id %r is not in sources.json" % manifest.get("source_id")))
    elif cts_source.get("commit") != manifest.get("commit"):
        problems.append(
            Problem("error", "C001", "cts_manifest.json", "manifest commit %s does not match pinned source commit %s" % (manifest.get("commit"), cts_source.get("commit")))
        )
    known_groups = {entry["group"] for entry in manifest.get("groups", [])}
    known_cases = None
    if bundle.cts_cache_dir:
        known_cases = load_cts_cache(manifest, bundle.cts_cache_dir, problems)
    for row in rows:
        cts = row.get("cts") or {}
        mapping = cts.get("mapping")
        stats[mapping] = stats.get(mapping, 0) + 1
        for group in cts.get("groups", []):
            if group not in known_groups:
                problems.append(Problem("error", "R014", row.get("id"), "CTS group %r is not in the pinned mustpass manifest" % group))
        if known_cases is not None:
            for case in cts.get("cases", []):
                if case not in known_cases:
                    problems.append(Problem("error", "R014", row.get("id"), "CTS case %r is not present in the pinned listing cache" % case))
    return stats


def load_cts_cache(manifest: dict, cache_dir: str, problems: list[Problem]) -> set[str]:
    tag = manifest.get("tag")
    prefix = os.path.join(cache_dir, tag) if tag else cache_dir
    if not os.path.isdir(prefix):
        problems.append(Problem("error", "C003", prefix, "CTS listing cache for the pinned revision is missing"))
        return set()
    cases: set[str] = set()
    for entry in manifest.get("groups", []):
        path = os.path.join(prefix, entry["path"])
        if not os.path.exists(path):
            problems.append(Problem("error", "C003", path, "cached listing file is missing"))
            continue
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line and not line.startswith("#"):
                    cases.add(line)
    return cases


def validate_target(bundle: Bundle, sources: dict, rows: list[dict], problems: list[Problem]) -> dict:
    """Check requirement classifications against the cumulative core requirement set.

    The basis is Vulkan 1.4 core for a graphics implementation: the cumulative
    obligations of 1.0 through 1.4, taken from the pinned specification section
    "Feature Requirements" and the pinned registry per-version blocks, with each
    conditional obligation carrying its exact trigger. Roadmap profiles are a
    comparison and must not drive classification.
    """
    target = bundle.target
    declared = (bundle.requirements or {}).get("target") or {}
    if target is None:
        problems.append(Problem("error", "T000", "core_target.json", "document is missing"))
        return {}
    if declared.get("target_file") != "core_target.json":
        problems.append(Problem("error", "T000", "requirements.json", "requirements.target.target_file must reference core_target.json"))

    basis = target.get("target", {}).get("basis", {})
    fr = basis.get("feature_requirements") or {}
    spec_source = sources.get(fr.get("source_id"))
    if spec_source is None:
        problems.append(Problem("error", "T001", "core_target.json", "feature-requirement basis names unknown source %r" % fr.get("source_id")))
    else:
        alias_sha = (spec_source.get("document_alias") or {}).get("sha256")
        if alias_sha != fr.get("document_sha256"):
            problems.append(Problem("error", "T001", "core_target.json", "specification basis sha256 does not match sources.json (%s vs %s)" % (fr.get("document_sha256"), alias_sha)))
    api = basis.get("api_surface") or {}
    reg_source = sources.get(api.get("source_id"))
    if reg_source is None:
        problems.append(Problem("error", "T001", "core_target.json", "api-surface basis names unknown source %r" % api.get("source_id")))
    else:
        artifact = next((a for a in reg_source.get("artifacts", []) if a.get("path") == api.get("path")), None)
        if artifact is None:
            problems.append(Problem("error", "T001", "core_target.json", "registry basis names artifact %r that sources.json does not pin" % api.get("path")))
        elif artifact.get("sha256") != api.get("sha256"):
            problems.append(Problem("error", "T001", "core_target.json", "registry basis sha256 does not match sources.json pin"))

    bits = target.get("mandatory_feature_bits", {})
    cumulative_bits = set(bits.get("cumulative", []))
    conditional_map: dict[str, str] = {}
    for entry in bits.get("conditional_in_version_blocks", []):
        for feature in entry.get("features", []):
            conditional_map[feature] = entry.get("condition") or ""
    optional_extension_bits: dict[str, str] = {}
    for entry in bits.get("conditional_on_optional_extension", []):
        for feature in entry.get("features", []):
            optional_extension_bits.setdefault(feature, entry.get("condition") or "")
    if not cumulative_bits:
        problems.append(Problem("error", "T000", "core_target.json", "no mandatory core feature bits were derived"))

    mandatory_bits = cumulative_bits - set(conditional_map)
    referenced_mandatory: set[str] = set()
    referenced_conditional: set[str] = set()
    optional_rows_with_core_capability = []
    for row in rows:
        location = row.get("id")
        applicability = row.get("applicability") or {}
        core_status = applicability.get("core")
        expected = TARGET_TO_CLASSIFICATION.get("required" if row.get("classification") == "mandatory" else row.get("classification"))
        feats = set(row.get("features") or [])
        hit_mandatory = feats & mandatory_bits
        hit_conditional = feats & (set(conditional_map) | (set(optional_extension_bits) - mandatory_bits - set(conditional_map)))
        referenced_mandatory |= hit_mandatory
        referenced_conditional |= hit_conditional

        if hit_mandatory and row.get("classification") != "mandatory":
            problems.append(
                Problem(
                    "error",
                    "T002",
                    location,
                    "declares core-mandatory capabilities %s but is classified %r" % (sorted(hit_mandatory)[:6], row.get("classification")),
                )
            )
        elif hit_conditional and not hit_mandatory and row.get("classification") not in ("conditional",):
            problems.append(
                Problem(
                    "error",
                    "T003",
                    location,
                    "declares core-conditional capabilities %s; classification must be conditional, not %r" % (sorted(hit_conditional)[:6], row.get("classification")),
                )
            )
        if row.get("classification") == "conditional" and not (row.get("condition") or applicability.get("target_condition") or "").strip():
            problems.append(Problem("error", "T004", location, "conditional row without an exact condition"))
        if row.get("classification") == "optional" and (hit_mandatory or hit_conditional):
            optional_rows_with_core_capability.append((location, sorted(hit_mandatory | hit_conditional)))
            problems.append(
                Problem(
                    "error",
                    "T005",
                    location,
                    "classified optional but declares core capabilities %s" % sorted(hit_mandatory | hit_conditional)[:6],
                )
            )
        if row.get("classification_basis", "").startswith("roadmap") or "roadmap" in (applicability.get("target_condition") or "").lower():
            problems.append(Problem("error", "T009", location, "classification must not be derived from a roadmap profile"))

    uncovered_mandatory = sorted(mandatory_bits - referenced_mandatory)
    uncovered_conditional = sorted(set(conditional_map) - referenced_conditional)
    for name in uncovered_mandatory:
        problems.append(Problem("error", "T007", "core_target.json", "core-mandatory feature bit %r is not referenced by any requirement row" % name))
    for name in uncovered_conditional:
        problems.append(Problem("error", "T008", "core_target.json", "core-conditional feature bit %r is not referenced by any requirement row" % name))

    roadmap = bundle.roadmap or {}
    comparison = roadmap.get("comparison") or {}
    if comparison and comparison.get("status") != "comparison-only":
        problems.append(Problem("error", "T010", "roadmap_comparison.json", "roadmap file must be marked comparison-only"))

    return {
        "id": target.get("target", {}).get("id"),
        "definition": target.get("target", {}).get("definition"),
        "core_mandatory_feature_bits": len(mandatory_bits),
        "core_conditional_feature_bits": len(conditional_map),
        "api_surface_commands": (target.get("api_surface", {}).get("surface_by_profile", {}).get("graphics_including_base", {}) or {}).get("commands_total"),
        "api_surface_types": (target.get("api_surface", {}).get("surface_by_profile", {}).get("graphics_including_base", {}) or {}).get("types_total"),
        "roadmap_comparison": {
            "status": comparison.get("status"),
            "reference_profile": roadmap.get("comparison", {}).get("basis", {}).get("profile"),
            "feature_bits": len(roadmap.get("required_feature_bits", [])),
            "extensions": len(roadmap.get("required_extensions", [])),
        },
        "rows_classified_against_target": len(rows),
        "optional_rows_with_core_capability": optional_rows_with_core_capability,
    }


def validate_core_tables(bundle: Bundle, rows: list[dict], problems: list[Problem]) -> dict:
    """Check the limits, formats and command-contract tables against the rows.

    The tables are the machine-readable form of three core requirement families;
    these checks make sure each table is actually represented by requirement
    rows, so a table cannot exist without a tracked obligation.
    """
    target = bundle.target or {}
    limits = target.get("limits") or {}
    formats = target.get("formats") or {}
    contracts = (target.get("command_contracts") or {}).get("contracts") or {}
    rule_list = (target.get("mandatory_feature_bits") or {}).get("extension_rules") or []
    stats = {"limit_rows": 0, "limits_raised_in_1_4": 0, "format_tables": 0, "format_rows": 0, "contracts": 0, "extension_rules": 0}

    for section, key in ((limits, "limits"), (formats, "formats"), (target.get("command_contracts") or {}, "command_contracts")):
        if not section.get("resolution_rule"):
            problems.append(Problem("error", "T011", "core_target.json", "%s section has no resolution_rule" % key))

    limit_names = {row["limit"] for row in limits.get("rows", [])}
    stats["limit_rows"] = len(limit_names)
    stats["limits_raised_in_1_4"] = len(limits.get("raised_in_1_4", []))
    row_limits = {entry.split()[0] for row in rows for entry in (row.get("limits") or []) if entry}
    for name in limits.get("raised_in_1_4", []):
        if name not in row_limits:
            problems.append(Problem("error", "T013", "core_target.json", "1.4-raised limit %r is not referenced by any requirement row" % name))

    tables = formats.get("tables", [])
    stats["format_tables"] = len(tables)
    stats["format_rows"] = sum(len(table.get("rows", [])) for table in tables)
    row_formats = {name for row in rows for name in (row.get("formats") or [])}
    for table in tables:
        for entry in table.get("rows", []):
            if entry["format"] not in row_formats:
                problems.append(Problem("error", "T014", table.get("anchor"), "mandatory format %r is not referenced by any requirement row" % entry["format"]))
                break

    stats["contracts"] = len(contracts)
    for area, commands in contracts.items():
        if not commands:
            problems.append(Problem("error", "T015", area, "command contract has no commands"))
            continue
        resolved = set((target.get("api_surface", {}).get("surface_by_profile", {}).get("graphics_resolved", {}) or {}).get("commands", []))
        unknown = sorted(set(commands) - resolved)
        if unknown:
            problems.append(Problem("error", "T015", area, "contract names commands outside the resolved core surface: %s" % unknown[:5]))
        if not any(set(row.get("commands") or []) & set(commands) for row in rows):
            problems.append(Problem("error", "T015", area, "no requirement row covers the %r contract" % area))

    stats["extension_rules"] = len(rule_list)
    for rule in rule_list:
        if rule.get("kind") == "unclassified":
            problems.append(Problem("error", "T016", "core_target.json", "extension rule without a taxonomy: %r" % rule.get("text", "")[:80]))
        if rule.get("kind") == "extension-required-by-feature" and not rule.get("trigger"):
            problems.append(Problem("error", "T016", "core_target.json", "extension rule without its trigger: %r" % rule.get("text", "")[:80]))
    if not any(row.get("extensions") for row in rows) and rule_list:
        problems.append(Problem("error", "T016", "core_target.json", "extension rules are not referenced by any requirement row"))

    # T017/T020/T021/T022: format rules, cells and scopes must survive into the
    # requirement rows without any generic or unresolved condition.
    rows_by_anchor = {}
    for row in rows:
        anchor = (row.get("source") or {}).get("anchor")
        if anchor:
            rows_by_anchor.setdefault(anchor, []).append(row)
    for table in tables:
        anchor = table["anchor"]
        target_rows = rows_by_anchor.get(anchor, [])
        recorded = " ".join(entry for row in target_rows for entry in (row.get("capability_conditions") or []))
        annotations = table.get("annotations", [])
        for annotation in annotations:
            if annotation.get("condition_kind") != "resolved":
                problems.append(
                    Problem("error", "T021", anchor, "annotation is unresolved: %r" % (annotation.get("text", "")[:90]))
                )
                continue
            if annotation.get("condition") and annotation["condition"] not in recorded:
                problems.append(
                    Problem("error", "T020", anchor, "annotation (%s) is not recorded in the requirement row" % annotation.get("kind"))
                )
            if annotation.get("symbol") and annotation["symbol"] not in recorded:
                problems.append(Problem("error", "T020", anchor, "symbol %s lost during row generation" % annotation["symbol"]))
            if annotation.get("guard") and annotation["guard"] not in recorded:
                problems.append(Problem("error", "T020", anchor, "guard %s lost during row generation" % annotation["guard"]))
        for feature, conditions in (table.get("column_conditions") or {}).items():
            for condition in conditions:
                if condition not in recorded:
                    problems.append(Problem("error", "T020", anchor, "column condition for %s is not recorded: %r" % (feature, condition[:60])))
        conditional_cells = 0
        for entry in table.get("rows", []):
            for cell in entry.get("cells", []):
                if cell.get("symbol") != "{sym1}" or cell.get("conditions"):
                    conditional_cells += 1
                    if not cell.get("conditions"):
                        problems.append(
                            Problem("error", "T021", anchor, "%s/%s carries a conditional symbol with no resolved condition" % (entry["format"], cell["feature"]))
                        )
                    legend_text = (table.get("symbol_legend") or {}).get(cell.get("symbol"))
                    if legend_text and [condition for condition in cell.get("conditions", []) if condition == legend_text]:
                        problems.append(
                            Problem("error", "T021", anchor, "%s/%s uses the generic legend text as its condition" % (entry["format"], cell["feature"]))
                        )
                    # A scope that the source states must reach the cell; a cell
                    # whose rules state no scope keeps the explicit
                    # "table-defined" marker instead of inventing one.
                    # The generator attributes each cell to the rules that apply
                    # to it (its symbol or its feature bit). A scope stated by
                    # one of those rules must survive; when none states a scope,
                    # the cell keeps an explicit "table-defined" marker.
                    stated_scopes = set(cell.get("rule_scopes") or [])
                    if stated_scopes and cell.get("scope") not in stated_scopes:
                        problems.append(
                            Problem("error", "T022", anchor, "%s/%s lost the scope %s stated by its rules" % (entry["format"], cell["feature"], sorted(stated_scopes)))
                        )
                    elif not stated_scopes and cell.get("scope_kind") != "table-defined":
                        problems.append(
                            Problem("error", "T022", anchor, "%s/%s has no scope and no explicit table-defined marker" % (entry["format"], cell["feature"]))
                        )
                for condition in cell.get("conditions", []):
                    if condition not in recorded:
                        problems.append(
                            Problem("error", "T017", anchor, "%s/%s condition not recorded in the row: %r" % (entry["format"], cell["feature"], condition[:60]))
                        )
        stats["conditional_format_cells"] = stats.get("conditional_format_cells", 0) + conditional_cells
        stats["format_annotations"] = stats.get("format_annotations", 0) + len(annotations)
    stats["format_annotation_kinds"] = sorted({annotation.get("kind") for table in tables for annotation in table.get("annotations", [])})
    stats["unresolved_format_annotations"] = sum(
        1 for table in tables for annotation in table.get("annotations", []) if annotation.get("condition_kind") != "resolved"
    )
    # T019: every limit value must have been interpreted, never stripped.
    known_kinds = {"integer", "decimal", "power", "fraction", "tuple", "expression", "reference", "enum", "none", "descriptive", "bitfield", "boolean"}
    for entry in limits.get("rows", []):
        for value in entry.get("values", []):
            if value.get("kind") not in known_kinds:
                problems.append(Problem("error", "T019", entry["limit"], "uninterpreted limit value %r (kind %r)" % (value.get("raw"), value.get("kind"))))
        if entry.get("required_kind") not in known_kinds:
            problems.append(Problem("error", "T019", entry["limit"], "unknown required_kind %r" % entry.get("required_kind")))
        if entry["limit_type"].startswith(("min", "max")) and entry.get("required_for_core_1_4") is None and entry.get("required_kind") not in ("none", "enum", "descriptive", "bitfield", "boolean", "reference"):
            problems.append(Problem("error", "T019", entry["limit"], "required limit without an interpreted requirement"))
    stats["conditional_format_cells"] = sum(len(entry.get("conditional_feature_bits", [])) for table in tables for entry in table.get("rows", []))
    stats["limit_value_kinds"] = sorted({value.get("kind") for entry in limits.get("rows", []) for value in entry.get("values", [])})
    return stats


README_BEGIN = "<!-- stats:begin -->"
README_END = "<!-- stats:end -->"


def readme_stats(bundle: Bundle, rows: list[dict]) -> dict:
    """The numbers quoted in README.md. Generated, never hand-maintained."""
    target = bundle.target or {}
    bits = target.get("mandatory_feature_bits", {})
    conditional = {feature for entry in bits.get("conditional_in_version_blocks", []) for feature in entry.get("features", [])}
    surface = (target.get("api_surface", {}).get("surface_by_profile", {}) or {}).get("graphics_resolved", {}) or {}
    manifest = bundle.manifest or {}
    counts = {"mandatory": 0, "conditional": 0, "optional": 0}
    quality = {}
    for row in rows:
        counts[row.get("classification")] = counts.get(row.get("classification"), 0) + 1
        value = (row.get("cts") or {}).get("coverage_quality")
        if value:
            quality[value] = quality.get(value, 0) + 1
    return {
        "requirements": len(rows),
        "requirements_mandatory": counts.get("mandatory", 0),
        "requirements_conditional": counts.get("conditional", 0),
        "requirements_optional": counts.get("optional", 0),
        "core_mandatory_feature_bits": len(set(bits.get("cumulative", [])) - conditional),
        "core_conditional_feature_bits": len(conditional),
        "core_commands": surface.get("commands_total"),
        "core_types": surface.get("types_total"),
        "limits_rows": len((target.get("limits") or {}).get("rows", [])),
        "limits_raised_in_1_4": len((target.get("limits") or {}).get("raised_in_1_4", [])),
        "format_tables": len((target.get("formats") or {}).get("tables", [])),
        "format_rows": sum(len(table.get("rows", [])) for table in (target.get("formats") or {}).get("tables", [])),
        "command_contracts": len((target.get("command_contracts") or {}).get("contracts", {})),
        "extension_rules": len(bits.get("extension_rules", [])),
        "cts_group_files": manifest.get("totals", {}).get("group_files"),
        "cts_cases": manifest.get("totals", {}).get("cases"),
        "cts_mapped_rows": sum(1 for row in rows if (row.get("cts") or {}).get("mapping") == "mapped"),
        "cts_representative_or_family_rows": quality.get("representative-case", 0) + quality.get("family-level", 0),
        "cts_direct_rows": quality.get("direct", 0),
    }


def validate_readme(bundle: Bundle, rows: list[dict], problems: list[Problem]) -> dict:
    """The README quotes generated numbers; a stale block is an error."""
    stats = readme_stats(bundle, rows)
    text = bundle.readme_text
    if text is None:
        # Synthetic bundles in tests do not always carry the README; when the
        # file is present it must contain a current block.
        return stats
    if README_BEGIN not in text or README_END not in text:
        problems.append(Problem("error", "T018", "README.md", "README.md has no generated stats block"))
        return stats
    block = text.split(README_BEGIN, 1)[1].split(README_END, 1)[0]
    payload = block.split("```json", 1)[-1].split("```", 1)[0]
    try:
        quoted = json.loads(payload)
    except json.JSONDecodeError as exc:
        problems.append(Problem("error", "T018", "README.md", "stats block is not valid JSON: %s" % exc))
        return stats
    for key, value in sorted(stats.items()):
        if key not in quoted:
            problems.append(Problem("error", "T018", "README.md", "stats block is missing %r" % key))
        elif quoted[key] != value:
            problems.append(
                Problem("error", "T018", "README.md", "stats block %s=%r but the data says %r" % (key, quoted[key], value))
            )
    return stats


def validate_baseline_surface(bundle: Bundle, rows: list[dict], problems: list[Problem]) -> dict:
    surface = bundle.surface
    if surface is None:
        problems.append(Problem("error", "B000", "baseline_surface.json", "document is missing"))
        return {}
    names = {entry["name"] for entry in surface.get("entry_points", [])}
    public = {entry["name"] for entry in surface.get("entry_points", []) if entry.get("public_header")}
    for row in rows:
        baseline = row.get("baseline") or {}
        present = baseline.get("entry_points") or []
        absent = baseline.get("absent_entry_points") or []
        for name in present:
            if name not in names:
                problems.append(Problem("error", "B001", row.get("id"), "baseline claims entry point %r that is not in baseline_surface.json" % name))
        for name in absent:
            if name in names:
                problems.append(Problem("error", "B001", row.get("id"), "baseline claims entry point %r is absent but the baseline surface inventory contains it" % name))
        if present and baseline.get("surface_state") == "none":
            problems.append(Problem("error", "B002", row.get("id"), "surface_state 'none' contradicts a non-empty entry_point list"))
        if present and not baseline.get("note"):
            problems.append(Problem("error", "B003", row.get("id"), "present entry points must carry an explicit baseline note"))
        if present and all(name not in public for name in present) and "public header" not in (baseline.get("note") or ""):
            problems.append(
                Problem("error", "B004", row.get("id"), "entry points exist only outside the public header; the note must say so explicitly")
            )
    return {
        "entry_points": surface.get("counts", {}).get("entry_points"),
        "dispatched": surface.get("counts", {}).get("dispatched"),
        "public_header": surface.get("counts", {}).get("public_header"),
        "implementation_only": surface.get("counts", {}).get("implementation_only"),
        "baseline_commit": surface.get("baseline_commit"),
    }


def validate_cts_coverage_quality(rows: list[dict], problems: list[Problem]) -> dict:
    counts = {name: 0 for name in sorted(CTS_COVERAGE_QUALITY_VOCAB)}
    for row in rows:
        cts = row.get("cts") or {}
        quality = cts.get("coverage_quality")
        if quality not in CTS_COVERAGE_QUALITY_VOCAB:
            problems.append(Problem("error", "C010", row.get("id"), "cts.coverage_quality %r is missing or unknown" % quality))
            continue
        counts[quality] = counts.get(quality, 0) + 1
        note = (cts.get("coverage_note") or "").strip()
        if quality == "not-mapped":
            if cts.get("mapping") == "mapped":
                problems.append(Problem("error", "C012", row.get("id"), "mapped CTS row cannot use coverage_quality 'not-mapped'"))
        elif not note:
            problems.append(Problem("error", "C011", row.get("id"), "coverage_quality %r requires a coverage_note describing the gap" % quality))
        if quality == "direct" and not (cts.get("cases") or []):
            problems.append(Problem("error", "C013", row.get("id"), "coverage_quality 'direct' requires at least one named case"))
        if quality == "family-level" and (cts.get("cases") or []):
            problems.append(Problem("warning", "C014", row.get("id"), "family-level mapping also names specific cases; confirm the quality label"))
    return counts


def validate_anchors(bundle: Bundle, rows: list[dict], problems: list[Problem]) -> None:
    if not bundle.anchor_index:
        return
    for row in rows:
        anchor = (row.get("source") or {}).get("anchor")
        if anchor and anchor not in bundle.anchor_index:
            problems.append(Problem("error", "A001", row.get("id"), "anchor %r is not present in the pinned specification" % anchor))


def validate_consumers(bundle: Bundle, sources: dict, row_ids: set[str], problems: list[Problem]) -> dict:
    doc = bundle.consumers
    if doc is None:
        problems.append(Problem("error", "N000", "consumers.json", "document is missing"))
        return {}
    stats = {"consumers": 0, "requirements": 0, "by_consumer": {}}
    seen = set()
    for index, consumer in enumerate(doc.get("consumers", [])):
        cid = consumer.get("id", "consumers[%d]" % index)
        if cid in seen:
            problems.append(Problem("error", "N001", cid, "duplicate consumer id"))
        seen.add(cid)
        if consumer.get("source_id") not in sources:
            problems.append(Problem("error", "N002", cid, "unknown source_id %r" % consumer.get("source_id")))
        stats["consumers"] += 1
        stats["by_consumer"][cid] = len(consumer.get("requirements", []))
        seen_rows = set()
        for item in consumer.get("requirements", []):
            rid = item.get("id", "?")
            location = "%s/%s" % (cid, rid)
            if rid in seen_rows:
                problems.append(Problem("error", "N004", location, "duplicate consumer requirement id"))
            seen_rows.add(rid)
            if item.get("classification") not in ("mandatory", "conditional", "optional"):
                problems.append(Problem("error", "N005", location, "unknown classification %r" % item.get("classification")))
            if not item.get("references"):
                problems.append(Problem("error", "N006", location, "consumer requirement has no pinned references"))
            for row_id in item.get("related_requirement_ids", []):
                if row_id not in row_ids:
                    problems.append(Problem("error", "N003", location, "related_requirement_ids names unknown requirement %r" % row_id))
            stats["requirements"] += 1
    return stats


def validate(bundle: Bundle) -> tuple[list[Problem], dict]:
    problems: list[Problem] = []
    sources = validate_sources(bundle, problems)
    validate_compatibility_rules(bundle, sources, problems)
    rows: list[dict] = []
    coverage_index = _coverage_index(bundle, problems) if bundle.coverage is not None else {}
    if bundle.requirements is not None:
        rows = validate_requirements(bundle, sources, coverage_index, problems)
        validate_ordering(bundle.requirements, rows, problems)
    row_ids = {row.get("id") for row in rows}
    if bundle.coverage is not None:
        validate_coverage(coverage_index, row_ids, problems)
    cts_stats = validate_cts_mapping(bundle, rows, problems)
    quality_stats = validate_cts_coverage_quality(rows, problems)
    target_stats = validate_target(bundle, sources, rows, problems)
    table_stats = validate_core_tables(bundle, rows, problems)
    readme_stats_result = validate_readme(bundle, rows, problems)
    surface_stats = validate_baseline_surface(bundle, rows, problems)
    validate_anchors(bundle, rows, problems)
    consumer_stats = validate_consumers(bundle, sources, row_ids, problems)

    report = build_report(bundle, rows, cts_stats, quality_stats, target_stats, table_stats, readme_stats_result, surface_stats, consumer_stats, problems)
    return problems, report


def build_report(
    bundle: Bundle,
    rows: list[dict],
    cts_stats: dict,
    quality_stats: dict,
    target_stats: dict,
    table_stats: dict,
    readme_stats_result: dict,
    surface_stats: dict,
    consumer_stats: dict,
    problems: list[Problem],
) -> dict:
    by_category: dict[str, int] = {}
    by_classification: dict[str, int] = {}
    by_state: dict[str, int] = {}
    by_introduction: dict[str, int] = {}
    unmapped_rows = []
    unaudited_rows = 0
    for row in rows:
        by_category[row.get("category")] = by_category.get(row.get("category"), 0) + 1
        by_classification[row.get("classification")] = by_classification.get(row.get("classification"), 0) + 1
        by_state[row.get("implementation_state")] = by_state.get(row.get("implementation_state"), 0) + 1
        by_introduction[row.get("core_introduction")] = by_introduction.get(row.get("core_introduction"), 0) + 1
        if (row.get("cts") or {}).get("mapping") == "unmapped":
            unmapped_rows.append(row.get("id"))
        if row.get("implementation_state") == "not-audited":
            unaudited_rows += 1

    coverage_counts = {"reviewed": 0, "partially-reviewed": 0, "not-reviewed": 0}
    appendix_counts = {"reviewed": 0, "partially-reviewed": 0, "not-reviewed": 0}
    coverage = bundle.coverage or {}
    for entry in coverage.get("core_chapters", []):
        state = entry.get("review_state")
        if state in coverage_counts:
            coverage_counts[state] += 1
    for entry in coverage.get("appendices", []):
        state = entry.get("review_state")
        if state in appendix_counts:
            appendix_counts[state] += 1

    manifest = bundle.manifest or {}
    report = {
        "documents": {
            "sources": len((bundle.sources or {}).get("sources", [])),
            "requirements": len(rows),
            "consumers": consumer_stats.get("consumers", 0),
            "coverage_entries": len((coverage.get("core_chapters", []) + coverage.get("appendices", []))),
        },
        "requirements": {
            "by_category": dict(sorted(by_category.items())),
            "by_classification": dict(sorted(by_classification.items())),
            "by_core_introduction": dict(sorted(by_introduction.items())),
            "by_implementation_state": dict(sorted(by_state.items())),
        },
        "cts": {
            "mapping_counts": dict(sorted(cts_stats.items())),
            "coverage_quality_counts": dict(sorted((k, v) for k, v in quality_stats.items() if v)),
            "pinned_tag": manifest.get("tag"),
            "pinned_commit": manifest.get("commit"),
            "group_files": manifest.get("totals", {}).get("group_files"),
            "cases_in_listing": manifest.get("totals", {}).get("cases"),
            "listing_kind": manifest.get("listing_kind"),
            "unmapped_requirement_ids": sorted(unmapped_rows),
        },
        "target": target_stats,
        "core_tables": table_stats,
        "readme_stats": readme_stats_result,
        "baseline_surface": surface_stats,
        "consumers": consumer_stats,
        "spec_coverage": {
            "core_chapters": coverage_counts,
            "appendices": appendix_counts,
            "extension_appendices_reviewed_exceptions": len(
                (coverage.get("extension_appendices") or {}).get("reviewed_exceptions", [])
            ),
            "extension_appendices_not_reviewed": (coverage.get("extension_appendices") or {}).get("total_extension_files"),
        },
        "unknowns": {
            "requirements_not_audited": unaudited_rows,
            "requirements_total": len(rows),
            "note": "not-audited, not-run, harness-blocked, missing, implemented-unvalidated, allowed-not-supported and cts-fail are never counted as passes.",
            "never_a_pass": sorted(NEVER_A_PASS),
        },
        "errors": [p.render() for p in problems if p.level == "error"],
        "warnings": [p.render() for p in problems if p.level == "warning"],
        "notes": [
            "No success percentage is computed: coverage, mapping and runtime evidence are incomplete by construction.",
            "CTS mapping counts describe a static join against a pinned source listing, not executed tests.",
        ],
    }
    return report


def render_text(report: dict) -> str:
    lines = []
    docs = report["documents"]
    lines.append("inventory: sources=%d requirements=%d consumers=%d coverage_entries=%d" % (
        docs["sources"], docs["requirements"], docs["consumers"], docs["coverage_entries"]))
    lines.append("")
    lines.append("requirements by category")
    for name, count in report["requirements"]["by_category"].items():
        lines.append("  %-20s %3d" % (name, count))
    lines.append("requirements by classification")
    for name, count in report["requirements"]["by_classification"].items():
        lines.append("  %-20s %3d" % (name, count))
    target = report.get("target") or {}
    if target:
        lines.append("")
        lines.append("declared target: %s" % target.get("id"))
        lines.append("  %s" % (target.get("definition") or ""))
        lines.append(
            "  core requirement sets: %s unconditional feature bits, %s conditional feature bits, %s core commands / %s core types"
            % (
                target.get("core_mandatory_feature_bits"),
                target.get("core_conditional_feature_bits"),
                target.get("api_surface_commands"),
                target.get("api_surface_types"),
            )
        )
        comparison = target.get("roadmap_comparison") or {}
        if comparison:
            lines.append(
                "  roadmap comparison (not the classification basis, %s): %s with %s feature bits and %s extensions"
                % (comparison.get("status"), comparison.get("reference_profile"), comparison.get("feature_bits"), comparison.get("extensions"))
            )
    tables = report.get("core_tables") or {}
    if tables:
        lines.append(
            "  core tables: %s limits (%s raised in 1.4), %s format tables (%s format rows), %s command contracts, %s classified extension rules"
            % (
                tables.get("limit_rows"),
                tables.get("limits_raised_in_1_4"),
                tables.get("format_tables"),
                tables.get("format_rows"),
                tables.get("contracts"),
                tables.get("extension_rules"),
            )
        )
    surface = report.get("baseline_surface") or {}
    if surface:
        lines.append("")
        lines.append(
            "baseline surface @ %s: %s entry points, %s dispatched, %s declared in the public header, %s implementation-only"
            % (
                (surface.get("baseline_commit") or "")[:12],
                surface.get("entry_points"),
                surface.get("dispatched"),
                surface.get("public_header"),
                surface.get("implementation_only"),
            )
        )
    lines.append("requirements by core introduction version")
    for name, count in report["requirements"]["by_core_introduction"].items():
        lines.append("  %-20s %3d" % (name, count))
    lines.append("implementation state (audited/unknown ledger)")
    for name, count in report["requirements"]["by_implementation_state"].items():
        lines.append("  %-20s %3d" % (name, count))
    lines.append("")
    cts = report["cts"]
    lines.append("CTS mapping against %s @ %s" % (cts["pinned_tag"], (cts["pinned_commit"] or "")[:12]))
    lines.append("  listing kind        %s" % cts["listing_kind"])
    lines.append("  group files         %s" % cts["group_files"])
    lines.append("  cases in listing    %s" % cts["cases_in_listing"])
    for name, count in cts["mapping_counts"].items():
        lines.append("  %-19s %3d" % (name, count))
    for name, count in (cts.get("coverage_quality_counts") or {}).items():
        lines.append("  quality %-11s %3d" % (name, count))
    if cts["unmapped_requirement_ids"]:
        lines.append("  unmapped: %s" % ", ".join(cts["unmapped_requirement_ids"]))
    lines.append("")
    cov = report["spec_coverage"]
    lines.append("specification coverage ledger")
    for name in ("reviewed", "partially-reviewed", "not-reviewed"):
        lines.append("  core chapters %-18s %3d" % (name, cov["core_chapters"][name]))
    for name in ("reviewed", "partially-reviewed", "not-reviewed"):
        lines.append("  appendices    %-18s %3d" % (name, cov["appendices"][name]))
    lines.append("  extension appendices not reviewed %3s (of which %d individually reviewed)" % (
        cov["extension_appendices_not_reviewed"], cov["extension_appendices_reviewed_exceptions"]))
    lines.append("")
    unknowns = report["unknowns"]
    lines.append("unknowns: %d of %d requirements are not-audited" % (unknowns["requirements_not_audited"], unknowns["requirements_total"]))
    for note in report["notes"]:
        lines.append("note: %s" % note)
    if report["warnings"]:
        lines.append("")
        lines.append("warnings (%d)" % len(report["warnings"]))
        for item in report["warnings"]:
            lines.append("  " + item)
    if report["errors"]:
        lines.append("")
        lines.append("errors (%d)" % len(report["errors"]))
        for item in report["errors"]:
            lines.append("  " + item)
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inventory-dir", default=HERE)
    parser.add_argument("--anchor-index", help="file produced by tools/extract_spec_anchors.py")
    parser.add_argument("--cts-cache", help="directory produced by tools/collect_cts_listing.py")
    parser.add_argument("--json", action="store_true", help="emit the report as JSON")
    parser.add_argument("--quiet", action="store_true", help="only print errors and warnings")
    args = parser.parse_args(argv)

    bundle = Bundle.load(args.inventory_dir)
    bundle.cts_cache_dir = args.cts_cache or os.path.join(args.inventory_dir, ".cache", "cts")
    if not os.path.isdir(bundle.cts_cache_dir):
        bundle.cts_cache_dir = None
    anchor_path = args.anchor_index or os.path.join(args.inventory_dir, ".cache", "spec_anchors.txt")
    if os.path.exists(anchor_path):
        with open(anchor_path, "r", encoding="utf-8") as handle:
            bundle.anchor_index = {line.strip() for line in handle if line.strip() and not line.startswith("#")}

    problems, report = validate(bundle)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=False))
    elif not args.quiet:
        print(render_text(report))
    else:
        for entry in report["warnings"] + report["errors"]:
            print(entry)
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
