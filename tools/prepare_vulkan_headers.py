"""Fetch complete, pinned Khronos headers into an ignored dependency checkout."""
from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "third_party/vulkan-headers"
URL = "https://github.com/KhronosGroup/Vulkan-Headers.git"
PIN = "b51f6b865c18fc5b33990d12f75e8dfd672cede6"


def git(*args):
    return subprocess.check_output(["git", "-C", str(DEST), *args], text=True).strip()


def main(check_only=False):
    if DEST.exists():
        # Never reset or repair an existing checkout implicitly.
        if git("rev-parse", "HEAD") != PIN or git("status", "--porcelain"):
            raise SystemExit("Vulkan-Headers checkout differs; inspect it before proceeding")
        if git("remote", "get-url", "origin") != URL:
            raise SystemExit("Unexpected Vulkan-Headers origin")
    else:
        if check_only:
            raise SystemExit("Pinned headers missing; run make vulkan-headers once")
        DEST.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", str(DEST)], check=True)
        git("remote", "add", "origin", URL)
        git("fetch", "--depth=1", "origin", PIN)
        git("checkout", "--detach", "FETCH_HEAD")
    if git("rev-parse", "HEAD") != PIN:
        raise SystemExit("Header identity mismatch")
    print(f"Vulkan-Headers {PIN}: {DEST / 'include'}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify only; never fetch")
    main(parser.parse_args().check)
