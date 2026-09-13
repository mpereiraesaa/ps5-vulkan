#!/usr/bin/env python3
"""Vulkan 1.0 mandatory command surface parity checker.

Audits parity across:
  1. Mandatory Vulkan 1.0 core commands in third_party/vulkan-headers/registry/vk.xml
  2. Exported public prototypes in include/ps5vk/ps5vk.h
  3. Dispatch table entries in src/vk_dispatch.c
  4. C function implementations in src/*.c and native/*.c

Fails closed on any drift:
  - Command declared in public header but not dispatched
  - Command in dispatch table but not implemented
  - Command implemented/dispatched but not declared in public header
  - Unexpected command count or regression removing any required bookkeeping command
"""

import argparse
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

EXPECTED_VULKAN10_TOTAL = 137
# Structural parity only: a command is counted here when it has a public
# prototype, a dispatch entry and an implementation. This is not a semantic
# support claim; see the report's advertised-obligation table for what each
# command may actually be used for.
EXPECTED_FULLY_WIRED_TOTAL = 137
EXPECTED_MISSING_TOTAL = 0

REQUIRED_DYNAMIC_STATE_COMMANDS = {
    "vkCmdSetLineWidth", "vkCmdSetDepthBias", "vkCmdSetDepthBounds",
    "vkCmdSetBlendConstants", "vkCmdSetStencilCompareMask",
    "vkCmdSetStencilWriteMask", "vkCmdSetStencilReference",
}

REQUIRED_BOOKKEEPING_COMMANDS = {
    "vkGetImageSubresourceLayout",
    "vkGetRenderAreaGranularity",
    "vkGetDeviceMemoryCommitment",
    "vkEnumerateDeviceLayerProperties",
    "vkResetDescriptorPool",
}

REQUIRED_SYNC_OBJECT_COMMANDS = {
    "vkCreateSemaphore", "vkDestroySemaphore",
    "vkCreateEvent", "vkDestroyEvent", "vkGetEventStatus",
    "vkSetEvent", "vkResetEvent",
    "vkCmdSetEvent", "vkCmdResetEvent", "vkCmdWaitEvents",
}

REQUIRED_BUFFER_TRANSFER_COMMANDS = {
    "vkCmdCopyBuffer", "vkCmdUpdateBuffer", "vkCmdFillBuffer",
}

REQUIRED_INDIRECT_COMMANDS = {
    "vkCmdDispatchIndirect", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect",
}

REQUIRED_QUERY_COMMANDS = {
    "vkGetQueryPoolResults", "vkCmdResetQueryPool", "vkCmdBeginQuery",
    "vkCmdEndQuery", "vkCmdCopyQueryPoolResults", "vkCmdWriteTimestamp",
}

REQUIRED_FAIL_CLOSED_COMMANDS = {
    "vkCmdNextSubpass", "vkCmdExecuteCommands", "vkQueueBindSparse",
    "vkCmdBlitImage", "vkCmdResolveImage", "vkCmdClearDepthStencilImage",
    "vkCmdClearAttachments",
}

EXPECTED_MISSING_CATEGORIES = {
    "Queries": set(),
    "Events": {
        "vkCreateEvent",
        "vkDestroyEvent",
        "vkGetEventStatus",
        "vkSetEvent",
        "vkResetEvent",
        "vkCmdSetEvent",
        "vkCmdResetEvent",
        "vkCmdWaitEvents",
    },
    "Semaphores": {
        "vkCreateSemaphore",
        "vkDestroySemaphore",
    },
    # The four pipeline-cache commands moved to the fully wired surface with the
    # header-only cache slice; they are no longer deficits.
    "Transfer/Clear/Indirect": set(),
    "Subpass/Secondary": set(),
    "Dynamic State": set(),
    "Sparse": set(),
}


def parse_core_commands(vk_xml_path: Path) -> list[str]:
    """Parse mandatory Vulkan 1.0 core commands from vk.xml."""
    tree = ET.parse(vk_xml_path)
    root = tree.getroot()
    core = set()
    for feat in root.findall("feature"):
        apis = (feat.get("api") or "").split(",")
        if feat.get("number") == "1.0" and "vulkan" in apis:
            for req in feat.findall("require"):
                for cmd in req.findall("command"):
                    name = cmd.get("name")
                    if name:
                        core.add(name)
    return sorted(core)


def parse_public_prototypes(header_path: Path) -> set[str]:
    """Extract public Vulkan function prototypes from ps5vk.h."""
    content = header_path.read_text()
    return set(re.findall(r"VKAPI_ATTR\s[^;]*?\b(vk[A-Za-z0-9_]+)\s*\(", content))


def parse_dispatch_entries(dispatch_path: Path) -> set[str]:
    """Extract dispatch table entries from vk_dispatch.c."""
    content = dispatch_path.read_text()
    return set(re.findall(r"ENTRY\((vk[A-Za-z0-9_]+)", content))


def parse_implementations(src_dir: Path, native_dir: Path | None = None) -> set[str]:
    """Extract C function definitions from source trees."""
    sources = list(src_dir.glob("*.c"))
    if native_dir and native_dir.is_dir():
        sources.extend(native_dir.glob("*.c"))
    impl = set()
    for s in sources:
        text = s.read_text()
        matches = re.findall(r"VKAPI_CALL\s+(vk[A-Za-z0-9_]+)\s*\(", text)
        impl.update(matches)
    return impl


def audit_command_surface(repo_root: Path) -> dict:
    """Run full command surface parity audit."""
    vk_xml = repo_root / "third_party/vulkan-headers/registry/vk.xml"
    header = repo_root / "include/ps5vk/ps5vk.h"
    dispatch_file = repo_root / "src/vk_dispatch.c"
    src_dir = repo_root / "src"
    native_dir = repo_root / "native"

    core_commands = parse_core_commands(vk_xml)
    public = parse_public_prototypes(header)
    dispatch = parse_dispatch_entries(dispatch_file)
    impl = parse_implementations(src_dir, native_dir)

    core_set = set(core_commands)
    fully_wired = {cmd for cmd in core_commands if cmd in public and cmd in dispatch and cmd in impl}
    missing = core_set - fully_wired

    # Asymmetry checks for 1.0 core commands
    core_public = public & core_set
    core_dispatch = dispatch & core_set
    core_impl = impl & core_set

    dispatch_not_public = sorted(core_dispatch - core_public)
    impl_not_public = sorted(core_impl - core_public)
    public_not_dispatch = sorted(core_public - core_dispatch)
    dispatch_not_impl = sorted(core_dispatch - core_impl)

    missing_required = sorted(
        (REQUIRED_BOOKKEEPING_COMMANDS | REQUIRED_SYNC_OBJECT_COMMANDS |
         REQUIRED_BUFFER_TRANSFER_COMMANDS | REQUIRED_INDIRECT_COMMANDS) - fully_wired
        | (REQUIRED_DYNAMIC_STATE_COMMANDS - fully_wired)
        | (REQUIRED_QUERY_COMMANDS - fully_wired)
        | (REQUIRED_FAIL_CLOSED_COMMANDS - fully_wired)
    )

    errors = []
    if len(core_commands) != EXPECTED_VULKAN10_TOTAL:
        errors.append(
            f"Vulkan 1.0 mandatory command count mismatch: expected {EXPECTED_VULKAN10_TOTAL}, got {len(core_commands)}"
        )
    if len(fully_wired) != EXPECTED_FULLY_WIRED_TOTAL:
        errors.append(
            f"Supported Vulkan 1.0 command count mismatch: expected {EXPECTED_FULLY_WIRED_TOTAL}, got {len(fully_wired)}"
        )
    if len(missing) != EXPECTED_MISSING_TOTAL:
        errors.append(
            f"Missing Vulkan 1.0 command count mismatch: expected {EXPECTED_MISSING_TOTAL}, got {len(missing)}"
        )
    if dispatch_not_public:
        errors.append(f"Commands in dispatch but not declared in public header: {dispatch_not_public}")
    if impl_not_public:
        errors.append(f"Commands implemented but not declared in public header: {impl_not_public}")
    if public_not_dispatch:
        errors.append(f"Commands declared in public header but not in dispatch table: {public_not_dispatch}")
    if dispatch_not_impl:
        errors.append(f"Commands in dispatch table but lacking C implementation: {dispatch_not_impl}")
    if missing_required:
        errors.append(f"Required bookkeeping commands missing from supported surface: {missing_required}")

    # Check categories of missing commands
    categorized_missing = set()
    for cat_cmds in EXPECTED_MISSING_CATEGORIES.values():
        categorized_missing.update(cat_cmds)
    uncategorized_missing = sorted(missing - categorized_missing)
    if uncategorized_missing:
        errors.append(f"Uncategorized missing commands: {uncategorized_missing}")

    return {
        "core_total": len(core_commands),
        "fully_wired_total": len(fully_wired),
        "missing_total": len(missing),
        "dispatch_not_public": dispatch_not_public,
        "impl_not_public": impl_not_public,
        "public_not_dispatch": public_not_dispatch,
        "dispatch_not_impl": dispatch_not_impl,
        "missing_required": missing_required,
        "uncategorized_missing": uncategorized_missing,
        "fully_wired": sorted(fully_wired),
        "missing": sorted(missing),
        "errors": errors,
        "passed": len(errors) == 0,
    }


def main():
    parser = argparse.ArgumentParser(description="Audit Vulkan 1.0 command surface parity")
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1],
                        help="Path to ps5vk repository root")
    parser.add_argument("--check", action="store_true", help="Exit non-zero if audit fails")
    args = parser.parse_args()

    result = audit_command_surface(args.repo_root)

    print("=== Vulkan 1.0 Command Surface Parity Audit ===")
    print(f"Mandatory Vulkan 1.0 core commands : {result['core_total']}")
    print(f"Fully wired (structural) commands  : {result['fully_wired_total']}")
    print(f"Known missing commands             : {result['missing_total']}")
    print(f"Required landed command families  : {'ALL PRESENT' if not result['missing_required'] else result['missing_required']}")
    print(f"Dispatched not public              : {result['dispatch_not_public'] or 'NONE'}")
    print(f"Implemented not public             : {result['impl_not_public'] or 'NONE'}")
    print(f"Public not dispatched              : {result['public_not_dispatch'] or 'NONE'}")
    print(f"Dispatched not implemented         : {result['dispatch_not_impl'] or 'NONE'}")

    if result["passed"]:
        print("\nResult: PASS (perfect 1:1 public/dispatch/implementation parity on the fully wired 1.0 surface; structural, not a semantic support claim)")
        return 0
    else:
        print("\nResult: FAIL")
        for err in result["errors"]:
            print(f"  - ERROR: {err}")
        return 1 if args.check else 0


if __name__ == "__main__":
    sys.exit(main())
