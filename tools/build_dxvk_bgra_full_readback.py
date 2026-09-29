#!/usr/bin/env python3
"""Build the public-SDK full-surface BGRA8 GPU readback witness."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_sdk import get_ps5_toolchain  # noqa: E402
from lab import lab_root  # noqa: E402

PROFILE = "dxvk-bgra-full-readback-public-sdk-witness"
WIDTH, HEIGHT, FRAMES = 1920, 1080, 2


def run(*command: str, env: dict | None = None) -> None:
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reuse-sdk", action="store_true",
                        help="link the already staged ordinary SDK")
    args = parser.parse_args()
    lab = lab_root()
    sdk, clang_wrapper = get_ps5_toolchain()
    if not sdk or not clang_wrapper:
        raise SystemExit("PS5 native toolchain is required")
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    builder = foundation / "build/host/ps5-native-tool"
    if not builder.is_file():
        raise SystemExit("ps5-native-tool is required")
    build = ROOT / "build/dxvk-bgra-full-readback"
    dist = build / "dist/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    if not args.reuse_sdk:
        run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    archive = staged / "lib/libps5vk.a"
    if not archive.is_file():
        raise RuntimeError("staged SDK is missing")
    source = ROOT / "examples/dxvk_bgra_full_readback/main.c"
    logger = lab / "projects/logging_server/client"
    obj = build / "main.o"
    dep = build / "main.d"
    run("sh", str(clang_wrapper), "-std=c11", "-O2", "-g", "-Wall", "-Wextra",
        "-Werror", "-ffunction-sections", "-fdata-sections", "-MD", "-MP",
        "-MF", str(dep), "-I" + str(staged / "include"), "-I" + str(logger),
        "-c", str(source), "-o", str(obj), env=sdk_env)
    dependencies = dep.read_text()
    if str(ROOT / "src/") in dependencies or str(ROOT / "native/") in dependencies:
        raise RuntimeError("witness includes a private runtime header")
    undefined = subprocess.check_output(["nm", "-u", str(obj)], text=True)
    if any(line.split()[-1].startswith(("ps5vk_", "ps5_"))
           for line in undefined.splitlines() if line.split()):
        raise RuntimeError("witness calls a private runtime symbol")

    pie = build / "witness_pie.elf"
    eboot_elf = build / "eboot.elf"
    eboot = dist / "eboot.bin"
    link = [str(sdk / "bin/prospero-lld"), "-L" + str(sdk / "target/lib"),
            "-T", str(staged / "lib/ps5-pie.ld"), "--eh-frame-hdr",
            "--version-script", str(staged / "lib/app-symbols.map"),
            "-Map=" + str(build / "witness.map"), "-e", "_start", "-o", str(pie),
            str(staged / "lib/crt.o"), str(obj), str(archive),
            str(staged / "lib/libpsbc.a"),
            *[str(sdk / f"target/lib/{name}") for name in
              ("libc++.a", "libc++abi.a", "libunwind.a", "libpthread.a", "libc.a")],
            "--as-needed", *sorted(str(path) for path in
                                    (sdk / "target/lib").glob("*.so")),
            str(staged / "lib/libSceAgc.so"),
            str(staged / "lib/libSceAgcDriver.so")]
    run(*link)
    run(str(builder), "link", "--in", str(pie), "--out", str(eboot_elf),
        "--stub-dir", str(sdk / "target/lib"), "--module-sdk", "0x02000009",
        "--stub", str(staged / "lib/libSceAgc.so"),
        "--stub", str(staged / "lib/libSceAgcDriver.so"),
        "--companion-sdk", "0x08050001", "--file-name", "eboot.elf")
    run(str(builder), "self", "--sign", "--in", str(eboot_elf), "--out",
        str(eboot), "--magic", "0x1D3D154F")
    param = json.loads((lab / "projects/ps5-agc-gears/sce_sys/param.json").read_text())
    param.update(titleId="PPSA99994", conceptId="99994",
                 contentId="UP9000-PPSA99994_00-PS5VKBGRAREAD0001")
    param["localizedParameters"]["en-US"]["titleName"] = "PS5 Vulkan BGRA Readback Witness"
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    sha256 = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    artifact = {"profile": PROFILE, "width": WIDTH, "height": HEIGHT,
                "frames": FRAMES, "bytes_per_frame": WIDTH * HEIGHT * 4,
                "eboot_sha256": sha256(eboot), "source_sha256": sha256(source),
                "libps5vk_sha256": sha256(archive)}
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
