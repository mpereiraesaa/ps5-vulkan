#!/usr/bin/env python3
"""Build libpsbc for PS5 or host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from lab import lab_root

ROOT = Path(__file__).resolve().parents[1]


def write_identity(out_lib: Path, psbc_dir: Path, target: str) -> None:
    revision = subprocess.check_output(
        ["git", "-C", str(psbc_dir), "rev-parse", "HEAD"], text=True).strip()
    identity = {
        "schema": 1,
        "target": target,
        "source_commit": revision,
        "archive_sha256": hashlib.sha256(out_lib.read_bytes()).hexdigest(),
    }
    out_lib.with_suffix(".json").write_text(json.dumps(identity, indent=2) + "\n")


def get_sdk():
    explicit = os.environ.get("PS5_PAYLOAD_SDK")
    if explicit:
        sdk = Path(explicit).resolve()
    else:
        sdk = lab_root() / "third_party/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk"
    if not (sdk / "bin/prospero-clang").is_file():
        sys.exit(f"PS5 payload SDK not found or incomplete at {sdk}")
    return sdk


def ensure_generated(psbc_dir):
    # Ensure generated files exist from Mesa/ACO/NIR
    subprocess.run([
        "make", "-C", str(psbc_dir),
        f"CONFIG={ROOT}/tools/psbc-host-config.mak",
        f"PS5VK_ROOT={ROOT}",
        "generated"
    ], check=True)

    # Format tables and headers
    fmt_dir = psbc_dir / "src/util/format"
    if not (fmt_dir / "u_format_gen.h").exists():
        subprocess.run(
            [sys.executable, "u_format_table.py", "u_format.yaml", "--enums"],
            cwd=fmt_dir, stdout=open(fmt_dir / "u_format_gen.h", "wb"), check=True
        )
    if not (fmt_dir / "u_format_pack.h").exists():
        subprocess.run(
            [sys.executable, "u_format_table.py", "u_format.yaml", "--header"],
            cwd=fmt_dir, stdout=open(fmt_dir / "u_format_pack.h", "wb"), check=True
        )
    if not (fmt_dir / "u_format_table.c").exists():
        subprocess.run(
            [sys.executable, "u_format_table.py", "u_format.yaml"],
            cwd=fmt_dir, stdout=open(fmt_dir / "u_format_table.c", "wb"), check=True
        )

    # Format sRGB
    srgb_c = psbc_dir / "src/util/format_srgb.c"
    if not srgb_c.exists():
        subprocess.run(
            [sys.executable, "format_srgb.py"],
            cwd=psbc_dir / "src/util", stdout=open(srgb_c, "wb"), check=True
        )

    # Builtin types
    comp_dir = psbc_dir / "src/compiler"
    if not (comp_dir / "builtin_types.h").exists():
        subprocess.run([sys.executable, "builtin_types_h.py", "builtin_types.h"], cwd=comp_dir, check=True)
    if not (comp_dir / "builtin_types.c").exists():
        subprocess.run([sys.executable, "builtin_types_c.py", "builtin_types.c"], cwd=comp_dir, check=True)

    # Shader stats
    stats_h = psbc_dir / "src/util/shader_stats.h"
    if not stats_h.exists():
        subprocess.run(
            [sys.executable, "process_shader_stats.py", "shader_stats.rnc", "shader_stats.xml"],
            cwd=psbc_dir / "src/util", stdout=open(stats_h, "wb"), check=True
        )

    # Vulkan util headers
    vk_xml = ROOT / "third_party/vulkan-headers/registry/vk.xml"
    vk_util_dir = psbc_dir / "src/vulkan/util"
    if not (vk_util_dir / "vk_struct_type_cast.h").exists():
        subprocess.run([
            sys.executable, "vk_struct_type_cast_gen.py",
            "--xml", str(vk_xml),
            "--out", "vk_struct_type_cast.h",
            "--beta", "false"
        ], cwd=vk_util_dir, check=True)

    if not (vk_util_dir / "vk_enum_defines.h").exists():
        subprocess.run([
            sys.executable, "gen_enum_to_str.py",
            "--xml", str(vk_xml),
            "--out-c", "vk_enum_to_str.c",
            "--out-h", "vk_enum_to_str.h",
            "--out-d", "vk_enum_defines.h",
            "--beta", "false"
        ], cwd=vk_util_dir, check=True)

    # GFX10 format table
    amd_common = psbc_dir / "src/amd/common"
    if not (amd_common / "gfx10_format_table.c").exists():
        subprocess.run([
            sys.executable, "gfx10_format_table.py",
            "../../util/format/u_format.yaml",
            "../registers/gfx10-rsrc.json",
            "../registers/gfx11-rsrc.json"
        ], cwd=amd_common, stdout=open(amd_common / "gfx10_format_table.c", "wb"), check=True)

    # AMD CP packets
    packets_dir = psbc_dir / "src/amd/packets"
    for gen in ["gfx11", "gfx12"]:
        pkt_h = amd_common / f"amd_cp_packets_{gen}.h"
        if not pkt_h.exists():
            subprocess.run([
                sys.executable, "parse_cp_pm4_table_data_json.py",
                "cp_pm4_table_data_gfx11.json",
                "pm4_it_opcodes_gfx11.h",
                "cp_pm4_table_data_gfx12.json",
                "pm4_it_opcodes_gfx12.h",
                gen, "packets_h"
            ], cwd=packets_dir, stdout=open(pkt_h, "wb"), check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=["ps5", "host"], default="ps5", help="Target platform (ps5 or host)")
    parser.add_argument("--host", action="store_true", help="Build libpsbc.host.a for host testing (alias for --target=host)")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 4, help="Parallel compile jobs")
    parser.add_argument("--clean", action="store_true", help="Clean object files before build")
    args = parser.parse_args()

    is_host = args.host or args.target == "host"
    psbc_dir = Path(os.environ.get(
        "PS5VK_PSBC_SOURCE", ROOT / "third_party/psbc-reference")).resolve()
    if not (psbc_dir / "libpsbc/psbc_compile.c").is_file():
        sys.exit(f"PSBC source tree not found or incomplete at {psbc_dir}")

    if is_host:
        makefile = ROOT / "tools/Makefile.psbc-host"
        out_lib = ROOT / "build/libpsbc.host.a"
        if args.clean:
            for p in psbc_dir.rglob("*.host.o"):
                p.unlink()
            for p in psbc_dir.rglob("*.host.cpp.o"):
                p.unlink()
            if out_lib.exists():
                out_lib.unlink()
        ensure_generated(psbc_dir)
        cmd = [
            "make", "-C", str(psbc_dir),
            "-f", str(makefile),
            f"-j{args.jobs}",
            f"PS5VK_ROOT={ROOT}",
            "libpsbc"
        ]
        subprocess.run(cmd, check=True)
        write_identity(out_lib, psbc_dir, "host")
        print(f"Successfully built {out_lib}")
        return

    sdk = get_sdk()
    makefile = ROOT / "tools/Makefile.psbc-ps5"
    out_lib = ROOT / "build/libpsbc.ps5.a"

    if args.clean:
        for p in psbc_dir.rglob("*.ps5.o"):
            p.unlink()
        for p in psbc_dir.rglob("*.ps5.cpp.o"):
            p.unlink()
        if out_lib.exists():
            out_lib.unlink()

    ensure_generated(psbc_dir)

    # Build libpsbc.ps5.a
    cmd = [
        "make", "-C", str(psbc_dir),
        "-f", str(makefile),
        f"-j{args.jobs}",
        f"PS5VK_ROOT={ROOT}",
        f"PS5_PAYLOAD_SDK={sdk}",
        "libpsbc"
    ]
    subprocess.run(cmd, check=True)
    write_identity(out_lib, psbc_dir, "ps5")
    print(f"Successfully built {out_lib}")


if __name__ == "__main__":
    main()
