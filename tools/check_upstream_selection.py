#!/usr/bin/env python3
"""Contract check for the frozen upstream CTS selection.

Verifies that every case named in cts/upstream/manifest.json is actually
produced by the pinned upstream sources, i.e. that the selection was not
invented and that the case list has not drifted from the sources it cites.

This is a host-side check: it never talks to the console and is safe to run in
CI. When the pinned vk-gl-cts checkout is not present (third_party is ignored)
the check reports that it was skipped instead of failing.
"""
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "cts/upstream/manifest.json"
UPSTREAM = ROOT / "third_party/vk-gl-cts"
# The integration supplies the package and its leading groups; upstream supplies
# everything below them.
INTEGRATION_SOURCE = ROOT / "cts/upstream/package_ps5.cpp"
# The capabilities a selection is allowed to rely on come from the device's own
# sources, not from the selection itself.
DEVICE_SOURCE = ROOT / "src/vk_device.c"
INTERNAL_HEADER = ROOT / "src/vk_internal.h"
# The driver's own image-usage surfaces, and the pinned upstream helper that
# builds the resources a selected family actually needs.
IMAGE_USAGE_CREATE_SOURCE = ROOT / "src/vk_memory.c"
IMAGE_USAGE_PREDICATE_SOURCE = ROOT / "src/texture_format.c"
IMAGE_USAGE_QUERY_SOURCE = ROOT / "src/graphics_formats.h"
MULTIVIEW_UTIL_SOURCE = ("external/vulkancts/modules/vulkan/multiview/"
                         "vktMultiViewRenderUtil.cpp")
MULTIVIEW_TEST_SOURCE = ("external/vulkancts/modules/vulkan/multiview/"
                         "vktMultiViewRenderTests.cpp")
# The exact execution requirements a contract must name. Every key is required
# and must be a real boolean; anything else - a missing key, an unknown one, a
# non-boolean value - fails closed rather than being ignored, because the
# execution stage is derived from these values and nothing else.
EXECUTION_REQUIREMENTS = ("descriptor_object_model", "descriptor_table_encoding",
                          "compiler_lowering", "gpu_subpass_readback")
# The exact, fail-closed hardware-evidence record a promoted resource stage must
# carry. A physical-console claim with a missing, unknown, ill-typed or
# unsuccessful field is not evidence, so no promotion can rest on one.
HARDWARE_EVIDENCE_FIELDS = ("source_commit", "artifact_sha256", "run_id", "log_sha256",
                            "firmware", "title",
                            "query_result", "query_max_array_layers", "create_result",
                            "bind_result", "allocation_bytes", "array_layers", "teardown")
HARDWARE_EVIDENCE_REQUIRED_LAYERS = 6
HEX_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
# A revision, not a digest: the exact commit the witnessed artifact was built
# from, so the receipt ties the run to one source revision.
COMMIT_ID = re.compile(r"[0-9a-f]{40}\Z")
# VkSampleCountFlagBits values, so a derived branch name can be compared with the
# number the fixture witnesses.
SAMPLE_COUNT_FLAGS = {
    "VK_SAMPLE_COUNT_1_BIT": 1, "VK_SAMPLE_COUNT_2_BIT": 2,
    "VK_SAMPLE_COUNT_4_BIT": 4, "VK_SAMPLE_COUNT_8_BIT": 8,
    "VK_SAMPLE_COUNT_16_BIT": 16, "VK_SAMPLE_COUNT_32_BIT": 32,
    "VK_SAMPLE_COUNT_64_BIT": 64,
}


def _source_function_at_line(text: str, line_number: int) -> str:
    """Return the C++ function beginning at the cited source line.

    Some upstream SPIR-V assembly factories build leaf names from two static
    tables rather than spelling the final name as one literal.  Keep that
    derivation bounded to the function cited by the manifest instead of
    accepting unrelated tokens from the entire (very large) module.
    """
    lines = text.splitlines(keepends=True)
    if line_number < 1 or line_number > len(lines):
        return ""
    start = sum(len(line) for line in lines[:line_number - 1])
    open_brace = text.find("{", start)
    if open_brace < 0:
        return ""

    depth = 0
    in_string = False
    escaped = False
    for offset in range(open_brace, len(text)):
        char = text[offset]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:offset + 1]
    return ""


def _table_composed_leaf_names(text: str, function_text: str) -> set[str]:
    """Derive leaves of the exact CAPABILITIES-name + cTypes-name form."""
    if not re.search(
        r"string\s*\(CAPABILITIES\[[^\]]+\]\.name\)\s*\+\s*\"_\"\s*\+\s*"
        r"cTypes\[[^\]]+\](?:\[[^\]]+\])?\.name",
        function_text,
    ):
        return set()

    capabilities_match = re.search(
        r"static\s+const\s+Capability\s+CAPABILITIES\s*\[\]\s*=\s*\{(.*?)\n\};",
        text,
        re.DOTALL,
    )
    if not capabilities_match:
        return set()
    capabilities = re.findall(r"\{\s*\"([a-z0-9_]+)\"\s*,", capabilities_match.group(1))

    # The first field of every CompositeType initializer is its generated name.
    # Restrict this to the cited function; shader assembly string literals do
    # not match because they are not brace-initializer fields.
    type_names = re.findall(r"\{+\s*\"([a-z0-9_]+)\"\s*,", function_text)
    return {f"{capability}_{type_name}"
            for capability in capabilities for type_name in type_names}


def _mapping_group_segment(text: str, segment: str) -> bool:
    """Recognize numeric mapping groups generated from fixed upstream tables."""
    table = "allocationSizes"
    value = segment
    if segment.startswith("offset_"):
        table, value = "offsets", segment.removeprefix("offset_")
    elif segment.startswith("size_"):
        table, value = "sizes", segment.removeprefix("size_")
    if not value.isdigit():
        return False
    match = re.search(
        rf"const\s+VkDeviceSize\s+{table}\[\]\s*=\s*\{{(.*?)\}};",
        text, re.DOTALL)
    if not match:
        return False
    # These focused cases intentionally use only decimal literals from the
    # pinned arrays; expressions such as 1 * 1024 * 1024 + 1 are not guessed.
    return int(value) in {int(token) for token in re.findall(r"\b\d+\b", match.group(1))}


def _multiview_leaf_requirements(text: str, function_text: str) -> dict[str, dict]:
    """Derive every render-pass leaf of the pinned multiview module with its
    complete upstream prerequisites.

    The factory registers its leaves through createViewMasksName() over fixed
    view-mask tables, two query names and a shader-family table whose order
    matches the module's TestType enum, and it does that once per rendering type
    (legacy, renderpass2, dynamic rendering). The four ViewIndex-in-stage
    families hang under one extra "index" group; every other family sits
    directly under the rendering-type group.

    Each leaf carries the prerequisites its own upstream gate declares: the
    "VK_KHR_multiview" functionality the factory always requires, the
    rendering-type extension, the shader-multiview feature a geometry or
    tessellation family needs, the core features two families need, and the
    view extent, which the factory compares against the reported
    maxMultiviewViewCount before it runs anything. Whole paths are returned, so
    a real leaf hung under a family that does not own it cannot pass by naming
    literals that exist elsewhere in the module, and any prerequisite that
    cannot be derived returns nothing at all rather than a partial answer.
    """
    construction = (
        '"renderpass2"' in text and
        '"dynamic_rendering"' in text and
        "createViewMasksName" in text and
        '"max_multi_view_view_count"' in text and
        '"get_query_pool_results"' in text and
        '"cmd_copy_query_pool_results"' in text and
        'new tcu::TestCaseGroup(testCtx, "index")' in text and
        "const uint32_t minSupportedMultiviewViewCount" in text and
        "groupViewIndex->addChild" in function_text and
        "targetGroupPtr->addChild" in function_text and
        "shaderName[testTypeNdx]" in function_text
    )
    if not construction:
        return {}

    enum_match = re.search(r"enum TestType\s*\{(.*?)\};", text, re.DOTALL)
    families_match = re.search(
        r"const string shaderName\[TEST_TYPE_LAST\]\s*=\s*\{(.*?)\n    \};",
        function_text, re.DOTALL)
    case_count = re.search(r"const uint32_t testCaseCount\s*=\s*(\d+)u;", function_text)
    if not enum_match or not families_match or not case_count:
        return {}
    test_types = [name for name in re.findall(r"([A-Z][A-Z0-9_]+)", enum_match.group(1))
                  if name != "TEST_TYPE_LAST"]
    family_names = re.findall(r'"([a-z0-9_]+)"', families_match.group(1))
    if len(test_types) != len(family_names):
        return {}
    # Every prerequisite below is derived from a named TestType arm. If upstream
    # renames one, derive nothing rather than silently drop its requirement.
    geometry_types = {"TEST_TYPE_VIEW_INDEX_IN_GEOMETRY",
                      "TEST_TYPE_INPUT_ATTACHMENTS_GEOMETRY",
                      "TEST_TYPE_SECONDARY_CMD_BUFFER_GEOMETRY"}
    needed = geometry_types | {"TEST_TYPE_VIEW_INDEX_IN_TESELLATION", "TEST_TYPE_QUERIES",
                               "TEST_TYPE_DEPTH_DIFFERENT_RANGES",
                               "TEST_TYPE_NESTED_CMD_BUFFER"}
    if not needed <= set(test_types):
        return {}

    # Which families the factory hangs under the "index" group: the cases whose
    # switch arm calls groupViewIndex->addChild.
    index_arm = re.search(
        r"case (TEST_TYPE_VIEW_INDEX_IN_VERTEX):(.*?)default:", function_text, re.DOTALL)
    if not index_arm or "groupViewIndex->addChild" not in index_arm.group(2):
        return {}
    index_types = {index_arm.group(1)} | set(
        re.findall(r"case (TEST_TYPE_[A-Z0-9_]+):", index_arm.group(2)))
    if not index_types:
        return {}

    # The view-mask leaves: the fixed tables, then the iteration table the
    # factory fills one bit at a time up to its own supported view count.
    mask_names: list[str] = []
    tables = re.findall(r"viewMasks\[(\d+)\]\.push_back\((\d+)u\);", function_text)
    by_index: dict[str, list[str]] = {}
    for index, value in tables:
        by_index.setdefault(index, []).append(value)
    for index in sorted(by_index, key=int):
        mask_names.append("_".join(by_index[index]))
    supported = re.search(r"minSupportedMultiviewViewCount\s*=\s*(\d+)u", function_text)
    if not supported:
        return {}
    mask_names.append("_".join(str(1 << bit) for bit in range(int(supported.group(1)))))
    if len(mask_names) != int(case_count.group(1)):
        return {}

    # Each mask table is paired index by index with the extent its case renders.
    extent_match = re.search(
        r"const VkExtent3D extent3D\[testCaseCount\]\s*=\s*\{(.*?)\n    \};",
        function_text, re.DOTALL)
    incomplete_match = re.search(
        r"const VkExtent3D incompleteExtent3D\s*=\s*\{\s*\d+u,\s*\d+u,\s*(\d+)u\s*\};",
        function_text)
    if not extent_match or not incomplete_match:
        return {}
    depths = [int(depth) for (_w, _h, depth) in re.findall(
        r"\{\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\s*\}", extent_match.group(1))]
    if len(depths) != len(mask_names):
        return {}
    limit_leaf_depth = int(incomplete_match.group(1))

    rendering_match = re.search(r"int numberOfRenderingTypes\s*=\s*(\d+);", function_text)
    renderpass2_match = re.search(
        r'new tcu::TestCaseGroup\(group->getTestContext\(\), "(renderpass2)"\)', function_text)
    dynamic_match = re.search(
        r'new tcu::TestCaseGroup\(group->getTestContext\(\), "(dynamic_rendering)"\)', function_text)
    if not rendering_match or not renderpass2_match or not dynamic_match:
        return {}
    rendering_names = ["", renderpass2_match.group(1), dynamic_match.group(1)]
    if int(rendering_match.group(1)) > len(rendering_names):
        return {}

    query_names = ("get_query_pool_results", "cmd_copy_query_pool_results")
    if not all(f'"{query}"' in text for query in query_names):
        return {}

    def family_prerequisites(test_type: str) -> list[str]:
        required = ["extension:VK_KHR_MULTIVIEW", "feature:multiview"]
        if test_type in geometry_types:
            required += ["core:geometryShader", "feature:multiviewGeometryShader"]
        if test_type == "TEST_TYPE_VIEW_INDEX_IN_TESELLATION":
            required.append("feature:multiviewTessellationShader")
        if test_type == "TEST_TYPE_QUERIES":
            required.append("core:occlusionQueryPrecise")
        if test_type == "TEST_TYPE_DEPTH_DIFFERENT_RANGES":
            required.append("extension:VK_EXT_DEPTH_RANGE_UNRESTRICTED")
        if test_type == "TEST_TYPE_NESTED_CMD_BUFFER":
            required.append("extension:VK_EXT_NESTED_COMMAND_BUFFER")
        return required

    leaves: dict[str, dict] = {}
    for rendering_ndx, group_name in enumerate(rendering_names):
        if rendering_ndx >= int(rendering_match.group(1)):
            continue
        prefix = "dEQP-VK.multiview." + (f"{group_name}." if group_name else "")
        rendering_required: list[str] = []
        if rendering_ndx == 1:
            rendering_required = ["extension:VK_KHR_CREATE_RENDERPASS_2"]
        elif rendering_ndx == 2:
            rendering_required = ["extension:VK_KHR_DYNAMIC_RENDERING"]
        for test_type, family in zip(test_types, family_names):
            family_prefix = prefix + ("index." if test_type in index_types else "")
            required = family_prerequisites(test_type) + rendering_required
            for query in query_names:
                for depth, mask in zip(depths, mask_names):
                    leaves[f"{family_prefix}{family}.{query}.{mask}"] = {
                        "required": list(required), "max_views": depth,
                        "family": family, "test_type": test_type}
                leaves[f"{family_prefix}{family}.{query}.max_multi_view_view_count"] = {
                    "required": list(required), "max_views": limit_leaf_depth,
                    "family": family, "test_type": test_type}
    return leaves


def _draw_shader_draw_parameters_leaf_names(text: str, function_text: str) -> set[str]:
    """Derive the shader_draw_parameters leaf names of the pinned draw module.

    The upstream factory names a leaf from the flags passed as its third
    argument and ORs the group's preset flags in afterwards, so a preset flag
    (INDIRECT|MULTIDRAW for the draw_index group, for example) never appears in
    the leaf name. Accept a name only when the module still contains the exact
    naming construction and the cited function is one of the group blocks that
    registers calls through it.
    """
    construction = (
        'name << "draw";' in text and
        'if (flags & TEST_FLAG_INDEXED)' in text and
        'name << "_indexed";' in text and
        'if (flags & TEST_FLAG_INDIRECT)' in text and
        'name << "_indirect";' in text and
        'if (flags & TEST_FLAG_INSTANCED)' in text and
        'name << "_instanced";' in text and
        'if (flags & TEST_FLAG_FIRST_INSTANCE)' in text and
        'name << "_first_instance";' in text and
        "testSpec.flags |= flags;" in text
    )
    if not construction:
        return set()
    calls = re.findall(r"addDrawCase\(group\.get\(\), testSpec,([^;]*)\);",
                       function_text)
    if not calls:
        return set()
    leaves: set[str] = set()
    for argument in calls:
        name = "draw"
        for token, suffix in (("TEST_FLAG_INDEXED", "_indexed"),
                              ("TEST_FLAG_INDIRECT", "_indirect"),
                              ("TEST_FLAG_INSTANCED", "_instanced"),
                              ("TEST_FLAG_FIRST_INSTANCE", "_first_instance")):
            if token in argument:
                name += suffix
        leaves.add(name)
    return leaves


def _fill_update_generated_leaf_names(function_text: str) -> set[str]:
    """Derive names constructed by createFillAndUpdateBufferTests.

    The pinned upstream factory composes most leaves from fixed ``testName``
    literals and two fixed prefixes. Its VK_WHOLE_SIZE loop composes the
    remaining names from the four uint32 byte remainders and aligned offsets.
    Keep this recognizer tied to the exact construction expressions so an
    unrelated token elsewhere in the module cannot satisfy provenance.
    """
    leaves: set[str] = set()
    if ('"fill_" + testName' in function_text and
            '"update_" + testName' in function_text):
        names = re.findall(
            r'const\s+std::string\s+testName\s*\(\s*"([a-z0-9_]+)"\s*\)\s*;',
            function_text,
        )
        leaves.update(prefix + name for name in names for prefix in ("fill_", "update_"))

    whole_name = re.search(
        r'"fill_buffer_vk_whole_size_"\s*\+\s*de::toString\(extraBytes\)\s*\+'
        r'\s*"_extra_bytes_offset_"\s*\+\s*de::toString\(params\.dstOffset\)',
        function_text,
    )
    fixed_loops = function_text.count(
        "for (VkDeviceSize i = 0; i < sizeof(uint32_t); ++i)") == 1 and function_text.count(
        "for (VkDeviceSize j = 0; j < sizeof(uint32_t); ++j)") == 1
    offset_is_words = "params.dstOffset = j * sizeof(uint32_t);" in function_text
    if whole_name and fixed_loops and offset_is_words:
        leaves.update(
            f"fill_buffer_vk_whole_size_{extra}_extra_bytes_offset_{word * 4}"
            for extra in range(4) for word in range(4)
        )
    return leaves


def _dynamic_state_compute_generated_segments(text: str) -> set[str]:
    """Derive brief state group names from the pinned compute factory.

    These groups are generated by lowercasing VkDynamicState enum spellings and
    removing their fixed prefix.  Require both the exact conversion body and
    the factory's use of that conversion before accepting any derived segment.
    """
    conversion = (
        'strlen("VK_DYNAMIC_STATE_")' in text and
        "de::toLower(fullName.substr(prefixLen))" in text
    )
    factory_use = "getDynamicStateBriefName(state)" in text
    state_list = re.search(
        r"const\s+VkDynamicState\s+dynamicStateList\[\]\s*=\s*\{(.*?)\};",
        text, re.DOTALL)
    if not (conversion and factory_use and state_list):
        return set()
    return {
        token.removeprefix("VK_DYNAMIC_STATE_").lower()
        for token in re.findall(r"\bVK_DYNAMIC_STATE_[A-Z0-9_]+\b", state_list.group(1))
    }


def _copy_and_blit_simple_image_leaf_names(function_text: str) -> set[str]:
    """Derive the image-to-image simple-test leaves of the pinned copy module.

    Those leaves are not written as literals: the factory composes each name
    from three fixed tables in the same function.  Accept the cross product only
    when the exact composition expression, the exact registration call and all
    three table names are present, so an unrelated token elsewhere in the module
    cannot satisfy provenance.
    """
    if not re.search(
        r'"partial_image_"\s*\+\s*extent\.name\s*\+\s*"_"\s*\+\s*format\.name'
        r'\s*\+\s*"_"\s*\+\s*clear\.name',
        function_text,
    ):
        return set()
    if "group->addChild(new CopyImageToImageTestCase(testCtx, testCaseName, params));" not in function_text:
        return set()
    formats = re.search(r"formats\[\]\s*=\s*\{(.*?)\n\s*\};", function_text, re.DOTALL)
    clears = re.search(r"clears\[\]\s*=\s*\{(.*?)\};", function_text, re.DOTALL)
    extents = re.search(r"extents\[\]\s*=\s*\{(.*?)\};", function_text, re.DOTALL)
    if not (formats and clears and extents):
        return set()
    format_names = re.findall(r'\{\s*"([a-z0-9_]+)"\s*,\s*vk::VK_FORMAT_', formats.group(1))
    clear_names = re.findall(r'\{\s*"([a-z0-9_]+)"\s*,\s*VK_(?:TRUE|FALSE)\s*\}', clears.group(1))
    extent_names = re.findall(r'\{\s*"([a-z0-9_]+)"\s*,\s*\{', extents.group(1))
    if not (format_names and clear_names and extent_names):
        return set()
    return {
        f"partial_image_{extent}_{format_name}_{clear}"
        for extent in extent_names
        for format_name in format_names
        for clear in clear_names
    }


def _duplicate_selection_failures(manifest: dict) -> list[str]:
    """Reject a selection that names the same upstream case more than once.

    A repeated path is not a larger selection.  The packaged case list is
    generated one line per manifest entry, so a duplicate would make the
    payload execute the same leaf twice, inflate the reported case count and
    could hide a case that was dropped in the same edit.  The invariant only
    reads the manifest, so it is enforced even when the pinned upstream
    checkout is unavailable.
    """
    failures = []
    seen: dict[str, str] = {}
    for label, key in (("acceptance", "cases"), ("diagnostic", "diagnostics")):
        for case in manifest.get(key, []):
            path = case["path"]
            if path in seen:
                failures.append(
                    f"{path}: selected twice ({seen[path]} and {label})")
            else:
                seen[path] = label
    return failures


def _cts_revision_failures(manifest: dict) -> list[str]:
    """Reject a selection that is not validated against the compiled revision.

    tools/build_upstream_cts.py compiles the package from third_party/vk-gl-cts
    and already refuses a checkout that is not the revision cts_pin records. The
    same rule belongs here, at selection time, so a manifest edit cannot be
    accepted against one revision while the packaging build uses another: the
    leaf names, group segments and source anchors all belong to one revision.
    The checkout is ignored by the lab repository and some environments vendor
    it without its own metadata; a directory that is not its own git work tree
    cannot be compared here and stays the build's check.
    """
    commit = manifest.get("cts_pin", {}).get("commit")
    if not commit or not UPSTREAM.is_dir():
        return []
    try:
        top = subprocess.check_output(
            ["git", "-C", str(UPSTREAM), "rev-parse", "--show-toplevel"],
            text=True, stderr=subprocess.DEVNULL).strip()
        actual = subprocess.check_output(
            ["git", "-C", str(UPSTREAM), "rev-parse", "HEAD"],
            text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return []
    if Path(top).resolve() != UPSTREAM.resolve():
        return []
    if actual != commit:
        return [f"third_party/vk-gl-cts is at {actual}; the selection pins {commit}"]
    return []


def _advertised_capabilities() -> tuple[dict, list[str]]:
    """Read the capabilities the device actually advertises from its sources.

    Strict acceptance requires a leaf to pass, so an acceptance entry may only
    rely on capabilities this device reports. The truth is taken from the
    device's own code: the extension macros it enumerates, the multiview feature
    flags it answers (including the ones it answers false) and the view-count
    floor it publishes. A value that cannot be read is reported as a failure
    rather than assumed, so a moved or renamed gate can never leave an
    unsupported acceptance entry silently green.
    """
    failures: list[str] = []
    device = DEVICE_SOURCE.read_text(encoding="utf-8", errors="replace")
    header = INTERNAL_HEADER.read_text(encoding="utf-8", errors="replace")

    enumeration = re.search(
        r"vkEnumerateDeviceExtensionProperties\([^)]*\)\s*\n\{(.*?)\n\}",
        device, re.DOTALL)
    if not enumeration:
        failures.append("cannot read the device extension list from src/vk_device.c")
        extensions: set[str] = set()
    else:
        extensions = {f"VK_{macro}" for macro in re.findall(
            r"\bVK_([A-Z0-9_]+)_EXTENSION_NAME\b", enumeration.group(1))}

    features: dict[str, bool] = {}
    for feature in ("multiview", "multiviewGeometryShader", "multiviewTessellationShader"):
        literal = re.search(rf"features->{feature}\s*=\s*(VK_TRUE|VK_FALSE)\s*;", device)
        if literal:
            features[feature] = literal.group(1) == "VK_TRUE"
        elif re.search(rf"features->{feature}\s*=\s*\(VkBool32\)\s*"
                       r"ps5vk_platform_multiview_supported\(", device):
            # The reported feature follows the platform capability that the
            # measured hardware run establishes.
            features[feature] = True
        else:
            failures.append(
                f"cannot read the reported {feature} feature from src/vk_device.c")

    core_body = re.search(r"static void get_core_features\(.*?\n\}", device, re.DOTALL)
    # The core features are reported from a table of (member, platform bit)
    # pairs: get_core_features zeroes the structure and assigns only the members
    # the table names, each when the platform mask carries its bit. Both the
    # table entries and any direct assignment inside the function count as
    # advertisable; anything else in VkPhysicalDeviceFeatures is never reported.
    core_table = re.search(r"core_feature_bits\[\]\s*=\s*\{(.*?)\n\};", device, re.DOTALL)
    if not core_body:
        failures.append("cannot read the core feature table from src/vk_device.c")
        core_features: set[str] = set()
    else:
        core_features = set(re.findall(r"features->([A-Za-z0-9_]+)\s*=", core_body.group(0)))
        if core_table:
            core_features |= set(re.findall(
                r"offsetof\(VkPhysicalDeviceFeatures,\s*([A-Za-z0-9_]+)\)", core_table.group(1)))
        if not core_features:
            failures.append("the core feature table in src/vk_device.c names no member")

    floor = re.search(r"PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR\s*=\s*(\d+)", header)
    if not floor:
        failures.append("cannot read PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR from src/vk_internal.h")
        max_views = 0
    else:
        max_views = int(floor.group(1))

    return ({"extensions": extensions, "features": features, "core_features": core_features,
             "max_multiview_view_count": max_views}, failures)


def _unadvertised(required: list[str], capabilities: dict) -> list[str]:
    """Return the prerequisites the device does not advertise."""
    missing: list[str] = []
    for token in required:
        kind, _, name = token.partition(":")
        if kind == "extension" and name not in capabilities["extensions"]:
            missing.append(token)
        elif kind == "feature" and not capabilities["features"].get(name):
            missing.append(token)
        elif kind == "core" and name not in capabilities["core_features"]:
            missing.append(token)
        elif kind not in ("extension", "feature", "core"):
            missing.append(token)
    return missing


def _multiview_attachment_contract(util_text: str, tests_text: str,
                                  family_types: dict) -> dict:
    """Derive the attachment image the selected multiview families build.

    A selection that only checks features and limits can still be unrunnable:
    the upstream helper decides which image a family needs, and the driver has
    to accept that exact resource. The derivation is bound to the factory
    branches the selection actually uses - the default colour-format branch and
    the non-multisample sample count - instead of matching tokens anywhere in
    the module, and anything that does not match returns nothing so the caller
    fails closed.
    """
    construction = (
        "VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO" in util_text and
        "VK_IMAGE_TILING_OPTIMAL" in util_text and
        "VK_SHARING_MODE_EXCLUSIVE" in util_text and
        "VK_IMAGE_LAYOUT_UNDEFINED" in util_text and
        "{extent.width, extent.height, 1u}" in util_text and
        "extent.depth" in util_text and
        "ImageAttachment::ImageAttachment" in tests_text and
        "imageUsageFlagsDependent" in tests_text and
        "makeImageCreateInfo(VK_IMAGE_TYPE_2D, extent, colorFormat, imageUsageFlags, samples)"
        in tests_text
    )
    if not construction or not family_types:
        return {}
    dependent = re.search(r"imageUsageFlagsDependent\s*=\s*(.*?);", tests_text, re.DOTALL)
    usage_tail = re.search(
        r"imageUsageFlags\s*=\s*imageUsageFlagsDependent\s*\|\s*(.*?);", tests_text, re.DOTALL)
    if not dependent or not usage_tail:
        return {}
    dependent_bits = set(re.findall(r"(VK_IMAGE_USAGE_[A-Z_]+_BIT)", dependent.group(1)))
    if dependent_bits != {"VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT",
                          "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT"}:
        return {}
    tail_bits = set(re.findall(r"(VK_IMAGE_USAGE_[A-Z_]+_BIT)", usage_tail.group(1)))
    if not tail_bits:
        return {}
    # The factory's colour-format and sample-count branches: a selected family
    # has to fall into the default colour branch and the non-multisample sample
    # count, otherwise its attachment is a different resource that needs its own
    # contract.
    default_format = re.search(r"else\s+colorFormat\s*=\s*(VK_FORMAT_[A-Z0-9_]+)\s*;", tests_text)
    samples_expr = re.search(
        r"sampleCountFlags\s*=\s*\(testType == TEST_TYPE_MULTISAMPLE\)\s*\?\s*"
        r"(VK_SAMPLE_COUNT_[A-Z0-9_]+)\s*:\s*(VK_SAMPLE_COUNT_[A-Z0-9_]+)\s*;", tests_text)
    special_formats = set(re.findall(r"colorFormat\s*=\s*(VK_FORMAT_[A-Z0-9_]+)\s*;", tests_text))
    if not default_format or not samples_expr or len(special_formats) < 2:
        return {}
    if any(test_type in ("TEST_TYPE_MULTISAMPLE", "TEST_TYPE_VIEW_MASK_ITERATION")
           for test_type in family_types.values()):
        return {}
    samples = SAMPLE_COUNT_FLAGS.get(samples_expr.group(2))
    if samples is None:
        return {}
    return {
        "format": default_format.group(1),
        "image_type": "VK_IMAGE_TYPE_2D",
        "tiling": "VK_IMAGE_TILING_OPTIMAL",
        "mip_levels": 1,
        "samples": samples,
        "array_layers": "extent.depth",
        "usage": sorted({"VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT"} | tail_bits),
    }


def _resource_witness() -> tuple[dict, list[str]]:
    """Run the real public-API fixture and read its measured contract witness.

    A contract is about a resource this driver either can or cannot create, so
    the answer comes from the driver itself: the fixture asks the public image-
    format query and really creates the image, and this gate reads that output
    instead of approximating the driver's accept/reject logic from its C text.
    """
    fixture = ROOT / "build/tests/dump_device_reporting"
    if not fixture.is_file():
        return {}, ["the resource witness fixture is missing; build it with make check"]
    try:
        completed = subprocess.run([str(fixture)], capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as error:
        return {}, [f"cannot run the resource witness fixture: {error}"]
    if completed.returncode != 0:
        return {}, [f"the resource witness fixture exited {completed.returncode}"]
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        return {}, [f"cannot parse the resource witness output: {error}"]
    witness = document.get("resourceContractWitness")
    if not isinstance(witness, dict) or not witness:
        return {}, ["the resource witness output carries no contract witness"]
    return witness, []


def _contract_verdict(contract_id: str, contract: dict, witnessed: dict,
                      required_layers: int) -> tuple[bool, list[str], str, str]:
    """Compare one declared contract with the measured witness, field by field.

    The witness is the public query/create answer for this host, i.e.
    IMPLEMENTATION READINESS - not physical-console evidence. Promoting a
    contract to resource_supported is a separate decision that needs a console
    witness, so readiness without that declaration is a valid promotion-pending
    state: it keeps every family diagnostic, it is reported rather than silently
    ignored, and it can never make anything eligible for acceptance.

    Readiness and the declared stages are all recomputed from primitives rather
    than trusted: the witness summary must agree with its own numbers, the
    execution stage derives from the requirement set, and the final state is the
    DECLARED resource stage AND the derived execution stage. Returns the
    eligibility for acceptance, the consistency failures, the note explaining a
    non-eligible contract, and the promotion-pending state when there is one.
    """
    failures: list[str] = []
    for field, measured in (("format", witnessed.get("formatName")),
                            ("image_type", witnessed.get("imageTypeName")),
                            ("tiling", witnessed.get("tilingName")),
                            ("mip_levels", witnessed.get("mipLevels")),
                            ("samples", witnessed.get("samples"))):
        if contract.get(field) != measured:
            failures.append(
                f"resource contract {contract_id!r} declares {field}="
                f"{contract.get(field)!r} while the measured witness says {measured!r}")
    if witnessed.get("arrayLayers") != required_layers:
        failures.append(
            f"resource contract {contract_id!r} is witnessed at "
            f"{witnessed.get('arrayLayers')} layers while the selection needs {required_layers}")
    if sorted(contract.get("usage", [])) != sorted(witnessed.get("usageNames", [])):
        failures.append(
            f"resource contract {contract_id!r} declares usage "
            f"{sorted(contract.get('usage', []))} while the measured witness used "
            f"{sorted(witnessed.get('usageNames', []))}")

    query_answered = witnessed.get("queryResult") == 0
    sample_flags = witnessed.get("querySampleCounts", 0)
    samples_ok = isinstance(sample_flags, int) and bool(sample_flags & int(contract.get("samples", 0)))
    layers_ok = witnessed.get("queryMaxArrayLayers", 0) >= required_layers
    mip_ok = witnessed.get("queryMaxMipLevels", 0) >= int(contract.get("mip_levels", 0))
    query_covers = query_answered and samples_ok and layers_ok and mip_ok
    create_ok = witnessed.get("createResult") == 0 and bool(witnessed.get("createSucceeded"))
    host_ready = query_covers and create_ok
    if bool(witnessed.get("queryCovers")) != query_covers or \
            bool(witnessed.get("supported")) != host_ready:
        failures.append(
            f"resource contract {contract_id!r}: the witness summary disagrees with its own "
            f"measurements (queryCovers={witnessed.get('queryCovers')}, "
            f"supported={witnessed.get('supported')})")
    if query_covers != create_ok:
        failures.append(
            f"resource contract {contract_id!r}: the image-format query and vkCreateImage "
            f"disagree about this shape (queryCovers={query_covers}, createSucceeded={create_ok})")

    # The declared resource stage is the hardware-evidence decision; the host
    # witness above is readiness for it. Claiming the stage without readiness is
    # an over-claim and fails closed; readiness without the stage is the
    # promotion-pending state, reported but never promotable.
    resource_declared = contract.get("resource_supported")
    if "resource_supported" not in contract:
        failures.append(f"resource contract {contract_id!r} declares no 'resource_supported' stage")
        resource_declared = None
    elif not isinstance(resource_declared, bool):
        failures.append(
            f"resource contract {contract_id!r} declares resource_supported="
            f"{resource_declared!r}, which is not a boolean")
        resource_declared = None
    elif resource_declared and not host_ready:
        failures.append(
            f"resource contract {contract_id!r} declares resource_supported=true while the "
            f"source query/create witness is not ready (queryCovers={query_covers}, "
            f"createSucceeded={create_ok})")

    # The resource stage is a physical-console claim, so it must carry the exact
    # well-formed record a console witness produces. Nothing else can promote it,
    # and evidence for an unpromoted stage fails closed in the other direction.
    evidence = contract.get("hardware_evidence")
    if resource_declared:
        if not isinstance(evidence, dict):
            failures.append(
                f"resource contract {contract_id!r} declares resource_supported=true without a "
                f"hardware_evidence object")
        else:
            unknown = sorted(set(evidence) - set(HARDWARE_EVIDENCE_FIELDS))
            if unknown:
                failures.append(
                    f"resource contract {contract_id!r} hardware evidence carries unknown fields: "
                    + ", ".join(unknown))
            for field in HARDWARE_EVIDENCE_FIELDS:
                if field not in evidence:
                    failures.append(
                        f"resource contract {contract_id!r} hardware evidence names no {field!r}")
            if not isinstance(evidence.get("source_commit"), str) or \
                    not COMMIT_ID.match(evidence.get("source_commit", "")):
                failures.append(
                    f"resource contract {contract_id!r} hardware evidence source_commit is not a "
                    f"40-character lowercase hex commit id")
            for field in ("artifact_sha256", "log_sha256"):
                if not isinstance(evidence.get(field), str) or not HEX_DIGEST.match(evidence.get(field, "")):
                    failures.append(
                        f"resource contract {contract_id!r} hardware evidence {field} is not a "
                        f"64-character lowercase hex digest")
            for field in ("run_id", "firmware", "title"):
                if not isinstance(evidence.get(field), str) or not evidence.get(field, "").strip():
                    failures.append(
                        f"resource contract {contract_id!r} hardware evidence {field} is empty")
            for field in ("query_result", "create_result", "bind_result"):
                if evidence.get(field) != "VK_SUCCESS":
                    failures.append(
                        f"resource contract {contract_id!r} hardware evidence {field} is not "
                        f"VK_SUCCESS")
            for field, minimum in (("query_max_array_layers", HARDWARE_EVIDENCE_REQUIRED_LAYERS),
                                   ("array_layers", HARDWARE_EVIDENCE_REQUIRED_LAYERS)):
                value = evidence.get(field)
                if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
                    failures.append(
                        f"resource contract {contract_id!r} hardware evidence {field} is not an "
                        f"integer at or above {minimum}")
            allocation = evidence.get("allocation_bytes")
            if isinstance(allocation, bool) or not isinstance(allocation, int) or allocation <= 0:
                failures.append(
                    f"resource contract {contract_id!r} hardware evidence allocation_bytes is not "
                    f"a positive integer")
            if evidence.get("teardown") != "clean":
                failures.append(
                    f"resource contract {contract_id!r} hardware evidence teardown is not clean")
    elif isinstance(evidence, dict):
        failures.append(
            f"resource contract {contract_id!r} carries hardware evidence while "
            f"resource_supported is false")

    requirements = contract.get("execution_requirements")
    derived: dict[str, bool] = {}
    if not isinstance(requirements, dict):
        failures.append(
            f"resource contract {contract_id!r} declares no execution_requirements object")
    else:
        unknown = sorted(set(requirements) - set(EXECUTION_REQUIREMENTS))
        if unknown:
            failures.append(
                f"resource contract {contract_id!r} declares unknown execution requirements: "
                + ", ".join(unknown))
        for key in EXECUTION_REQUIREMENTS:
            if key not in requirements:
                failures.append(
                    f"resource contract {contract_id!r} names no {key!r} execution requirement")
            elif not isinstance(requirements[key], bool):
                failures.append(
                    f"resource contract {contract_id!r} declares execution requirement {key}="
                    f"{requirements[key]!r}, which is not a boolean")
            else:
                derived[key] = requirements[key]
    derived_execution = (len(derived) == len(EXECUTION_REQUIREMENTS) and all(derived.values()))
    execution_declared = contract.get("execution_supported")
    if "execution_supported" not in contract:
        failures.append(f"resource contract {contract_id!r} declares no 'execution_supported' stage")
        execution_declared = None
    elif not isinstance(execution_declared, bool):
        failures.append(
            f"resource contract {contract_id!r} declares execution_supported="
            f"{execution_declared!r}, which is not a boolean")
        execution_declared = None
    elif execution_declared != derived_execution:
        failures.append(
            f"resource contract {contract_id!r} declares execution_supported="
            f"{execution_declared} while its execution requirements derive {derived_execution}")

    # The final state is the DECLARED resource stage AND the derived execution
    # stage. Host readiness never enters this expression, so no build can
    # promote a family by implementing a path the console has not witnessed.
    declared_supported = contract.get("supported")
    expected_supported = None
    if resource_declared is not None and execution_declared is not None:
        expected_supported = bool(resource_declared and derived_execution)
    if "supported" not in contract:
        failures.append(f"resource contract {contract_id!r} declares no final 'supported' field")
    elif not isinstance(declared_supported, bool):
        failures.append(
            f"resource contract {contract_id!r} declares supported="
            f"{declared_supported!r}, which is not a boolean")
    elif expected_supported is not None and declared_supported != expected_supported:
        failures.append(
            f"resource contract {contract_id!r} declares supported={declared_supported} while the "
            f"declared resource stage is {resource_declared} and the derived execution stage is "
            f"{derived_execution}")
    eligible = bool(expected_supported) and bool(declared_supported)

    host_reason = ""
    if not host_ready:
        details = []
        if not query_answered:
            details.append(f"the format query answers {witnessed.get('queryResult')}")
        else:
            if not layers_ok:
                details.append(
                    f"the query reports maxArrayLayers={witnessed.get('queryMaxArrayLayers')} "
                    f"while the selection needs {required_layers}")
            if not mip_ok:
                details.append(
                    f"the query reports maxMipLevels={witnessed.get('queryMaxMipLevels')} "
                    f"while the contract needs {contract.get('mip_levels')}")
            if not samples_ok:
                details.append(
                    f"the query reports sampleCounts={sample_flags} which does not cover "
                    f"{contract.get('samples')}")
        if not create_ok:
            details.append(f"vkCreateImage answers {witnessed.get('createResult')}")
        host_reason = "; ".join(details)

    # A promotion-pending contract is reported, never silently ignored: the host
    # can create the shape, but the stage that makes it acceptance is the
    # hardware-evidence decision this build has not made.
    pending = ""
    if host_ready and not resource_declared:
        pending = (f"resource contract {contract_id!r}: host readiness is true and "
                   f"resource_supported is false - promotion awaits the physical-console witness, "
                   f"so every family stays diagnostic")
    note = ""
    if not eligible:
        reasons = []
        if not host_ready:
            reasons.append(host_reason)
        if not resource_declared:
            reasons.append("resource_supported is not promoted (host readiness is not "
                           "physical-console evidence)")
        if not derived_execution:
            reasons.append("execution requirements are not all met")
        note = "; ".join(reason for reason in reasons if reason)
    return eligible, failures, note, pending


def main() -> int:
    manifest = json.loads(MANIFEST.read_text())
    # Diagnostics are frozen upstream cases that are executed but are known not
    # to pass yet; they are held to the same traceability rule as acceptance
    # cases so that a failing case cannot be relabelled from an invented name.
    cases = manifest["cases"] + manifest.get("diagnostics", [])

    duplicate_failures = _duplicate_selection_failures(manifest)
    revision_failures = _cts_revision_failures(manifest)

    if not UPSTREAM.is_dir():
        if duplicate_failures or revision_failures:
            print("upstream selection check failed:", file=sys.stderr)
            for failure in duplicate_failures + revision_failures:
                print(f"  {failure}", file=sys.stderr)
            return 1
        print("upstream vk-gl-cts checkout not present; selection check skipped")
        return 0

    failures = list(duplicate_failures) + revision_failures
    acceptance_paths = {case["path"] for case in manifest["cases"]}
    capabilities, capability_failures = _advertised_capabilities()
    failures.extend(capability_failures)
    # The resource footprint a family needs is derived from the pinned helper
    # that builds it, and a family may only be strict acceptance when this
    # driver can create that exact resource: features, limits and extensions
    # alone do not make a leaf runnable.
    contracts = manifest.get("resource_contracts", {})
    multiview_util_path = UPSTREAM / MULTIVIEW_UTIL_SOURCE
    multiview_test_path = UPSTREAM / MULTIVIEW_TEST_SOURCE
    multiview_derived: dict = {}
    contract_support: dict[str, bool] = {}
    contract_reasons: dict[str, str] = {}
    contract_pending: list[str] = []
    manifest_paths = {case["path"] for case in
                      manifest["cases"] + manifest.get("diagnostics", [])}
    if multiview_util_path.is_file() and multiview_test_path.is_file():
        tests_text = multiview_test_path.read_text(encoding="utf-8", errors="replace")
        util_text = multiview_util_path.read_text(encoding="utf-8", errors="replace")
        leaves = _multiview_leaf_requirements(
            tests_text, _source_function_at_line(tests_text, 4908))
        all_family_types: dict[str, str] = {}
        for leaf in leaves.values():
            all_family_types.setdefault(leaf["family"], leaf["test_type"])
        # Only the families the contracts claim are derived: a family the
        # selection does not use (a multisampled or view-mask-iteration one, for
        # example) builds a different attachment and must not decide this
        # contract.
        declared_families: set[str] = set()
        for declared_contract in contracts.values():
            declared_families.update(declared_contract.get("families", []))
        unknown_families = sorted(declared_families - set(all_family_types))
        for family in unknown_families:
            failures.append(
                f"resource contracts declare family {family!r}, which the pinned factory "
                f"does not build")
        family_types = {name: all_family_types[name] for name in sorted(declared_families)
                        if name in all_family_types}
        multiview_derived = _multiview_attachment_contract(util_text, tests_text, family_types)
        if not multiview_derived:
            failures.append(
                "cannot derive the multiview attachment contract from the pinned sources")
        witness, witness_failures = _resource_witness()
        failures.extend(witness_failures)
        for contract_id, declared in sorted(contracts.items()):
            if declared.get("derive_from") != MULTIVIEW_TEST_SOURCE:
                failures.append(
                    f"resource contract {contract_id!r} names no derivation this gate can read")
                continue
            if multiview_derived:
                for field in ("format", "image_type", "tiling", "mip_levels", "samples",
                              "array_layers", "usage"):
                    if declared.get(field) != multiview_derived.get(field):
                        failures.append(
                            f"resource contract {contract_id!r} field {field!r} does not match "
                            f"the attachment the pinned helper builds "
                            f"({multiview_derived.get(field)!r})")
            evidenced = witness.get(contract_id)
            if not isinstance(evidenced, dict):
                failures.append(
                    f"resource contract {contract_id!r} has no measured witness in the "
                    f"public-API fixture")
                continue
            families = set(declared.get("families", []))
            layers = [leaf["max_views"] for path, leaf in leaves.items()
                      if leaf["family"] in families and path in manifest_paths]
            required_layers = max(layers) if layers else 0
            if not required_layers:
                failures.append(
                    f"resource contract {contract_id!r} covers no selected leaf, so its "
                    f"ceiling cannot be checked")
                continue
            supported, verdict_failures, reason, pending = _contract_verdict(
                contract_id, declared, evidenced, required_layers)
            failures.extend(verdict_failures)
            contract_support[contract_id] = supported
            contract_reasons[contract_id] = reason
            if pending:
                contract_pending.append(pending)
    manifest_families: set[str] = set()
    manifest_contract_ids: set[str] = set()
    integration_text = INTEGRATION_SOURCE.read_text(encoding="utf-8")

    for case in cases:
        path = case["path"]
        source_ref = case["source"]
        source_parts = source_ref.rsplit(":", 1)
        source_path = UPSTREAM / source_parts[0]
        try:
            source_line = int(source_parts[1]) if len(source_parts) == 2 else 1
        except ValueError:
            source_line = 1
        segments = path.split(".")
        leaf = segments[-1]

        if not source_path.is_file():
            failures.append(f"{path}: cited source {source_ref} does not exist")
            continue

        text = source_path.read_text(encoding="utf-8", errors="replace")

        # Intermediate groups may come from the integration (package_ps5.cpp) or
        # from the upstream module tree rooted at the cited file's directory.
        module_root = source_path.parent
        tree_text = "\n".join(
            p.read_text(encoding="utf-8", errors="replace")
            for p in sorted(module_root.rglob("*.cpp"))
        )
        searchable = integration_text + "\n" + tree_text
        generated_format_segments = {
            token[len("VK_FORMAT_"):].lower()
            for token in re.findall(r"\bVK_FORMAT_[A-Z0-9_]+\b", text)
        }
        generated_segments = (
            _dynamic_state_compute_generated_segments(text)
            if source_path.name == "vktDynamicStateComputeTests.cpp" else set()
        )
        for segment in segments[1:-1]:
            if (not re.search(r'"' + re.escape(segment) + r'"', searchable) and
                    segment not in generated_segments and
                    segment not in generated_format_segments and
                    not (source_path.name == "vktMemoryMappingTests.cpp" and
                         _mapping_group_segment(text, segment))):
                failures.append(
                    f"{path}: group segment {segment!r} not produced by the "
                    f"integration or {module_root}")

        function_text = _source_function_at_line(text, source_line)

        # The multiview render factory composes every leaf from the
        # shader-family table, the two query names, the rendering types and the
        # fixed view-mask tables, so membership is checked against the whole
        # derived path: a real leaf moved under a family or query group that does
        # not own it would otherwise pass by naming literals that exist elsewhere
        # in the module. Acceptance additionally has to be runnable on the device
        # this repository builds, which the factory decides from the rendering
        # type, the shader-multiview features, the core features it names and the
        # case's own view extent - so an acceptance entry may not require
        # anything the device sources do not advertise. Diagnostics describe
        # known gaps and are exempt from that last rule.
        if source_path.name == "vktMultiViewRenderTests.cpp":
            derived_leaf = _multiview_leaf_requirements(text, function_text).get(path)
            if derived_leaf is None:
                failures.append(
                    f"{path}: not produced by the pinned multiview factory "
                    f"{source_ref}")
            elif path in acceptance_paths:
                manifest_families.add(derived_leaf["family"])
                contract_id = case.get("resource_contract")
                if not contract_id:
                    failures.append(
                        f"{path}: acceptance builds its attachment through the pinned helper "
                        f"but names no resource contract")
                elif contract_id not in contracts:
                    failures.append(
                        f"{path}: acceptance names unknown resource contract {contract_id!r}")
                    manifest_contract_ids.add(contract_id)
                else:
                    manifest_contract_ids.add(contract_id)
                    if not contract_support.get(contract_id, False):
                        failures.append(
                            f"{path}: acceptance needs final support for resource contract "
                            f"{contract_id!r} "
                            f"({contract_reasons.get(contract_id) or 'no measured witness'})")
                missing = _unadvertised(derived_leaf["required"], capabilities)
                if missing and path in acceptance_paths:
                    failures.append(
                        f"{path}: acceptance requires {', '.join(missing)}, which "
                        f"this device does not advertise")
                elif (path in acceptance_paths and
                      capabilities["max_multiview_view_count"] and
                      derived_leaf["max_views"] > capabilities["max_multiview_view_count"]):
                    failures.append(
                        f"{path}: acceptance needs maxMultiviewViewCount >= "
                        f"{derived_leaf['max_views']}; the reported floor is "
                        f"{capabilities['max_multiview_view_count']}")
            else:
                manifest_families.add(derived_leaf["family"])
                contract_id = case.get("resource_contract")
                if contract_id:
                    manifest_contract_ids.add(contract_id)
                    if contract_id not in contracts:
                        failures.append(
                            f"{path}: names unknown resource contract {contract_id!r}")
            continue

        # The leaf must be a literal name in the cited function/file, a bounded
        # table-derived name, or a number produced
        # by an instance factory whose parent group is a literal in that file.
        if re.search(r'"' + re.escape(leaf) + r'"', text):
            continue
        if leaf in _table_composed_leaf_names(text, function_text):
            continue
        # This factory's manifest citations point at individual registration
        # blocks inside one function, so the generic forward-only extractor
        # cannot recover the enclosing function. The recognizer itself is
        # bounded to the factory's exact construction expressions; apply it to
        # this one pinned source module only.
        if (source_path.name == "vktApiFillBufferTests.cpp" and
                leaf in _fill_update_generated_leaf_names(text)):
            continue
        # The copies/blits image-to-image factory composes its partial-image
        # leaves from the extent, format and clear tables inside the cited
        # function. Bounded to that factory's exact construction expressions.
        if (source_path.name == "vktApiCopiesAndBlittingTests.cpp" and
                leaf in _copy_and_blit_simple_image_leaf_names(function_text)):
            continue
        # The draw-parameter groups register their leaves through addDrawCase,
        # whose name only shows the flags passed there while the group's preset
        # flags are ORed in afterwards. Bounded to that factory's exact
        # construction and to the cited group block. Applied to this one module.
        if (source_path.name == "vktDrawShaderDrawParametersTests.cpp" and
                leaf in _draw_shader_draw_parameters_leaf_names(text, function_text)):
            continue
        if leaf.isdigit():
            continue
        # Format sub-groups are not written as literals: the upstream factories
        # register them with the lowercased format enum minus its "VK_FORMAT_"
        # prefix, so "r32_uint" traces back to VK_FORMAT_R32_UINT in the file.
        if leaf in {token[len("VK_FORMAT_"):].lower()
                    for token in re.findall(r"\bVK_FORMAT_[A-Z0-9_]+\b", text)}:
            continue
        # Shader-stage sub-groups are generated the same way from stage bit
        # literals (pipeline factories build names such as "compute_stage" or
        # "vertex_stage_fragment_stage" with de::toString-style joins).
        stages = set(re.findall(r"\bVK_SHADER_STAGE_([A-Z0-9_]+?)_BIT\b", text))
        derived = []
        if "COMPUTE" in stages:
            derived.append("compute_stage")
        if "VERTEX" in stages and "FRAGMENT" in stages:
            middle = ""
            if "GEOMETRY" in stages:
                middle += "_geometry_stage"
            if "TESSELLATION_CONTROL" in stages:
                middle += "_tessellation_control_stage"
            if "TESSELLATION_EVALUATION" in stages:
                middle += "_tessellation_evaluation_stage"
            derived.append("vertex_stage" + middle + "_fragment_stage")
        if leaf in derived:
            continue
        failures.append(
            f"{path}: leaf name {leaf!r} is not registered in {source_ref}")

    # The declared contracts must cover exactly the families the selection
    # contains. A family that appears without a contract would be a silent
    # expansion nothing has vetted; a contract for a family that is no longer
    # selected is stale. Both directions fail closed.
    declared_families: set[str] = set()
    for declared in contracts.values():
        declared_families.update(declared.get("families", []))
    if manifest_families or declared_families:
        uncovered = sorted(manifest_families - declared_families)
        stale = sorted(declared_families - manifest_families)
        if uncovered:
            failures.append(
                "selected multiview families with no declared resource contract: "
                + ", ".join(uncovered))
        if stale:
            failures.append(
                "resource contracts declare families the selection does not contain: "
                + ", ".join(stale))

    if failures:
        print("upstream selection check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    accepted = len(manifest["cases"])
    diagnostics = len(manifest.get("diagnostics", []))
    # A promotion-pending contract is reported, never silently ignored: the host
    # can create the shape, but the stage that makes it acceptance is a
    # hardware-evidence decision this build has not made.
    for note in contract_pending:
        print(f"pending: {note}")
    print(f"Upstream selection check passed: {len(cases)} cases traceable to sources "
          f"({accepted} acceptance, {diagnostics} diagnostic).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
