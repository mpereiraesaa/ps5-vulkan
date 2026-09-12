#!/usr/bin/env python3
"""Build and package focused Vulkan CTS runner for native PlayStation 5."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DIST_SDK = ROOT / "dist-sdk"
CTS_DIR = ROOT / "cts"
BUILD_DIR = CTS_DIR / "build"
DIST_DIR = ROOT / "dist-cts/PPSA99994"


def main():
    parser = argparse.ArgumentParser(description="Build native PS5 CTS runner")
    parser.parse_args()

    # Ensure SDK is staged
    if not (DIST_SDK / "lib/libps5vk.a").is_file():
        print("Staged SDK missing; running tools/build_sdk.py...")
        subprocess.run([sys.executable, str(ROOT / "tools/build_sdk.py")], check=True)

    lab = ROOT.parents[1]
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    sdk = foundation / ".deps/native/ps5-payload-sdk"
    clang_wrapper = foundation / "tooling/prospero-clang18"
    linker = sdk / "bin/prospero-lld"
    builder = foundation / "build/host/ps5-native-tool"
    gears = lab / "projects/ps5-agc-gears"

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    (DIST_DIR / "sce_sys").mkdir(parents=True, exist_ok=True)
    (DIST_DIR / "sce_module").mkdir(parents=True, exist_ok=True)

    obj_file = BUILD_DIR / "cts_adapter.o"
    pie_elf = BUILD_DIR / "cts_pie.elf"
    eboot_elf = BUILD_DIR / "eboot.elf"
    eboot_bin = DIST_DIR / "eboot.bin"

    # 1. Compile cts_adapter.c
    cflags = [
        "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections",
        "-DPS5VK_TARGET_PS5=1",
        "-I" + str(DIST_SDK / "include"),
        "-I" + str(CTS_DIR),
    ]

    print("Compiling native CTS adapter...")
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    subprocess.run(
        ["sh", str(clang_wrapper), *cflags, "-c", str(CTS_DIR / "cts_adapter.c"), "-o", str(obj_file)],
        env=env, check=True
    )

    # 2. Link PIE ELF
    map_file = BUILD_DIR / "cts.map"
    link_cmd = [
        str(linker),
        "-L" + str(sdk / "target/lib"),
        "-T", str(DIST_SDK / "lib/ps5-pie.ld"),
        "--eh-frame-hdr",
        "--version-script", str(DIST_SDK / "lib/app-symbols.map"),
        f"-Map={map_file}",
        "-e", "_start",
        "-o", str(pie_elf),
        str(DIST_SDK / "lib/crt.o"),
        str(obj_file),
        str(DIST_SDK / "lib/libps5vk.a"),
        str(DIST_SDK / "lib/libpsbc.a"),
        str(sdk / "target/lib/libc++.a"),
        str(sdk / "target/lib/libc++abi.a"),
        str(sdk / "target/lib/libunwind.a"),
        str(sdk / "target/lib/libpthread.a"),
        str(sdk / "target/lib/libc.a"),
        "--as-needed",
        *sorted(str(p) for p in (sdk / "target/lib").glob("*.so")),
        str(DIST_SDK / "lib/libSceAgc.so"),
        str(DIST_SDK / "lib/libSceAgcDriver.so"),
    ]
    print("Linking native CTS PIE ELF...")
    subprocess.run(link_cmd, check=True)

    # 3. Create eboot.elf and signed eboot.bin
    print("Signing native CTS eboot.bin via ps5-native-tool...")
    subprocess.run([
        str(builder), "link", "--in", str(pie_elf), "--out", str(eboot_elf),
        "--stub-dir", str(sdk / "target/lib"), "--module-sdk", "0x02000009",
        "--stub", str(DIST_SDK / "lib/libSceAgc.so"),
        "--stub", str(DIST_SDK / "lib/libSceAgcDriver.so"),
        "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"
    ], check=True)

    subprocess.run([
        str(builder), "self", "--sign", "--in", str(eboot_elf), "--out",
        str(eboot_bin), "--magic", "0x1D3D154F"
    ], check=True)

    # 4. Package metadata and assets
    param = json.loads((gears / "sce_sys/param.json").read_text())
    param.update(
        titleId="PPSA99994",
        conceptId="99994",
        contentId="UP9000-PPSA99994_00-PS5VKCOMPUTE0001"
    )
    param["localizedParameters"]["en-US"]["titleName"] = "PS5 Vulkan CTS Runner"
    (DIST_DIR / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")

    shutil.copyfile(foundation / "runtime/libc.prx", DIST_DIR / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", DIST_DIR / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", DIST_DIR / "dev.conf")

    print(f"Native CTS successfully built and packaged into {DIST_DIR}")
    print(f"eboot.bin sha256: {hashlib.sha256(eboot_bin.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
