#!/usr/bin/env python3
"""Tests for the conformance inventory validator.

Run from the worktree root (the second -t is required because this directory is
not an importable package):

    python3 -m unittest discover -s conformance_inventory/tests -t conformance_inventory/tests

Most tests build small synthetic bundles so that every failure mode is exercised
without touching the real data. The suite covers two kinds of correctness:

* structural correctness of the documents, and
* accuracy of the interesting fields, by cross-checking classifications against
  the pinned target profile, baseline claims against the baseline surface
  inventory, and CTS mappings against the pinned listing.

The integration tests validate the checked-in inventory and are skipped only
when the optional caches (anchor index, CTS listing cache) have not been
generated.
"""

from __future__ import annotations

import copy
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INVENTORY_DIR = os.path.dirname(HERE)
if INVENTORY_DIR not in sys.path:
    sys.path.insert(0, INVENTORY_DIR)

import validate  # noqa: E402  (path is prepared above)

TARGET_SHA = "a" * 64


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
                    "kinds": ["specification", "registry", "conformance-test-suite", "profile"],
                    "field": "commit",
                },
            },
            {
                "id": "profile-and-registry-same-pin",
                "description": "profile file lives in the registry repo",
                "check": {
                    "op": "field_equals_field",
                    "left": "khronos-vulkan-roadmap-profiles.commit",
                    "right": "khronos-vulkan-registry.commit",
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
                    {"path": "chapters/versions.adoc", "kind": "file", "sha256": "b" * 64, "git_blob_sha1": "c" * 40}
                ],
                "document_alias": {"url": "https://example.invalid/vkspec.html", "size_bytes": 1, "sha256": "b" * 64},
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
                "artifacts": [
                    {"path": "registry/vk.xml", "kind": "file", "sha256": "c" * 64, "git_blob_sha1": "d" * 40}
                ],
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
            {
                "id": "khronos-vulkan-roadmap-profiles",
                "kind": "profile",
                "role": "target-definition",
                "title": "roadmap profiles",
                "publisher": "Khronos",
                "repo": "https://example.invalid/headers.git",
                "revision_kind": "annotated-tag",
                "tag": "v1.4.0",
                "commit": "d" * 40,
                "license": {"spdx": "Apache-2.0"},
                "retrieved_date": "2026-01-01",
                "artifacts": [
                    {
                        "path": "registry/profiles/VP_KHR_roadmap.json",
                        "kind": "file",
                        "sha256": TARGET_SHA,
                        "git_blob_sha1": "f" * 40,
                    }
                ],
            },
        ],
    }


def base_core_target(**overrides) -> dict:
    target = {
        "schema_version": "1.0",
        "target": {
            "id": "example-1.4-graphics-core",
            "statement": "example target",
            "definition": "cumulative core obligations",
            "basis": {
                "feature_requirements": {
                    "source_id": "khronos-vulkan-spec",
                    "anchor": "features-requirements",
                    "spec_version": "1.4.0",
                    "document_sha256": "b" * 64,
                },
                "api_surface": {
                    "source_id": "khronos-vulkan-registry",
                    "path": "registry/vk.xml",
                    "sha256": "c" * 64,
                },
            },
        },
        "mandatory_feature_bits": {
            "registry_by_version": {"1.2": ["timelineSemaphore", "storageBuffer8BitAccess"]},
            "cumulative": ["timelineSemaphore", "storageBuffer8BitAccess"],
            "specification_by_version": {},
            "specification_registry_mismatches": [],
            "conditional_in_version_blocks": [{"features": ["storageBuffer8BitAccess"], "condition": "uniformAndStorageBuffer8BitAccess is supported", "version": "1.2"}],
            "conditional_on_optional_extension": [{"features": ["nullDescriptor"], "condition": "VK_KHR_robustness2 is supported", "version": None}],
            "at_least_one_groups": [{"features": ["hostImageCopy"], "text": "either host image copy or an extra transfer queue"}],
            "extension_rules_not_core_classification": [],
        },
        "api_surface": {"surface_by_profile": {"graphics_including_base": {"commands_total": 2, "types_total": 3}}},
        "roadmap_comparison": {"file": "roadmap_comparison.json", "note": "comparison only"},
    }
    target.update(overrides)
    return target


def base_roadmap(**overrides) -> dict:
    roadmap = {
        "schema_version": "1.0",
        "comparison": {
            "id": "vp-khr-roadmap-comparison",
            "status": "comparison-only",
            "statement": "recorded for comparison",
            "basis": {"source_id": "khronos-vulkan-roadmap-profiles", "profile": "VP_KHR_roadmap_2026", "sha256": TARGET_SHA},
        },
        "required_feature_bits": ["timelineSemaphore"],
        "required_extensions": ["VK_KHR_surface"],
    }
    roadmap.update(overrides)
    return roadmap


def base_surface() -> dict:
    return {
        "schema_version": "1.0",
        "baseline_commit": "3" * 40,
        "method": "parsed",
        "counts": {"entry_points": 2, "dispatched": 2, "public_header": 1, "implementation_only": 1},
        "entry_points": [
            {"name": "vkCreateInstance", "files": ["src/vk_device.c"], "dispatch_scope": "GLOBAL", "public_header": True, "observed": None},
            {
                "name": "vkEnumerateDeviceExtensionProperties",
                "files": ["src/vk_device.c"],
                "dispatch_scope": "INSTANCE",
                "public_header": False,
                "observed": "returns an empty list",
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
        "appendices": [{"path": "appendices/versions.adoc", "review_state": "reviewed", "rows": []}],
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
        "applicability": {"core": "core-mandatory", "target": "required", "note": "n"},
        "classification": "mandatory",
        "classification_basis": "core-cumulative-required",
        "condition": None,
        "coverage_chapter": "chapters/initialization.adoc",
        "source": {"source_id": "khronos-vulkan-spec", "anchor": "initialization", "note": "n"},
        "features": ["timelineSemaphore"],
        "limits": [],
        "formats": [],
        "commands": ["vkCreateInstance"],
        "extensions": [],
        "cts": {
            "mapping": "mapped",
            "basis": "static-source-listing",
            "groups": ["vk-default/api.txt"],
            "cases": ["dEQP-VK.api.version_check.entry_points"],
            "coverage_quality": "representative-case",
            "coverage_note": "Single representative case; not complete coverage.",
            "note": "n",
        },
        "implementation_state": "not-audited",
        "baseline": {
            "surface_state": "symbol-present",
            "entry_points": ["vkCreateInstance"],
            "absent_entry_points": [],
            "refs": ["src/vk_device.c"],
            "note": "Baseline surface: 1 of 1 named entry points exist and are dispatched (vkCreateInstance); 1 are declared in the public header.",
        },
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


def base_conditional_row(**overrides) -> dict:
    row = base_row(
        id="VK14-CORE-006",
        category="shaders",
        summary="Core feature obligation conditioned on a supporting capability.",
        features=["storageBuffer8BitAccess"],
        applicability={"core": "core-feature-gated", "target": "conditional", "target_condition": "uniformAndStorageBuffer8BitAccess is supported", "note": "n"},
        classification="conditional",
        classification_basis="core-conditional",
        condition="uniformAndStorageBuffer8BitAccess is supported",
    )
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
        "category_order": [["instance_device"], ["shaders"], ["extensions", "platform_wsi"]],
        "core_introduction_versions": ["1.0", "1.1", "1.2", "1.3", "1.4"],
        "classification_vocabulary": ["mandatory", "conditional", "optional"],
        "applicability_vocabulary": ["core-mandatory", "core-feature-gated", "extension-optional", "outside-core"],
        "classification_basis_vocabulary": ["core-cumulative-required", "core-conditional", "not-required-by-core", "consumer-required", "project-conditional"],
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
        "target": {
            "id": "example-1.4-graphics-core",
            "statement": "example target",
            "definition": "cumulative core obligations",
            "target_file": "core_target.json",
            "comparison_file": "roadmap_comparison.json",
            "specification_anchor": "features-requirements",
        },
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
            {"group": "vk-default/api.txt", "path": "external/vulkancts/mustpass/main/vk-default/api.txt", "git_blob_sha1": "f" * 40}
        ],
        "totals": {"group_files": 1, "cases": 1, "missing_group_files": []},
    }


def bundle(**overrides) -> validate.Bundle:
    defaults = {
        "sources": base_sources(),
        "requirements": base_requirements([base_row(), base_conditional_row()]),
        "coverage": base_coverage(["VK14-INSTANCE-001", "VK14-CORE-006"]),
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
        "target": base_core_target(),
        "roadmap": base_roadmap(),
        "surface": base_surface(),
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
        self.assertEqual(report["documents"]["requirements"], 2)

    # ---- structural ------------------------------------------------------

    def test_duplicate_ids_are_rejected(self):
        problems, _ = self.run_bundle(requirements=base_requirements([base_row(), base_row()]))
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
        row = base_row(classification="conditional", condition=None, applicability={"core": "outside-core", "target": "conditional", "target_condition": "only for packaged ICDs"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R007", errors(problems))

    def test_unknown_version_value(self):
        row = base_row(core_introduction="1.5")
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R004", errors(problems))

    def test_unknown_state_values(self):
        problems, _ = self.run_bundle(requirements=base_requirements([base_row(implementation_state="passed")]))
        self.assertIn("R009", errors(problems))
        row = base_row()
        row["evidence"]["runtime"] = {"state": "definitely-passed", "refs": []}
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R008", errors(problems))

    def test_broken_cts_mapping(self):
        row = base_row(
            cts={"mapping": "mapped", "basis": "static-source-listing", "groups": [], "cases": [], "coverage_quality": "not-mapped", "coverage_note": "n", "note": "n"}
        )
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R013", errors(problems))

    def test_unmapped_mapping_requires_a_note(self):
        row = base_row(
            cts={"mapping": "unmapped", "basis": "static-source-listing", "groups": [], "cases": [], "coverage_quality": "not-mapped", "coverage_note": "", "note": ""}
        )
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R013", errors(problems))

    def test_not_applicable_mapping_must_be_empty(self):
        row = base_row(
            cts={"mapping": "not-applicable", "basis": "static-source-listing", "groups": ["vk-default/api.txt"], "cases": [], "coverage_quality": "not-mapped", "coverage_note": "", "note": "n"}
        )
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R017", errors(problems))

    def test_unknown_cts_group_against_manifest(self):
        row = base_row(
            cts={"mapping": "mapped", "basis": "static-source-listing", "groups": ["vk-default/nope.txt"], "cases": [], "coverage_quality": "family-level", "coverage_note": "n", "note": "n"}
        )
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R014", errors(problems))

    def test_deterministic_ordering(self):
        later = base_row(id="VK14-EXTENSIONS-001", category="extensions")
        earlier = base_row(id="VK14-INSTANCE-001", category="instance_device")
        coverage = base_coverage(["VK14-INSTANCE-001", "VK14-EXTENSIONS-001"])
        problems, _ = self.run_bundle(requirements=base_requirements([later, earlier]), coverage=coverage)
        self.assertIn("R015", errors(problems))
        problems, _ = self.run_bundle(requirements=base_requirements([earlier, later]), coverage=coverage)
        self.assertNotIn("R015", errors(problems))

    def test_coverage_ledger_disagreement(self):
        problems, _ = self.run_bundle(coverage=base_coverage([]))
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

    def test_anchor_check_fires_with_index(self):
        problems, _ = self.run_bundle(anchor_index={"initialization", "versions"})
        self.assertEqual(errors(problems), [])
        row = base_row(source={"source_id": "khronos-vulkan-spec", "anchor": "not-a-real-anchor"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]), anchor_index={"initialization"})
        self.assertIn("A001", errors(problems))

    def test_consumer_requirement_join_is_checked(self):
        consumers = copy.deepcopy(bundle().consumers)
        consumers["consumers"][0]["requirements"][0]["related_requirement_ids"] = ["VK14-NOPE-999"]
        problems, _ = self.run_bundle(consumers=consumers)
        self.assertIn("N003", errors(problems))

    # ---- classification accuracy against the cumulative core sets --------
    # These are the accuracy checks that structural validation cannot provide.

    def test_core_capability_may_not_be_optional(self):
        """The defect found in review: a core requirement classified optional."""
        row = base_row(
            classification="optional",
            classification_basis="not-required-by-core",
            applicability={"core": "core-feature-gated", "target": "not-required", "target_condition": None, "note": "n"},
        )
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("T005", errors(problems))

    def test_core_mandatory_capability_must_be_mandatory(self):
        row = base_row(classification="optional", applicability={"core": "core-mandatory", "target": "not-required", "note": "n"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("T002", errors(problems))

    def test_core_conditional_capability_must_be_conditional(self):
        row = base_row(features=["storageBuffer8BitAccess"], applicability={"core": "core-mandatory", "target": "required", "note": "n"})
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("T003", errors(problems))

    def test_conditional_row_needs_an_exact_condition(self):
        row = base_row(
            classification="conditional",
            classification_basis="core-conditional",
            condition="",
            applicability={"core": "core-feature-gated", "target": "conditional", "target_condition": "", "note": "n"},
        )
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("T004", errors(problems))

    def test_every_core_capability_must_be_referenced(self):
        target = base_core_target()
        target["mandatory_feature_bits"]["cumulative"] = ["robustBufferAccess", "timelineSemaphore", "multiview", "bufferDeviceAddress"]
        problems, _ = self.run_bundle(target=target)
        self.assertIn("T007", errors(problems))

    def test_every_core_conditional_capability_must_be_referenced(self):
        target = base_core_target()
        target["mandatory_feature_bits"]["conditional_in_version_blocks"] = [
            {"features": ["shaderInt64"], "condition": "shaderSharedInt64Atomics is supported", "version": "1.2"}
        ]
        problems, _ = self.run_bundle(target=target)
        self.assertIn("T008", errors(problems))

    def test_classification_may_not_come_from_a_roadmap_profile(self):
        row = base_row(classification_basis="roadmap-profile-required")
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("R020", errors(problems))

    def test_roadmap_file_must_be_marked_comparison_only(self):
        roadmap = base_roadmap()
        roadmap["comparison"]["status"] = "classification-basis"
        problems, _ = self.run_bundle(roadmap=roadmap)
        self.assertIn("T010", errors(problems))

    def test_core_target_pin_must_match_sources(self):
        target = base_core_target()
        target["target"]["basis"]["feature_requirements"]["document_sha256"] = "9" * 64
        problems, _ = self.run_bundle(target=target)
        self.assertIn("T001", errors(problems))

    # ---- baseline surface accuracy ---------------------------------------

    def test_baseline_claim_about_unknown_symbol(self):
        row = base_row()
        row["baseline"]["entry_points"] = ["vkNotInSurface"]
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("B001", errors(problems))

    def test_baseline_claim_that_an_existing_symbol_is_absent(self):
        row = base_row()
        row["baseline"]["entry_points"] = ["vkCreateInstance"]
        row["baseline"]["absent_entry_points"] = ["vkEnumerateDeviceExtensionProperties"]
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("B001", errors(problems))

    def test_baseline_none_with_entry_points(self):
        row = base_row()
        row["baseline"]["surface_state"] = "none"
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("B002", errors(problems))

    def test_implementation_only_symbols_must_be_labelled(self):
        row = base_row(commands=["vkEnumerateDeviceExtensionProperties"])
        row["baseline"] = {
            "surface_state": "symbol-present",
            "entry_points": ["vkEnumerateDeviceExtensionProperties"],
            "absent_entry_points": [],
            "refs": ["src/vk_device.c"],
            "note": "Baseline surface: 1 of 1 named entry points exist and are dispatched.",
        }
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("B004", errors(problems))

    # ---- CTS coverage quality --------------------------------------------

    def test_cts_coverage_quality_is_required(self):
        row = base_row()
        del row["cts"]["coverage_quality"]
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("C010", errors(problems))

    def test_non_direct_mapping_requires_a_gap_note(self):
        row = base_row()
        row["cts"]["coverage_note"] = ""
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("C011", errors(problems))

    def test_direct_quality_requires_a_named_case(self):
        row = base_row()
        row["cts"]["coverage_quality"] = "direct"
        row["cts"]["cases"] = []
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("C013", errors(problems))

    def test_mapped_row_may_not_use_not_mapped_quality(self):
        row = base_row()
        row["cts"]["coverage_quality"] = "not-mapped"
        problems, _ = self.run_bundle(requirements=base_requirements([row]))
        self.assertIn("C012", errors(problems))

    # ---- result vocabulary ------------------------------------------------

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
        coverage = base_coverage([row["id"] for row in rows])
        _, report = self.run_bundle(requirements=base_requirements(rows), coverage=coverage)
        states = report["requirements"]["by_implementation_state"]
        self.assertEqual(states.get("cts-pass"), 1)
        self.assertNotIn("success_rate", report)
        pass_like = sum(states.get(state, 0) for state in ("cts-pass", "native-evidence", "host-only-evidence"))
        self.assertEqual(pass_like, 1)


class CheckedInInventoryTests(unittest.TestCase):
    def strict_bundle(self):
        bundle = validate.Bundle.load(INVENTORY_DIR)
        cache = os.path.join(INVENTORY_DIR, ".cache", "cts")
        anchors = os.path.join(INVENTORY_DIR, ".cache", "spec_anchors.txt")
        if os.path.isdir(cache):
            bundle.cts_cache_dir = cache
        if os.path.exists(anchors):
            with open(anchors, "r", encoding="utf-8") as handle:
                bundle.anchor_index = {line.strip() for line in handle if line.strip() and not line.startswith("#")}
        return bundle

    def test_checked_in_inventory_has_no_errors(self):
        problems, report = validate.validate(validate.Bundle.load(INVENTORY_DIR))
        self.assertEqual(errors(problems), [], "\n".join(p.render() for p in problems))
        self.assertGreater(report["documents"]["requirements"], 50)
        self.assertNotIn("percent", report["unknowns"])

    def test_core_target_and_baseline_surface_are_consistent(self):
        problems, report = validate.validate(validate.Bundle.load(INVENTORY_DIR))
        self.assertEqual(errors(problems), [], "\n".join(p.render() for p in problems))
        bits = report["target"]["core_mandatory_feature_bits"]
        conditional = report["target"]["core_conditional_feature_bits"]
        self.assertGreater(bits, 0)
        self.assertGreater(conditional, 0)
        self.assertEqual(report["target"]["roadmap_comparison"]["status"], "comparison-only")
        self.assertGreater(report["baseline_surface"]["entry_points"], 0)

    def test_no_optional_requirement_carries_a_core_capability(self):
        bundle = validate.Bundle.load(INVENTORY_DIR)
        bits = bundle.target["mandatory_feature_bits"]
        core_bits = set(bits["cumulative"])
        conditional = {f for entry in bits["conditional_in_version_blocks"] for f in entry["features"]}
        required = core_bits | conditional
        offenders = []
        for row in (bundle.requirements or {}).get("requirements", []):
            if row["classification"] == "optional":
                hits = set(row["features"]) & required
                if hits:
                    offenders.append((row["id"], sorted(hits)))
        self.assertEqual(offenders, [], "optional rows carry core capabilities: %r" % offenders)

    def test_the_four_reviewed_capabilities_are_mandatory_core(self):
        bundle = validate.Bundle.load(INVENTORY_DIR)
        mandatory = set(bundle.target["mandatory_feature_bits"]["cumulative"])
        conditional = {f for e in bundle.target["mandatory_feature_bits"]["conditional_in_version_blocks"] for f in e["features"]}
        rows = {r["id"]: r for r in bundle.requirements["requirements"]}
        for bit, row_id in (("timelineSemaphore", "VK14-SYNC-008"), ("synchronization2", "VK14-SYNC-009"),
                            ("dynamicRendering", "VK14-RENDERPASS-007"), ("bufferDeviceAddress", "VK14-RESOURCES-012")):
            self.assertIn(bit, mandatory)
            self.assertNotIn(bit, conditional)
            self.assertEqual(rows[row_id]["classification"], "mandatory", row_id)

    def test_resolved_core_surface_is_a_dependency_walk(self):
        """Regression: the compute surface must not be dropped by summing categories."""
        bundle = validate.Bundle.load(INVENTORY_DIR)
        resolved = bundle.target["api_surface"]["surface_by_profile"]["graphics_resolved"]
        roots = resolved["roots"]
        for family in ("VK_GRAPHICS_VERSION_1_0", "VK_COMPUTE_VERSION_1_0", "VK_BASE_VERSION_1_0"):
            self.assertIn(family, roots)
        for command in ("vkCmdDispatch", "vkCreateComputePipelines", "vkCmdDrawIndexed", "vkCreateImage"):
            self.assertIn(command, resolved["commands"])
        self.assertGreater(len(resolved["commands"]), 200)

    def test_any_of_obligations_are_not_expanded(self):
        """Regression: 'at least one of A, B or C' must stay one disjunctive obligation."""
        bundle = validate.Bundle.load(INVENTORY_DIR)
        entries = bundle.target["mandatory_feature_bits"]["conditional_on_optional_extension"]
        any_of = [entry for entry in entries if entry.get("requirement", {}).get("kind") == "any-of"]
        self.assertGreater(len(any_of), 0)
        for entry in any_of:
            self.assertGreater(len(entry["features"]), 1, entry)
            self.assertTrue(entry.get("at_least_one"))
        by_trigger = {}
        for entry in entries:
            key = (json.dumps(entry.get("trigger"), sort_keys=True), entry.get("condition"))
            by_trigger.setdefault(key, []).append(entry.get("requirement", {}).get("kind"))
        for key, kinds in by_trigger.items():
            if "any-of" in kinds:
                self.assertEqual(kinds.count("any-of"), 1, "any-of group duplicated for %r" % (key,))

    def test_limits_table_separates_core_from_roadmap(self):
        bundle = validate.Bundle.load(INVENTORY_DIR)
        rows = {row["limit"]: row for row in bundle.target["limits"]["rows"]}
        self.assertEqual(rows["maxImageDimension1D"]["required_for_core_1_4"], 8192)
        self.assertEqual(rows["maxPushConstantsSize"]["required_for_core_1_4"], 256)
        self.assertEqual(rows["bufferImageGranularity"]["required_for_core_1_4"], 4096)
        self.assertGreater(len(bundle.target["limits"]["raised_in_1_4"]), 20)

    def test_format_tables_are_machine_readable(self):
        bundle = validate.Bundle.load(INVENTORY_DIR)
        tables = bundle.target["formats"]["tables"]
        self.assertGreaterEqual(len(tables), 8)
        rows = [row for table in tables for row in table["rows"]]
        self.assertGreater(len(rows), 100)
        self.assertTrue(all("required_feature_bits" in row for row in rows))

    def test_wsi_is_not_a_core_requirement(self):
        bundle = validate.Bundle.load(INVENTORY_DIR)
        rows = {r["id"]: r for r in bundle.requirements["requirements"]}
        wsi = rows["VK14-EXTENSIONS-002"]
        self.assertEqual(wsi["classification"], "optional")
        self.assertEqual(wsi["classification_basis"], "not-required-by-core")

    def test_mapped_cases_and_anchors_exist_in_pins(self):
        bundle = self.strict_bundle()
        if bundle.cts_cache_dir is None:
            self.skipTest("CTS listing cache not generated; run tools/collect_cts_listing.py")
        if bundle.anchor_index is None:
            self.skipTest("anchor index not generated; run tools/extract_spec_anchors.py")
        problems, _ = validate.validate(bundle)
        self.assertEqual(errors(problems), [], "\n".join(p.render() for p in problems))


if __name__ == "__main__":
    unittest.main()
