import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "third_party/vk-gl-cts"
MODULE = "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderTests.cpp"
UTIL = "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderUtil.cpp"
MV_CONTRACT = "multiview-attachment-image"
# The legacy render-pass families the blocked contract covers. Their paths use
# the module's own family names under the index group.
MULTIVIEW_FAMILIES = (
    "dEQP-VK.multiview.clear_attachments.",
    "dEQP-VK.multiview.masks.",
    "dEQP-VK.multiview.index.vertex_shader.",
    "dEQP-VK.multiview.index.fragment_shader.",
)


def load_gate():
    spec = importlib.util.spec_from_file_location(
        "check_upstream_selection", ROOT / "tools/check_upstream_selection.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


class UpstreamSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gate = load_gate()
        cls.manifest = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        cls.current_manifest = copy.deepcopy(cls.manifest)
        # Explicit pre-execution fixture: retain the negative-stage regression
        # tests after the real hardware-backed selection has been promoted.
        leaves = [c for c in cls.manifest["cases"]
                  if c["path"].startswith(MULTIVIEW_FAMILIES)]
        cls.manifest["cases"] = [c for c in cls.manifest["cases"]
                                 if not c["path"].startswith(MULTIVIEW_FAMILIES)]
        for leaf in leaves:
            leaf["expected_status"] = "Fail"
        cls.manifest["diagnostics"].extend(leaves)
        contract = cls.manifest["resource_contracts"][MV_CONTRACT]
        contract["execution_requirements"]["gpu_subpass_readback"] = False
        contract["execution_supported"] = False
        contract["supported"] = False
        contract.pop("execution_evidence", None)
        contract["blocker"] = (
            "Pre-execution fixture: three of the four recorded requirements are now true: "
            "descriptor_object_model, descriptor_table_encoding, compiler_lowering. "
            "gpu_subpass_readback is the sole remaining requirement; 48 leaves stay diagnostics.")

    def test_canonical_selection_promotes_only_measured_multiview_leaves(self):
        manifest = self.current_manifest
        leaves = [c for c in manifest["cases"]
                  if c["path"].startswith(MULTIVIEW_FAMILIES)]
        # 211 + the 64 promoted clipping leaves + the 11 geometry leaves whose own
        # upstream oracles passed on hardware when the feature was advertised
        # (promotion run 20260917T190009Z: 286 acceptance leaves, 286 Pass, zero
        # Fail, title closed). The diagnostics are the earlier 14 plus the 49
        # geometry leaves this slice started from, of which 11 are now acceptance:
        # 30 are still refused by a gate this profile documents (a non-triangle
        # input primitive the policy does not carry, an undelivered built-in, or
        # an adjacency input) and 8 were MEASURED as Fail in the promotion run
        # (the geometry stage's uniform/sampled descriptor variants and the
        # varying crosses), so they stay diagnostics with expected_status Fail
        # instead of being claimed as coverage.
        geometry = [c for c in manifest["cases"]
                    if "geometryShader" in " ".join(c.get("features_required", []))]
        # 34 diagnostics plus the 28 rasterization culling leaves that are the
        # oracle for fillModeNonSolid (they run and pass with the feature
        # advertised and move to acceptance in the change that advertises it),
        # plus the 40 T05 entries of the complete eligibility pass: 25 applicable
        # leaves held as t05-measurement-pending until one console window
        # (2 clip_volume.depth_clamp triangles, 16 fragment_ops multi_viewport,
        # 6 draw.renderpass.scissor multi-scissor, 1 line_continuity amber) and
        # 15 same-family leaves that document a refusal or a capability gap,
        # The two T06 fragment side-effect leaves are now acceptance after the
        # exact native witness and the canonical 306/306 hardware run.
        # The 98 dual-source blend leaves of the one colour format this driver
        # creates raise the diagnostic count: they are the applicable oracle
        # for dualSrcBlend, held pending because the shipping blend profile
        # refuses every equation the family draws.
        self.assertEqual((306, 200, 48),
                         (len(manifest["cases"]), len(manifest["diagnostics"]), len(leaves)))
        pending = [d for d in manifest["diagnostics"]
                   if d["category"] == "t05-measurement-pending"]
        self.assertEqual(25, len(pending))
        self.assertTrue(all(d["expected_status"] == "Pass" for d in pending))
        self.assertEqual(
            {"dEQP-VK.clipping.clip_volume.depth_clamp",
             "dEQP-VK.fragment_ops.scissor.multi_viewport",
             "dEQP-VK.draw.renderpass.scissor",
             "dEQP-VK.rasterization.line_continuity"},
            {d["path"].rsplit(".", 1)[0] for d in pending})
        dual_source = [d for d in manifest["diagnostics"]
                       if d["category"] == "t06-dual-source-pending"]
        self.assertEqual(98, len(dual_source))
        self.assertTrue(all(d["expected_status"] == "Pass" for d in dual_source))
        self.assertEqual(
            {"dEQP-VK.pipeline.monolithic.blend.dual_source.format.r8g8b8a8_unorm.states"},
            {d["path"].rsplit(".", 1)[0] for d in dual_source})
        self.assertTrue(all(c["expected_status"] == "Pass" for c in leaves))
        self.assertEqual(29, len(geometry))
        self.assertEqual(29, len({c["path"] for c in geometry}))
        self.assertTrue(all(c["expected_status"] == "Pass" for c in geometry))
        # The four strip-topology leaves that were blocked on primitive restart
        # are acceptance now: the profile carries the state and programs the cut.
        self.assertFalse([d for d in manifest["diagnostics"]
                          if d.get("category") == "primitive-restart-unsupported"])
        self.assertEqual(0, self._gate_exit_code_for_manifest(manifest))
        broken = copy.deepcopy(manifest)
        contract = broken["resource_contracts"][MV_CONTRACT]
        contract["execution_requirements"]["gpu_subpass_readback"] = False
        contract["execution_supported"] = False
        contract["supported"] = False
        self.assertEqual(1, self._gate_exit_code_for_manifest(broken))

    def setUp(self):
        self.source = UPSTREAM / MODULE
        if not self.source.is_file():
            self.skipTest("pinned vk-gl-cts checkout not present")
        text = self.source.read_text(encoding="utf-8", errors="replace")
        self.leaves = self.gate._multiview_leaf_requirements(
            text, self.gate._source_function_at_line(text, 4908))
        self.capabilities, self.capability_failures = self.gate._advertised_capabilities()
        self.witness, self.witness_failures = self.gate._resource_witness()
        self.tests_text = text
        self.util_text = (UPSTREAM / UTIL).read_text(encoding="utf-8", errors="replace")

    def _ready_witness(self):
        """The measured host witness: this host can create the shape."""
        witness, _ = self.gate._resource_witness()
        return {MV_CONTRACT: copy.deepcopy(witness[MV_CONTRACT])}

    def _not_ready_witness(self):
        """A host that cannot answer or create the shape, with a summary that
        agrees with its own numbers."""
        measured = copy.deepcopy(self._ready_witness()[MV_CONTRACT])
        measured.update({"queryResult": -11, "createResult": -13, "createSucceeded": False,
                         "queryCovers": False, "supported": False,
                         "queryMaxArrayLayers": 0, "queryMaxMipLevels": 0,
                         "querySampleCounts": 0})
        return {MV_CONTRACT: measured}

    def _gate_exit_code_with_witness(self, manifest, witness):
        original = self.gate._resource_witness
        self.gate._resource_witness = lambda: (witness, [])
        try:
            return self._gate_exit_code_for_manifest(manifest)
        finally:
            self.gate._resource_witness = original

    def _promote_one_leaf(self, manifest, path):
        entry = copy.deepcopy([c for c in manifest["diagnostics"] if c["path"] == path][0])
        entry["expected_status"] = "Pass"
        manifest["diagnostics"] = [c for c in manifest["diagnostics"] if c["path"] != path]
        manifest["cases"].append(entry)
        return manifest

    def _gate_exit_code_for_manifest(self, manifest):
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            original = self.gate.MANIFEST
            self.gate.MANIFEST = manifest_path
            try:
                return self.gate.main()
            finally:
                self.gate.MANIFEST = original

    def _derived_contract(self, tests_text=None, util_text=None):
        tests_text = self.tests_text if tests_text is None else tests_text
        util_text = self.util_text if util_text is None else util_text
        leaves = self.gate._multiview_leaf_requirements(
            tests_text, self.gate._source_function_at_line(tests_text, 4908))
        family_types = {}
        for leaf in leaves.values():
            family_types.setdefault(leaf["family"], leaf["test_type"])
        families = self.manifest["resource_contracts"][MV_CONTRACT]["families"]
        return self.gate._multiview_attachment_contract(
            util_text, tests_text, {name: family_types[name] for name in families})

    def _gate_exit_code_with_case(self, path, source, status="Pass",
                                  contract=MV_CONTRACT):
        manifest = copy.deepcopy(self.manifest)
        manifest["cases"] = [case for case in manifest["cases"]
                             if not case["path"].startswith(MULTIVIEW_FAMILIES)]
        manifest["diagnostics"] = [case for case in manifest["diagnostics"]
                                   if case["path"] != path]
        entry = {
            "path": path, "source": source, "category": "multiview-render-pass",
            "expected_status": status,
            "features_required": ["VkPhysicalDeviceMultiviewFeatures::multiview"],
            "rationale": "deliberately wrong",
        }
        if contract:
            entry["resource_contract"] = contract
        manifest["cases"].append(entry)
        return self._gate_exit_code_for_manifest(manifest)

    def test_t05_recognizers_are_bound_to_their_constructions(self):
        """The three T05 name recognizers derive exactly the pinned factories'
        leaves and nothing when the construction they were written against is
        gone."""
        fo = (UPSTREAM / "external/vulkancts/modules/vulkan/fragment_ops/"
              "vktFragmentOperationsScissorMultiViewportTests.cpp").read_text()
        names = self.gate._fragment_ops_multi_viewport_leaf_names(fo)
        self.assertEqual({f"scissor_{n}" for n in range(1, 17)}, names)
        self.assertEqual(set(), self.gate._fragment_ops_multi_viewport_leaf_names(
            fo.replace("MIN_MAX_VIEWPORTS = 16", "MIN_MAX_VIEWPORTS = 64")))
        self.assertEqual(set(), self.gate._fragment_ops_multi_viewport_leaf_names(
            fo.replace('"scissor_" + de::toString(numViewports)', '"vp_" + name')))

        clip = (UPSTREAM / "external/vulkancts/modules/vulkan/clipping/"
                "vktClippingTests.cpp").read_text()
        util = (UPSTREAM / self.gate.DRAW_UTIL_SOURCE).read_text()
        names = self.gate._clip_volume_topology_leaf_names(clip, util)
        self.assertEqual({"point_list", "line_list", "line_list_with_adjacency", "line_strip",
                          "line_strip_with_adjacency", "triangle_list",
                          "triangle_list_with_adjacency", "triangle_strip",
                          "triangle_strip_with_adjacency", "triangle_fan"}, names)
        self.assertEqual(set(), self.gate._clip_volume_topology_leaf_names(clip, ""))
        self.assertEqual(set(), self.gate._clip_volume_topology_leaf_names(
            clip.replace("getPrimitiveTopologyShortName(cases[caseNdx])", "name(caseNdx)"), util))

        depth = (UPSTREAM / "external/vulkancts/modules/vulkan/draw/"
                 "vktDrawDepthClampTests.cpp").read_text()
        names = self.gate._draw_depth_clamp_leaf_names(depth)
        self.assertIn("d32_sfloat", names)
        self.assertIn("d32_sfloat_depth_bias_clamp_input_negative", names)
        self.assertIn("d32_sfloat_clamp_four_viewports", names)
        self.assertNotIn("d32_sfloat_bogus", names)
        self.assertEqual(6 * 8, len(names))
        self.assertEqual(set(), self.gate._draw_depth_clamp_leaf_names(
            depth.replace("formatCaseName + params.testNameSuffix", "name")))

    def test_t05_modules_are_registered_where_their_diagnostics_point(self):
        """The fragment_ops and draw scissor factories the pending leaves need
        are compiled and registered, the amber script they parse is staged, and
        the families that are only documented as gaps are not registered."""
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn('vkt::FragmentOperations::createTests(m_testCtx, "fragment_ops")', package)
        self.assertIn("vkt::Draw::createScissorTests(", package)
        self.assertNotIn("createDepthClampTests", package)
        self.assertNotIn("DynamicStateRSTests", package)
        for unit in ("vktFragmentOperationsTests.cpp",
                     "vktFragmentOperationsScissorTests.cpp",
                     "vktFragmentOperationsScissorMultiViewportTests.cpp",
                     "vktFragmentOperationsEarlyFragmentTests.cpp",
                     "vktFragmentOperationsOcclusionQueryTests.cpp",
                     "vktFragmentOperationsTransientAttachmentTests.cpp",
                     "vktDrawScissorTests.cpp"):
            self.assertIn(unit, builder)
        self.assertIn("vulkan/amber/rasterization/line_continuity/polygon-mode-lines.amber", builder)
        self.assertTrue((UPSTREAM / "external/vulkancts/data/vulkan/amber/rasterization/"
                         "line_continuity/polygon-mode-lines.amber").is_file())

    def test_t06_fragment_store_leaves_are_exact_promoted_upstream_oracles(self):
        expected_paths = {
            "dEQP-VK.rasterization.frag_side_effects.color_at_beginning.kill",
            "dEQP-VK.rasterization.frag_side_effects.color_at_end.kill",
        }
        selected = [case for case in self.current_manifest["cases"]
                    if case.get("category") == "fragment-stores-and-atomics"]
        self.assertEqual(expected_paths, {case["path"] for case in selected})
        self.assertEqual(set(), expected_paths & {
            case["path"] for case in self.current_manifest["diagnostics"]})
        self.assertTrue(all(case["expected_status"] == "Pass" for case in selected))
        self.assertTrue(all(case["features_required"] ==
                            ["core:fragmentStoresAndAtomics"] for case in selected))
        self.assertTrue(all(case["source"] ==
                            "external/vulkancts/modules/vulkan/rasterization/"
                            "vktRasterizationFragShaderSideEffectsTests.cpp:684"
                            for case in selected))

        source = (UPSTREAM / "external/vulkancts/modules/vulkan/rasterization/"
                  "vktRasterizationFragShaderSideEffectsTests.cpp").read_text()
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn('new FragSideEffectsTestCase(testCtx, "kill", params)', source)
        self.assertIn('{false, "color_at_beginning"}', source)
        self.assertIn('{true, "color_at_end"}', source)
        self.assertIn('vkt::rasterization::createTests(m_testCtx, "rasterization")', package)
        self.assertIn("vktRasterizationFragShaderSideEffectsTests.cpp", builder)

    def test_device_capabilities_come_from_the_device_sources(self):
        self.assertEqual([], self.capability_failures)
        self.assertIn("VK_KHR_MULTIVIEW", self.capabilities["extensions"])
        self.assertNotIn("VK_KHR_CREATE_RENDERPASS_2", self.capabilities["extensions"])
        self.assertFalse(self.capabilities["features"]["multiviewGeometryShader"])
        self.assertFalse(self.capabilities["features"]["multiviewTessellationShader"])
        self.assertEqual(6, self.capabilities["max_multiview_view_count"])

    def test_contract_is_derived_from_the_selected_factory_branches(self):
        """(1) The contract comes from the branches the selected families use,
        and a different branch changes it."""
        derived = self._derived_contract()
        declared = self.manifest["resource_contracts"][MV_CONTRACT]
        for field in ("format", "image_type", "tiling", "mip_levels", "samples",
                      "array_layers", "usage"):
            self.assertEqual(derived[field], declared[field], field)
        self.assertIn("VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT", derived["usage"])

        other_format = self.tests_text.replace(
            "colorFormat = VK_FORMAT_R8G8B8A8_UNORM;", "colorFormat = VK_FORMAT_B8G8R8A8_UNORM;")
        self.assertNotEqual(self.tests_text, other_format)
        self.assertEqual("VK_FORMAT_B8G8R8A8_UNORM",
                         self._derived_contract(tests_text=other_format)["format"])

        other_samples = self.tests_text.replace(
            "? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT",
            "? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT")
        self.assertEqual(2, self._derived_contract(tests_text=other_samples)["samples"])

        # A declaration that no longer matches the source fails the gate.
        stale = copy.deepcopy(self.manifest)
        stale["resource_contracts"][MV_CONTRACT]["format"] = "VK_FORMAT_B8G8R8A8_UNORM"
        self.assertEqual(1, self._gate_exit_code_for_manifest(stale))

    def test_resource_stage_is_promoted_on_hardware_evidence(self):
        """This host creates the exact shape and the manifest promotes the
        resource stage with a well-formed hardware receipt; the execution stage
        is still false, so nothing is final and nothing enters acceptance."""
        self.assertEqual([], self.witness_failures)
        measured = self.witness[MV_CONTRACT]
        self.assertTrue(measured["supported"])
        self.assertEqual(0, measured["queryResult"])
        self.assertEqual(6, measured["queryMaxArrayLayers"])
        self.assertEqual(0, measured["createResult"])
        # The neighbouring readback shape without the input-attachment role is
        # untouched: one layer, so this is not a general capability.
        probe = measured["withoutInputAttachment"]
        self.assertEqual(0, probe["queryResult"])
        self.assertEqual(1, probe["queryMaxArrayLayers"])
        self.assertFalse(probe["queryCovers"])

        declared = self.manifest["resource_contracts"][MV_CONTRACT]
        self.assertTrue(declared["resource_supported"])
        self.assertFalse(declared["execution_supported"])
        self.assertFalse(declared["supported"])
        evidence = declared["hardware_evidence"]
        self.assertEqual("346b7838414cb4d126962c05dfb29fafa9ab592d", evidence["source_commit"])
        self.assertEqual(6, evidence["array_layers"])
        self.assertEqual(6, evidence["query_max_array_layers"])
        self.assertEqual("VK_SUCCESS", evidence["create_result"])
        eligible, verdict_failures, note, pending = self.gate._contract_verdict(
            MV_CONTRACT, declared, measured, measured["arrayLayers"])
        self.assertFalse(eligible)
        self.assertEqual([], verdict_failures)
        self.assertEqual("", pending)
        self.assertIn("execution requirements are not all met", note)

        promoted = self._promote_one_leaf(copy.deepcopy(self.manifest),
                                          "dEQP-VK.multiview.masks.get_query_pool_results.15")
        self.assertEqual(1, self._gate_exit_code_with_witness(promoted, self._ready_witness()))

    def test_hardware_promotion_requires_a_complete_receipt(self):
        """resource_supported is a physical-console claim: without a complete,
        well-formed receipt every promotion is refused."""
        mutations = (
            (lambda contract: contract.pop("hardware_evidence"), "no receipt"),
            (lambda contract: contract["hardware_evidence"].pop("source_commit"),
             "missing source commit"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "source_commit", "346b7838"), "short source commit"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "source_commit", "346b7838414cb4d126962c05dfb29fafa9ab592"), "39-character commit id"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "source_commit", "Z" * 40), "non-hex source commit"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "source_commit", "346B7838414CB4D126962C05DFB29FAFA9AB592D"),
             "uppercase source commit"),
            (lambda contract: contract["hardware_evidence"].pop("run_id"), "missing run id"),
            (lambda contract: contract["hardware_evidence"].pop("firmware"), "missing firmware"),
            (lambda contract: contract["hardware_evidence"].pop("title"), "missing title"),
            (lambda contract: contract["hardware_evidence"].pop("teardown"), "missing teardown"),
            (lambda contract: contract["hardware_evidence"].pop("allocation_bytes"),
             "missing allocation"),
            (lambda contract: contract["hardware_evidence"].__setitem__("artifact_sha256", "deadbeef"),
             "short artifact digest"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "artifact_sha256", "Z" * 64), "non-hex artifact digest"),
            (lambda contract: contract["hardware_evidence"].__setitem__("log_sha256", ""),
             "empty log digest"),
            (lambda contract: contract["hardware_evidence"].__setitem__("run_id", "   "),
             "blank run id"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "query_result", "VK_ERROR_FORMAT_NOT_SUPPORTED"), "query not successful"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "create_result", "VK_ERROR_UNKNOWN"), "create not successful"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "bind_result", "VK_ERROR_UNKNOWN"), "bind not successful"),
            (lambda contract: contract["hardware_evidence"].__setitem__(
                "query_max_array_layers", 5), "query below six layers"),
            (lambda contract: contract["hardware_evidence"].__setitem__("array_layers", 1),
             "fewer than six layers"),
            (lambda contract: contract["hardware_evidence"].__setitem__("allocation_bytes", 0),
             "zero allocation"),
            (lambda contract: contract["hardware_evidence"].__setitem__("teardown", "dirty"),
             "unclean teardown"),
            (lambda contract: contract["hardware_evidence"].__setitem__("mystery", True),
             "unknown evidence field"),
            (lambda contract: contract["hardware_evidence"].__setitem__("array_layers", True),
             "boolean layer count"),
        )
        for mutate, why in mutations:
            manifest = copy.deepcopy(self.manifest)
            mutate(manifest["resource_contracts"][MV_CONTRACT])
            self.assertEqual(1, self._gate_exit_code_with_witness(
                manifest, self._ready_witness()), why)

    def test_evidence_without_the_promoted_stage_fails(self):
        """Evidence for an unpromoted stage is a stale claim, not a spare one."""
        manifest = copy.deepcopy(self.manifest)
        manifest["resource_contracts"][MV_CONTRACT]["resource_supported"] = False
        self.assertEqual(1, self._gate_exit_code_with_witness(manifest, self._ready_witness()))

    def test_declared_resource_requires_host_readiness(self):
        """A hardware-promoted stage may not be claimed on a host the source
        witness shows is not ready."""
        declared = copy.deepcopy(self.manifest["resource_contracts"][MV_CONTRACT])
        declared.update({"resource_supported": True, "supported": False})
        eligible, verdict_failures, _, _ = self.gate._contract_verdict(
            MV_CONTRACT, declared, self._not_ready_witness()[MV_CONTRACT], 6)
        self.assertFalse(eligible)
        self.assertTrue(any("source query/create witness is not ready" in f
                            for f in verdict_failures))
        manifest = copy.deepcopy(self.manifest)
        manifest["resource_contracts"][MV_CONTRACT]["resource_supported"] = True
        self.assertEqual(1, self._gate_exit_code_with_witness(
            manifest, self._not_ready_witness()))

    def test_declared_resource_with_ready_host_is_valid_but_not_final(self):
        """Declared true with a ready host is a resource-valid state; the
        execution stage still decides final support, and acceptance needs it."""
        declared = copy.deepcopy(self.manifest["resource_contracts"][MV_CONTRACT])
        declared.update({"resource_supported": True, "execution_supported": False,
                         "supported": False})
        eligible, verdict_failures, note, pending = self.gate._contract_verdict(
            MV_CONTRACT, declared, self._ready_witness()[MV_CONTRACT], 6)
        self.assertFalse(eligible)
        self.assertEqual([], verdict_failures)
        self.assertEqual("", pending)
        self.assertIn("execution requirements are not all met", note)
        manifest = copy.deepcopy(self.manifest)
        manifest["resource_contracts"][MV_CONTRACT]["resource_supported"] = True
        self.assertEqual(0, self._gate_exit_code_with_witness(
            manifest, self._ready_witness()))
        promoted = self._promote_one_leaf(
            copy.deepcopy(manifest), "dEQP-VK.multiview.masks.get_query_pool_results.15")
        self.assertEqual(1, self._gate_exit_code_with_witness(promoted, self._ready_witness()))

    def test_declared_support_cannot_be_stale(self):
        """The declared supported field must equal what the driver measures, so
        it cannot stay green after the driver changes."""
        stale = copy.deepcopy(self.manifest)
        stale["resource_contracts"][MV_CONTRACT]["supported"] = True
        self.assertEqual(1, self._gate_exit_code_for_manifest(stale))

    def test_witness_must_be_taken_at_the_required_ceiling(self):
        witness = copy.deepcopy(self.witness)
        witness[MV_CONTRACT]["arrayLayers"] = 1
        declared = self.manifest["resource_contracts"][MV_CONTRACT]
        support, verdict_failures, _, _ = self.gate._contract_verdict(
            MV_CONTRACT, declared, witness[MV_CONTRACT], 6)
        self.assertFalse(support)
        self.assertTrue(any("witnessed at 1 layers" in f for f in verdict_failures))

    def test_blocked_leaves_stay_visible_and_traceable(self):
        """(3) All 48 measured failures remain, as blocked diagnostics naming the
        contract, and none of them is strict acceptance."""
        blocked = [case for case in self.manifest["diagnostics"]
                   if case["path"].startswith(MULTIVIEW_FAMILIES)]
        self.assertEqual(48, len(blocked))
        for case in blocked:
            self.assertEqual("Fail", case["expected_status"])
            self.assertEqual(MV_CONTRACT, case["resource_contract"])
            self.assertTrue(case["rationale"].strip())
            self.assertEqual(["VkPhysicalDeviceMultiviewFeatures::multiview"],
                             case["features_required"])
        self.assertEqual([], [case["path"] for case in self.manifest["cases"]
                              if case["path"].startswith(MULTIVIEW_FAMILIES)])
        # The preserved paths are still the exact source-derived legacy set the
        # selection carried before this slice: the leaves whose derived
        # prerequisites the device meets. Their resource contract is the only
        # thing that changed, so nothing was silently dropped or added.
        runnable = sorted(
            path for path in self.leaves
            if path.startswith(MULTIVIEW_FAMILIES)
            and not self.gate._unadvertised(self.leaves[path]["required"], self.capabilities)
            and self.leaves[path]["max_views"] <= self.capabilities["max_multiview_view_count"])
        self.assertEqual(48, len(runnable))
        self.assertEqual(runnable, sorted(case["path"] for case in blocked))

    def test_acceptance_cannot_reference_the_unsupported_contract(self):
        """(1) Putting a blocked leaf back into strict acceptance fails on the
        contract, before packaging or hardware, with no rerun needed."""
        self.assertEqual(1, self._gate_exit_code_with_case(
            "dEQP-VK.multiview.masks.get_query_pool_results.15", f"{MODULE}:4908"))
        # ...and a capability bit does not change that: only the resource
        # contract decides, so advertising more features cannot unblock it.
        self.assertEqual(1, self._gate_exit_code_with_case(
            "dEQP-VK.multiview.index.vertex_shader.get_query_pool_results.15",
            f"{MODULE}:4908"))

    def test_missing_contract_mapping_or_usage_bit_fails(self):
        """(2) Deleting the contract mapping, or one usage bit from it, fails."""
        without_contract = copy.deepcopy(self.manifest)
        del without_contract["resource_contracts"][MV_CONTRACT]
        self.assertEqual(1, self._gate_exit_code_for_manifest(without_contract))

        missing_bit = copy.deepcopy(self.manifest)
        missing_bit["resource_contracts"][MV_CONTRACT]["usage"] = [
            bit for bit in missing_bit["resource_contracts"][MV_CONTRACT]["usage"]
            if bit != "VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT"]
        self.assertEqual(1, self._gate_exit_code_for_manifest(missing_bit))

        # A contract with no derivation this gate can read is not accepted
        # either - unknown requirements fail closed rather than passing.
        unreadable = copy.deepcopy(self.manifest)
        unreadable["resource_contracts"][MV_CONTRACT]["derive_from"] = "somewhere/else.cpp"
        self.assertEqual(1, self._gate_exit_code_for_manifest(unreadable))

    def test_unknown_contract_reference_fails_closed(self):
        manifest = copy.deepcopy(self.manifest)
        for case in manifest["diagnostics"]:
            if case["path"].startswith(MULTIVIEW_FAMILIES):
                case["resource_contract"] = "no-such-contract"
                break
        self.assertEqual(1, self._gate_exit_code_for_manifest(manifest))

    def test_stage_fields_are_required_and_boolean(self):
        """A missing or non-boolean stage field fails closed: the promotion
        decision has to be explicit and machine-readable."""
        mutations = (
            (lambda contract: contract.pop("resource_supported"), "missing resource stage"),
            (lambda contract: contract.pop("execution_supported"), "missing execution stage"),
            (lambda contract: contract.__setitem__("resource_supported", "yes"),
             "non-boolean resource stage"),
            (lambda contract: contract.__setitem__("execution_supported", 1),
             "non-boolean execution stage"),
            (lambda contract: contract.pop("supported"), "missing final state"),
            (lambda contract: contract.__setitem__("supported", "no"),
             "non-boolean final state"),
            (lambda contract: contract.pop("execution_requirements"),
             "no execution requirements"),
            (lambda contract: contract["execution_requirements"].pop("compiler_lowering"),
             "missing execution requirement"),
            (lambda contract: contract["execution_requirements"].__setitem__(
                "gpu_subpass_readback", "yes"), "non-boolean execution requirement"),
            (lambda contract: contract["execution_requirements"].__setitem__(
                "mystery_stage", True), "unknown execution requirement"),
        )
        for mutate, why in mutations:
            manifest = copy.deepcopy(self.manifest)
            mutate(manifest["resource_contracts"][MV_CONTRACT])
            self.assertEqual(1, self._gate_exit_code_for_manifest(manifest), why)

    def test_execution_flag_alone_cannot_claim_execution(self):
        """Flipping the stage flag is not evidence: the derived requirement set
        decides, so a lone execution_supported=true is rejected."""
        manifest = copy.deepcopy(self.manifest)
        manifest["resource_contracts"][MV_CONTRACT].update(
            {"execution_supported": True, "supported": False})
        self.assertEqual(1, self._gate_exit_code_for_manifest(manifest))
        # ...and a complete requirement set with a stale flag is rejected too.
        manifest = copy.deepcopy(self.manifest)
        for key in ("descriptor_object_model", "descriptor_table_encoding", "compiler_lowering", "gpu_subpass_readback"):
            manifest["resource_contracts"][MV_CONTRACT]["execution_requirements"][key] = True
        manifest["resource_contracts"][MV_CONTRACT]["execution_supported"] = False
        self.assertEqual(1, self._gate_exit_code_for_manifest(manifest))

    def test_complete_execution_requirements_without_the_resource_stay_final_false(self):
        """Executable semantics without a createable resource is a legitimate
        intermediate state: the stages are independent, the final state is the
        conjunction, and nothing is promoted."""
        manifest = copy.deepcopy(self.manifest)
        contract = manifest["resource_contracts"][MV_CONTRACT]
        for key in ("descriptor_object_model", "descriptor_table_encoding", "compiler_lowering", "gpu_subpass_readback"):
            contract["execution_requirements"][key] = True
        # Withdrawing the promoted resource stage means withdrawing its receipt
        # too: evidence for an unpromoted stage fails closed.
        contract.pop("hardware_evidence", None)
        contract.update({"resource_supported": False, "execution_supported": True,
                         "supported": False})
        self.assertEqual(0, self._gate_exit_code_for_manifest(manifest))

    def test_pre_execution_fixture_is_three_of_four_and_stays_final_false(self):
        """The merged host work is recorded, not advertised: this tree now meets
        the descriptor object model, the descriptor table encoding and the
        compiler lowering, and the one outstanding requirement keeps the
        execution stage - and so the final state - false. The blocker prose has
        to name both the true and the outstanding requirements, and a leaf still
        may not re-enter strict acceptance."""
        contract = self.manifest["resource_contracts"][MV_CONTRACT]
        requirements = contract["execution_requirements"]
        self.assertEqual(
            {"descriptor_object_model": True, "descriptor_table_encoding": True,
             "compiler_lowering": True, "gpu_subpass_readback": False},
            requirements)
        self.assertEqual(3, sum(1 for value in requirements.values() if value))
        self.assertEqual(4, len(requirements))
        # Both stages and their conjunction: the resource stage is promoted on
        # the console receipt, the execution stage is not, so the final state
        # stays false.
        self.assertTrue(contract["resource_supported"])
        self.assertFalse(contract["execution_supported"])
        self.assertFalse(contract["supported"])
        blocker = contract["blocker"]
        for named in ("three of the four recorded requirements are now true",
                      "descriptor_object_model", "descriptor_table_encoding",
                      "compiler_lowering", "gpu_subpass_readback",
                      "sole remaining requirement", "48 leaves stay diagnostics"):
            self.assertIn(named, blocker)
        # The selection itself is unchanged: the same acceptance cases minus the
        # 48 multiview leaves this fixture demotes, and the same diagnostics plus
        # those 48 demoted leaves.
        self.assertEqual(len(self.current_manifest["cases"]) - 48, len(self.manifest["cases"]))
        self.assertEqual(len(self.current_manifest["diagnostics"]) + 48,
                         len(self.manifest["diagnostics"]))
        self.assertEqual(48, len([case for case in self.manifest["diagnostics"]
                                  if case["path"].startswith(MULTIVIEW_FAMILIES)]))
        # The derived verdict agrees with the ledger: not eligible, no failure,
        # nothing pending, and the only reason left is what is still missing.
        witness = self._ready_witness()
        eligible, verdict_failures, note, pending = self.gate._contract_verdict(
            MV_CONTRACT, contract, witness[MV_CONTRACT], witness[MV_CONTRACT]["arrayLayers"])
        self.assertFalse(eligible)
        self.assertEqual([], verdict_failures)
        self.assertEqual("", pending)
        self.assertIn("execution requirements are not all met", note)
        # So the strict manifest still passes with every leaf diagnostic, and a
        # leaf that tries to use the contract is still refused.
        self.assertEqual(0, self._gate_exit_code_with_witness(
            copy.deepcopy(self.manifest), witness))
        self.assertEqual(1, self._gate_exit_code_with_witness(
            self._promote_one_leaf(copy.deepcopy(self.manifest),
                                   "dEQP-VK.multiview.masks.get_query_pool_results.15"),
            witness))

    def test_supported_must_be_the_conjunction_of_the_stages(self):
        for resource, execution in ((True, False), (False, True), (False, False)):
            manifest = copy.deepcopy(self.manifest)
            manifest["resource_contracts"][MV_CONTRACT].update(
                {"resource_supported": resource, "execution_supported": execution,
                 "supported": True})
            self.assertEqual(1, self._gate_exit_code_for_manifest(manifest),
                             f"resource={resource} execution={execution}")

    def test_resource_only_success_stays_diagnostic_and_cannot_enter_acceptance(self):
        """A future query/create success is not promotion: with no executable
        semantics the family stays diagnostic and acceptance is refused."""
        witness = self._ready_witness()
        path = "dEQP-VK.multiview.masks.get_query_pool_results.15"

        diagnostic = copy.deepcopy(self.manifest)
        diagnostic["resource_contracts"][MV_CONTRACT].update(
            {"resource_supported": True, "execution_supported": False, "supported": False})
        self.assertEqual(0, self._gate_exit_code_with_witness(diagnostic, witness))
        self.assertEqual(48, len([c for c in diagnostic["diagnostics"]
                                  if c["path"].startswith(MULTIVIEW_FAMILIES)]))

        acceptance = self._promote_one_leaf(copy.deepcopy(diagnostic), path)
        self.assertEqual(1, self._gate_exit_code_with_witness(acceptance, witness))

    def test_both_stages_true_is_the_only_promotable_state(self):
        """With the resource created and executable semantics in place, and only
        then, a leaf may re-enter strict acceptance."""
        witness = self._ready_witness()
        path = "dEQP-VK.multiview.masks.get_query_pool_results.15"
        manifest = copy.deepcopy(self.manifest)
        contract = manifest["resource_contracts"][MV_CONTRACT]
        for key in ("descriptor_object_model", "descriptor_table_encoding", "compiler_lowering", "gpu_subpass_readback"):
            contract["execution_requirements"][key] = True
        contract.update({"resource_supported": True, "execution_supported": True,
                         "supported": True})
        self.assertEqual(0, self._gate_exit_code_with_witness(manifest, witness))
        promoted = self._promote_one_leaf(copy.deepcopy(manifest), path)
        self.assertEqual(0, self._gate_exit_code_with_witness(promoted, witness))
        # ...and the same promotion without the execution stage is refused.
        frozen = copy.deepcopy(manifest)
        frozen["resource_contracts"][MV_CONTRACT]["execution_requirements"][
            "gpu_subpass_readback"] = False
        frozen["resource_contracts"][MV_CONTRACT].update(
            {"execution_supported": False, "supported": False})
        frozen = self._promote_one_leaf(frozen, path)
        self.assertEqual(1, self._gate_exit_code_with_witness(frozen, witness))

    def test_passing_families_stay_acceptance(self):
        """(4) Families that pass keep their strict acceptance entries."""
        accepted = {case["path"] for case in self.manifest["cases"]}
        self.assertEqual(len(self.current_manifest["cases"]) - 48, len(self.manifest["cases"]))
        for path in (
            "dEQP-VK.compute.basic.ubo_to_ssbo_single_invocation",
            "dEQP-VK.compute.indirect_dispatch.upload_buffer.single_invocation",
            "dEQP-VK.draw.renderpass.shader_draw_parameters.base_instance.draw_indexed_indirect",
            "dEQP-VK.synchronization.basic.fence.one",
            "dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests.partial_image_pot_same_format_clear",
            "dEQP-VK.robustness.buffer_access.compute.scalar_copy.r32_uint.oob_uniform_read.range_1_byte",
        ):
            self.assertIn(path, accepted, path)

    def test_acceptance_may_not_require_an_unadvertised_capability(self):
        for path in (
            "dEQP-VK.multiview.renderpass2.masks.get_query_pool_results.15",
            "dEQP-VK.multiview.index.geometry_shader.get_query_pool_results.15",
            "dEQP-VK.multiview.index.tessellation_shader.get_query_pool_results.15",
            "dEQP-VK.multiview.masks.get_query_pool_results.8",
        ):
            self.assertIn(path, self.leaves, path)
            with self.subTest(path=path):
                self.assertEqual(1, self._gate_exit_code_with_case(path, f"{MODULE}:4908"),
                                 f"{path} must fail closed as acceptance")

    def test_unproduced_and_miscited_paths_fail_closed(self):
        for path, source in (
            ("dEQP-VK.multiview.masks.get_query_pool_results.invented.15", f"{MODULE}:4908"),
            ("dEQP-VK.multiview.masks.get_query_pool_results.5_10_5_10", f"{MODULE}:1"),
            ("dEQP-VK.multiview.index.masks.get_query_pool_results.15", f"{MODULE}:4908"),
        ):
            with self.subTest(path=path):
                self.assertEqual(1, self._gate_exit_code_with_case(path, source),
                                 f"{path} must fail closed")

    def test_duplicate_selection_fails_closed_without_the_checkout(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["cases"].append(copy.deepcopy(manifest["cases"][0]))
        failures = self.gate._duplicate_selection_failures(manifest)
        self.assertTrue(failures)
        self.assertIn("selected twice", failures[0])

    def test_selection_must_match_the_compiled_cts_revision(self):
        if not UPSTREAM.is_dir() or self.gate._cts_revision_failures(self.manifest):
            self.skipTest("pinned vk-gl-cts checkout is not a comparable checkout")
        manifest = copy.deepcopy(self.manifest)
        manifest["cts_pin"]["commit"] = "0" * 40
        failures = self.gate._cts_revision_failures(manifest)
        self.assertEqual(1, len(failures))
        self.assertIn("selection pins", failures[0])
        self.assertEqual(1, self._gate_exit_code_for_manifest(manifest))

    def test_current_selection_passes(self):
        self.assertEqual(0, self.gate.main())


if __name__ == "__main__":
    unittest.main()
