"""Compile a requested compute program and export its checked ABI, host-only.

This is an offline compiler adapter, not a native runtime compiler or evidence
of GPU execution. Exact SPIR-V identity travels with every generated artifact.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import tempfile

import yaml

from compile_control import EXPECTED_COMPILER, digest
from lab import lab_root

ROOT = Path(__file__).resolve().parents[1]


def module_contract(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError("Malformed SPIR-V size")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203 or words[4] != 0:
        raise ValueError("Unsupported SPIR-V header")
    entries, local_sizes = [], {}
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xffff
        if not count or count > len(words) - i:
            raise ValueError("Malformed SPIR-V instruction")
        args = words[i + 1:i + count]
        if opcode == 15:  # OpEntryPoint
            if len(args) < 3:
                raise ValueError("Malformed entry point")
            name_bytes = struct.pack(f"<{len(args) - 2}I", *args[2:])
            if b"\0" not in name_bytes:
                raise ValueError("Unterminated entry name")
            entries.append((args[0], args[1], name_bytes.split(b"\0", 1)[0].decode("utf-8")))
        if opcode == 16 and len(args) >= 2 and args[1] == 17:  # LocalSize
            if len(args) != 5 or args[0] in local_sizes:
                raise ValueError("Malformed/duplicate local size")
            local_sizes[args[0]] = list(args[2:])
        i += count
    if len(entries) != 1 or entries[0][0] != 5 or entries[0][2] != "main":
        raise ValueError("Adapter requires one compute entry named main")
    if entries[0][1] not in local_sizes:
        raise ValueError("Adapter requires literal LocalSize (no specialization)")
    return {"entry": entries[0][2], "local_size": local_sizes[entries[0][1]]}


class MetadataLoader(yaml.SafeLoader):
    pass


MetadataLoader.add_constructor("!str", lambda loader, node: loader.construct_scalar(node))


def metadata(notes):
    if "amdgcn--amdpal--gfx1013" not in notes:
        raise ValueError("Wrong ISA target")
    start = notes.index("AMDGPU Metadata: ---") + len("AMDGPU Metadata: ")
    end = notes.index("\n...", start) + len("\n...")
    parsed = yaml.load(notes[start:end], Loader=MetadataLoader)
    if parsed["amdpal.version"] != [3, 0] or len(parsed["amdpal.pipelines"]) != 1:
        raise ValueError("Unsupported metadata version/pipeline count")
    pipeline = parsed["amdpal.pipelines"][0]
    if pipeline[".type"] != "Cs" or set(pipeline[".hardware_stages"]) != {".cs"}:
        raise ValueError("Not an isolated compute stage")
    cs = pipeline[".hardware_stages"][".cs"]
    for name in (".scratch_en", ".scratch_memory_size", ".lds_size", ".debug_mode",
                 ".trap_present", ".excp_en", ".wgp_mode"):
        if cs[name]:
            raise ValueError(f"Unsupported compute requirement: {name}")
    if cs[".wavefront_size"] != 32 or cs[".entry_point_symbol"] != "_amdgpu_cs_main":
        raise ValueError("Unsupported wave/entry ABI")
    if cs[".user_sgprs"] != 2 or cs[".user_data_reg_map"] != [0x10000000, 0] + [0xffffffff] * 30:
        raise ValueError("Unsupported user-data mapping")
    dims = cs[".threadgroup_dimensions"]
    if len(dims) != 3 or any(type(n) is not int or n < 1 or n > 1024 for n in dims):
        raise ValueError("Invalid local size")
    if dims[0] * dims[1] * dims[2] > 1024:
        raise ValueError("Local invocation count exceeds adapter bound")
    if not 1 <= cs[".vgpr_count"] <= 256 or not 1 <= cs[".sgpr_count"] <= 106:
        raise ValueError("Register allocation outside adapter bounds")
    registers = pipeline[".compute_registers"]
    if set(registers) != {".tg_size_en", ".tgid_x_en", ".tgid_y_en", ".tgid_z_en", ".tidig_comp_cnt"}:
        raise ValueError("Unknown compute register requirement")
    if registers[".tidig_comp_cnt"] not in (0, 1, 2):
        raise ValueError("Unsupported local invocation ID ABI")
    return {"local_size": dims, "wave_size": cs[".wavefront_size"],
            "vgprs": cs[".vgpr_count"], "sgprs": cs[".sgpr_count"],
            "float_mode": cs[".float_mode"], "ieee_mode": cs[".ieee_mode"],
            "mem_ordered": cs[".mem_ordered"], "user_sgprs": cs[".user_sgprs"],
            "compute_registers": registers, "entry_symbol": cs[".entry_point_symbol"],
            "scratch_bytes": 0, "lds_bytes": 0}


def resources(pipe):
    section = pipe.split("[ResourceMapping]\n", 1)[1].split("\n[", 1)[0]
    values = {}
    for line in section.splitlines():
        if not line.strip():
            continue
        key, value = (item.strip() for item in line.split("=", 1))
        if key in values:
            raise ValueError("Duplicate resource mapping key")
        values[key] = value
    root = "userDataNode[0]"
    required = {root + ".visibility": "128", root + ".type": "DescriptorTableVaPtr",
                root + ".offsetInDwords": "0", root + ".sizeInDwords": "1"}
    for key, value in required.items():
        if values.pop(key, None) != value:
            raise ValueError(f"Unsupported table root: {key}")
    nodes = {}
    for key, value in values.items():
        match = re.fullmatch(r"userDataNode\[0\]\.next\[(\d+)\]\.(\w+)", key)
        if not match:
            raise ValueError(f"Unsupported resource mapping: {key}")
        nodes.setdefault(int(match[1]), {})[match[2]] = value
    if not nodes or len(nodes) > 32 or sorted(nodes) != list(range(len(nodes))):
        raise ValueError("Invalid descriptor nodes")
    result, occupied, bindings = [], set(), set()
    for index in sorted(nodes):
        node = nodes[index]
        if set(node) != {"type", "offsetInDwords", "sizeInDwords", "set", "binding", "strideInDwords"}:
            raise ValueError("Unknown descriptor field")
        if node["type"] != "DescriptorBuffer" or int(node["sizeInDwords"], 0) != 4 or int(node["strideInDwords"], 0):
            raise ValueError("Only scalar raw-buffer descriptors are supported by this adapter")
        offset = int(node["offsetInDwords"], 0)
        binding, descriptor_set = int(node["binding"], 0), int(node["set"], 0)
        if descriptor_set != 0 or binding not in range(32) or binding in bindings:
            raise ValueError("Unsupported/duplicate descriptor binding")
        if offset not in range(0, 128, 4) or offset in occupied:
            raise ValueError("Overlapping/unaligned descriptor table")
        occupied.add(offset); bindings.add(binding)
        result.append({"set": descriptor_set, "binding": binding, "array_element": 0,
                       "table_offset_dwords": offset, "size_dwords": 4})
    return result


def compile_program(source):
    source = source.resolve()
    if source.suffix not in (".comp", ".spv"):
        raise ValueError("Expected GLSL compute .comp or SPIR-V .spv")
    build = Path(os.environ.get("PS5VK_LLPC_BUILD", str(
        lab_root() / "third_party/amd-llpc/build-gfx1030"))).resolve()
    compiler = build / "llpc/amdllpc"
    if digest(compiler) != EXPECTED_COMPILER:
        raise ValueError("LLPC compiler identity changed")
    output = ROOT / "build/programs"
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=source.stem + "-", dir=output))

    def invoke(*args):
        subprocess.run([str(compiler), "-gfxip=10.1.3", *map(str, args)],
                       cwd=run, check=True, capture_output=True, text=True)

    invoke("-enable-pipeline-dump", f"-pipeline-dump-dir={run}", "-o=dump.elf", source)
    if source.suffix == ".spv":
        spirv = source
    else:
        spirv, = run.glob("Shader_*.spv")
    # Retain exact module bytes, not an identifier that silently substitutes a
    # built-in shader. Each repeated compile is from these SPIR-V bytes.
    (run / "module.spv").write_bytes(spirv.read_bytes())
    spirv = run / "module.spv"
    module = module_contract(spirv.read_bytes())
    for name in ("program.elf", "repeat.elf"):
        invoke(f"-o={name}", spirv)
    elf = run / "program.elf"
    if elf.read_bytes() != (run / "repeat.elf").read_bytes():
        raise ValueError("Compiler output is not reproducible")
    # Dump the mapping again from the same SPIR-V used for the final ELF.
    mapping_dir = run / "mapping"
    mapping_dir.mkdir()
    invoke("-enable-pipeline-dump", f"-pipeline-dump-dir={mapping_dir}", "-o=mapped.elf", spirv)
    # Pipeline dumping adds ELF sections. Require identical ISA and raw PAL
    # notes, not identical container offsets or diagnostic-only sections.
    for name, input_file in (("program", elf), ("mapping", run / "mapped.elf")):
        subprocess.run(["llvm-objcopy-18", "--dump-section", f".text={run / (name + '.text')}",
                        "--dump-section", f".note={run / (name + '.note')}",
                        str(input_file), str(run / (name + '-copy.elf'))], check=True)
    for section in ("text", "note"):
        if (run / f"program.{section}").read_bytes() != (run / f"mapping.{section}").read_bytes():
            raise ValueError(f"Mapping dump differs from compiled {section}")
    pipe, = mapping_dir.glob("PipelineCs_*.pipe")
    notes = subprocess.check_output([str(build / "llvm/bin/llvm-readobj"), "--notes", str(elf)], text=True)
    abi = metadata(notes)
    if abi["local_size"] != module["local_size"]:
        raise ValueError("Compiler local size differs from requested module")
    abi["descriptors"] = resources(pipe.read_text())
    subprocess.run(["llvm-objcopy-18", "--dump-section", f".text={run / 'code.bin'}",
                    str(elf), str(run / "copy.elf")], check=True)
    symbols = subprocess.check_output([str(build / "llvm/bin/llvm-readobj"), "--symbols", str(elf)], text=True)
    entry = re.search(r"Name: _amdgpu_cs_main[^\n]*\n\s+Value: (0x[0-9A-Fa-f]+)", symbols)
    if not entry or int(entry[1], 16) != 0:
        raise ValueError("Unsupported nonzero/absent code entry offset")
    code = run / "code.bin"
    if not code.stat().st_size or code.stat().st_size % 4:
        raise ValueError("Invalid ISA byte length")
    abi["entry_offset"] = 0
    record = {"schema": "ps5vk-compute-artifact/1", "target": "gfx1013", "entry": "main",
              "scope": "offline-compiler-only", "hardware_validated": False,
              "compiler_sha256": digest(compiler), "source_sha256": digest(source),
              "spirv_sha256": digest(spirv), "elf_sha256": digest(elf),
              "code_sha256": digest(code), "code_bytes": code.stat().st_size,
              "mapping_elf_sha256": digest(run / "mapped.elf"), "mapping_code_and_notes_identical": True,
              "repeat_identical": True, "abi": abi}
    (run / "metadata.txt").write_text(notes)
    (run / "resource-mapping.pipe").write_text(pipe.read_text())
    (run / "manifest.json").write_text(json.dumps(record, indent=2) + "\n")
    return run, record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    directory, manifest = compile_program(args.source)
    print(json.dumps(manifest, indent=2))
    print(f"Generated private artifact: {directory}")
