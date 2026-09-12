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
        "url": "https://github.com/PS4-OpenGNM/opengnm-psbc.git",
        "pin": "a92a1228ea3a64e4be9f0e61c2a65a5aa7ffed92",
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
            subprocess.run(["git", "-C", str(dest), "fetch", "--depth=1", "origin", pin], check=True)
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


def main(check_only=False):
    for dep in DEPS:
        prepare_dep(dep, check_only)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify only; never fetch")
    args = parser.parse_args()
    main(args.check)
