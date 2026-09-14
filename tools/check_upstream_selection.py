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
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "cts/upstream/manifest.json"
UPSTREAM = ROOT / "third_party/vk-gl-cts"
# The integration supplies the package and its leading groups; upstream supplies
# everything below them.
INTEGRATION_SOURCE = ROOT / "cts/upstream/package_ps5.cpp"


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


def main() -> int:
    manifest = json.loads(MANIFEST.read_text())
    # Diagnostics are frozen upstream cases that are executed but are known not
    # to pass yet; they are held to the same traceability rule as acceptance
    # cases so that a failing case cannot be relabelled from an invented name.
    cases = manifest["cases"] + manifest.get("diagnostics", [])

    duplicate_failures = _duplicate_selection_failures(manifest)

    if not UPSTREAM.is_dir():
        if duplicate_failures:
            print("upstream selection check failed:", file=sys.stderr)
            for failure in duplicate_failures:
                print(f"  {failure}", file=sys.stderr)
            return 1
        print("upstream vk-gl-cts checkout not present; selection check skipped")
        return 0

    failures = list(duplicate_failures)
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

    if failures:
        print("upstream selection check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    accepted = len(manifest["cases"])
    diagnostics = len(manifest.get("diagnostics", []))
    print(f"Upstream selection check passed: {len(cases)} cases traceable to sources "
          f"({accepted} acceptance, {diagnostics} diagnostic).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
