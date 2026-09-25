#!/usr/bin/env python3
"""Vulkan 1.1-1.3 device contract and the reported-apiVersion promotion gate.

conformance_inventory/core_version_contract.json holds, per core version:

* the registry-derived surface: global/instance/device commands of the
  version, their extension aliases, the mandatory feature requirements
  (<require><feature> blocks, with their `depends` conditions) and the
  extensions promoted into the version; and
* curated requirement rows (features, limits, query structures and promoted
  behaviours) with a status and the evidence behind it.

The physical device reports PS5VK_DEVICE_API_VERSION
(src/physical_device_profile.h). Raising it is one source edit, and this gate
refuses it unless, for every core version up to the reported one:

* every command of the version resolves by its core name through
  vkGetDeviceProcAddr/vkGetInstanceProcAddr (a core ENTRY in
  src/vk_dispatch.c, or a core_aliases row whose implementation has an ENTRY);
* every mandatory feature, promoted extension and curated requirement row is
  `satisfied` or `condition-false`, each with evidence; and
* the instance version (PS5VK_INSTANCE_API_VERSION) is not lower.

`--assume-version 1.N` evaluates the gate as if the switch were raised and
lists every unmet item without editing anything; `--derive` rewrites the
registry-derived parts from the pinned vk.xml and keeps the curated rows.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "conformance_inventory/core_version_contract.json"
VERSIONS = ("1.1", "1.2", "1.3")
STATUSES = ("satisfied", "condition-false", "missing", "blocker", "in-progress")
SATISFIED = ("satisfied", "condition-false")
LEVELS = ("global", "instance", "device")


def fail(message):
    raise AssertionError(message)


def require(condition, message):
    if not condition:
        fail(message)


def parse_version(text):
    match = re.fullmatch(r"1\.([0-9])", text)
    require(match, f"bad version {text!r}")
    return (1, int(match.group(1)))


def version_macro(source, macro):
    match = re.search(rf"#define\s+{macro}\s+VK_API_VERSION_1_([0-9])\b", source)
    require(match, f"{macro} must be defined as VK_API_VERSION_1_N")
    return (1, int(match.group(1)))


def derive_registry(root):
    """The per-version surface exactly as the pinned registry states it."""
    first_param, aliases = {}, {}
    for command in root.find("commands"):
        if command.get("alias"):
            aliases.setdefault(command.get("alias"), []).append(command.get("name"))
            continue
        params = command.findall("param")
        first_param[command.findtext("proto/name")] = (
            params[0].findtext("type") if params else None)

    def level(name):
        first = first_param.get(name)
        if first in ("VkInstance", "VkPhysicalDevice"):
            return "instance"
        if first in ("VkDevice", "VkQueue", "VkCommandBuffer"):
            return "device"
        return "global"

    derived = {version: {"commands": {level: [] for level in LEVELS},
                         "core_aliases": {}, "mandatory_features": [],
                         "promoted_extensions": []} for version in VERSIONS}
    for feature in root.findall("feature"):
        if "vulkan" not in feature.get("api", "").split(","):
            continue
        version = feature.get("number")
        if version not in derived:
            continue
        out = derived[version]
        for block in feature.findall("require"):
            for command in block.findall("command"):
                name = command.get("name")
                out["commands"][level(name)].append(name)
                if aliases.get(name):
                    out["core_aliases"][name] = sorted(aliases[name])
            names = sorted({node.get("name") for node in block.findall("feature")})
            for name in names:
                row = {"feature": name, "depends": block.get("depends")}
                if row not in out["mandatory_features"]:
                    out["mandatory_features"].append(row)
    for extension in root.find("extensions"):
        promoted = extension.get("promotedto", "")
        for version in VERSIONS:
            if promoted == "VK_VERSION_" + version.replace(".", "_"):
                derived[version]["promoted_extensions"].append(extension.get("name"))
    for version in VERSIONS:
        for level in LEVELS:
            derived[version]["commands"][level].sort()
        derived[version]["mandatory_features"].sort(
            key=lambda row: (row["feature"], row["depends"] or ""))
        derived[version]["promoted_extensions"].sort()
    return derived


def dispatch_surface(dispatch_source):
    """Names vkGet*ProcAddr can resolve: ENTRY rows plus core aliases whose
    implementation is itself an ENTRY."""
    entries = set(re.findall(r"ENTRY\((vk\w+),\s*(?:GLOBAL|INSTANCE|DEVICE)\)",
                             dispatch_source))
    aliases = dict(re.findall(r'\{\s*"(vk\w+)",\s*"(vk\w+)",\s*VK_API_VERSION_1_[0-9]\s*\}',
                              dispatch_source))
    return entries | {core for core, impl in aliases.items() if impl in entries}


def rows_by_id(version_data):
    rows = {}
    for row in version_data["requirements"]:
        require(row["id"] not in rows, f"duplicate requirement {row['id']}")
        rows[row["id"]] = row
    return rows


def check_shape(contract):
    require(contract.get("schema") == "ps5vk-core-version-contract/1", "unknown contract schema")
    require(set(contract["versions"]) == set(VERSIONS), "contract must cover 1.1, 1.2 and 1.3")
    for version in VERSIONS:
        data = contract["versions"][version]
        rows = rows_by_id(data)
        for row in data["requirements"]:
            require(row.get("status") in STATUSES, f"{version} {row['id']}: bad status")
            require(row.get("requirement"), f"{version} {row['id']}: requirement text missing")
            if row["status"] in SATISFIED:
                require(row.get("evidence"), f"{version} {row['id']}: {row['status']} without evidence")
        for feature in data["mandatory_features"]:
            require(f"feature:{feature['feature']}" in rows,
                    f"{version} mandatory feature {feature['feature']} has no requirement row")
        for extension in data["promoted_extensions"]:
            require(f"extension:{extension}" in rows,
                    f"{version} promoted extension {extension} has no requirement row")


def unmet(contract, target, surface):
    """Every item a device reporting `target` would be missing."""
    missing = []
    for version in VERSIONS:
        if parse_version(version) > target:
            continue
        data = contract["versions"][version]
        for level in LEVELS:
            for name in data["commands"][level]:
                if name not in surface:
                    missing.append(f"{version} command {name} ({level}) does not resolve by its core name")
        for row in data["requirements"]:
            if row["status"] not in SATISFIED:
                missing.append(f"{version} {row['id']}: {row['status']}")
    return missing


def check(root=ROOT, assume=None, contract=None, profile_source=None,
          internal_source=None, dispatch_source=None, registry=True):
    contract = contract or json.loads((root / "conformance_inventory/core_version_contract.json").read_text())
    check_shape(contract)
    if registry:
        path = root / contract["registry"]["path"]
        if path.is_file():
            actual = subprocess.check_output(
                ["git", "-C", str(path.parents[1]), "rev-parse", "HEAD"], text=True).strip()
            require(actual == contract["registry"]["commit"], "registry checkout is not pinned")
            derived = derive_registry(ET.parse(path).getroot())
            for version in VERSIONS:
                for key, value in derived[version].items():
                    require(contract["versions"][version][key] == value,
                            f"{version} {key} differs from the pinned registry; rerun --derive")
    profile_source = profile_source or (root / "src/physical_device_profile.h").read_text()
    internal_source = internal_source or (root / "src/vk_internal.h").read_text()
    dispatch_source = dispatch_source or (root / "src/vk_dispatch.c").read_text()
    reported = version_macro(profile_source, "PS5VK_DEVICE_API_VERSION")
    require("properties->apiVersion = PS5VK_DEVICE_API_VERSION;" in profile_source and
            "properties->apiVersion != PS5VK_DEVICE_API_VERSION" in profile_source,
            "the profile must report and validate PS5VK_DEVICE_API_VERSION")
    instance = version_macro(internal_source, "PS5VK_INSTANCE_API_VERSION")
    target = parse_version(assume) if assume else reported
    surface = dispatch_surface(dispatch_source)
    gaps = unmet(contract, target, surface)
    if instance < target:
        gaps.insert(0, f"PS5VK_INSTANCE_API_VERSION 1.{instance[1]} is lower than the device version")
    if gaps:
        fail(f"device apiVersion 1.{target[1]} is not backed by the core contract "
             f"({len(gaps)} unmet):\n  " + "\n  ".join(gaps))
    return reported


def derive(root=ROOT):
    contract = json.loads(CONTRACT.read_text())
    path = root / contract["registry"]["path"]
    derived = derive_registry(ET.parse(path).getroot())
    for version in VERSIONS:
        data = contract["versions"][version]
        data.update(derived[version])
        ids = {row["id"] for row in data["requirements"]}
        for feature in data["mandatory_features"]:
            if f"feature:{feature['feature']}" not in ids:
                data["requirements"].append({
                    "id": f"feature:{feature['feature']}", "kind": "feature",
                    "requirement": "mandatory" + (f" if {feature['depends']}" if feature["depends"] else ""),
                    "status": "missing"})
        for extension in data["promoted_extensions"]:
            if f"extension:{extension}" not in ids:
                data["requirements"].append({
                    "id": f"extension:{extension}", "kind": "behaviour",
                    "requirement": f"{extension} behaviour is core", "status": "missing"})
    CONTRACT.write_text(json.dumps(contract, indent=1) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="validate the reported version (default)")
    parser.add_argument("--assume-version", help="evaluate the gate for 1.N without editing sources")
    parser.add_argument("--derive", action="store_true", help="rewrite the registry-derived parts")
    args = parser.parse_args()
    if args.derive:
        derive()
    try:
        reported = check(assume=args.assume_version)
    except AssertionError as error:
        print(error, file=sys.stderr)
        return 1
    shown = args.assume_version or f"1.{reported[1]}"
    print(f"Core version contract: device apiVersion {shown} is backed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
