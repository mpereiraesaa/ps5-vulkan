"""Bounded AMDPAL ELF64 -> code/constants image and explicit REL records.

Not a general ELF linker. Unknown allocated sections, external symbols,
relocation forms or targets fail before any native artifact is accepted.
"""
import struct


def unpack_graphics_object(data):
    def require(ok, message):
        if not ok:
            raise ValueError(message)
    def part(offset, size):
        require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, "ELF range")
        return data[offset:offset + size]
    require(len(data) >= 64 and data[:8] == b"\x7fELF\x02\x01\x01\x41", "AMDPAL ELF64 header")
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    kind, machine, version, _, _, table, flags, ehsize, _, phnum, shsize, count, strings = header
    require((kind, machine, version, ehsize, phnum, shsize) == (1, 224, 1, 64, 0, 64), "ELF layout")
    require(flags & 255 == 0x42 and 0 < count <= 128 and 0 < strings < count, "gfx1013 sections")
    sections = [struct.unpack("<IIQQQQIIQQ", part(table + i * 64, 64)) for i in range(count)]
    string_section = sections[strings]
    require(string_section[1] == 3, "section string table")
    names = part(string_section[4], string_section[5])
    image = bytearray(); loaded = {}; public_sections = []
    for index, s in enumerate(sections):
        if not s[2] & 2:
            continue
        name_end = names.find(b"\0", s[0])
        require(s[0] < len(names) and name_end >= s[0], "section name")
        name = names[s[0]:name_end].decode("ascii")
        require(name in (".text", ".rodata") and s[1] == 1 and not s[2] & ~6,
                "unsupported allocated section")
        require(not any(item["name"] == name for item in public_sections), "duplicate section")
        alignment = s[8]
        require(alignment > 0 and alignment <= 65536 and not alignment & (alignment - 1), "section alignment")
        offset = (len(image) + alignment - 1) & -alignment
        require(offset + s[5] <= 16 * 1024 * 1024, "image capacity")
        image += bytes(offset - len(image)) + part(s[4], s[5])
        loaded[index] = offset
        public_sections.append(dict(name=name, offset=offset, bytes=s[5], alignment=alignment))
    require(any(s["name"] == ".text" and s["bytes"] for s in public_sections), "missing code")
    relocations = []
    for s in sections:
        if s[1] not in (4, 9):
            continue
        require(s[1] == 9 and s[9] == 16 and s[5] % 16 == 0, "unsupported relocation form")
        require(s[7] in loaded and s[6] < count, "relocation target/link")
        symbols = sections[s[6]]
        require(symbols[1] == 2 and symbols[9] == 24 and symbols[5] % 24 == 0, "symbol table")
        for cursor in range(0, s[5], 16):
            target, info = struct.unpack("<QQ", part(s[4] + cursor, 16))
            symbol_index, relocation_type = info >> 32, info & 0xffffffff
            require(relocation_type in (1, 2) and symbol_index < symbols[5] // 24, "unsupported relocation")
            _, _, _, section, value, _ = struct.unpack("<IBBHQQ", part(symbols[4] + symbol_index * 24, 24))
            require(section in loaded and value < sections[section][5], "external/out-of-range symbol")
            require(target % 4 == 0 and target + 4 <= sections[s[7]][5], "relocation field range")
            offset = loaded[s[7]] + target
            addend = struct.unpack_from("<I", image, offset)[0]
            require(value + addend < sections[section][5], "symbol addend range")
            require(not any(r["offset"] == offset for r in relocations), "overlapping relocation")
            relocations.append(dict(offset=offset, symbol_offset=loaded[section] + value,
                                    addend=addend, type=relocation_type))
    return bytes(image), public_sections, relocations
