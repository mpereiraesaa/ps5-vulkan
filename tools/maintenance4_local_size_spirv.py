#!/usr/bin/env python3
"""Turn the minimal compute shader's literal LocalSize into a specialized LocalSizeId."""
import argparse
from pathlib import Path
import struct
import re

OP_TYPE_INT = 21
OP_CONSTANT = 43
OP_SPEC_CONSTANT = 50
OP_FUNCTION = 54
OP_DECORATE = 71
OP_EXECUTION_MODE = 16
OP_EXECUTION_MODE_ID = 331
LOCAL_SIZE = 17
LOCAL_SIZE_ID = 38
SPEC_ID = 1
BUILTIN = 11
WORKGROUP_SIZE = 25


def require_no_fixed_builtin_source(source):
    # GLSLang can constant-fold a gl_WorkGroupSize read before SPIR-V emission;
    # the binary alone cannot distinguish that literal from an unrelated 64.
    if re.search(r"\bgl_WorkGroupSize\b", source):
        raise ValueError("shader reads fixed WorkgroupSize built-in")


def instructions(words):
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError("not SPIR-V")
    at = 5
    while at < len(words):
        length = words[at] >> 16
        if not length or at + length > len(words):
            raise ValueError("malformed SPIR-V instruction")
        yield at, words[at] & 0xffff, length
        at += length


def local_size_id(data):
    if len(data) % 4:
        raise ValueError("unaligned SPIR-V")
    words = list(struct.unpack(f"<{len(data)//4}I", data))
    # GLSLang emits a constant composite decorated BuiltIn WorkgroupSize even
    # when the shader never reads gl_WorkGroupSize. The runtime gives that
    # built-in precedence over LocalSizeId, so retaining its fixed 64x1x1
    # value would silently defeat the specialized32 witness.
    workgroup = [(at, words[at+1]) for at, opcode, length in instructions(words)
                 if opcode == OP_DECORATE and length == 4 and
                 words[at+2:at+4] == [BUILTIN, WORKGROUP_SIZE]]
    if len(workgroup) > 1:
        raise ValueError("multiple WorkgroupSize built-ins")
    if workgroup:
        at, builtin_id = workgroup[0]
        in_function = False
        for position, opcode, length in instructions(words):
            in_function |= opcode == OP_FUNCTION
            if in_function and builtin_id in words[position+1:position+length]:
                raise ValueError("shader reads fixed WorkgroupSize built-in")
        del words[at:at+4]
    unsigned_type = None
    mode = None
    first_type = None
    first_function = None
    for at, opcode, length in instructions(words):
        if opcode == OP_TYPE_INT and length == 4 and words[at+2:at+4] == [32, 0]:
            unsigned_type = words[at+1]
        if 19 <= opcode <= 39 and first_type is None:
            first_type = at
        if opcode == OP_FUNCTION and first_function is None:
            first_function = at
        if opcode == OP_EXECUTION_MODE and length == 6 and words[at+2] == LOCAL_SIZE:
            if mode is not None or words[at+3:at+6] != [64, 1, 1]:
                raise ValueError("expected one 64x1x1 LocalSize")
            mode = at
    if None in (unsigned_type, mode, first_type, first_function):
        raise ValueError("required compute declarations missing")
    if words[1] > 0x00010200:
        raise ValueError("source SPIR-V version exceeds the LocalSizeId fixture")
    x_id, one_id = words[3], words[3] + 1
    words[1] = 0x00010200
    words[3] += 2
    words[mode] = 6 << 16 | OP_EXECUTION_MODE_ID
    words[mode+2:mode+6] = [LOCAL_SIZE_ID, x_id, one_id, one_id]
    decorate = [4 << 16 | OP_DECORATE, x_id, SPEC_ID, 0]
    constants = [4 << 16 | OP_SPEC_CONSTANT, unsigned_type, x_id, 64,
                 4 << 16 | OP_CONSTANT, unsigned_type, one_id, 1]
    words[first_function:first_function] = constants
    words[first_type:first_type] = decorate
    return struct.pack(f"<{len(words)}I", *words)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    require_no_fixed_builtin_source(args.source.read_text())
    args.output.write_bytes(local_size_id(args.input.read_bytes()))


if __name__ == "__main__":
    main()
