#!/usr/bin/env python3
"""Derive the pinned DXVK v2.6.2 D3D11 FL11_0 requirement surface.

The upstream Vulkan Profiles document is fetched only for an explicit refresh
or supplied with ``--source``.  Normal offline checks validate the checked-in
derivative and its immutable source identity; third-party source is never
vendored into this repository.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "conformance_inventory/sources.json"
OUTPUT = ROOT / "conformance_inventory/dxvk_v262_profile.json"
HEADER = ROOT / "examples/native_consumer/dxvk_v262_profile.h"
SOURCE_ID = "dxvk"
PROFILE_ID = "VP_DXVK_d3d11_level_11_0_baseline"
SCHEMA = "ps5vk-dxvk-profile/1"


def canonical_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def git_blob_sha1(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data).hexdigest()  # noqa: S324 - Git identity


def source_pin() -> tuple[dict, dict]:
    document = json.loads(SOURCES.read_text())
    source = next((item for item in document["sources"]
                   if item.get("id") == SOURCE_ID), None)
    if source is None:
        raise ValueError(f"{SOURCES}: missing source id {SOURCE_ID!r}")
    artifact = next((item for item in source.get("artifacts", [])
                     if item.get("path") == "VP_DXVK_requirements.json"), None)
    if artifact is None:
        raise ValueError(f"{SOURCES}: DXVK profile artifact is not pinned")
    if artifact.get("profile_id") != PROFILE_ID:
        raise ValueError("pinned DXVK profile id changed")
    return source, artifact


def verify_source(data: bytes, artifact: dict) -> None:
    checks = {
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "git_blob_sha1": git_blob_sha1(data),
    }
    for field, actual in checks.items():
        if artifact.get(field) != actual:
            raise ValueError(
                f"DXVK source {field} mismatch: expected {artifact.get(field)!r}, "
                f"got {actual!r}")


def walk_properties(prefix: str, value: object):
    if isinstance(value, dict):
        for key, child in value.items():
            yield from walk_properties(f"{prefix}.{key}" if prefix else key, child)
    else:
        yield prefix, value


def derive(upstream: dict, source: dict, artifact: dict) -> dict:
    profiles = upstream.get("profiles", {})
    profile = profiles.get(PROFILE_ID)
    if not isinstance(profile, dict):
        raise ValueError(f"upstream document has no {PROFILE_ID}")
    capabilities = profile.get("capabilities")
    if not isinstance(capabilities, list) or not capabilities:
        raise ValueError("DXVK profile has no ordered capability list")

    merged: dict[str, dict] = {}

    def add(kind: str, container: str | None, name: str,
            expected: object, capability: str) -> None:
        identifier = ":".join(part for part in (kind, container, name) if part)
        row = {
            "id": identifier,
            "kind": kind,
            "container": container,
            "name": name,
            "expected": expected,
            "capabilities": [capability],
        }
        previous = merged.get(identifier)
        if previous is None:
            merged[identifier] = row
            return
        if previous["expected"] != expected:
            raise ValueError(f"conflicting values for {identifier}")
        if capability not in previous["capabilities"]:
            previous["capabilities"].append(capability)

    add("api-version", None, "apiVersion", profile.get("api-version"), "profile")
    upstream_capabilities = upstream.get("capabilities", {})
    for capability in capabilities:
        body = upstream_capabilities.get(capability)
        if not isinstance(body, dict):
            raise ValueError(f"profile references missing capability {capability!r}")
        for name, version in body.get("extensions", {}).items():
            add("extension", None, name, version, capability)
        for structure, members in body.get("features", {}).items():
            if not isinstance(members, dict):
                raise ValueError(f"features.{structure} is not an object")
            for name, expected in members.items():
                add("feature", structure, name, expected, capability)
        for structure, members in body.get("properties", {}).items():
            if not isinstance(members, dict):
                raise ValueError(f"properties.{structure} is not an object")
            for path, expected in walk_properties("", members):
                add("property", structure, path, expected, capability)

    order = {"api-version": 0, "extension": 1, "feature": 2, "property": 3}
    requirements = sorted(merged.values(), key=lambda row: (
        order[row["kind"]], row.get("container") or "", row["name"]))
    counts = {kind: sum(row["kind"] == kind for row in requirements)
              for kind in order}
    return {
        "schema": SCHEMA,
        "source": {
            "source_id": source["id"],
            "tag": source["tag"],
            "tag_object": source["tag_object"],
            "commit": source["commit"],
            "artifact": artifact["path"],
            "artifact_size_bytes": artifact["size_bytes"],
            "artifact_sha256": artifact["sha256"],
            "artifact_git_blob_sha1": artifact["git_blob_sha1"],
        },
        "profile": {
            "id": PROFILE_ID,
            "version": profile.get("version"),
            "api_version": profile.get("api-version"),
            "label": profile.get("label"),
            "description": profile.get("description"),
            "capabilities": capabilities,
        },
        "summary": {"requirements": len(requirements), **counts},
        "requirements": requirements,
    }


def macro_name(structure: str) -> str:
    words = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", structure).upper()
    return re.sub(r"[^A-Z0-9]+", "_", words)


def render_header(document: dict) -> str:
    groups: dict[tuple[str, str], list[dict]] = {}
    extensions = []
    for row in document["requirements"]:
        if row["kind"] == "extension":
            extensions.append(row)
        elif row["kind"] in ("feature", "property"):
            groups.setdefault((row["kind"], row["container"]), []).append(row)

    lines = [
        "/* Generated by tools/derive_dxvk_profile.py; do not edit. */",
        "#ifndef PS5VK_DXVK_V262_PROFILE_H",
        "#define PS5VK_DXVK_V262_PROFILE_H",
        "",
        f'#define DXVK262_PROFILE_ID "{document["profile"]["id"]}"',
        f'#define DXVK262_PROFILE_API_VERSION "{document["profile"]["api_version"]}"',
        f"#define DXVK262_PROFILE_REQUIREMENT_COUNT {document['summary']['requirements']}u",
        "",
        "#define DXVK262_REQUIRED_EXTENSIONS(X) \\",
    ]
    for index, row in enumerate(extensions):
        suffix = " \\" if index + 1 < len(extensions) else ""
        lines.append(f'    X("{row["name"]}", {row["expected"]}u){suffix}')
    lines.append("")
    for (kind, structure), rows in sorted(groups.items()):
        family = "FEATURES" if kind == "feature" else "PROPERTIES"
        name = f"DXVK262_{family}_{macro_name(structure)}"
        lines.append(f"#define {name}(X) \\")
        for index, row in enumerate(rows):
            expected = row["expected"]
            if isinstance(expected, bool):
                literal = "1u" if expected else "0u"
            elif isinstance(expected, int):
                literal = f"UINT64_C({expected})"
            else:
                raise ValueError(f"unsupported C expectation for {row['id']}")
            suffix = " \\" if index + 1 < len(rows) else ""
            lines.append(f"    X({row['name']}, {literal}){suffix}")
        lines.append("")
    lines.extend(["#endif", ""])
    return "\n".join(lines)


def validate_committed(document: dict, source: dict, artifact: dict) -> None:
    if document.get("schema") != SCHEMA:
        raise ValueError("DXVK derivative schema mismatch")
    expected_source = {
        "source_id": source["id"], "tag": source["tag"],
        "tag_object": source["tag_object"], "commit": source["commit"],
        "artifact": artifact["path"],
        "artifact_size_bytes": artifact["size_bytes"],
        "artifact_sha256": artifact["sha256"],
        "artifact_git_blob_sha1": artifact["git_blob_sha1"],
    }
    if document.get("source") != expected_source:
        raise ValueError("DXVK derivative does not match sources.json pin")
    rows = document.get("requirements")
    if not isinstance(rows, list) or not rows:
        raise ValueError("DXVK derivative has no requirements")
    ids = [row.get("id") for row in rows]
    if len(ids) != len(set(ids)):
        raise ValueError("DXVK derivative contains duplicate requirement ids")
    counts = document.get("summary", {})
    if counts.get("requirements") != len(rows):
        raise ValueError("DXVK derivative requirement count drift")
    for kind in ("api-version", "extension", "feature", "property"):
        if counts.get(kind) != sum(row.get("kind") == kind for row in rows):
            raise ValueError(f"DXVK derivative {kind} count drift")
    if document.get("profile", {}).get("id") != PROFILE_ID:
        raise ValueError("DXVK derivative profile id drift")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path,
                        help="local upstream VP_DXVK_requirements.json")
    parser.add_argument("--refresh", action="store_true",
                        help="fetch the immutable pinned artifact and regenerate")
    parser.add_argument("--check", action="store_true",
                        help="verify checked-in derivative/header without writing")
    args = parser.parse_args()
    if args.refresh and args.source:
        parser.error("choose --refresh or --source")

    source, artifact = source_pin()
    if args.check and not args.source:
        document = json.loads(OUTPUT.read_text())
        validate_committed(document, source, artifact)
    else:
        if args.source:
            data = args.source.read_bytes()
        else:
            if not args.refresh:
                parser.error("generation requires --refresh or --source")
            with urllib.request.urlopen(artifact["url"], timeout=30) as response:
                data = response.read()
        verify_source(data, artifact)
        upstream = json.loads(data)
        document = derive(upstream, source, artifact)
        validate_committed(document, source, artifact)
        if args.check:
            if canonical_bytes(document) != OUTPUT.read_bytes():
                raise ValueError("checked-in DXVK derivative is stale")
        else:
            OUTPUT.write_bytes(canonical_bytes(document))

    header = render_header(document).encode()
    if args.check:
        if header != HEADER.read_bytes():
            raise ValueError("generated DXVK probe header is stale")
    else:
        HEADER.parent.mkdir(parents=True, exist_ok=True)
        HEADER.write_bytes(header)
    print(f"DXVK {document['source']['tag']} {PROFILE_ID}: "
          f"{document['summary']['requirements']} requirements verified")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"derive_dxvk_profile.py: {error}", file=sys.stderr)
        raise SystemExit(1)
