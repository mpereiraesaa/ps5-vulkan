#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build public-SDK LocalSizeId default and specialization witnesses offline."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / "tools")]
from tools.build_integer_dot_witness import build_payload, run
from tools.build_sdk import get_ps5_toolchain
from tools.build_upstream_cts import tessellation_build_profile
from tools.lab import lab_root
from tools.maintenance4_local_size_spirv import local_size_id
from tools.prepare_consumer_sync_shaders import emit_array
from tools.verify_maintenance4_local_size_witness import fixture_contract


def ordinary_environment(source, sdk):
    environment = dict(source)
    for name in tessellation_build_profile({})["switches"]:
        environment.pop(name, None)
    environment.update(PS5_PAYLOAD_SDK=str(sdk), PS5VK_USE_SDK="1")
    return environment


def main():
    sdk, wrapper = get_ps5_toolchain()
    if not sdk or not wrapper:
        raise SystemExit("PS5 native toolchain required")
    glslang = shutil.which("glslangValidator")
    if not glslang:
        glslang = ROOT / "build/runtime-graphics/toolchain/usr/bin/glslangValidator"
    if not glslang or not Path(glslang).is_file():
        raise SystemExit("GLSLang required")
    environment = ordinary_environment(os.environ, sdk)
    run(sys.executable, str(ROOT / "tools/build_sdk.py"), env=environment)
    build = ROOT / "build/maintenance4-local-size"
    build.mkdir(parents=True, exist_ok=True)
    base = build / "base.spv"
    subprocess.run([str(glslang), "-V", "--target-env", "vulkan1.0",
        str(ROOT / "experiments/compute/maintenance4_local_size.comp"),
        "-o", str(base)], check=True)
    code = local_size_id(base.read_bytes())
    (build / "local-size-id.spv").write_bytes(code)
    for name, local_x, specialize in (("default64", 64, False), ("specialized32", 32, True)):
        header = ("#include <stdint.h>\n" + emit_array("maintenance4_spirv", code) +
                  f"#define ML_LOCAL_X {local_x}u\n#define ML_SPECIALIZE {int(specialize)}\n")
        build_payload(build / name, ROOT / "examples/maintenance4_local_size_witness/main.c",
            ROOT / "examples/maintenance4_local_size_witness/compute.h",
            "maintenance4_fixture.h", header,
            {**fixture_contract(local_x), "spirv_sha256": hashlib.sha256(code).hexdigest()},
            sdk, wrapper, lab_root(), environment, "PS5 Vulkan Maintenance4 Local Size Witness")


if __name__ == "__main__":
    main()
