"""Relocate only the two Patch locations in our owned total-component fixture.

This is not a general SPIR-V sanitizer. GLSL frontend location allocation is
stricter than SPIR-V's independent Patch/non-Patch namespaces (SPIRV-Tools #5654).
Instructions and values remain unchanged; unexpected source shape is an error.
"""
import struct


def relocate_total_patch(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('invalid SPIR-V size')
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    patch_ids, locations = set(), []
    at = 5
    while at < len(words):
        size, op = words[at] >> 16, words[at] & 65535
        if not size or at + size > len(words):
            raise ValueError('invalid SPIR-V instruction')
        if op == 71:  # OpDecorate
            if size < 3:
                raise ValueError('truncated decoration')
            target, decoration = words[at + 1:at + 3]
            if decoration == 15:  # Patch
                if size != 3:
                    raise ValueError('invalid Patch decoration')
                patch_ids.add(target)
            elif decoration == 30:  # Location
                if size != 4:
                    raise ValueError('invalid Location decoration')
                locations.append((target, at + 3))
        at += size
    selected = [(target, offset) for target, offset in locations if target in patch_ids]
    if (len(selected) != 2 or len({target for target, _ in selected}) != 2 or
            sorted(words[offset] for _, offset in selected) != [31, 53]):
        raise ValueError('unexpected owned Patch interface')
    for _, offset in selected:
        words[offset] -= 31  # vec4[22] at0; vec2 at22
    return struct.pack('<%dI' % len(words), *words)
