#!/usr/bin/env python3
"""Run four pinned DXVK native frontends on the host Vulkan driver.

This checks DXVK's D3D8/9/10/11 clear/present paths. It does not run PE DLLs,
Prospero Win, ps5vk's PS5 backend, or the console.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.20602"
LIBRARIES = {
    "dxgi": Path(f"src/dxgi/libdxvk_dxgi.so.{VERSION}"),
    "d3d11": Path(f"src/d3d11/libdxvk_d3d11.so.{VERSION}"),
    "d3d10": Path(f"src/d3d10/libdxvk_d3d10core.so.{VERSION}"),
    "d3d9": Path(f"src/d3d9/libdxvk_d3d9.so.{VERSION}"),
    "d3d8": Path(f"src/d3d8/libdxvk_d3d8.so.{VERSION}"),
}
LINK = {
    "d3d8": ("d3d8", "d3d9"),
    "d3d9": ("d3d9",),
    "d3d10": ("d3d10", "dxgi"),
    "d3d11": ("d3d11", "dxgi"),
}
ZERO = "0x00000000"
EXPECTED_FIELDS = {
    "d3d8": ("create", "clear", "present"),
    "d3d9": ("create", "clear", "present"),
    "d3d10": ("factory", "adapter", "device", "swap", "buffer", "view", "present"),
    "d3d11": ("factory", "adapter", "device", "swap", "buffer", "view", "present"),
}


def command(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, check=True, capture_output=True, text=True, **kwargs)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_marker(api: str, stdout: str) -> dict[str, str] | None:
    matches = re.findall(rf"^DXVK_NATIVE_{api.upper()} (.+)$", stdout, re.MULTILINE)
    if len(matches) != 1:
        return None
    fields = dict(re.findall(r"\b([a-z]+)=(0x[0-9a-f]+)", matches[0]))
    expected = set(EXPECTED_FIELDS[api]) | ({"level"} if api == "d3d11" else set())
    return fields if set(fields) == expected else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk-dir", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--build-dir", type=Path,
                        default=ROOT / "build/dxvk-native-all")
    parser.add_argument("--out-dir", type=Path,
                        default=ROOT / "build/dxvk-host-frontends")
    args = parser.parse_args()
    source_dir = args.dxvk_dir.resolve()
    build_dir = args.build_dir.resolve()
    out_dir = args.out_dir.resolve()
    try:
        pinned = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                            .read_text())["source"]["commit"]
        actual = command(["git", "-C", str(source_dir), "rev-parse", "HEAD"]).stdout.strip()
        if actual != pinned:
            raise ValueError(f"DXVK source {actual} differs from pin {pinned}")
        if command(["git", "-C", str(source_dir), "status", "--porcelain",
                    "--untracked-files=no"]).stdout.strip():
            raise ValueError("DXVK tracked source has local changes")
        libraries = {name: build_dir / path for name, path in LIBRARIES.items()}
        missing = [str(path) for path in libraries.values() if not path.is_file()]
        if missing:
            raise ValueError("build all five native DXVK libraries first: " + ", ".join(missing))
        cflags = shlex.split(command(["sdl2-config", "--cflags"]).stdout)
        sdl_libs = shlex.split(command(["sdl2-config", "--libs"]).stdout)
        out_dir.mkdir(parents=True, exist_ok=True)
        library_dirs = list(dict.fromkeys(str(path.parent) for path in libraries.values()))
        environment = dict(os.environ, DXVK_WSI_DRIVER="SDL2", DXVK_LOG_PATH="none",
                           DXVK_STATE_CACHE="disable")
        prior_library_path = os.environ.get("LD_LIBRARY_PATH")
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(
            [*library_dirs, *([prior_library_path] if prior_library_path else [])])
        receipt = {"schema": "ps5vk-dxvk-native-frontends/1", "dxvk_commit": actual,
                   "scope": "host Vulkan, native DXVK shared libraries; no PE/Prospero Win/PS5",
                   "libraries": {name: sha256(path) for name, path in libraries.items()},
                   "frontends": {}}
        all_passed = True
        for api in ("d3d8", "d3d9", "d3d10", "d3d11"):
            source = ROOT / "examples/dxvk_host_frontends" / f"{api}.cpp"
            executable = out_dir / api
            link = [str(libraries[name]) for name in LINK[api]]
            command(["c++", "-std=c++17",
                     f"-I{source_dir / 'include/native/directx'}",
                     f"-I{source_dir / 'include/native/windows'}",
                     *cflags, str(source), *link, *sdl_libs,
                     *(f"-Wl,-rpath,{directory}" for directory in library_dirs),
                     "-o", str(executable)])
            try:
                result = subprocess.run([str(executable)], env=environment,
                                        capture_output=True, text=True, timeout=60,
                                        check=False)
                output = result.stdout + result.stderr
                returncode = result.returncode
            except subprocess.TimeoutExpired as error:
                output = (error.stdout or b"").decode(errors="replace") if isinstance(
                    error.stdout, bytes) else (error.stdout or "")
                output += (error.stderr or b"").decode(errors="replace") if isinstance(
                    error.stderr, bytes) else (error.stderr or "")
                returncode = None
            log = out_dir / f"{api}.log"
            log.write_text(output)
            fields = parse_marker(api, output)
            passed = returncode == 0 and fields is not None and all(
                fields[key] == ZERO for key in EXPECTED_FIELDS[api]) and (
                    api != "d3d11" or fields["level"] == "0xb000")
            receipt["frontends"][api] = {
                "source_sha256": sha256(source), "executable_sha256": sha256(executable),
                "exit_code": returncode, "marker": fields, "passed": passed,
                "log": str(log), "log_sha256": sha256(log),
            }
            print(f"{api}: {'pass' if passed else 'FAIL'} {fields}")
            all_passed &= passed
        receipt["all_passed"] = all_passed
        output = out_dir / "receipt.json"
        output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        print(output)
        return 0 if all_passed else 1
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"DXVK frontend host run failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout[-1500:], file=sys.stderr)
            print(error.stderr[-1500:], file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
