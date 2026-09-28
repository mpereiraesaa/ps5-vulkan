#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned integer-dot fixtures with runtime SSBO operands, for host/native tests.

SPIR-V 1.3 plus SPV_KHR_integer_dot_product; no assembler or GLSL extension
support is needed. Bindings 0/1 hold inputs, 2 the accumulator, 3 the result.
Each invocation writes one result. Vector3 arrays have the required padding.
"""
from dataclasses import dataclass
import struct


@dataclass(frozen=True)
class Case:
    mode: str
    components: int
    saturating: bool = False
    packed_types: tuple[bool, bool] | None = None

    def __post_init__(self):
        if (self.mode not in ("u", "s", "su", "float") or self.components not in (2, 3, 4) or
                type(self.saturating) is not bool):
            raise ValueError("unsupported dot shape")
        if self.mode == "float" and (self.saturating or self.packed_types is not None):
            raise ValueError("floating control is neither packed nor saturating")
        if self.packed_types is not None and (
                self.components != 4 or len(self.packed_types) != 2 or
                any(type(sign) is not bool for sign in self.packed_types)):
            raise ValueError("packed 4x8 requires two scalar signedness flags")

    @property
    def name(self):
        shape = f"v{self.components}"
        if self.packed_types is not None:
            shape = "packed-" + "".join("s" if sign else "u" for sign in self.packed_types)
        return f"{self.mode}-{shape}-{'sat' if self.saturating else 'dot'}"

    @property
    def stride(self):
        return 4 if self.packed_types is not None else (8 if self.components == 2 else 16)


def cases():
    vector = [Case(mode, n, sat) for n in (2, 3, 4)
              for mode in ("u", "s", "su") for sat in (False, True)]
    packed = [Case(mode, 4, sat, (lhs, rhs)) for mode in ("u", "s", "su")
              for sat in (False, True) for lhs in (False, True) for rhs in (False, True)]
    return tuple(vector + packed)


def instruction(opcode, *operands):
    return [(len(operands) + 1) << 16 | opcode, *operands]


def string_words(value):
    raw = value.encode() + b"\0"
    raw += b"\0" * (-len(raw) % 4)
    return list(struct.unpack(f"<{len(raw) // 4}I", raw))


def module(case: Case):
    """Return deterministic words; arithmetic inputs are never constants."""
    types, annotations, globals_, body = [], [], [], []
    bound, type_ids = 1, {}

    def new_id():
        nonlocal bound
        result = bound
        bound += 1
        return result

    def type_id(op, *args):
        key = (op, *args)
        if key not in type_ids:
            value = new_id()
            type_ids[key] = value
            types.extend(instruction(op, value, *args))
        return type_ids[key]

    def result(op, typ, *args):
        value = new_id()
        body.extend(instruction(op, typ, value, *args))
        return value

    void = type_id(19)
    uint = type_id(21, 32, 0)
    sint = type_id(21, 32, 1)
    uvec3 = type_id(23, uint, 3)
    function_type = type_id(33, void)
    zero = new_id()
    types.extend(instruction(43, uint, zero, 0))
    gid_ptr = type_id(32, 1, uvec3)
    gid = new_id()
    globals_.extend(instruction(59, gid_ptr, gid, 1))
    annotations.extend(instruction(71, gid, 11, 28))  # BuiltIn GlobalInvocationId
    scalar = type_id(22, 32) if case.mode == "float" else (uint if case.mode == "u" else sint)
    if case.packed_types is not None:
        lhs_type, rhs_type = [sint if sign else uint for sign in case.packed_types]
    else:
        lhs_type = type_id(23, scalar, case.components)
        rhs_type = type_id(23, uint, case.components) if case.mode == "su" else lhs_type
    variables, pointers = [], []
    decorated_arrays, decorated_structs = set(), set()
    for binding, element in enumerate((lhs_type, rhs_type, scalar, scalar)):
        array = type_id(29, element)
        if array not in decorated_arrays:
            annotations.extend(instruction(71, array, 6, case.stride if binding < 2 else 4))
            decorated_arrays.add(array)
        block = type_id(30, array)
        if block not in decorated_structs:
            annotations.extend(instruction(71, block, 2))  # Block
            annotations.extend(instruction(72, block, 0, 35, 0))  # Offset
            decorated_structs.add(block)
        block_ptr = type_id(32, 12, block)
        pointers.append(type_id(32, 12, element))
        variable = new_id()
        globals_.extend(instruction(59, block_ptr, variable, 12))
        annotations.extend(instruction(71, variable, 34, 0))  # DescriptorSet
        annotations.extend(instruction(71, variable, 33, binding))
        annotations.extend(instruction(71, variable, 24 if binding < 3 else 25))
        variables.append(variable)
    main = new_id()
    body.extend(instruction(54, void, main, 0, function_type))
    body.extend(instruction(248, new_id()))
    invocation = result(61, uvec3, gid)
    index = result(81, uint, invocation, 0)
    inputs = []
    for binding, element in enumerate((lhs_type, rhs_type, scalar)):
        if binding == 2 and not case.saturating:
            break
        pointer = result(65, pointers[binding], variables[binding], zero, index)
        inputs.append(result(61, element, pointer))
    if case.mode == "float":
        opcode = 148
    else:
        opcode = {"s": 4450, "u": 4451, "su": 4452}[case.mode] + (3 if case.saturating else 0)
    if case.packed_types is not None:
        inputs.append(0)  # PackedVectorFormat4x8Bit
    value = result(opcode, scalar, *inputs)
    output = result(65, pointers[3], variables[3], zero, index)
    body.extend(instruction(62, output, value))
    body.extend(instruction(253))
    body.extend(instruction(56))
    preamble = instruction(17, 1)  # Shader
    if case.mode != "float":
        preamble += instruction(17, 6019)
        preamble += instruction(17, 6018 if case.packed_types is not None else 6016)
        preamble += instruction(10, *string_words("SPV_KHR_integer_dot_product"))
    preamble += instruction(14, 0, 1)  # Logical GLSL450
    preamble += instruction(15, 5, main, *string_words("main"), gid)
    preamble += instruction(16, main, 17, 64, 1, 1)
    return [0x07230203, 0x00010300, 0, bound, 0, *preamble, *annotations, *types, *globals_, *body]


def binary(case: Case):
    words = module(case)
    return struct.pack(f"<{len(words)}I", *words)


def integerize_dot_add(words, case: Case, *, preserve_result_bits=False):
    """Replace one float dot-plus-add in an owned test template.

This is fixture generation, not runtime shader rewriting. Bitcasts retain
buffer bits; the final integer result is converted back to the template's
float output, leaving its stage interfaces and pipeline context unchanged.
With preserve_result_bits=True, the result is bitcast instead. Such templates
must immediately recover a uint with floatBitsToUint and carry it through flat
integer interfaces; numerical float operations would invalidate the oracle.
"""
    if type(preserve_result_bits) is not bool:
        raise ValueError("preserve_result_bits must be boolean")
    if case.mode == "float" or len(words) < 5 or words[0] != 0x07230203:
        raise ValueError("requires an integer case and a SPIR-V template")
    instructions, index = [], 5
    while index < len(words):
        size = words[index] >> 16
        if not size or index + size > len(words):
            raise ValueError("malformed template instruction")
        instructions.append(list(words[index:index + size]))
        index += size
    dots = [x for x in instructions if x[0] & 65535 == 148]
    if len(dots) != 1 or len(dots[0]) != 5:
        raise ValueError("template requires exactly one float dot")
    original = dots[0]
    adds = [x for x in instructions if x[0] & 65535 == 129 and original[2] in x[3:]]
    if len(adds) != 1 or len(adds[0]) != 5 or adds[0][1] != original[1]:
        raise ValueError("template requires one dot-plus-add")
    add = adds[0]
    if not any(x == instruction(22, original[1], 32) for x in instructions):
        raise ValueError("template requires 32-bit floating point")
    vectors = {x[1]: x[2:] for x in instructions if x[0] & 65535 == 23}
    values = {x[2]: x[1] for x in instructions
              if x[0] & 65535 in (55, 57, 61, 79, 81, 83)}
    for operand in original[3:]:
        if vectors.get(values.get(operand)) != [original[1], case.components]:
            raise ValueError("template dot operands do not match the requested vector shape")
    accumulator = next((v for v in add[3:] if v != original[2]), None)
    if accumulator is None:
        raise ValueError("template accumulator must be independent of dot")
    bound, new_types = words[3], []
    cache = {(x[0] & 65535, *x[2:]): x[1] for x in instructions if x[0] & 65535 in (21, 23)}

    def new_id():
        nonlocal bound
        value = bound
        bound += 1
        return value

    def typ(opcode, *args):
        key = (opcode, *args)
        if key not in cache:
            cache[key] = new_id()
            new_types.extend(instruction(opcode, cache[key], *args))
        return cache[key]

    uint, sint = typ(21, 32, 0), typ(21, 32, 1)
    result_type = uint if case.mode == "u" else sint
    if case.packed_types is None:
        operand_types = [typ(23, result_type, case.components),
                         typ(23, uint if case.mode == "su" else result_type, case.components)]
    else:
        operand_types = [sint if sign else uint for sign in case.packed_types]
    replacement, operands = [], []
    for source, operand_type in zip(original[3:], operand_types):
        if case.packed_types is not None:
            value = new_id()
            replacement += instruction(81, original[1], value, source, 0)
            source = value
        value = new_id()
        replacement += instruction(124, operand_type, value, source)
        operands.append(value)
    if case.saturating:
        value = new_id()
        replacement += instruction(124, result_type, value, accumulator)
        operands.append(value)
    if case.packed_types is not None:
        operands.append(0)
    value = new_id()
    replacement += instruction({"s": 4450, "u": 4451, "su": 4452}[case.mode] + 3 * case.saturating,
                               result_type, value, *operands)
    result_opcode = 124 if preserve_result_bits else (112 if case.mode == "u" else 111)
    replacement += instruction(result_opcode, add[1], add[2], value)
    out = list(words[:5])
    out[3] = bound
    capabilities = instruction(17, 6019) + instruction(17, 6018 if case.packed_types is not None else 6016)
    extension = instruction(10, *string_words("SPV_KHR_integer_dot_product"))
    added_capabilities = added_types = False
    for current in instructions:
        opcode = current[0] & 65535
        if opcode != 17 and not added_capabilities:
            out += capabilities + extension
            added_capabilities = True
        if opcode == 54 and not added_types:
            out += new_types
            added_types = True
        if current is original:
            continue
        out += replacement if current is add else current
    if not added_types:
        raise ValueError("template requires a function")
    return out
