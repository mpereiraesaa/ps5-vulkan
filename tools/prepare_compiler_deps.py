#!/usr/bin/env python3
"""Fetch pinned compiler dependencies (opengnm-psbc and opengnm) into third_party/."""
from pathlib import Path
import argparse
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

DEPS = [
    {
        "name": "opengnm-psbc",
        "dest": ROOT / "third_party/psbc-reference",
        "url": "https://github.com/mpereiraesaa/opengnm-psbc.git",
        # The published T04 candidate, pinned by exact commit: it carries the
        # metadata this integration reads (the merged pair's system-SGPR indices
        # and launch counts, the driver user-data window base, the pixel stage's
        # distance reads, the tessellation evaluation half's loadable NGG
        # package and the hull's tessellation workgroup layout) under metadata
        # version 17. That dependency review is explicitly pending; this pin
        # is the reproducible candidate the tessellation work builds against,
        # not a claim that dependency main contains it. It now carries the
        # cross-stage tessellation linkage as well (branch
        # codex/tess-merged-lshs, opengnm-psbc PR #19, also open and
        # unreviewed): the hull compiled as ONE merged LS/HS program so the
        # vertex half is a real LS, and the domain linked against the control
        # half so its patch count, attribute stride and tess-factor read flag
        # stay compile-time constants. PR #16's TESS_EVAL package is included
        # by history. The pin now also carries the link in the OTHER direction
        # - the evaluation half linked into the hull compile - without which
        # the control half reports TESS_PRIMITIVE_UNSPECIFIED and stores
        # QUAD-shaped tessellation factors for a triangle domain, measured on
        # hardware as inner[0] landing one dword past where the tessellator
        # reads it. The pin is the whole
        # contract, so a different commit has to be pinned explicitly and the
        # driver cache key moves with the metadata version.
        "pin": "79c4166b972f1a3ab4fbf6b2520c0091a1a013f9",
    },
    {
        "name": "opengnm",
        "dest": ROOT / "third_party/opengnm",
        "url": "https://github.com/PS4-OpenGNM/opengnm.git",
        "pin": "4b295ca54c82c83acf308d1c646a2dfa9ae57350",
    },
]


def git_cmd(dest, *args):
    return subprocess.check_output(["git", "-C", str(dest), *args], text=True).strip()


def prepare_dep(dep, check_only=False):
    dest = dep["dest"]
    url = dep["url"]
    pin = dep["pin"]
    name = dep["name"]

    if dest.exists() and (dest / ".git").exists():
        head = git_cmd(dest, "rev-parse", "HEAD")
        if head != pin:
            if check_only:
                raise SystemExit(f"{name} checkout differs (at {head}, expected {pin})")
            print(f"Updating {name} to pinned commit {pin}...")
            subprocess.run(["git", "-C", str(dest), "fetch", "--depth=1", url, pin], check=True)
            subprocess.run(["git", "-C", str(dest), "checkout", "--detach", "FETCH_HEAD"], check=True)
    elif dest.exists() and not (dest / ".git").exists():
        raise SystemExit(f"{dest} exists but is not a git repository")
    else:
        if check_only:
            raise SystemExit(f"Pinned dependency {name} missing at {dest}")
        print(f"Cloning {name} at pinned commit {pin}...")
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", str(dest)], check=True)
        git_cmd(dest, "remote", "add", "origin", url)
        git_cmd(dest, "fetch", "--depth=1", "origin", pin)
        git_cmd(dest, "checkout", "--detach", "FETCH_HEAD")

    head = git_cmd(dest, "rev-parse", "HEAD")
    if head != pin:
        raise SystemExit(f"{name} commit mismatch: got {head}, expected {pin}")
    print(f"{name} {pin}: {dest}")


import shutil


def main(check_only=False):
    for dep in DEPS:
        prepare_dep(dep, check_only)

    # Ensure SPIRV-Headers compatibility link exists for psbc-reference codegen
    spirv_dir = ROOT / "third_party/SPIRV-Headers/include/spirv"
    spirv_dir.mkdir(parents=True, exist_ok=True)
    xml_target = spirv_dir / "spir-v.xml"
    src_xml = ROOT / "third_party/psbc-reference/src/compiler/spirv/spir-v.xml"
    if not xml_target.exists() and src_xml.exists():
        try:
            xml_target.symlink_to(src_xml)
        except OSError:
            shutil.copyfile(src_xml, xml_target)

    # Ensure Vulkan-Headers case-compatible link exists if needed
    vh_link = ROOT / "third_party/Vulkan-Headers"
    vh_dir = ROOT / "third_party/vulkan-headers"
    if not vh_link.exists() and vh_dir.exists():
        try:
            vh_link.symlink_to(vh_dir)
        except OSError:
            pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify only; never fetch")
    args = parser.parse_args()
    main(args.check)
