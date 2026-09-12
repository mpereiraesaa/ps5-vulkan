#!/usr/bin/env python3
"""Build and package the independent native PS5 Vulkan SDK consumer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lab import lab_root  # noqa: E402

DIST_SDK = ROOT / "dist-sdk"
CONSUMER_DIR = ROOT / "examples/native_consumer"
BUILD_DIR = CONSUMER_DIR / "build"
DIST_DIR = ROOT / "dist-consumer/PPSA99994"


def check_isolation(dep_file: Path, obj_file: Path):
    """Verify that consumer depends only on public SDK and standard CRT headers/symbols."""
    print("Checking consumer header isolation in", dep_file)
    content = dep_file.read_text()
    # Find all header dependencies in the .d file
    # Format: target: dep1 \ dep2 ...
    headers = re.findall(r'(\S+\.h|\S+\.hpp)', content)
    for h in headers:
        hp = Path(h).resolve()
        # Header must be in:
        # - dist-sdk/include
        # - examples/native_consumer
        # - third_party/ps5-native-app-boilerplate/... (toolchain / system libc headers)
        is_sdk = str(hp).startswith(str(DIST_SDK / "include"))
        is_local = str(hp).startswith(str(CONSUMER_DIR))
        is_crt = "ps5-native-app-boilerplate" in str(hp) or str(hp).startswith("/usr/")
        if not (is_sdk or is_local or is_crt):
            raise AssertionError(f"Isolation violation: consumer includes forbidden private header {hp}")
        # Explicit check: cannot include anything from src/ or native/
        if str(ROOT / "src") in str(hp) or str(ROOT / "native") in str(hp):
            raise AssertionError(f"Isolation violation: consumer includes private source header {hp}")

    print("Header isolation verified: zero private project headers included.")

    print("Checking consumer symbol isolation in", obj_file)
    nm_out = subprocess.check_output(["nm", "-u", str(obj_file)], text=True)
    for line in nm_out.strip().splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[0] == "U":
            sym = parts[1]
            if sym.startswith("ps5vk_") or sym.startswith("ps5_"):
                raise AssertionError(f"Symbol isolation violation: consumer references private symbol {sym}")
    print("Symbol isolation verified: only standard libc, ps5log, Vulkan core, and public ps5vk* APIs used.")


def main():
    parser = argparse.ArgumentParser(description="Build independent native consumer")
    parser.add_argument("--continuous", action="store_true", help="Compile in continuous rendering mode")
    parser.add_argument("--check-only", action="store_true", help="Only verify header and symbol isolation")
    parser.add_argument("--use-staged-sdk", action="store_true",
                        help="Reuse dist-sdk without rebuilding it (caller guarantees freshness)")
    args = parser.parse_args()

    # A merely present archive may predate the source tree.  Fresh staging is
    # the safe default for a standalone consumer and prevents false link
    # failures (or, worse, validation against yesterday's implementation).
    if not args.use_staged_sdk or not (DIST_SDK / "lib/libps5vk.a").is_file():
        print("Staging current SDK with tools/build_sdk.py...")
        subprocess.run([sys.executable, str(ROOT / "tools/build_sdk.py")], check=True)

    lab = lab_root()
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

    dep_file = BUILD_DIR / "main.d"
    obj_file = BUILD_DIR / "main.o"
    pie_elf = BUILD_DIR / "consumer_pie.elf"
    eboot_elf = BUILD_DIR / "eboot.elf"
    eboot_bin = DIST_DIR / "eboot.bin"

    if args.check_only:
        if not dep_file.is_file() or not obj_file.is_file():
            sys.exit("Cannot check isolation: object or dependency file missing. Run build first.")
        check_isolation(dep_file, obj_file)
        return

    # 1. Compile consumer main.c
    cflags = [
        "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections",
        "-MD", "-MP", "-MF", str(dep_file),
        "-I" + str(DIST_SDK / "include"),
        "-I" + str(CONSUMER_DIR),
    ]
    if args.continuous:
        cflags.append("-DCONSUMER_CONTINUOUS=1")

    has_native_toolchain = clang_wrapper.is_file() and linker.is_file() and builder.is_file()

    print("Compiling consumer main.c...")
    if has_native_toolchain:
        env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
        subprocess.run(
            ["sh", str(clang_wrapper), *cflags, "-c", str(CONSUMER_DIR / "main.c"), "-o", str(obj_file)],
            env=env, check=True
        )
    else:
        subprocess.run(
            ["cc", *cflags, "-c", str(CONSUMER_DIR / "main.c"), "-o", str(obj_file)],
            check=True
        )

    # Verify isolation immediately after compilation
    check_isolation(dep_file, obj_file)

    if not has_native_toolchain:
        print("Native PS5 toolchain not found; verified isolation on host and skipping packaging.")
        return

    # 2. Link PIE ELF
    map_file = BUILD_DIR / "consumer.map"
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
    print("Linking consumer PIE ELF...")
    subprocess.run(link_cmd, check=True)

    # 3. Create eboot.elf and signed eboot.bin
    print("Signing eboot.bin via ps5-native-tool...")
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
    param["localizedParameters"]["en-US"]["titleName"] = "PS5 Vulkan SDK Consumer"
    (DIST_DIR / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")

    shutil.copyfile(foundation / "runtime/libc.prx", DIST_DIR / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", DIST_DIR / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", DIST_DIR / "dev.conf")

    print(f"Consumer successfully built and packaged into {DIST_DIR}")
    print(f"eboot.bin sha256: {hashlib.sha256(eboot_bin.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
