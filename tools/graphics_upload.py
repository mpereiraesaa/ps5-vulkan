"""Plan an owned AGC code/constants allocation, before address relocation.

The native reference's 48-byte stage trailer is retained, but .rodata is not
dropped when stages are separated. References into discarded text padding fail.
This is packaging only: no AGC shader object or hardware acceptance is implied.
"""


def plan_graphics_upload(image, sections, relocations, stages):
    if set(stages) != {"pre_raster", "fragment"}:
        raise ValueError("Expected pre-raster and fragment stages")
    if {s["name"] for s in sections} - {".text", ".rodata"}:
        raise ValueError("Unsupported upload section")
    if len({s["name"] for s in sections}) != len(sections):
        raise ValueError("Duplicate upload section")
    text = next((s for s in sections if s["name"] == ".text"), None)
    if text is None:
        raise ValueError("Missing text section")
    for section in sections:
        if section["offset"] < 0 or section["bytes"] <= 0 or section["offset"] + section["bytes"] > len(image):
            raise ValueError("Section outside source image")
    result = bytearray()
    ranges = []
    output_stages = {}

    def append(old, size, alignment, trailer=False):
        if alignment < 1 or alignment > 65536 or alignment & (alignment - 1):
            raise ValueError("Unsupported alignment")
        if any(old < start + length and start < old + size for start, length, _ in ranges):
            raise ValueError("Overlapping source ranges")
        offset = (len(result) + alignment - 1) & -alignment
        if offset + size + (48 if trailer else 0) > 16 * 1024 * 1024:
            raise ValueError("Upload too large")
        result.extend(bytes(offset - len(result)))
        result.extend(image[old:old + size])
        if trailer:
            result.extend(b"barefoot" + bytes(40))
        ranges.append((old, size, offset))
        return offset

    for name in ("pre_raster", "fragment"):
        stage = stages[name]
        offset, size = stage["offset"], stage["bytes"]
        if offset < 0 or size <= 0 or size % 4 or offset % 256 or offset + size > text["bytes"]:
            raise ValueError("Unsupported stage extent")
        target = append(text["offset"] + offset, size, 256, True)
        output_stages[name] = dict(offset=target, isa_bytes=size, shader_bytes=size + 48)
    rodata = next((s for s in sections if s["name"] == ".rodata"), None)
    if rodata:
        append(rodata["offset"], rodata["bytes"], rodata["alignment"])

    def translate(offset, size=1):
        for old, length, new in ranges:
            if old <= offset and offset + size <= old + length:
                return new + offset - old
        raise ValueError("Reference into discarded padding or outside image")

    normalized = []
    for relocation in relocations:
        if relocation["type"] not in (1, 2) or relocation["offset"] % 4:
            raise ValueError("Unsupported relocation")
        if not 0 <= relocation["addend"] <= 0xffffffff:
            raise ValueError("Unsupported addend")
        patch = translate(relocation["offset"], 4)
        if any(r["offset"] == patch for r in normalized):
            raise ValueError("Duplicate patch")
        # Fold the already explicit S+A into the new target offset. Translating
        # S alone then keeping A is wrong if code was moved across a trailer.
        target = translate(relocation["symbol_offset"] + relocation["addend"])
        normalized.append(dict(offset=patch, symbol_offset=target, addend=0,
                               type=relocation["type"]))
    return bytes(result), output_stages, normalized
