#!/usr/bin/env python3
"""Collect the pinned VK-GL-CTS mustpass case listing into a committed manifest.

What this does
--------------
1. Reads the CTS source pin (repo/tag/commit) from ``sources.json``.
2. Reads ``external/vulkancts/mustpass/main/vk-default.txt`` at that revision.
   That file lists the group files that make up the mustpass run.
3. Downloads each group file, counts its cases, and records the SHA-256 and the
   Git blob id (``git hash-object`` equivalent) of the exact bytes.
4. Cross-checks the Git blob id against the blob id reported by the GitHub
   ``git/trees`` API for the same path and revision.
5. Writes ``conformance_inventory/cts_manifest.json``.

The downloaded group files are large (hundreds of MB in total) and are written
to an ignored cache directory. Only the small manifest is committed.

Provenance warning
------------------
This produces a *static source listing*: the case names checked into the pinned
CTS revision. It is not an executable-generated case list, and it does not
account for platform, capability or extension conditions that decide which
cases are generated or runnable on a given device. Do not treat it as a run
result or as evidence that any case passed.

Usage
-----
    python3 conformance_inventory/tools/collect_cts_listing.py
    python3 conformance_inventory/tools/collect_cts_listing.py --sources-only

``--sources-only`` records group-file identities from the pinned Git tree
without downloading file contents, so ``case_count`` and ``sha256`` are absent.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
INVENTORY_DIR = os.path.dirname(HERE)
DEFAULT_SOURCES = os.path.join(INVENTORY_DIR, "sources.json")
DEFAULT_OUT = os.path.join(INVENTORY_DIR, "cts_manifest.json")
DEFAULT_CACHE = os.path.join(INVENTORY_DIR, ".cache", "cts")

CTS_SOURCE_ID = "khronos-vk-gl-cts"
GROUPS_FILE = "external/vulkancts/mustpass/main/vk-default.txt"
GROUPS_DIR = os.path.dirname(GROUPS_FILE)


def log(message: str) -> None:
    print(message, file=sys.stderr)


def git_blob_id(data: bytes) -> str:
    header = ("blob %d\0" % len(data)).encode("ascii")
    return hashlib.sha1(header + data).hexdigest()


def http_get(url: str, accept: str = "application/json") -> bytes:
    request = urllib.request.Request(url, headers={"Accept": accept, "User-Agent": "ps5vk-cis-collector"})
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            return response.read()
    except urllib.error.HTTPError as exc:  # pragma: no cover - network failure path
        body = exc.read(400).decode("utf-8", "replace")
        raise SystemExit("HTTP %s for %s: %s" % (exc.code, url, body)) from exc


def api_json(url: str) -> object:
    return json.loads(http_get(url).decode("utf-8"))


def raw_url(repo: str, commit: str, path: str) -> str:
    owner = repo.rsplit("/", 2)[-2]
    name = repo.rsplit("/", 1)[-1]
    if name.endswith(".git"):
        name = name[: -len(".git")]
    return "https://raw.githubusercontent.com/%s/%s/%s/%s" % (owner, name, commit, path)


def find_cts_source(sources: dict) -> dict:
    for source in sources.get("sources", []):
        if source.get("id") == CTS_SOURCE_ID:
            return source
    raise SystemExit("sources.json has no %s entry" % CTS_SOURCE_ID)


def load_group_list(repo: str, commit: str) -> tuple[bytes, list[str]]:
    data = http_get(raw_url(repo, commit, GROUPS_FILE), accept="text/plain")
    groups: list[str] = []
    for line in data.decode("utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        groups.append(line)
    if not groups:
        raise SystemExit("group list %s is empty" % GROUPS_FILE)
    return data, groups


def tree_index(repo: str, commit: str) -> dict[str, dict]:
    owner = repo.rsplit("/", 2)[-2]
    name = repo.rsplit("/", 1)[-1]
    if name.endswith(".git"):
        name = name[: -len(".git")]
    payload = api_json("https://api.github.com/repos/%s/%s/git/trees/%s?recursive=1" % (owner, name, commit))
    if payload.get("truncated"):
        raise SystemExit("GitHub tree listing was truncated; cannot verify blob ids reliably")
    return {entry["path"]: entry for entry in payload.get("tree", []) if entry.get("type") == "blob"}


def count_cases(data: bytes) -> int:
    count = 0
    for line in data.decode("utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            count += 1
    return count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sources", default=DEFAULT_SOURCES, help="path to sources.json")
    parser.add_argument("--out", default=DEFAULT_OUT, help="path of the manifest to write")
    parser.add_argument("--cache-dir", default=DEFAULT_CACHE, help="where to store downloaded group files")
    parser.add_argument(
        "--sources-only",
        action="store_true",
        help="record group identities from the Git tree without downloading contents",
    )
    args = parser.parse_args(argv)

    with open(args.sources, "r", encoding="utf-8") as handle:
        sources = json.load(handle)
    cts = find_cts_source(sources)

    repo = cts["repo"]
    commit = cts["commit"]
    log("CTS %s @ %s" % (cts["tag"], commit))

    groups_bytes, groups = load_group_list(repo, commit)
    log("group list: %d entries" % len(groups))

    tree = tree_index(repo, commit)

    manifest = {
        "manifest_version": "1.0",
        "generated_by": "conformance_inventory/tools/collect_cts_listing.py",
        "source_id": CTS_SOURCE_ID,
        "listing_kind": "static-source-listing",
        "listing_scope": "external/vulkancts/mustpass/main/vk-default",
        "repo": repo,
        "tag": cts["tag"],
        "commit": commit,
        "retrieved_date": sources.get("retrieved_date"),
        "groups_file": {
            "path": GROUPS_FILE,
            "sha256": hashlib.sha256(groups_bytes).hexdigest(),
            "git_blob_sha1": git_blob_id(groups_bytes),
            "group_count": len(groups),
        },
        "groups": [],
        "totals": {},
        "limits": [
            "Static listing checked into the pinned source revision; not an executable case list.",
            "Case presence does not imply the case is runnable on any particular device or driver.",
            "No case in this manifest has been executed by collecting it.",
        ],
    }

    expected_groups_sha = tree.get(GROUPS_FILE, {}).get("sha")
    if expected_groups_sha and expected_groups_sha != manifest["groups_file"]["git_blob_sha1"]:
        raise SystemExit(
            "group list blob mismatch: tree=%s content=%s"
            % (expected_groups_sha, manifest["groups_file"]["git_blob_sha1"])
        )

    total_cases = 0
    missing: list[str] = []
    for group in groups:
        path = os.path.normpath(os.path.join(GROUPS_DIR, group))
        entry = tree.get(path)
        if entry is None:
            missing.append(path)
            continue
        record = {
            "group": group,
            "path": path,
            "size_bytes": entry.get("size"),
            "git_blob_sha1": entry.get("sha"),
        }
        if not args.sources_only:
            data = http_get(raw_url(repo, commit, path), accept="text/plain")
            blob = git_blob_id(data)
            if blob != entry.get("sha"):
                raise SystemExit("content/blob mismatch for %s: %s != %s" % (path, blob, entry.get("sha")))
            record["sha256"] = hashlib.sha256(data).hexdigest()
            record["case_count"] = count_cases(data)
            total_cases += record["case_count"]
            cache_path = os.path.join(args.cache_dir, cts["tag"], path)
            os.makedirs(os.path.dirname(cache_path), exist_ok=True)
            with open(cache_path, "wb") as handle:
                handle.write(data)
            log("  %-70s %6d cases" % (group, record["case_count"]))

        manifest["groups"].append(record)

    manifest["groups"].sort(key=lambda item: item["path"])
    manifest["totals"] = {
        "group_files": len(manifest["groups"]),
        "cases": total_cases,
        "missing_group_files": sorted(missing),
    }

    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=False)
        handle.write("\n")
    os.replace(tmp, args.out)
    log(
        "wrote %s: %d group files, %d cases"
        % (args.out, manifest["totals"]["group_files"], manifest["totals"]["cases"])
    )
    if missing:
        log("WARNING: %d group files listed in vk-default.txt were not found in the tree" % len(missing))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
