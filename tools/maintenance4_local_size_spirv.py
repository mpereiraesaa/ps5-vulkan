#!/usr/bin/env python3
"""Turn the minimal compute shader's literal LocalSize into a specialized LocalSizeId."""
import struct

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
