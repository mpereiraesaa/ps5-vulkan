"""Compile a project-owned graphics control; no native/API acceptance implied."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import struct
import tempfile
from compile_control import EXPECTED_COMPILER, digest
from lab import lab_root
from graphics_object import unpack_graphics_object
from graphics_metadata import decode_graphics_metadata
from graphics_upload import plan_graphics_upload
from graphics_registers import RegisterSchema, shader_context
from graphics_header import header_values, render_header
from graphics_spirv import graphics_entry

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pipe", type=Path)
    parser.add_argument("--inspect-only", action="store_true",
                        help="emit compiler evidence without a native ABI adapter or API library")
    args = parser.parse_args()
    source = args.pipe.resolve()
    if ROOT not in source.parents or not source.is_file():
        raise SystemExit("Use an owned input inside ps5vk")
    compiler_root = lab_root() / "third_party/amd-llpc/build-gfx1030"
    compiler = compiler_root / "llpc/amdllpc"
    if digest(compiler) != EXPECTED_COMPILER:
        raise SystemExit("Compiler identity changed; audit before proceeding")
    parent = ROOT / "build/graphics"
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="control-", dir=parent))
    for name in ("first", "repeat"):
        subprocess.run([str(compiler), "-gfxip=10.1.3", f"-o={out / (name + '.elf')}", str(source)],
                       cwd=out, check=True, capture_output=True)
    elf = out / "first.elf"
    if digest(elf) != digest(out / "repeat.elf"):
        raise SystemExit("Graphics compiler output is not reproducible")
    reader = compiler_root / "llvm/bin/llvm-readelf"
    def read(*flags):
        return subprocess.check_output([str(reader), *flags, str(elf)], text=True)
    header = read("--file-header")
    if "AMDGPU" not in header or "0x42" not in header:
        raise SystemExit("Not the audited gfx1013 ELF target")
    sections = read("--sections", "--elf-output-style=GNU")
    has_relocations = bool(re.search(r"\]\s+\.rela?(?:\.|\s)", sections))
    relocations = read("--relocations", "--elf-output-style=GNU")
    (out / "relocations.txt").write_text(relocations)
    symbols = read("--symbols", "--elf-output-style=GNU")
    notes = read("--notes", "--elf-output-style=LLVM")
    (out / "notes.txt").write_text(notes)
    (out / "symbols.txt").write_text(symbols)
    subprocess.run(["llvm-objcopy-18", "--dump-section", f".text={out / 'text.bin'}",
                    str(elf), str(out / "section-copy.elf")], check=True)
    code = (out / "text.bin").read_bytes()
    dump = out / "pipeline-dump"
    dump.mkdir()
    subprocess.run([str(compiler), "-gfxip=10.1.3", "-enable-pipeline-dump",
                    f"-pipeline-dump-dir={dump}", f"-o={dump / 'dump.elf'}", str(source)],
                   cwd=out, check=True, capture_output=True)
    # Dumps must describe the exact compiled program, not a nearby rebuild with
    # different code or PAL metadata. Ignore only additional diagnostic sections.
    for section in (".text", ".note"):
        for label, path in (("original", elf), ("dump", dump / "dump.elf")):
            subprocess.run(["llvm-objcopy-18", "--dump-section", f"{section}={dump / (label+section)}",
                            str(path), str(dump / (label+'-copy.elf'))], check=True)
        if (dump / ("original"+section)).read_bytes() != (dump / ("dump"+section)).read_bytes():
            raise SystemExit(f"SPIR-V dump compilation changed {section}")
    modules = {}
    for path in sorted(dump.glob("Shader_*.spv")):
        label = graphics_entry(path.read_bytes())
        if label in modules:
            raise SystemExit("Duplicate dumped graphics stage")
        target = out / (label + '.spv')
        target.write_bytes(path.read_bytes())
        modules[label] = dict(file=target.name, sha256=digest(target), entry="main", bytes=target.stat().st_size)
    if set(modules) != {"vertex", "fragment"}:
        raise SystemExit("Missing exact graphics SPIR-V inputs")
    upload_image, image_sections, image_relocations = unpack_graphics_object(elf.read_bytes())
    (out / "upload.unlinked.bin").write_bytes(upload_image)
    (out / "upload_relocations.json").write_text(json.dumps(image_relocations, indent=2) + "\n")
    stages = {}
    for label, symbol in (("pre_raster", "_amdgpu_gs_main"), ("fragment", "_amdgpu_ps_main")):
        match = re.search(rf"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+FUNC.*\s{symbol}$", symbols, re.M)
        if not match:
            raise SystemExit(f"Missing expected graphics stage: {symbol}")
        offset, size = int(match[1], 16), int(match[2])
        if not size or offset + size > len(code):
            raise SystemExit("Stage exceeds code section")
        path = out / f"{label}.unlinked.bin"
        path.write_bytes(code[offset:offset + size])
        stages[label] = dict(symbol=symbol, offset=offset, bytes=size, sha256=digest(path))
    manifest = dict(scope="host-graphics-control-only", target="gfx1013", hardware_validated=False,
        source=str(source.relative_to(ROOT)), source_sha256=digest(source), compiler_sha256=digest(compiler),
        elf_sha256=digest(elf), metadata_sha256=digest(out / "notes.txt"), stages=stages,
        has_relocations=has_relocations, relocations_applied=False,
        relocations_sha256=digest(out / "relocations.txt"), native_ready=False)
    manifest.update(upload_image_sha256=digest(out / "upload.unlinked.bin"),
                    image_sections=image_sections, image_relocations=image_relocations)
    manifest["graphics_requirements"] = decode_graphics_metadata(notes)
    manifest["spirv_modules"] = modules
    schema = RegisterSchema((lab_root() / "third_party/mesa-gfx1013/src/amd/registers/gfx10.json").read_bytes())
    manifest["shader_context"] = shader_context(manifest["graphics_requirements"], schema)
    native_image, native_stages, native_relocations = plan_graphics_upload(
        upload_image, image_sections, image_relocations, stages)
    (out / "agc-upload.unlinked.bin").write_bytes(native_image)
    manifest["agc_upload"] = dict(image_sha256=digest(out / "agc-upload.unlinked.bin"),
        bytes=len(native_image), stages=native_stages, relocations=native_relocations,
        relocated=False, shader_objects_created=False)
    if args.inspect_only:
        manifest.update(scope="compiler-inspection-only", native_adapter_generated=False,
                        graphics_library_generated=False, hardware_validated=False)
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps({"output": str(out), "scope": manifest["scope"],
                          "native_adapter_generated": False, "hardware_validated": False}, indent=2))
        return
    (out / "graphics_metadata.h").write_text(render_header(header_values(manifest)))
    manifest["header_adapter"] = dict(profile="procedural-triangle-global-table-base-vertex-instance",
        sha256=digest(out / "graphics_metadata.h"), hardware_validated=False)
    embedded = '#include "graphics_metadata.h"\n#include "graphics_pair.h"\n'
    interpolators = manifest['shader_context']['interpolators']
    if not 1 <= len(interpolators) <= 32 or any(
            row['byte_address'] != 0x28644 + 4*i for i,row in enumerate(interpolators)):
        raise SystemExit('Interpolator table outside native register ABI')
    embedded += 'static const ps5_agc_register graphics_interpolators[] = {\n'
    embedded += ',\n'.join(f'{{{(row["byte_address"]-0x28000)//4}u,{row["value"]}u}}' for row in interpolators)
    embedded += '\n};\n'
    embedded += 'static const unsigned char graphics_image[] = {' + ','.join(map(str, native_image)) + '};\n'
    embedded += 'static const struct ps5vk_shader_relocation graphics_relocations[] = {\n'
    embedded += ',\n'.join('{' + ','.join(str(r[k]) for k in ("offset", "symbol_offset", "addend", "type")) + '}' for r in native_relocations)
    embedded += '\n};\nstatic const struct ps5vk_graphics_pair_input graphics_input = {\n'
    embedded += '.image=graphics_image, .image_bytes=sizeof(graphics_image),\n'
    embedded += f'.relocations=graphics_relocations, .relocation_count={len(native_relocations)},\n'
    for field, name in (("gs", "pre_raster"), ("ps", "fragment")):
        stage = native_stages[name]
        embedded += f'.{field}={{{stage["offset"]}, {stage["isa_bytes"]}}},\n'
    quantization = manifest['shader_context']['vertex_quantization']
    if quantization['byte_address'] != 0x28be4 or quantization['value'] != 0x2d:
        raise SystemExit('Vertex quantization outside audited 1/256 round-to-even profile')
    embedded += f'.vertex_quantization={quantization["value"]}u,\n'
    embedded += f'.interpolators=graphics_interpolators,.interpolator_count={len(interpolators)}u,\n'
    embedded += '.metadata=&ps5vk_graphics_metadata\n};\n'
    (out / 'graphics_embedded.h').write_text(embedded)
    state = source.read_text().split('[GraphicsPipelineState]\n', 1)[1].split('\n[', 1)[0]
    expected_state = {'topology': 'VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST',
        'nggState.enableNgg': '1', 'nggState.enableGsUse': '0',
        'colorBuffer[0].format': 'VK_FORMAT_B8G8R8A8_UNORM',
        'colorBuffer[0].channelWriteMask': '15', 'colorBuffer[0].blendEnable': '0'}
    parsed_state = {}
    for line in state.splitlines():
        if not line.strip():
            continue
        key, value = (s.strip() for s in line.split('=', 1))
        if key in parsed_state:
            raise SystemExit('Duplicate graphics compilation state')
        parsed_state[key] = value
    vertex_input = '[VertexInputState]' in source.read_text()
    if vertex_input:
        vertex_state = source.read_text().split('[VertexInputState]\n', 1)[1].split('\n[', 1)[0]
        fields = {}
        for line in vertex_state.splitlines():
            if not line.strip():
                continue
            k, v = (part.strip() for part in line.split('=', 1))
            if k in fields:
                raise SystemExit('Duplicate vertex input field')
            fields[k] = v
        expected_vertex = {'binding[0].binding': '0', 'binding[0].stride': '24',
            'binding[0].inputRate': 'VK_VERTEX_INPUT_RATE_VERTEX'}
        for i in range(2):
            expected_vertex.update({f'attribute[{i}].location': str(i), f'attribute[{i}].binding': '0',
                f'attribute[{i}].format': 'VK_FORMAT_R32G32B32_SFLOAT', f'attribute[{i}].offset': str(i*12)})
        if fields != expected_vertex or manifest['graphics_requirements']['stages']['gs']['user_map'][:4] != [0x10000000,0x1000000f,0x10000003,0x10000004]:
            raise SystemExit('Vertex layout/user ABI outside audited interleaved profile')
    elif manifest['graphics_requirements']['stages']['gs']['user_sgprs'] != 3:
        raise SystemExit('Procedural layout/user ABI mismatch')
    textured = '[ResourceMapping]' in source.read_text()
    if textured:
        section = source.read_text().split('[ResourceMapping]\n',1)[1].split('\n[',1)[0]
        mapping = {}
        for line in section.splitlines():
            if not line.strip():
                continue
            k,v = (part.strip() for part in line.split('=',1))
            if k in mapping:
                raise SystemExit('Duplicate resource mapping field')
            mapping[k]=v
        expected_mapping = {
            'userDataNode[0].visibility':'2','userDataNode[0].type':'IndirectUserDataVaPtr',
            'userDataNode[0].offsetInDwords':'0','userDataNode[0].sizeInDwords':'1',
            'userDataNode[0].indirectUserDataCount':'1',
            'userDataNode[1].visibility':'64','userDataNode[1].type':'DescriptorTableVaPtr',
            'userDataNode[1].offsetInDwords':'0','userDataNode[1].sizeInDwords':'1',
            'userDataNode[1].next[0].type':'DescriptorCombinedTexture',
            'userDataNode[1].next[0].offsetInDwords':'0','userDataNode[1].next[0].sizeInDwords':'12',
            'userDataNode[1].next[0].set':'0','userDataNode[1].next[0].binding':'0'}
        if not vertex_input or mapping != expected_mapping or manifest['graphics_requirements']['stages']['ps']['user_map'][:2] != [0x10000000,0]:
            raise SystemExit('Resource layout/user ABI outside audited combined texture profile')
    elif manifest['graphics_requirements']['stages']['ps']['user_sgprs'] != 1:
        raise SystemExit('Fragment resource ABI requires explicit mapping')
    if parsed_state != expected_state:
        raise SystemExit('Graphics library key adapter needs expansion for this pipeline state')
    library = '#include "graphics_program.h"\n#include "graphics_embedded.h"\n'
    if vertex_input:
        library += '''static const VkVertexInputBindingDescription graphics_binding = {0,24,VK_VERTEX_INPUT_RATE_VERTEX};
static const VkVertexInputAttributeDescription graphics_attributes[2] = {
    {0,0,VK_FORMAT_R32G32B32_SFLOAT,0}, {1,0,VK_FORMAT_R32G32B32_SFLOAT,12}};
'''
    for stage in ('vertex', 'fragment'):
        data = (out / (stage + '.spv')).read_bytes()
        words = struct.unpack(f'<{len(data)//4}I', data)
        library += f'static const uint32_t graphics_{stage}_spirv[] = {{' + ','.join(map(str, words)) + '};\n'
    library += '''static const struct ps5vk_graphics_program graphics_program = {
    .key = {
        .vertex = {.words=graphics_vertex_spirv, .word_count=sizeof(graphics_vertex_spirv)/4, .entry="main"},
        .fragment = {.words=graphics_fragment_spirv, .word_count=sizeof(graphics_fragment_spirv)/4, .entry="main"},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        /* One colour attachment, in the per-attachment form the profile carries
         * since the DXVK262-T06 colour-state work: the format, the blend state
         * and the write mask are arrays indexed by attachment. */
        .color_format = {VK_FORMAT_B8G8R8A8_UNORM}, .color_attachment_count = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .color_write_mask = {15},
    },
    .backend_data = &graphics_input,
};
static const struct ps5vk_graphics_library graphics_library = {&graphics_program, 1};
'''
    if vertex_input:
        library = library.replace('.samples = VK_SAMPLE_COUNT_1_BIT, .color_write_mask = {15},',
            '.samples = VK_SAMPLE_COUNT_1_BIT, .color_write_mask = {15},\n'
            '        .vertex_binding_count=1, .vertex_attribute_count=2,\n'
            '        .vertex_bindings=&graphics_binding, .vertex_attributes=graphics_attributes,')
        manifest['header_adapter']['profile'] = 'interleaved-float-vertex-table-base-vertex-instance'
    if textured:
        empty_bindings = ',\n'.join(
            f'        [{i}]={{.count=0,.first=1,.stages=0}}' for i in range(1, 32))
        signature = ('static const struct ps5vk_set_signature graphics_set = {\n'
            '    .count=1,\n'
            '    .type={[0]=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},\n'
            '    .binding={\n'
            '        [0]={.count=1,.first=0,.stages=VK_SHADER_STAGE_FRAGMENT_BIT},\n' +
            empty_bindings + '\n'
            '    },\n'
            '};\n')
        library = library.replace('static const struct ps5vk_graphics_program graphics_program',
            signature + 'static const struct ps5vk_graphics_program graphics_program')
        library = library.replace('.samples = VK_SAMPLE_COUNT_1_BIT, .color_write_mask = {15},',
            '.samples = VK_SAMPLE_COUNT_1_BIT, .color_write_mask = {15},\n'
            '        .descriptor_set_count=1, .descriptor_sets=&graphics_set,')
        manifest['header_adapter']['profile']='vertex-table-fragment-combined-texture-set0-binding0'
    (out / 'graphics_library.h').write_text(library)
    manifest['graphics_library_sha256'] = digest(out / 'graphics_library.h')
    # Version 3 makes the complete 32-binding set signature explicit.  Version
    # 2 left the empty binding offsets implicitly zero, which no longer matches
    # the fail-closed descriptor-signature invariant after binding zero.
    manifest['native_input_version'] = 3
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(out / "manifest.json")


if __name__ == "__main__":
    main()
