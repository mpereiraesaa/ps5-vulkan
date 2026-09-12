"""Bounded graphics SPIR-V entry identification, not semantic validation."""
import struct


def graphics_entry(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError("Malformed SPIR-V size")
    words = struct.unpack(f"<{len(data)//4}I", data)
    if words[0] != 0x07230203 or not words[3] or words[4]:
        raise ValueError("Malformed SPIR-V header")
    entries = []
    cursor = 5
    while cursor < len(words):
        n, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if not n or n > len(words) - cursor:
            raise ValueError("Malformed SPIR-V instruction")
        if opcode == 15:
            if n < 4:
                raise ValueError("Malformed entry instruction")
            model, identifier = words[cursor+1:cursor+3]
            name = data[(cursor+3)*4:(cursor+n)*4]
            if b"\0" not in name or not 0 < identifier < words[3]:
                raise ValueError("Malformed entry name/ID")
            entries.append((model, name.split(b"\0", 1)[0].decode("utf-8")))
        cursor += n
    if len(entries) != 1 or entries[0][0] not in (0, 4) or entries[0][1] != "main":
        raise ValueError("Expected one vertex or fragment main entry")
    return "vertex" if entries[0][0] == 0 else "fragment"
