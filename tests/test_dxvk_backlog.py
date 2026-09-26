import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_tool():
    path = ROOT / "tools/check_dxvk_backlog.py"
    spec = importlib.util.spec_from_file_location("check_dxvk_backlog", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


backlog = load_tool()


class DxvkBacklogTests(unittest.TestCase):
    def setUp(self):
        self.document = json.loads(backlog.BACKLOG.read_text())
        self.matrix = json.loads(backlog.MATRIX.read_text())

    def test_original_61_blockers_keep_membership_after_multiview_promotion(self):
        # The counts move with each promotion: T03's three draw features, the
        # clip/cull pair, T06's fragment stores/atomics and dual-source blend, and
        # the four DXVK262-T05 rasterization and viewport features are
        # implementation-ready now, and the backlog keeps the original membership
        # (61 requirements) it started with.
        summary = backlog.validate(self.document, self.matrix)
        self.assertEqual(15, summary["tranches"])
        self.assertEqual(61, summary["requirements"])
        # DXVK262-T06 independentBlend (2026-09-22) and then sampleRateShading
        # (2026-09-23) were promoted, followed by T07's four resource/query
        # rows on 2026-09-24.
        # The Vulkan 1.0 KHR DeviceScope route has ordinary SDK execution and
        # public-query evidence; original message-passing CTS is ineligible.

        # T09 host reset, mirror clamp, timeline and separate depth/stencil
        # routes are public. Imageless has native evidence but remains blocked.
        # T11 demote and terminate are public through their EXT/KHR routes.
        # sync2 and imageless (KHR routes), T14 VK_EXT_transform_feedback and
        # VK_EXT_robustness2 (extension, robustBufferAccess2, nullDescriptor)
        # are public.
        self.assertEqual(39, summary["implementation_ready"])
        self.assertEqual(39, summary["profile_satisfied"])
        self.assertEqual(22, summary["remaining_profile_blockers"])

        self.assertEqual({
            "api-version": 1,
            "extension": 2,
            "feature": 48,
            "property": 10,
        }, summary["kinds"])

    def test_missing_duplicate_and_satisfied_rows_fail_closed(self):
        for mutation, message in (
            ("missing", "unassigned baseline requirements"),
            ("duplicate", "assigned more than once"),
            ("satisfied", "baseline-excluded"),
        ):
            with self.subTest(mutation=mutation):
                broken = copy.deepcopy(self.document)
                if mutation == "missing":
                    broken["tranches"][0]["requirements"].clear()
                elif mutation == "duplicate":
                    broken["tranches"][1]["requirements"].append(
                        broken["tranches"][0]["requirements"][0])
                else:
                    broken["tranches"][0]["requirements"] = [
                        "feature:VkPhysicalDeviceFeatures:robustBufferAccess"]
                with self.assertRaisesRegex(ValueError, message):
                    backlog.validate(broken, self.matrix)

    def test_implementation_readiness_is_distinct_from_profile_satisfaction(self):
        baseline = backlog.validate(self.document, self.matrix)
        promoted = copy.deepcopy(self.matrix)
        identifier = self.document["tranches"][0]["requirements"][0]
        row = next(item for item in promoted["requirements"]
                   if item["id"] == identifier)
        row["implementation"]["state"] = "implemented"
        row["cts"]["state"] = "cts-pass"
        row["native"]["state"] = "native-evidence"
        summary = backlog.validate(self.document, promoted)
        self.assertEqual(baseline["implementation_ready"] + 1, summary["implementation_ready"])
        self.assertEqual(baseline["profile_satisfied"], summary["profile_satisfied"])
        self.assertEqual(baseline["remaining_profile_blockers"], summary["remaining_profile_blockers"])

        row["api"]["state"] = "satisfied"
        row["verdict"] = "satisfied"
        summary = backlog.validate(self.document, promoted)
        self.assertEqual(baseline["implementation_ready"] + 1, summary["implementation_ready"])
        self.assertEqual(baseline["profile_satisfied"] + 1, summary["profile_satisfied"])
        self.assertEqual(baseline["remaining_profile_blockers"] - 1, summary["remaining_profile_blockers"])

    def test_initially_satisfied_requirement_may_not_regress(self):
        regressed = copy.deepcopy(self.matrix)
        row = next(item for item in regressed["requirements"]
                   if item["id"] ==
                   "feature:VkPhysicalDeviceFeatures:robustBufferAccess")
        row["verdict"] = "blocker"
        with self.assertRaisesRegex(ValueError, "regressed"):
            backlog.validate(self.document, regressed)

    def test_dependencies_must_exist_and_point_backward(self):
        for dependency, message in (
            ("DXVK262-UNKNOWN", "unknown dependency"),
            ("DXVK262-T02", "is not earlier"),
        ):
            with self.subTest(dependency=dependency):
                broken = copy.deepcopy(self.document)
                broken["tranches"][0]["depends_on"] = [dependency]
                with self.assertRaisesRegex(ValueError, message):
                    backlog.validate(broken, self.matrix)

    def test_api_version_is_an_all_dependencies_final_gate(self):
        broken = copy.deepcopy(self.document)
        broken["tranches"][-1]["depends_on"].pop()
        with self.assertRaisesRegex(ValueError, "depend on every"):
            backlog.validate(broken, self.matrix)

        broken = copy.deepcopy(self.document)
        broken["tranches"][-1]["requirements"] = [
            "feature:VkPhysicalDeviceVulkan13Features:dynamicRendering"]
        with self.assertRaisesRegex(ValueError, "final tranche must contain only"):
            backlog.validate(broken, self.matrix)


    def test_only_an_observed_cts_failure_blocks_readiness(self):
        baseline = backlog.validate(self.document, self.matrix)
        for state in ("cts-focused-pass", "mapped-not-run", "not-mapped"):
            with self.subTest(state=state):
                changed = copy.deepcopy(self.matrix)
                row = next(item for item in changed["requirements"]
                           if item["id"] == "feature:VkPhysicalDeviceFeatures:imageCubeArray")
                row["cts"]["state"] = state
                summary = backlog.validate(self.document, changed)
                self.assertEqual(baseline["implementation_ready"],
                                 summary["implementation_ready"])
        failed = copy.deepcopy(self.matrix)
        row = next(item for item in failed["requirements"]
                   if item["id"] == "feature:VkPhysicalDeviceFeatures:imageCubeArray")
        row["cts"]["state"] = "cts-fail"
        summary = backlog.validate(self.document, failed)
        self.assertEqual(baseline["implementation_ready"] - 1, summary["implementation_ready"])

    def test_gate_policy_must_match_the_matrix_cts_routes(self):
        drifted = copy.deepcopy(self.matrix)
        drifted["policy"]["cts_blocking_states"] = []
        with self.assertRaisesRegex(ValueError, "CTS blocking states"):
            backlog.validate(self.document, drifted)
        for gate in ("profile_completion_gate", "implementation_readiness_gate"):
            with self.subTest(gate=gate):
                broken = copy.deepcopy(self.document)
                broken["policy"][gate]["cts_blocking"] = []
                with self.assertRaisesRegex(ValueError, "gate drift"):
                    backlog.validate(broken, self.matrix)

    def test_api_promotion_no_longer_demands_whole_suite_cts(self):
        final = self.document["tranches"][-1]["acceptance"]
        self.assertIn("our DXVK v2.6.2 build creates its device", final)
        self.assertIn("mandatory is implemented", final)
        self.assertIn("any observed applicable failure blocks", final)
        self.assertIn("whole-suite CTS and official conformance are separate", final)

if __name__ == "__main__":
    unittest.main()
