#!/usr/bin/env python3
"""Extract the set of section anchors from the pinned Vulkan specification HTML.

The inventory stores ``source.anchor`` values (HTML fragment ids such as
``synchronization-semaphores``) instead of copying normative text. This tool
builds the index that lets ``validate.py --anchor-index`` prove those anchors
exist in the pinned specification.

The registry serves a *moving* alias for the current specification. This tool
therefore refuses to run unless the page title matches the ``spec_version``
recorded in ``sources.json``, and it can additionally require a byte-exact hash
match. That converts a stale alias into a hard failure instead of silent drift.

Usage
-----
    python3 conformance_inventory/tools/extract_spec_anchors.py
    python3 conformance_inventory/tools/extract_spec_anchors.py --html /path/to/vkspec.html --strict-hash

The extracted index is written to an ignored cache path.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
INVENTORY_DIR = os.path.dirname(HERE)
DEFAULT_SOURCES = os.path.join(INVENTORY_DIR, "sources.json")
DEFAULT_OUT = os.path.join(INVENTORY_DIR, ".cache", "spec_anchors.txt")
SPEC_SOURCE_ID = "khronos-vulkan-spec"

ID_PATTERN = re.compile(r'\bid="([^"]+)"')
TITLE_PATTERN = re.compile(r"<title>([^<]*)</title>", re.IGNORECASE)


def load_spec_source(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        sources = json.load(handle)
    for source in sources.get("sources", []):
        if source.get("id") == SPEC_SOURCE_ID:
            return source
    raise SystemExit("sources.json has no %s entry" % SPEC_SOURCE_ID)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sources", default=DEFAULT_SOURCES)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--html", help="use a local specification HTML file instead of downloading")
    parser.add_argument("--strict-hash", action="store_true", help="fail if the bytes differ from the recorded sha256")
    args = parser.parse_args(argv)

    spec = load_spec_source(args.sources)
    alias = spec.get("document_alias")
    if not alias:
        raise SystemExit("specification source has no document_alias to extract anchors from")

    if args.html:
        with open(args.html, "rb") as handle:
            data = handle.read()
        origin = args.html
    else:
        request = urllib.request.Request(alias["url"], headers={"User-Agent": "ps5vk-cis-anchors"})
        with urllib.request.urlopen(request, timeout=180) as response:
            data = response.read()
        origin = alias["url"]

    digest = hashlib.sha256(data).hexdigest()
    html = data.decode("utf-8", "replace")
    titles = TITLE_PATTERN.findall(html)
    title = titles[0] if titles else ""
    expected_version = spec["spec_version"]
    if expected_version not in title:
        raise SystemExit(
            "stale/incompatible specification source: %s reports title %r, expected version %s"
            % (origin, title, expected_version)
        )
    if args.strict_hash and digest != alias.get("sha256"):
        raise SystemExit(
            "specification bytes changed: %s != recorded %s (version %s still matches)"
            % (digest, alias.get("sha256"), expected_version)
        )

    anchors = sorted(set(ID_PATTERN.findall(html)))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write("# Anchors extracted from %s\n" % origin)
        handle.write("# title: %s\n" % title)
        handle.write("# sha256: %s\n" % digest)
        for anchor in anchors:
            handle.write(anchor + "\n")

    print("wrote %d anchors to %s (source sha256 %s)" % (len(anchors), args.out, digest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
