#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build separate SDK-linked integer-dot executables offline, one per variant."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'tools')]
from tools.build_sdk import get_ps5_toolchain
from tools.lab import lab_root
from tools.build_upstream_cts import tessellation_build_profile
from tools.prepare_consumer_sync_shaders import emit_array
from tools.integer_dot_spirv import binary
from tools.integer_dot_vectors import buffers
from tools.verify_integer_dot_witness import CASES, fixture_contract


def run(*command, env=None):
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def diagnostic_environment(environment, sdk):
    result = dict(environment)
    for name in tessellation_build_profile({})['switches']:
        result.pop(name, None)
    result.update(PS5_PAYLOAD_SDK=str(sdk), PS5VK_USE_SDK='1', PS5VK_INTEGER_DOT_DIAGNOSTIC='1')
    return result


def fixture_header(case):
    values = buffers(case)
    names = ('dot_lhs', 'dot_rhs', 'dot_accumulator', 'dot_expected')
    text = '#include <stdint.h>\n' + emit_array('integer_dot_spirv', binary(case))
    for name, value in zip(names, values):
        text += f"static const uint8_t {name}[] = {{\n"
        text += ",\n".join("    " + ", ".join(f"0x{byte:02x}" for byte in value[i:i+16])
                             for i in range(0, len(value), 16)) + "\n};\n"
    text += f'\n#define DOT_CASE_NAME "{case.name}"\n'
    text += 'static const struct integer_dot_data dot_fixture = {\n.inputs={'
    text += ','.join('(const unsigned char *)' + name for name in names[:3])
    text += '},.sizes={' + ','.join(str(len(value)) for value in values[:3])
    text += '},.expected=(const unsigned char *)dot_expected};\n'
    return text


def build_case(case, sdk, clang_wrapper, lab, sdk_env):
    foundation = lab / 'third_party/ps5-native-app-boilerplate'
    builder = foundation / 'build/host/ps5-native-tool'
    logger = lab / 'projects/logging_server/client'
    build = ROOT / 'build/integer-dot-witness' / case.name
    dist = build / 'dist/PPSA99994'
    for directory in (build, dist / 'sce_sys', dist / 'sce_module'):
        directory.mkdir(parents=True, exist_ok=True)
    (build / 'integer_dot_fixture.h').write_text(fixture_header(case))
    staged = ROOT / "dist-sdk"
    source = ROOT / "examples/integer_dot_witness/main.c"
    obj = build / "main.o"
    dep = build / "main.d"
    run("sh", str(clang_wrapper), "-std=c11", "-O2", "-g", "-Wall",
        "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        "-MD", "-MP", "-MF", str(dep),
        "-I" + str(staged / "include"), "-I" + str(build),
        "-I" + str(logger), "-c", str(source), "-o", str(obj), env=sdk_env)
    dependencies = dep.read_text()
    if str(ROOT / "src/") in dependencies or str(ROOT / "native/") in dependencies:
        raise RuntimeError("witness includes a private runtime header")
    undefined = subprocess.check_output(["nm", "-u", str(obj)], text=True)
    if any(line.split()[-1].startswith(("ps5vk_", "ps5_"))
           for line in undefined.splitlines() if line.split()):
        raise RuntimeError("witness calls a private runtime symbol")

    linker = sdk / "bin/prospero-lld"
    pie = build / "witness_pie.elf"
    eboot_elf = build / "eboot.elf"
    eboot = dist / "eboot.bin"
    link = [str(linker), "-L" + str(sdk / "target/lib"),
            "-T", str(staged / "lib/ps5-pie.ld"), "--eh-frame-hdr",
            "--version-script", str(staged / "lib/app-symbols.map"),
            "-Map=" + str(build / "witness.map"), "-e", "_start", "-o", str(pie),
            str(staged / "lib/crt.o"), str(obj),
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
                 contentId="UP9000-PPSA99994_00-PS5VKDOT00000001")
    param["localizedParameters"]["en-US"]["titleName"] = (
        "PS5 Vulkan Integer Dot Witness")
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")
    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")

    artifact = fixture_contract(case.name)
    artifact.update(
        eboot_sha256=hashlib.sha256(eboot.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        helper_sha256=hashlib.sha256((ROOT / 'examples/dxvk_render_witness/integer_dot_compute.h').read_bytes()).hexdigest(),
        sdk_sha256=hashlib.sha256((staged / 'lib/libps5vk.a').read_bytes()).hexdigest(),
        build_profile=tessellation_build_profile(sdk_env), native_executed=False)
    artifact_path = dist.parent / 'artifact.json'
    artifact_path.write_text(json.dumps(artifact, indent=2) + '\n')
    print(f'{artifact_path}: eboot {artifact["eboot_sha256"]}')
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument('--case', choices=CASES)
    choice.add_argument('--all', action='store_true')
    args = parser.parse_args()
    sdk, clang_wrapper = get_ps5_toolchain()
    lab = lab_root()
    if not sdk or not clang_wrapper or not (lab / 'third_party/ps5-native-app-boilerplate/build/host/ps5-native-tool').is_file():
        raise SystemExit('PS5 native toolchain is required')
    env = diagnostic_environment(os.environ, sdk)
    run(sys.executable, str(ROOT / 'tools/build_sdk.py'), env=env)
    for case in (CASES.values() if args.all else (CASES[args.case],)):
        build_case(case, sdk, clang_wrapper, lab, env)


if __name__ == '__main__':
    main()
