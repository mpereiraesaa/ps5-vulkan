"""Thin lab integration; no console operations occur during import or checks."""
from pathlib import Path
import os
import subprocess
import sys

REQUIRED = (
    "tools/ps5_remoteplay.py",
    "projects/logging_server/server/ps5logd.py",
    "projects/logging_server/client/ps5log.h",
    "projects/logging_server/client/ps5log_ps5_net.c",
    "projects/logging_server/PROTOCOL.md",
)


def _is_lab_root(path):
    return all((path / relative).is_file() for relative in REQUIRED)


def resolve_lab_root(script_path):
    """Find the shared lab from either its canonical checkout or a worktree."""
    script_path = Path(script_path).resolve()
    fallback = script_path.parents[3]
    for parent in script_path.parents:
        for candidate in (parent, parent / "homebrew_ps5"):
            if _is_lab_root(candidate):
                return candidate.resolve()
    return fallback.resolve()


def lab_root():
    explicit = os.environ.get("PS5VK_LAB_ROOT")
    return (Path(explicit).expanduser().resolve() if explicit else
            resolve_lab_root(__file__))


def command(root, args):
    if not args or args[0] not in ("remoteplay", "logs"):
        raise ValueError("usage: lab.py doctor | remoteplay <args> | logs <args>")
    script = REQUIRED[0] if args[0] == "remoteplay" else REQUIRED[1]
    return [sys.executable, str(root / script), *args[1:]]


def main(args=None):
    args = list(sys.argv[1:] if args is None else args)
    root = lab_root()
    missing = [path for path in REQUIRED if not (root / path).is_file()]
    if missing:
        print("Missing lab tooling: " + ", ".join(missing), file=sys.stderr)
        return 2
    if args == ["doctor"]:
        print("Shared tooling found. Host-only check; console and TCP listener not tested.")
        return 0
    try:
        argv = command(root, args)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    return subprocess.run(argv, cwd=root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
