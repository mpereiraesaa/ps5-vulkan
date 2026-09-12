#!/usr/bin/env python3
"""Tests for the conformance inventory validator.

Run from the worktree root (the second -t is required because this directory is
not an importable package):

    python3 -m unittest discover -s conformance_inventory/tests -t conformance_inventory/tests

Most tests build small synthetic bundles so that every failure mode is exercised
without touching the real data. Three integration tests validate the checked-in
inventory and are skipped when the optional pins (anchor index, CTS listing
cache) have not been generated.
"""

from __future__ import annotations

import copy
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INVENTORY_DIR = os.path.dirname(HERE)
if INVENTORY_DIR not in sys.path:
    sys.path.insert(0, INVENTORY_DIR)

import validate  # noqa: E402  (path is prepared above)


def base_sources() -> dict:
    return {
        "schema_version": "1.0",
        "retrieved_date": "2026-01-01",
        "compatibility_rules": [
            {
                "id": "spec-registry-release-train",
                "description": "same release train",
                "check": {
                    "op": "field_equals_field",
                    "left": "khronos-vulkan-spec.spec_version",
                    "right": "khronos-vulkan-registry.header_version",
                },
            },
            {
                "id": "commit-pinned",
                "description": "commits pinned",
                "check": {
                    "op": "all_of_kind_have_field",
                    "kinds": ["specification", "registry", "conformance-test-suite"],
                    "field": "commit",
                },
            },
        ],
        "sources": [
            {
                "id": "khronos-vulkan-spec",
                "kind": "specification",
                "role": "normative",
                "title": "spec",
                "publisher": "Khronos",
                "repo": "https://example.invalid/spec.git",
                "revision_kind": "annotated-tag",
                "tag": "v1.4.0",
                "commit": "a" * 40,
                "spec_version": "1.4.0",
                "license": {"spdx": "CC-BY-4.0"},
                "retrieved_date": "2026-01-01",
                "artifacts": [
                    {
                        "path": "chapters/versions.adoc",
                        "kind": "file",
                        "sha256": "b" * 64,
                        "git_blob_sha1": "c" * 40,
                    }
                ],
            },
            {
                "id": "khronos-vulkan-registry",
                "kind": "registry",
                "role": "normative",
                "title": "registry",
                "publisher": "Khronos",
                "repo": "https://example.invalid/headers.git",
                "revision_kind": "annotated-tag",
                "tag": "v1.4.0",
                "commit": "d" * 40,
                "header_version": "1.4.0",
                "license": {"spdx": "Apache-2.0"},
                "retrieved_date": "2026-01-01",
            },
            {
                "id": "khronos-vk-gl-cts",
                "kind": "conformance-test-suite",
                "role": "test-reference",
                "title": "cts",
                "publisher": "Khronos",
                "repo": "https://example.invalid/cts.git",
                "revision_kind": "annotated-tag",
                "tag": "vulkan-cts-1.4.0.0",
                "commit": "e" * 40,
                "license": {"spdx": "Apache-2.0"},
                "retrieved_date": "2026-01-01",
            },
        ],
    }


def base_coverage(rows) -> dict:
    return {
        "schema_version": "1.0",
        "source_id": "khronos-vulkan-spec",
        "core_chapters": [
            {"path": "chapters/initialization.adoc", "review_state": "reviewed", "rows": rows},
            {"path": "chapters/introduction.adoc", "review_state": "not-reviewed", "rows": []},
        ],
        "appendices": [
            {"path": "appendices/versions.adoc", "review_state": "reviewed", "rows": []},
        ],
        "extension_appendices": {
            "path_prefix": "appendices/",
            "total_extension_files": 483,
            "review_state": "not-reviewed",
            "reviewed_exceptions": [],
        },
    }


def base_row(**overrides) -> dict:
    row = {
        "id": "VK14-INSTANCE-001",
        "category": "instance_device",
        "summary": "Create an instance for the reported API version.",
        "core_introduction": "1.0",
        "applicability": {"vulkan14": "mandatory", "note": "inherited"},
        "classification": "mandatory",
        "condition": None,
        "coverage_chapter": "chapters/initialization.adoc",
        "source": {"source_id": "khronos-vulkan-spec", "anchor": "initialization", "note": "n"},
        "features": [],
        "limits": [],
        "formats": [],
        "commands": ["vkCreateInstance"],
        "extensions": [],
        "cts": {
            "mapping": "mapped",
            "basis": "static-source-listing",
            "groups": ["vk-default/api.txt"],
            "cases": ["dEQP-VK.api.version_check.entry_points"],
            "note": "n",
        },
        "implementation_state": "not-audited",
        "baseline": {"surface_state": "symbol-present", "refs": ["include/ps5vk/ps5vk.h"], "note": "n"},
        "evidence": {
            "source": {"state": "not-audited", "refs": []},
            "runtime": {"state": "not-run", "refs": []},
            "conformance": {"state": "not-established", "refs": []},
        },
        "owner": "implementation",
        "provenance": "manual-review",
        "manual_review": True,
        "review_notes": None,
    }
    row.update(overrides)
    return row


def base_requirements(rows) -> dict:
    return {
        "schema_version": "1.0",
        "state_vocabulary": [
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
        ],
        "category_order": [["instance_device"], ["extensions", "platform_wsi"]],
        "core_introduction_versions": ["1.0", "1.1", "1.2", "1.3", "1.4"],
        "classification_vocabulary": ["mandatory", "conditional", "optional"],
        "applicability_vocabulary": ["mandatory", "conditional", "optional-feature-gated", "allowed-not-supported"],
        "cts_mapping_vocabulary": ["mapped", "unmapped", "not-applicable"],
        "cts_mapping_basis": ["static-source-listing"],
        "evidence_state_vocabulary": {
            "source": ["not-audited", "missing", "implemented-unvalidated", "host-only-evidence", "native-evidence", "allowed-not-supported", "not-run"],
            "runtime": ["not-audited", "not-run", "host-only-evidence", "native-evidence", "cts-pass", "cts-fail", "harness-blocked"],
            "conformance": ["not-established", "not-sought"],
        },
        "pass_like_states": ["cts-pass", "native-evidence", "host-only-evidence"],
        "non_pass_states_never_counted_as_pass": [
            "not-audited",
            "not-run",
            "harness-blocked",
            "missing",
            "implemented-unvalidated",
            "allowed-not-supported",
            "cts-fail",
        ],
        "requirements": rows,
    }


def base_manifest() -> dict:
    return {
        "manifest_version": "1.0",
        "source_id": "khronos-vk-gl-cts",
        "listing_kind": "static-source-listing",
        "tag": "vulkan-cts-1.4.0.0",
        "commit": "e" * 40,
        "groups": [
            {"group": "vk-default/api.txt", "path": "external/vulkancts/mustpass/main/vk-default/api.txt", "git_blob_sha1": "f" * 40},
        ],
        "totals": {"group_files": 1, "cases": 1, "missing_group_files": []},
    }


def bundle(**overrides) -> validate.Bundle:
    defaults = {
        "sources": base_sources(),
        "requirements": base_requirements([base_row()]),
        "coverage": base_coverage(["VK14-INSTANCE-001"]),
        "consumers": {
            "schema_version": "1.0",
            "join_notes": ["n"],
            "consumers": [
                {
                    "id": "demo",
                    "title": "demo",
                    "source_id": "khronos-vk-gl-cts",
                    "revision": {},
                    "evidence_basis": "static",
                    "requirements": [
                        {
                            "id": "DEMO-001",
                            "classification": "mandatory",
                            "summary": "s",
                            "evidence": "e",
                            "references": ["https://example.invalid/x"],
                            "related_requirement_ids": ["VK14-INSTANCE-001"],
                        }
                    ],
                    "limits": ["l"],
                }
            ],
        },
        "manifest": base_manifest(),
    }
    defaults.update(overrides)
    return validate.Bundle(**defaults)


def errors(problems) -> list[str]:
    return [p.code for p in problems if p.level == "error"]


class ValidatorTests(unittest.TestCase):
    def run_bundle(self, **overrides):
        problems, report = validate.validate(bundle(**overrides))
        return problems, report

    def test_valid_bundle_has_no_errors(self):
        problems, report = self.run_bundle()
        self.assertEqual(errors(problems), [], "\n".join(p.render() for p in problems))
        self.assertEqual(report["documents"]["requirements"], 1)

    def test_duplicate_ids_are_rejected(self):
        rows = [base_row(), base_row()]
        problems, _ = self.run_bundle(
            requirements=base_requirements(rows),
            coverage={
                **base_coverage(["VK14-INSTANCE-001"]),
                "core_chapters": [
                    {"path": "chapters/initialization.adoc", "review_state": "reviewed", "rows": ["VK14-INSTANCE-001"]},
                ],
            },
        )
        self.assertIn("R002", errors(problems))

    def test_malformed_row_missing_required_field(self):
        row = base_row()
        del row["classification"]
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R001", errors(problems))

    def test_unresolved_source_reference(self):
        row = base_row(source={"source_id": "does-not-exist", "anchor": "initialization"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R010", errors(problems))

    def test_conditional_requirement_without_condition(self):
        row = base_row(classification="conditional", condition=None)
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R007", errors(problems))

    def test_unknown_version_value(self):
        row = base_row(core_introduction="1.5")
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R004", errors(problems))

    def test_unknown_applicability_value(self):
        row = base_row(applicability={"vulkan14": "definitely-required"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R005", errors(problems))

    def test_unknown_state_values(self):
        row = base_row(implementation_state="passed")
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R009", errors(problems))

        row = base_row()
        row["evidence"]["runtime"] = {"state": "definitely-passed", "refs": []}
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R008", errors(problems))

    def test_broken_cts_mapping(self):
        row = base_row(cts={"mapping": "mapped", "basis": "static-source-listing", "groups": [], "cases": [], "note": "n"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R013", errors(problems))

    def test_unmapped_mapping_requires_a_note(self):
        row = base_row(cts={"mapping": "unmapped", "basis": "static-source-listing", "groups": [], "cases": [], "note": ""})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R013", errors(problems))

    def test_not_applicable_mapping_must_be_empty(self):
        row = base_row(cts={"mapping": "not-applicable", "basis": "static-source-listing", "groups": ["vk-default/api.txt"], "cases": [], "note": "n"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R017", errors(problems))

    def test_unknown_cts_group_against_manifest(self):
        row = base_row(cts={"mapping": "mapped", "basis": "static-source-listing", "groups": ["vk-default/nope.txt"], "cases": [], "note": "n"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R014", errors(problems))

    def test_bad_case_name_shape(self):
        row = base_row(cts={"mapping": "mapped", "basis": "static-source-listing", "groups": ["vk-default/api.txt"], "cases": ["tests.something.pass"], "note": "n"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R014", errors(problems))

    def test_deterministic_ordering(self):
        later = base_row(id="VK14-EXTENSIONS-001", category="extensions")
        earlier = base_row(id="VK14-INSTANCE-001", category="instance_device")
        coverage = {
            "schema_version": "1.0",
            "source_id": "khronos-vulkan-spec",
            "core_chapters": [
                {"path": "chapters/initialization.adoc", "review_state": "reviewed", "rows": ["VK14-INSTANCE-001", "VK14-EXTENSIONS-001"]},
            ],
            "appendices": [],
            "extension_appendices": {"path_prefix": "appendices/", "total_extension_files": 0, "review_state": "not-reviewed", "reviewed_exceptions": []},
        }
        problems, _ = self.run_bundle(requirements=base_requirements([later, earlier]), coverage=coverage)
        self.assertIn("R015", errors(problems))
        problems, _ = self.run_bundle(requirements=base_requirements([earlier, later]), coverage=coverage)
        self.assertNotIn("R015", errors(problems))

    def test_coverage_ledger_disagreement(self):
        problems, _ = self.run_bundle(coverage=base_coverage([]))
        self.assertIn("V002", errors(problems))

    def test_coverage_names_unknown_requirement(self):
        coverage = base_coverage(["VK14-INSTANCE-001", "VK14-NOPE-999"])
        problems, _ = self.run_bundle(coverage=coverage)
        self.assertIn("V002", errors(problems))

    def test_incompatible_source_pins(self):
        sources = base_sources()
        sources["sources"][1]["header_version"] = "1.3.0"
        problems, _ = self.run_bundle(sources=sources)
        self.assertIn("S101", errors(problems))

    def test_moving_branch_source_is_rejected(self):
        sources = base_sources()
        sources["sources"][0]["revision_kind"] = "moving-branch"
        problems, _ = self.run_bundle(sources=sources)
        self.assertIn("S102", errors(problems))

    def test_artifact_without_hashes(self):
        sources = base_sources()
        sources["sources"][0]["artifacts"][0].pop("sha256")
        problems, _ = self.run_bundle(sources=sources)
        self.assertIn("S003", errors(problems))

    def test_cts_manifest_pin_mismatch(self):
        manifest = base_manifest()
        manifest["commit"] = "0" * 40
        problems, _ = self.run_bundle(manifest=manifest)
        self.assertIn("C001", errors(problems))

    def test_cts_manifest_listing_kind(self):
        manifest = base_manifest()
        manifest["listing_kind"] = "executable-generated"
        problems, _ = self.run_bundle(manifest=manifest)
        self.assertIn("C002", errors(problems))

    def test_anchor_check_fires_with_index(self):
        problems, _ = self.run_bundle(anchor_index={"initialization", "versions"})
        self.assertEqual(errors(problems), [])
        row = base_row(source={"source_id": "khronos-vulkan-spec", "anchor": "not-a-real-anchor"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]), anchor_index={"initialization"})
        self.assertIn("A001", errors(problems))

    def test_consumer_requirement_join_is_checked(self):
        consumers = bundle().consumers
        consumers = copy.deepcopy(consumers)
        consumers["consumers"][0]["requirements"][0]["related_requirement_ids"] = ["VK14-NOPE-999"]
        problems, _ = self.run_bundle(consumers=consumers)
        self.assertIn("N003", errors(problems))

    def test_report_never_counts_non_pass_states_as_pass(self):
        rows = [
            base_row(id="VK14-INSTANCE-001", implementation_state="not-audited"),
            base_row(id="VK14-INSTANCE-002", implementation_state="not-run"),
            base_row(id="VK14-INSTANCE-003", implementation_state="harness-blocked"),
            base_row(id="VK14-INSTANCE-004", implementation_state="cts-fail"),
            base_row(id="VK14-INSTANCE-005", implementation_state="missing"),
            base_row(id="VK14-INSTANCE-006", implementation_state="implemented-unvalidated"),
            base_row(id="VK14-INSTANCE-007", implementation_state="allowed-not-supported"),
            base_row(id="VK14-INSTANCE-008", implementation_state="cts-pass"),
        ]
        coverage = {
            "schema_version": "1.0",
            "source_id": "khronos-vulkan-spec",
            "core_chapters": [
                {
                    "path": "chapters/initialization.adoc",
                    "review_state": "reviewed",
                    "rows": [row["id"] for row in rows],
                }
            ],
            "appendices": [],
            "extension_appendices": {"path_prefix": "appendices/", "total_extension_files": 0, "review_state": "not-reviewed", "reviewed_exceptions": []},
        }
        _, report = self.run_bundle(requirements=base_requirements(rows), coverage=coverage)
        states = report["requirements"]["by_implementation_state"]
        self.assertEqual(states.get("cts-pass"), 1)
        for state in validate.NEVER_A_PASS:
            self.assertNotIn(state, report["cts"]["mapping_counts"])
        self.assertNotIn("success_rate", report)
        self.assertNotIn("percent", report["unknowns"])
        # The only pass-like state present is the single cts-pass row.
        pass_like = sum(states.get(state, 0) for state in ("cts-pass", "native-evidence", "host-only-evidence"))
        self.assertEqual(pass_like, 1)


class CheckedInInventoryTests(unittest.TestCase):
    def test_manifest_entries_are_fully_hashed(self):
        manifest = validate.Bundle.load(INVENTORY_DIR).manifest
        self.assertIsNotNone(manifest, "cts_manifest.json is missing")
        self.assertEqual(manifest["listing_kind"], "static-source-listing")
        self.assertGreater(manifest["totals"]["group_files"], 0)
        self.assertGreater(manifest["totals"]["cases"], 0)
        for entry in manifest["groups"]:
            self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(entry["git_blob_sha1"], r"^[0-9a-f]{40}$")
            self.assertGreater(entry["case_count"], 0)

    def test_checked_in_inventory_has_no_errors(self):
        problems, report = validate.validate(validate.Bundle.load(INVENTORY_DIR))
        self.assertEqual(errors(problems), [], "\n".join(p.render() for p in problems))
        self.assertGreater(report["documents"]["requirements"], 50)

    def test_mapped_cases_exist_in_pinned_listing(self):
        cache = os.path.join(INVENTORY_DIR, ".cache", "cts")
        anchors = os.path.join(INVENTORY_DIR, ".cache", "spec_anchors.txt")
        bundle = validate.Bundle.load(INVENTORY_DIR)
        if os.path.isdir(cache):
            bundle.cts_cache_dir = cache
        else:
            self.skipTest("CTS listing cache not generated; run tools/collect_cts_listing.py")
        if os.path.exists(anchors):
            with open(anchors, "r", encoding="utf-8") as handle:
                bundle.anchor_index = {line.strip() for line in handle if line.strip() and not line.startswith("#")}
        problems, _ = validate.validate(bundle)
        self.assertEqual(errors(problems), [], "\n".join(p.render() for p in problems))

    def test_no_anchor_index_is_reported_as_skipped_not_passed(self):
        bundle = validate.Bundle.load(INVENTORY_DIR)
        bundle.anchor_index = None
        problems, _ = validate.validate(bundle)
        self.assertNotIn("A001", errors(problems))


if __name__ == "__main__":
    unittest.main()
