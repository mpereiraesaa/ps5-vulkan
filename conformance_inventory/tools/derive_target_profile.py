#!/usr/bin/env python3
"""Derive the declared target requirement set from the pinned Khronos profile file.

The Khronos registry ships machine-readable Vulkan profiles. At the pinned
registry revision the file ``registry/profiles/VP_KHR_roadmap.json`` defines
``VP_KHR_roadmap_2022``, ``VP_KHR_roadmap_2024`` and ``VP_KHR_roadmap_2026``.
Only the last one declares a Vulkan 1.4 API version, so it is the primary-source
basis for this project's "Vulkan 1.4 graphics" target.

Profile semantics implemented here:

* A capability entry that is a *list* is a one-of group: one of its members must
  be supported. Requirements common to every member of the group are treated as
  required, the varying part is recorded as the one-of group.
* A profile may inherit from another profile.

Output is deterministic: the file is written with sorted keys, so re-running it
against the same pinned bytes produces identical output.

Usage
-----
    python3 conformance_inventory/tools/derive_target_profile.py
    python3 conformance_inventory/tools/derive_target_profile.py --profile-file /path/to/VP_KHR_roadmap.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
INVENTORY_DIR = os.path.dirname(HERE)
DEFAULT_SOURCES = os.path.join(INVENTORY_DIR, "sources.json")
DEFAULT_OUT = os.path.join(INVENTORY_DIR, "target_profile.json")
PROFILE_SOURCE_ID = "khronos-vulkan-roadmap-profiles"
PROFILE_PATH = "registry/profiles/VP_KHR_roadmap.json"
SELECTED_PROFILE = "VP_KHR_roadmap_2026"


def load_profile_source(path: str) -> tuple[dict, dict]:
    with open(path, "r", encoding="utf-8") as handle:
        sources = json.load(handle)
    for source in sources.get("sources", []):
        if source.get("id") == PROFILE_SOURCE_ID:
            for artifact in source.get("artifacts", []):
                if artifact.get("path") == PROFILE_PATH:
                    return source, artifact
            raise SystemExit("source %s has no %s artifact" % (PROFILE_SOURCE_ID, PROFILE_PATH))
    raise SystemExit("sources.json has no %s entry" % PROFILE_SOURCE_ID)


def raw_url(repo: str, commit: str, path: str) -> str:
    owner = repo.rsplit("/", 2)[-2]
    name = repo.rsplit("/", 1)[-1]
    if name.endswith(".git"):
        name = name[: -len(".git")]
    return "https://raw.githubusercontent.com/%s/%s/%s/%s" % (owner, name, commit, path)


def load_bytes(source: dict, artifact: dict, local_path: str | None) -> tuple[bytes, str]:
    if local_path:
        with open(local_path, "rb") as handle:
            return handle.read(), local_path
    request = urllib.request.Request(
        raw_url(source["repo"], source["commit"], artifact["path"]), headers={"User-Agent": "ps5vk-cis-target"}
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read(), artifact["path"]


def expand(entries, caps, feats, exts, props, one_of, seen) -> None:
    for entry in entries:
        if isinstance(entry, list):
            one_of.append(sorted(entry))
            continue
        if entry in seen:
            continue
        seen.add(entry)
        capability = caps.get(entry)
        if capability is None:
            raise SystemExit("profile references unknown capability %r" % entry)
        for struct, members in (capability.get("features") or {}).items():
            feats.setdefault(struct, {}).update(members)
        for struct, members in (capability.get("properties") or {}).items():
            props.setdefault(struct, {}).update(members)
        exts.update(capability.get("extensions") or {})


def expand_profile(profiles: dict, caps: dict, name: str, feats=None, exts=None, props=None, one_of=None):
    feats = {} if feats is None else feats
    exts = {} if exts is None else exts
    props = {} if props is None else props
    one_of = [] if one_of is None else one_of
    profile = profiles[name]
    for parent in profile.get("profiles", []):
        expand_profile(profiles, caps, parent, feats, exts, props, one_of)
    expand(profile["capabilities"], caps, feats, exts, props, one_of, set())
    return feats, exts, props, one_of


def bits_of(feats: dict) -> list[str]:
    return sorted({bit for struct in feats for bit, value in feats[struct].items() if value is True})


def expand_capability(caps: dict, name: str) -> tuple[list[str], dict]:
    feats: dict = {}
    exts: dict = {}
    props: dict = {}
    expand([name], caps, feats, exts, props, [], set())
    return bits_of(feats), exts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sources", default=DEFAULT_SOURCES)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--profile-file", help="local copy of the pinned profile file")
    args = parser.parse_args(argv)

    source, artifact = load_profile_source(args.sources)
    data, origin = load_bytes(source, artifact, args.profile_file)
    digest = hashlib.sha256(data).hexdigest()
    if digest != artifact["sha256"]:
        raise SystemExit(
            "stale/incompatible profile pin: %s has sha256 %s, sources.json records %s" % (origin, digest, artifact["sha256"])
        )

    document = json.loads(data.decode("utf-8"))
    profiles = document["profiles"]
    caps = document["capabilities"]
    if SELECTED_PROFILE not in profiles:
        raise SystemExit("pinned profile file does not define %s" % SELECTED_PROFILE)

    def summarise(name: str) -> dict:
        feats, exts, props, one_of = expand_profile(profiles, caps, name)
        return {
            "profile": name,
            "api_version": profiles[name].get("api-version"),
            "inherits": profiles[name].get("profiles", []),
            "feature_bits": len(bits_of(feats)),
            "extensions": len(exts),
            "one_of_groups": len(one_of),
        }

    feats, exts, props, one_of = expand_profile(profiles, caps, SELECTED_PROFILE)

    # Resolve one-of groups: anything required by every alternative is promoted to
    # the flat required sets; the remainder stays an explicit one-of group.
    resolved_one_of = []
    promoted_exts: dict[str, int] = {}
    promoted_bits: list[str] = []
    for group in one_of:
        alternatives = [expand_capability(caps, member) for member in group]
        common_bits = set.intersection(*[set(bits) for bits, _ in alternatives]) if alternatives else set()
        common_exts = set.intersection(*[set(e) for _, e in alternatives]) if alternatives else set()
        promoted_bits.extend(sorted(common_bits))
        for name in sorted(common_exts):
            promoted_exts[name] = 1
        resolved_one_of.append(
            {
                "capabilities": group,
                "required_in_every_alternative": {
                    "feature_bits": sorted(common_bits),
                    "extensions": sorted(common_exts),
                },
                "alternatives": [
                    {
                        "capability": member,
                        "feature_bits": [bit for bit in bits if bit not in common_bits],
                        "extensions": [name for name in sorted(ext) if name not in common_exts],
                    }
                    for member, (bits, ext) in zip(group, alternatives)
                ],
            }
        )

    required_bits = sorted(set(bits_of(feats)) | set(promoted_bits))
    required_exts = dict(exts)
    required_exts.update(promoted_exts)

    output = {
        "schema_version": "1.0",
        "target": {
            "id": "ps5vk-vulkan14-graphics",
            "statement": "Declared project target: a graphics-capable Vulkan 1.4 implementation. This is a target, not a claim of support.",
            "basis": {
                "source_id": source["id"],
                "path": PROFILE_PATH,
                "profile": SELECTED_PROFILE,
                "api_version": profiles[SELECTED_PROFILE].get("api-version"),
                "sha256": digest,
            },
            "why_this_profile": "It is the only roadmap profile in the pinned registry revision whose api-version is 1.4.x; the 2024 and 2022 profiles declare 1.3.x and are recorded for reference only.",
            "other_profiles_in_file": [summarise(name) for name in sorted(profiles) if name != SELECTED_PROFILE],
        },
        "required_feature_bits": required_bits,
        "required_extensions": sorted(required_exts),
        "required_properties": props,
        "one_of_groups": resolved_one_of,
        "core_mandatory_from_spec_1_4": {
            "note": "Feature bits that Vulkan 1.4 itself makes mandatory. Transcribed from the pinned specification appendix (anchor versions-1.4-new-features); this list is not derived from the profile file.",
            "anchor": "versions-1.4-new-features",
            "feature_bits": [
                "fullDrawIndexUint32",
                "imageCubeArray",
                "independentBlend",
                "sampleRateShading",
                "drawIndirectFirstInstance",
                "depthClamp",
                "depthBiasClamp",
                "samplerAnisotropy",
                "fragmentStoresAndAtomics",
                "shaderStorageImageExtendedFormats",
                "shaderUniformBufferArrayDynamicIndexing",
                "shaderSampledImageArrayDynamicIndexing",
                "shaderStorageBufferArrayDynamicIndexing",
                "shaderStorageImageArrayDynamicIndexing",
                "shaderImageGatherExtended",
                "shaderInt16",
                "largePoints",
                "samplerYcbcrConversion",
                "storageBuffer16BitAccess",
                "variablePointers",
                "variablePointersStorageBuffer",
                "samplerMirrorClampToEdge",
                "scalarBlockLayout",
                "shaderUniformTexelBufferArrayDynamicIndexing",
                "shaderStorageTexelBufferArrayDynamicIndexing",
                "shaderInt8",
                "storageBuffer8BitAccess",
            ],
        },
        "limits_note": "Profiles express required limits and properties per struct; required_properties above is the machine-readable form. Values in the specification's own limits table are separate obligations recorded in requirements.json.",
    }

    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(output, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(tmp, args.out)
    print(
        "wrote %s: %s api-version %s, %d required feature bits, %d required extensions, %d one-of groups"
        % (
            args.out,
            SELECTED_PROFILE,
            profiles[SELECTED_PROFILE].get("api-version"),
            len(required_bits),
            len(required_exts),
            len(resolved_one_of),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
