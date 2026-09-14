"""Build an exact-module offline program library from audited LLPC outputs."""
import json
from pathlib import Path
import struct

from compile_program import compile_program
from compile_control import digest

ROOT = Path(__file__).resolve().parents[1]


def emit_program(directory, record, index):
    for file, key in (("module.spv", "spirv_sha256"), ("code.bin", "code_sha256")):
        if digest(directory / file) != record[key]:
            raise ValueError(f"Artifact changed before embedding: {file}")
    arrays = []
    lengths = {}
    for field, filename in (("spirv", "module.spv"), ("code", "code.bin")):
        data = (directory / filename).read_bytes()
        if not data or len(data) % 4:
            raise ValueError("Invalid word array")
        words = struct.unpack(f"<{len(data) // 4}I", data)
        lengths[field] = len(words)
        arrays.append(f"static const uint32_t program_{index}_{field}[] = {{\n" +
                      ",".join(f"0x{word:08x}u" for word in words) + "\n};\n")
    abi = record["abi"]
    registers = abi["compute_registers"]
    values = [f".spirv=program_{index}_spirv", f".spirv_words={lengths['spirv']}",
              f".code=program_{index}_code", f".code_words={lengths['code']}",
              '.entry="main"', ".gfx=1013",
              ".local_size={" + ",".join(map(str, abi["local_size"])) + "}"]
    for field in ("wave_size", "vgprs", "sgprs", "float_mode", "ieee_mode", "mem_ordered", "user_sgprs"):
        values.append(f".{field}={int(abi[field])}")
    values += [f".tg_size={int(registers['.tg_size_en'])}",
               ".tgid={" + ",".join(str(int(registers[f".tgid_{axis}_en"])) for axis in "xyz") + "}",
               f".tidig_components={registers['.tidig_comp_cnt']}",
               # Pinned standalone amdllpc ABI: s0 internal, set 0 table at s1.
               # This is intentionally distinct from PSBC's reusable s2+ ABI.
               ".descriptor_set_mask=1", ".descriptor_set_sgpr={1}",
               f".descriptor_count={len(abi['descriptors'])}"]
    descriptors = ["{" + ",".join(str(d[k]) for k in ("set", "binding", "array_element", "table_offset_dwords")) +
                   ",VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}"
                   for d in abi["descriptors"]]
    values.append(".descriptors={" + ",".join(descriptors) + "}")
    return "".join(arrays), "{" + ",\n".join(values) + "}"


def main():
    artifacts = [compile_program(ROOT / "experiments/compute" / name)
                 for name in ("minimal.comp", "xor.comp")]
    emitted = [emit_program(directory, record, j) for j, (directory, record) in enumerate(artifacts)]
    output = ROOT / "build/program-library"
    output.mkdir(parents=True, exist_ok=True)
    header = output / "program_library.h"
    header.write_text("/* Generated exact-module offline library; no GPU validation. */\n"
                      '#include "vk_pipeline.h"\n' + "".join(pair[0] for pair in emitted) +
                      "static const struct ps5vk_compiled_program compiled_programs[] = {\n" +
                      ",\n".join(pair[1] for pair in emitted) + "\n};\n"
                      "static struct ps5vk_program_library ps5vk_compiled_library = {compiled_programs, "
                      "sizeof(compiled_programs)/sizeof(compiled_programs[0])};\n")
    manifest = {"scope": "offline-program-library", "hardware_validated": False,
                "header_sha256": digest(header), "programs": [r for _, r in artifacts]}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Generated {len(artifacts)} exact-module programs: {header}")


if __name__ == "__main__":
    main()
