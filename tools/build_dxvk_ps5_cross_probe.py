#!/usr/bin/env python3
"""Cross-link pinned DXVK 2.6.2 D3D11/DXGI for PS5, without a WSI backend.

This answers a toolchain question only. The output cannot initialize DXVK:
the SDL2 WSI is excluded and no PS5 WSI or Vulkan surface is substituted.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
from pathlib import Path
import shlex
import subprocess
import sys

from build_sdk import get_ps5_toolchain
from run_dxvk_host_smoke import sha256


ROOT = Path(__file__).resolve().parents[1]
COMPAT_HEADER = """#include <sys/types.h>
#ifdef major
#undef major
#endif
#ifdef minor
#undef minor
#endif
#ifdef __cplusplus
#include <pthread.h>
/* The PS5 SDK has no pthread_setname_np. Naming is diagnostic only. */
static inline int pthread_setname_np(pthread_t, const char*) { return 0; }
#endif
"""
NO_EDID = """#include "wsi_edid.h"
namespace dxvk::wsi {
std::optional<WsiDisplayMetadata> parseColorimetryInfo(const WsiEdidData&) {
  return std::nullopt;
}
}
"""
EXCLUDED_SOURCES = (
    "wsi_monitor_sdl2.cpp", "wsi_platform_sdl2.cpp", "wsi_window_sdl2.cpp",
    "wsi_edid.cpp",
)


def command(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, check=True, text=True, capture_output=True,
                          **kwargs)


def compile_entry(index: int, entry: dict, build: Path, output: Path,
                  cc: Path, cxx: Path, compat: Path) -> tuple[str, str, Path]:
    source = entry["file"]
    args = shlex.split(entry["command"])
    args[0] = str(cc if source.endswith(".c") else cxx)
    filtered = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
        elif arg in ("-o", "-MQ", "-MF"):
            skip_next = True
        elif arg not in ("-MD", "-O2", "-g"):
            filtered.append(arg)
    filtered[1:1] = ["-include", str(compat)]
    if source.endswith("/wsi_platform.cpp"):
        filtered.append("-UDXVK_WSI_SDL2")
    obj = output / f"{index:03d}.o"
    filtered.extend(("-O0", "-o", str(obj)))
    try:
        command(filtered, cwd=build)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"PS5 compile failed for {source}:\n{error.stderr[-2000:]}") from error
    return "/".join(entry["output"].split("/")[:2]), source, obj


def archive(output: Path, name: str, objects: list[Path]) -> Path:
    path = output / (name.replace("/", "_") + ".a")
    path.unlink(missing_ok=True)
    command(["ar", "rcs", str(path), *(str(obj) for obj in objects)])
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk-dir", type=Path,
                        default=ROOT / "third_party/dxvk-v2.6.2")
    parser.add_argument("--build-dir", type=Path,
                        help="Configured native Meson build (default: <dxvk-dir>/build-native)")
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/dxvk-ps5-cross-probe")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    dxvk = args.dxvk_dir.resolve()
    build = (args.build_dir or dxvk / "build-native").resolve()
    output = args.output_dir.resolve()
    try:
        if args.jobs < 1:
            raise ValueError("--jobs must be positive")
        expected = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                              .read_text())["source"]["commit"]
        actual = command(["git", "-C", str(dxvk), "rev-parse", "HEAD"]).stdout.strip()
        if actual != expected:
            raise ValueError(f"DXVK source commit {actual} differs from pinned {expected}")
        dirty = command(["git", "-C", str(dxvk), "status", "--porcelain",
                         "--untracked-files=no"]).stdout.strip()
        if dirty:
            raise ValueError("DXVK tracked source has local changes")
        sdk, _ = get_ps5_toolchain()
        if sdk is None:
            raise ValueError("PS5 payload toolchain is unavailable")
        cc, cxx = sdk / "bin/prospero-clang", sdk / "bin/prospero-clang++"
        entries = json.loads(command(["ninja", "-C", str(build), "-t", "compdb",
                                      "c_COMPILER", "cpp_COMPILER"]).stdout)
        if not entries or any(Path(entry["directory"]).resolve() != build for entry in entries):
            raise ValueError("unexpected DXVK native compilation database")
        if not all("-DDXVK_WSI_SDL2" in entry["command"] and
                   "-DDXVK_WSI_SDL3" not in entry["command"] and
                   "-DDXVK_WSI_GLFW" not in entry["command"]
                   for entry in entries if entry["output"].startswith("src/")):
            raise ValueError("configure DXVK native with SDL2 alone for this probe")
        selected = [(index, entry) for index, entry in enumerate(entries)
                    if not entry["output"].startswith("subprojects/libdisplay-info/")
                    and not entry["file"].endswith(EXCLUDED_SOURCES)]
        if not any(entry["output"].startswith("src/d3d11/") for _, entry in selected):
            raise ValueError("native DXVK build has no D3D11 target")
        if not any(entry["output"].startswith("src/dxgi/") for _, entry in selected):
            raise ValueError("native DXVK build has no DXGI target")
        output.mkdir(parents=True, exist_ok=True)
        objects = output / "objects"
        objects.mkdir(exist_ok=True)
        compat = output / "ps5_compat.h"
        compat.write_text(COMPAT_HEADER)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(
                lambda item: compile_entry(item[0], item[1], build, objects,
                                           cc, cxx, compat), selected))
        no_edid = output / "no_edid.cpp"
        no_edid.write_text(NO_EDID)
        no_edid_obj = output / "no_edid.o"
        command([str(cxx), "-std=c++17", "-O0", "-fPIC",
                 f"-I{dxvk / 'src/wsi'}", "-c", str(no_edid),
                 "-o", str(no_edid_obj)])
        grouped: dict[str, list[Path]] = {}
        for group, _, obj in results:
            grouped.setdefault(group, []).append(obj)
        grouped.setdefault("src/wsi", []).append(no_edid_obj)
        archives = {group: archive(output, group, objects)
                    for group, objects in grouped.items()
                    if group not in ("src/dxgi", "src/d3d11")}
        common = [archives[group] for group in (
            "src/dxvk", "src/util", "src/spirv", "src/wsi", "src/vulkan", "src/dxbc")]
        artifacts = {}
        for target in ("dxgi", "d3d11"):
            library = output / f"libdxvk_{target}.so"
            link_group = ([output / "libdxvk_dxgi.so"] if target == "d3d11" else []) + common
            command([str(cxx), "-shared", "-Wl,--no-undefined",
                     f"-Wl,-soname,{library.name}",
                     *(str(obj) for obj in grouped[f"src/{target}"]),
                     "-Wl,--start-group", *(str(path) for path in link_group),
                     "-Wl,--end-group", "-o", str(library)])
            artifact_path = (library.relative_to(ROOT) if library.is_relative_to(ROOT)
                             else library)
            artifacts[target] = {"path": str(artifact_path),
                                 "sha256": sha256(library), "bytes": library.stat().st_size}
        dynamic = command(["readelf", "-d", str(output / "libdxvk_d3d11.so")]).stdout
        if "Shared library: [libdxvk_dxgi.so]" not in dynamic:
            raise ValueError("D3D11 does not depend on the relocatable DXGI soname")
        receipt = {
            "profile": "dxvk-v262-ps5-cross-link-probe",
            "dxvk_commit": actual,
            "target": "x86_64-sie-ps5",
            "compiled_units": len(results),
            "excluded_units": len(entries) - len(results),
            "artifacts": artifacts,
            "runtime_ready": False,
            "runtime_blocker": "No PS5 WSI driver or Vulkan surface/swapchain route",
        }
        manifest = output / "receipt.json"
        manifest.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        print(json.dumps(receipt, indent=2, sort_keys=True))
    except (OSError, subprocess.CalledProcessError, RuntimeError, ValueError) as error:
        print(f"DXVK PS5 cross-link probe failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr[-2000:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
