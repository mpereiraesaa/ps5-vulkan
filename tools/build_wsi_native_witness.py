#!/usr/bin/env python3
"""Build an isolated, public-SDK Vulkan WSI presentation witness."""

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

PROFILE = "vulkan-wsi-native-lifecycle"


def run(*command: str, env: dict | None = None) -> None:
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def main() -> None:
    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    sdk, clang_wrapper = get_ps5_toolchain()
    if not sdk or not clang_wrapper:
        raise SystemExit("PS5 native toolchain is required")
    builder = foundation / "build/host/ps5-native-tool"
    if not builder.is_file():
        raise SystemExit("ps5-native-tool is required")
    logger = lab / "projects/logging_server/client"
    dxvk = ROOT / "third_party/dxvk-v2.6.2"
    expected_dxvk = json.loads((ROOT / "conformance_inventory/dxvk_v262_profile.json")
                               .read_text())["source"]["commit"]
    actual_dxvk = subprocess.check_output(
        ["git", "-C", str(dxvk), "rev-parse", "HEAD"], text=True).strip()
    if actual_dxvk != expected_dxvk:
        raise SystemExit("pinned DXVK source is required for adapter witness")
    build = ROOT / "build/wsi-native-witness"
    dist = build / "dist/PPSA99994"
    for directory in (build, dist / "sce_sys", dist / "sce_module"):
        directory.mkdir(parents=True, exist_ok=True)

    sdk_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk), PS5VK_USE_SDK="1")
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=sdk_env)
    staged = ROOT / "dist-sdk"
    source = ROOT / "tests/test_wsi_native_witness.c"
    obj = build / "main.o"
    dependencies = build / "main.d"
    run("sh", str(clang_wrapper), "-std=c11", "-O2", "-g", "-Wall",
        "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        "-MD", "-MP", "-MF", str(dependencies),
        "-I" + str(staged / "include"), "-I" + str(logger),
        "-c", str(source), "-o", str(obj), env=sdk_env)
    dependency_text = dependencies.read_text()
    if str(ROOT / "src/") in dependency_text or str(ROOT / "native/") in dependency_text:
        raise RuntimeError("witness includes a private runtime header")
    undefined = subprocess.check_output(["nm", "-u", str(obj)], text=True)
    if any(line.split()[-1].startswith(("ps5vk_", "ps5_"))
           for line in undefined.splitlines() if line.split()):
        raise RuntimeError("witness calls a private runtime symbol")

    cxx = sdk / "bin/prospero-clang++"
    adapter_source = ROOT / "tools/dxvk_ps5_wsi.cpp"
    bridge_source = ROOT / "tests/test_wsi_adapter_native.cpp"
    dxvk_includes = [dxvk / "src/wsi", dxvk / "include/native",
                     dxvk / "include/native/windows",
                     dxvk / "include/native/directx",
                     dxvk / "include/vulkan/include"]
    cxx_objects = []
    for name, cpp_source in (("dxvk_adapter", adapter_source),
                             ("adapter_bridge", bridge_source)):
        cpp_obj = build / f"{name}.o"
        cpp_deps = build / f"{name}.d"
        run(str(cxx), "-std=c++17", "-O2", "-g", "-Wall", "-Wextra",
            "-Werror", "-Wno-unused-parameter", "-DDXVK_WSI_PS5",
            "-ffunction-sections", "-fdata-sections", "-MD", "-MP",
            "-MF", str(cpp_deps),
            *("-I" + str(path) for path in dxvk_includes),
            "-I" + str(staged / "include"),
            "-c", str(cpp_source), "-o", str(cpp_obj), env=sdk_env)
        if (str(ROOT / "src/") in cpp_deps.read_text() or
                str(ROOT / "native/") in cpp_deps.read_text()):
            raise RuntimeError("adapter witness includes a private runtime header")
        cxx_objects.append(cpp_obj)

    linker = sdk / "bin/prospero-lld"
    pie = build / "witness_pie.elf"
    eboot_elf = build / "eboot.elf"
    eboot = dist / "eboot.bin"
    link = [str(linker), "-L" + str(sdk / "target/lib"),
            "-T", str(staged / "lib/ps5-pie.ld"), "--eh-frame-hdr",
            "--version-script", str(staged / "lib/app-symbols.map"),
            "-Map=" + str(build / "witness.map"), "-e", "_start", "-o", str(pie),
            str(staged / "lib/crt.o"), str(obj), *(str(path) for path in cxx_objects),
            str(staged / "lib/libps5vk.a"), str(staged / "lib/libpsbc.a"),
            *[str(sdk / f"target/lib/{name}") for name in
              ("libc++.a", "libc++abi.a", "libunwind.a", "libpthread.a", "libc.a")],
            "--as-needed",
            *sorted(str(path) for path in (sdk / "target/lib").glob("*.so")),
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
                 contentId="UP9000-PPSA99994_00-PS5VKWSI00000001")
    param["localizedParameters"]["en-US"]["titleName"] = "PS5 Vulkan WSI Witness"
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")
    artifact = {"profile": PROFILE, "sdk_switches": {},
                "dxvk_commit": actual_dxvk,
                "adapter_sha256": hashlib.sha256(adapter_source.read_bytes()).hexdigest(),
                "bridge_sha256": hashlib.sha256(bridge_source.read_bytes()).hexdigest(),
                "eboot_sha256": hashlib.sha256(eboot.read_bytes()).hexdigest(),
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
    artifact_path = dist.parent / "artifact.json"
    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"{artifact_path}: eboot {artifact['eboot_sha256']}")


if __name__ == "__main__":
    main()
