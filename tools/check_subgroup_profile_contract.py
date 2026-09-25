#!/usr/bin/env python3
"""Check the source-derived subgroup prerequisites and the current public gate."""
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "conformance_inventory/subgroup_profile_contract.json"
MATRIX = ROOT / "conformance_inventory/dxvk_v262_matrix.json"
REPORT = ROOT / "conformance_inventory/reporting_matrix.json"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_registry(root, contract):
    for version, feature in (("11", "VK_BASE_VERSION_1_1"),
                             ("12", "VK_BASE_VERSION_1_2")):
        command_gate = contract[f"api{version}_command_gate"]
        require(command_gate["registry_feature"] == feature,
                f"Vulkan 1.{version[-1]} command gate feature changed")
        base = next(node for node in root.findall("feature")
                    if node.get("name") == feature)
        core_commands = [command.get("name") for requirement in base.findall("require")
                         if "vulkan" in requirement.get("api", "vulkan").split(",")
                         for command in requirement.findall("command")]
        require(core_commands == command_gate["core_commands"],
                f"Vulkan 1.{version[-1]} core command census changed")
    extensions = {node.get("name"): node for node in root.findall(".//extensions/extension")}
    extended = extensions["VK_KHR_shader_subgroup_extended_types"]
    route = contract["routes"]["shaderSubgroupExtendedTypes"]
    require(extended.get("depends") == "VK_VERSION_1_1", "extended types extension API dependency changed")
    require(extended.get("promotedto") == "VK_VERSION_1_2", "extended types promotion changed")
    require(route["extension_min_api"] == "1.1" and route["core_api"] == "1.2",
            "extended types route disagrees with registry")
    require(extended.find(".//feature[@name='shaderSubgroupExtendedTypes']") is not None,
            "extended types extension feature missing")
    float16_int8 = extensions["VK_KHR_shader_float16_int8"]
    prerequisite = contract["cts_float16_int8_prerequisite"]
    require(prerequisite["extension"] == "VK_KHR_shader_float16_int8" and
            prerequisite["registry_depends"] == float16_int8.get("depends") ==
            "VK_KHR_get_physical_device_properties2,VK_VERSION_1_1" and
            float16_int8.get("promotedto") == "VK_VERSION_1_2" and
            prerequisite["core_api"] == "1.2" and
            prerequisite["current_public"] is False,
            "CTS float16/int8 prerequisite API dependency changed")
    require(not any(node.find(".//feature[@name='subgroupBroadcastDynamicId']") is not None
                    for node in extensions.values()), "dynamic ID extension route appeared")
    require(contract["routes"]["subgroupBroadcastDynamicId"]["extension"] is None,
            "dynamic ID has no extension alias at this pin")
    version12 = next(node for node in root.findall("feature") if node.get("name") == "VK_VERSION_1_2")
    for name in ("shaderSubgroupExtendedTypes", "subgroupBroadcastDynamicId"):
        require(version12.find(f".//feature[@name='{name}'][@struct='VkPhysicalDeviceVulkan12Features']")
                is not None, f"{name} missing from Vulkan 1.2")
    types = {node.get("name"): node for node in root.findall(".//types/type")}
    subgroup = types["VkPhysicalDeviceSubgroupProperties"]
    require(subgroup.get("structextends") == "VkPhysicalDeviceProperties2",
            "subgroup properties query chain changed")
    fields = {member.findtext("name") for member in subgroup.findall("member")}
    require(set(contract["properties"]["fields"]) <= fields, "subgroup property fields changed")
    extended_struct = types["VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures"]
    require(extended_struct.get("structextends") == "VkPhysicalDeviceFeatures2,VkDeviceCreateInfo",
            "extended types feature query/create chain changed")


def check_cts(utils, broadcast, arithmetic, contract):
    require(contract["cts_extended_type_extension_gate"] ==
            ["VK_KHR_shader_subgroup_extended_types", "VK_KHR_shader_float16_int8"],
            "CTS extension gate changed")
    require(contract["cts_float16_int8_prerequisite"]["extension"] in
            contract["cts_extended_type_extension_gate"],
            "CTS float16/int8 prerequisite missing from extension gate")
    for extension in contract["cts_extended_type_extension_gate"]:
        require(f'context.isDeviceFunctionalitySupported("{extension}")' in utils,
                f"CTS extension gate missing: {extension}")
    require("context.contextSupports(vk::ApiVersion(0, 1, 1, 0))" in utils,
            "CTS subgroup API gate changed")
    require("context.contextSupports(vk::ApiVersion(0, 1, 2, 0))" in utils and
            "getPhysicalDeviceVulkan12Features" in utils,
            "CTS dynamic ID API gate changed")
    require("supportedStages & shaderStages" in utils and
            "VK_SHADER_STAGE_COMPUTE_BIT" in utils,
            "CTS stage rule changed")
    stages = contract["cts_stages"]
    require(set(stages["framebuffer"]) == {
        "VK_SHADER_STAGE_VERTEX_BIT", "VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT",
        "VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT", "VK_SHADER_STAGE_GEOMETRY_BIT"},
        "CTS framebuffer stage matrix changed")
    for stage in stages["framebuffer"] + stages["optional"] + ["VK_SHADER_STAGE_ALL_GRAPHICS"]:
        require(stage in broadcast, f"CTS stage factory missing: {stage}")
    require("is8BitUBOStorageSupported(context)" in broadcast and
            "is16BitUBOStorageSupported(context)" in broadcast and
            "requiredSubgroupSizeStages" in broadcast,
            "CTS optional storage or subgroup-size gate changed")
    require("supportedOperations" in utils and
            "VK_SUBGROUP_FEATURE_BALLOT_BIT" in broadcast,
            "CTS ballot property gate changed")
    require(contract["properties"]["arithmetic_operation"] ==
            "VK_SUBGROUP_FEATURE_ARITHMETIC_BIT" and
            "isSubgroupFeatureSupportedForDevice(context, VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)" in arithmetic and
            "isSubgroupSupported(context)" in arithmetic and
            "isFormatSupportedForDevice(context, caseDef.format)" in arithmetic and
            "is8BitUBOStorageSupported(context)" in arithmetic and
            "is16BitUBOStorageSupported(context)" in arithmetic and
            "getAllFormats()" in arithmetic,
            "CTS arithmetic operation or format gate changed")
    require("OPTYPE_BROADCAST_NONCONST" in broadcast and
            "isSubgroupBroadcastDynamicIdSupported(context)" in broadcast and
            "SPIRV_VERSION_1_5" in broadcast,
            "CTS dynamic broadcast oracle changed")
    expected = {
        "8-bit signed/unsigned integer scalar and vec2/3/4":
            ["shaderSubgroupExtendedTypes", "shaderInt8", "storageBuffer8BitAccess"],
        "16-bit signed/unsigned integer scalar and vec2/3/4":
            ["shaderSubgroupExtendedTypes", "shaderInt16", "storageBuffer16BitAccess"],
        "16-bit float scalar and vec2/3/4":
            ["shaderSubgroupExtendedTypes", "shaderFloat16", "storageBuffer16BitAccess"],
        "64-bit signed/unsigned integer scalar and vec2/3/4":
            ["shaderSubgroupExtendedTypes", "shaderInt64"],
        "64-bit float scalar and vec2/3/4": ["shaderFloat64"],
    }
    require(contract["cts_formats"] == expected, "CTS format matrix changed")
    for features in expected.values():
        require(" && ".join(features) in utils, f"CTS format gate missing: {features}")
    require("getAllFormats()" in broadcast, "CTS broadcast format factory changed")


def check_reporting(contract, report, matrix, profile_source, device_source,
                    dispatch_source):
    current = contract["current"]
    require(current["api"] == "1.0", "contract must describe current Vulkan 1.0 report")
    require(current["shaderSubgroupExtendedTypes"] is False and
            current["subgroupBroadcastDynamicId"] is False, "contract enables subgroup bits")
    for profile in ("compute", "graphics"):
        require(report["profiles"][profile]["apiVersion"] == 4194304,
                f"{profile} public API version changed")
    require("#define PS5VK_DEVICE_API_VERSION VK_API_VERSION_1_0\n" in profile_source and
            "properties->apiVersion = PS5VK_DEVICE_API_VERSION;" in profile_source,
            "source API version changed; re-audit subgroup profile")
    exported = set(re.findall(r"ENTRY\((vk\w+),\s*(?:GLOBAL|INSTANCE|DEVICE)\)",
                              dispatch_source))
    for version, count in (("11", 21), ("12", 7)):
        command_gate = contract[f"api{version}_command_gate"]
        commands = command_gate["core_commands"]
        require(len(commands) == count and len(set(commands)) == count,
                f"Vulkan 1.{version[-1]} core command census is incomplete")
        require(sorted(exported.intersection(commands)) ==
                command_gate["current_core_dispatch"],
                f"Vulkan 1.{version[-1]} core command dispatch changed; re-audit API gate")
    require("VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES" not in device_source and
            "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES" not in device_source,
            "new subgroup query route requires contract review")
    marker = "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES"
    if marker in device_source:
        start = device_source.find("vkGetPhysicalDeviceProperties2KHR(")
        end = device_source.find("VKAPI_ATTR", start + 1)
        query = device_source[start:end if end >= 0 else None]
        require(start >= 0 and device_source.count(marker) == 1 and
                f"if (next->sType == {marker})" in query and
                "VkPhysicalDeviceSubgroupProperties *properties =" in query,
                "new subgroup query route requires contract review")
        assignments = re.findall(
            r"properties->(subgroupSize|supportedStages|supportedOperations|"
            r"quadOperationsInAllStages)\s*=\s*([^;]+);", device_source)
        require(assignments == [("subgroupSize", "0u"),
                                ("supportedStages", "0u"),
                                ("supportedOperations", "0u"),
                                ("quadOperationsInAllStages", "VK_FALSE")],
                "new subgroup query route requires contract review")
    rows = {row["id"]: row for row in matrix["requirements"]}
    for name in ("shaderSubgroupExtendedTypes", "subgroupBroadcastDynamicId"):
        row = rows[f"feature:VkPhysicalDeviceVulkan12Features:{name}"]
        require(row["api"]["state"] == "blocker" and row["verdict"] == "blocker" and
                row["cts"]["state"] == "not-mapped" and
                row["implementation"]["state"] == "missing",
                f"{name} matrix was promoted without subgroup contract review")


def check_core_sources(contract, public_source, implementation_source):
    public = set(re.findall(r"VKAPI_ATTR\s[^;]*?\b(vk[A-Za-z0-9_]+)\s*\(",
                            public_source))
    implemented = set(re.findall(r"VKAPI_CALL\s+(vk[A-Za-z0-9_]+)\s*\(",
                                 implementation_source))
    for version in ("11", "12"):
        gate = contract[f"api{version}_command_gate"]
        commands = set(gate["core_commands"])
        require(sorted(public & commands) == gate["current_public_prototypes"],
                f"Vulkan 1.{version[-1]} core public prototypes changed; re-audit API gate")
        require(sorted(implemented & commands) == gate["current_implementations"],
                f"Vulkan 1.{version[-1]} core implementations changed; re-audit API gate")


def check(root=ROOT):
    contract = json.loads((root / "conformance_inventory/subgroup_profile_contract.json").read_text())
    manifest = json.loads((root / "cts/upstream/manifest.json").read_text())
    require(manifest["cts_pin"]["commit"] == contract["cts"]["commit"], "CTS pin changed")
    headers_tool = (root / "tools/prepare_vulkan_headers.py").read_text()
    require(f'PIN = "{contract["registry"]["commit"]}"' in headers_tool, "registry pin changed")
    registry_path = root / contract["registry"]["path"]
    if registry_path.is_file():
        actual = subprocess.check_output(
            ["git", "-C", str(registry_path.parents[1]), "rev-parse", "HEAD"], text=True).strip()
        require(actual == contract["registry"]["commit"], "registry checkout is not pinned")
        check_registry(ET.parse(registry_path).getroot(), contract)
    cts_root = root / "third_party/vk-gl-cts"
    utils = cts_root / contract["cts"]["utility"]
    broadcast = cts_root / contract["cts"]["broadcast"]
    arithmetic = cts_root / contract["cts"]["arithmetic"]
    if utils.is_file() and broadcast.is_file() and arithmetic.is_file():
        actual = subprocess.check_output(
            ["git", "-C", str(cts_root), "rev-parse", "HEAD"], text=True).strip()
        require(actual == contract["cts"]["commit"], "CTS checkout is not pinned")
        check_cts(utils.read_text(), broadcast.read_text(), arithmetic.read_text(), contract)
    report = json.loads((root / "conformance_inventory/reporting_matrix.json").read_text())
    matrix = json.loads((root / "conformance_inventory/dxvk_v262_matrix.json").read_text())
    check_reporting(contract, report, matrix,
                    (root / "src/physical_device_profile.h").read_text(),
                    (root / "src/vk_device.c").read_text(),
                    (root / "src/vk_dispatch.c").read_text())
    sources = list((root / "src").glob("*.c")) + list((root / "native").glob("*.c"))
    check_core_sources(contract, (root / "include/ps5vk/ps5vk.h").read_text(),
                       "\n".join(path.read_text() for path in sources))


if __name__ == "__main__":
    check()
    print("Subgroup profile contract: current public bits and API remain off")
