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
        # The published T04 candidate plus the T05 viewport-index export, pinned
        # by exact commit. It carries the metadata this integration reads (the
        # merged pair's system-SGPR indices and launch counts, the driver
        # user-data window base, the pixel stage's distance reads) under
        # metadata version 17, and it names the geometry stage's gl_ViewportIndex
        # parameter export, and the packed register a fragment distance read
        # actually targets (a fragment reading gl_ClipDistance[4] used to name
        # the first register, so the pixel stage interpolated the wrong one). That last part is additive: a pipeline without the
        # export emits nothing, so the version stays 17 and no other consumer's
        # metadata changes - which is why the driver cache key does not move with
        # this pin. It is an audited dependency candidate, not a merge of the
        # dependency's main (PRs mpereiraesaa/opengnm-psbc#17 and #18): the pin is the
        # whole contract, so a different commit has to be pinned explicitly.
        "pin": "4d4a65aad8f178d2437df2ec97159392bae7f844",
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
