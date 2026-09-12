#!/usr/bin/env python3
"""Inventory the baseline implementation's entry-point surface.

Why this exists
---------------
An earlier revision of this inventory surveyed only the public header
``include/ps5vk/ps5vk.h`` and concluded that several entry points did not exist.
That was wrong: the implementation exports more entry points than the public
header declares, and some of them are registered in the dispatch table. Any
"missing" claim in ``requirements.json`` must therefore be derived from the
implementation, not from the public header alone.

This tool parses, at the pinned baseline commit:

* ``VKAPI_ATTR ... VKAPI_CALL <name>(`` definitions in ``src/*.c`` and ``native/*.c``
* ``ENTRY(<name>, GLOBAL|INSTANCE|DEVICE)`` rows in ``src/vk_dispatch.c``
* ``VKAPI_ATTR`` declarations in ``include/ps5vk/*.h``

and writes ``conformance_inventory/baseline_surface.json``. The ``observed``
notes are hand-written, verified statements about behaviour (for example that
extension enumeration succeeds with an empty list); they are part of the tool so
that the claim is reproducible and reviewable.

Usage
-----
    python3 conformance_inventory/tools/collect_baseline_surface.py
    python3 conformance_inventory/tools/collect_baseline_surface.py --repo /path/to/baseline
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INVENTORY_DIR = os.path.dirname(HERE)
DEFAULT_REPO = os.path.dirname(INVENTORY_DIR)
DEFAULT_OUT = os.path.join(INVENTORY_DIR, "baseline_surface.json")

DEFINITION_RE = re.compile(
    r"^\s*VKAPI_ATTR\s+[A-Za-z0-9_ \*]+VKAPI_CALL\s+(vk[A-Za-z0-9_]+|ps5vk[A-Za-z0-9_]+)\s*\(",
    re.MULTILINE,
)
DISPATCH_RE = re.compile(r"ENTRY\(\s*(vk[A-Za-z0-9_]+)\s*,\s*([A-Z]+)\s*\)")
DECLARATION_RE = re.compile(
    r"VKAPI_ATTR\s+[\w \*]+VKAPI_CALL\s+(vk\w+|ps5vk\w+)\s*\("
)

# Verified behaviour of specific entry points at the baseline. These are the
# statements that requirement rows are allowed to rely on.
OBSERVED = {
    "vkEnumerateInstanceExtensionProperties": "Exists and is dispatched (GLOBAL). Returns VK_SUCCESS with *count = 0; a non-NULL layer name returns VK_ERROR_LAYER_NOT_PRESENT. Evidence: src/vk_device.c.",
    "vkEnumerateInstanceLayerProperties": "Exists and is dispatched (GLOBAL). Returns VK_SUCCESS with *count = 0. Evidence: src/vk_device.c.",
    "vkEnumerateDeviceExtensionProperties": "Exists and is dispatched (INSTANCE); forwards to the instance enumeration, so it also reports zero extensions. Evidence: src/vk_device.c.",
    "vkCreateDevice": "Rejects any enabledExtensionCount with VK_ERROR_EXTENSION_NOT_PRESENT and rejects pNext/flags with VK_ERROR_FEATURE_NOT_PRESENT. Evidence: src/vk_device.c.",
    "vkGetInstanceProcAddr": "Exists and is dispatched (GLOBAL) over a fixed internal dispatch table. Evidence: src/vk_dispatch.c.",
    "vkGetDeviceProcAddr": "Exists and is dispatched (DEVICE) over the same table. Evidence: src/vk_dispatch.c.",
    "vkCreateInstance": "Accepts only apiVersion 1.0; other versions are rejected. Evidence: src/vk_device.c, src/platform_host.c.",
    "vkGetPhysicalDeviceProperties": "Reports apiVersion = VK_API_VERSION_1_0. Evidence: src/platform_host.c.",
    "vkGetPhysicalDeviceQueueFamilyProperties": "Reports a single queue family with queueCount = 1. Evidence: src/vk_device.c.",
    "vkCreateImage": "Exists, is dispatched (DEVICE) and is declared in the public header. Evidence: src/vk_memory.c, include/ps5vk/ps5vk.h.",
    "vkCreateSampler": "Exists, is dispatched (DEVICE) and is declared in the public header. Evidence: src/vk_sampler.c, include/ps5vk/ps5vk.h.",
    "vkCreateRenderPass": "Exists, is dispatched (DEVICE) and is declared in the public header. Evidence: src/vk_render_pass.c, include/ps5vk/ps5vk.h.",
    "vkCreateGraphicsPipelines": "Exists, is dispatched (DEVICE) and is declared in the public header. Evidence: src/vk_graphics_pipeline.c, include/ps5vk/ps5vk.h.",
    "vkCmdDraw": "Exists, is dispatched (DEVICE) and is declared in the public header. Evidence: src/vk_command.c, include/ps5vk/ps5vk.h.",
}


def git(repo: str, *args: str) -> str:
    return subprocess.check_output(["git", "-C", repo, *args], text=True).strip()


def collect(repo: str) -> dict:
    definitions: dict[str, list[str]] = {}
    for directory in ("src", "native"):
        root = os.path.join(repo, directory)
        for name in sorted(os.listdir(root)):
            if not name.endswith(".c"):
                continue
            path = os.path.join(root, name)
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for match in DEFINITION_RE.finditer(handle.read()):
                    definitions.setdefault(match.group(1), []).append("%s/%s" % (directory, name))

    dispatch: dict[str, str] = {}
    dispatch_path = os.path.join(repo, "src", "vk_dispatch.c")
    with open(dispatch_path, "r", encoding="utf-8") as handle:
        for match in DISPATCH_RE.finditer(handle.read()):
            dispatch[match.group(1)] = match.group(2)

    declared: set[str] = set()
    include_dir = os.path.join(repo, "include", "ps5vk")
    for name in sorted(os.listdir(include_dir)):
        if not name.endswith(".h"):
            continue
        with open(os.path.join(include_dir, name), "r", encoding="utf-8") as handle:
            declared.update(DECLARATION_RE.findall(handle.read()))

    entry_points = []
    for name in sorted(definitions):
        entry_points.append(
            {
                "name": name,
                "files": sorted(definitions[name]),
                "dispatch_scope": dispatch.get(name),
                "public_header": name in declared,
                "observed": OBSERVED.get(name),
            }
        )

    dirty = git(repo, "status", "--porcelain", "--", "src", "include", "native")
    if dirty:
        raise SystemExit(
            "implementation surface has uncommitted changes; refusing to publish a surface inventory:\n" + dirty
        )
    surface_commit = git(repo, "log", "-1", "--format=%H", "--", "src", "include", "native")

    return {
        "schema_version": "1.0",
        "baseline_commit": surface_commit,
        "inspected_commit": git(repo, "rev-parse", "HEAD"),
        "baseline_commit_note": "baseline_commit is the newest commit that touched src/, include/ or native/, which is the revision the surface below describes. inspected_commit is HEAD of the worktree used to collect it.",
        "method": "Parsed VKAPI_ATTR definitions in src/*.c and native/*.c, the ENTRY(name, SCOPE) dispatch table in src/vk_dispatch.c, and VKAPI_ATTR declarations in include/ps5vk/*.h at the pinned baseline commit. Regenerate with tools/collect_baseline_surface.py.",
        "reading_note": "public_header=false means the entry point is not declared in include/ps5vk/; it does not mean the entry point is missing. Symbols absent from this list are absent from the baseline implementation.",
        "counts": {
            "entry_points": len(entry_points),
            "dispatched": sum(1 for item in entry_points if item["dispatch_scope"]),
            "public_header": sum(1 for item in entry_points if item["public_header"]),
            "implementation_only": sum(1 for item in entry_points if not item["public_header"]),
        },
        "entry_points": entry_points,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=DEFAULT_REPO, help="checkout to inventory (must be at the baseline revision)")
    parser.add_argument("--out", default=DEFAULT_OUT)
    args = parser.parse_args(argv)

    surface = collect(args.repo)
    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(surface, handle, indent=2)
        handle.write("\n")
    os.replace(tmp, args.out)
    counts = surface["counts"]
    print(
        "wrote %s: %d entry points (%d dispatched, %d in the public header) at %s"
        % (args.out, counts["entry_points"], counts["dispatched"], counts["public_header"], surface["baseline_commit"][:12])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
