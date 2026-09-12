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
        )
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
        if (row.get("applicability") or {}).get("vulkan14") not in applicability:
            problems.append(Problem("error", "R005", location, "unknown applicability.vulkan14 %r" % (row.get("applicability") or {}).get("vulkan14")))
        if row.get("classification") not in classifications:
            problems.append(Problem("error", "R006", location, "unknown classification %r" % row.get("classification")))
        if row.get("classification") == "conditional" and not (row.get("condition") or "").strip():
            problems.append(Problem("error", "R007", location, "conditional requirement without an exact condition"))
        if row.get("classification") == "mandatory" and (row.get("applicability") or {}).get("vulkan14") == "optional-feature-gated":
            problems.append(Problem("warning", "R016", location, "mandatory classification with optional-feature-gated applicability"))
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
    validate_anchors(bundle, rows, problems)
    consumer_stats = validate_consumers(bundle, sources, row_ids, problems)

    report = build_report(bundle, rows, cts_stats, consumer_stats, problems)
    return problems, report


def build_report(bundle: Bundle, rows: list[dict], cts_stats: dict, consumer_stats: dict, problems: list[Problem]) -> dict:
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
            "pinned_tag": manifest.get("tag"),
            "pinned_commit": manifest.get("commit"),
            "group_files": manifest.get("totals", {}).get("group_files"),
            "cases_in_listing": manifest.get("totals", {}).get("cases"),
            "listing_kind": manifest.get("listing_kind"),
            "unmapped_requirement_ids": sorted(unmapped_rows),
        },
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
