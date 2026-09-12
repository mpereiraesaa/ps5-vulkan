"""Inventory declared entry-point interfaces; not a Vulkan limit validator.

Preserve built-in block members and locations instead of equating a PAL
interpolator register count with SPIR-V interface component consumption.
"""
import argparse
import json
from pathlib import Path
import struct
from graphics_spirv import graphics_entry


def inventory(data):
    stage = graphics_entry(data)
    words = struct.unpack(f"<{len(data)//4}I", data)
    types, variables, decorations, members, constants = {}, {}, {}, {}, {}
    interfaces = []
    cursor = 5
    while cursor < len(words):
        n, op = words[cursor] >> 16, words[cursor] & 65535
        args = words[cursor+1:cursor+n]
        minimum = {21:3, 22:2, 23:3, 28:3, 30:1, 32:3, 43:3, 59:3, 71:2, 72:3}
        if op in minimum and len(args) < minimum[op]:
            raise ValueError('Truncated interface declaration')
        if op == 15:
            # Entry-point name starts at operand 2 and occupies whole words.
            end = 2
            while not any(b == 0 for b in struct.pack('<I', args[end])):
                end += 1
            interfaces = list(args[end+1:])
        elif op in (21, 22, 23, 28, 30, 32):
            types[args[0]] = (op, args[1:])
        elif op == 43 and len(args) == 3:
            constants[args[1]] = args[2]
        elif op == 59:
            variables[args[1]] = (args[0], args[2])
        elif op == 71:
            decorations.setdefault(args[0], []).append(list(args[1:]))
        elif op == 72:
            members.setdefault((args[0], args[1]), []).append(list(args[2:]))
        cursor += n

    def describe(identifier, seen=()):
        if identifier in seen or identifier not in types:
            raise ValueError('Unknown or recursive interface type')
        op, a = types[identifier]
        seen = (*seen, identifier)
        if op == 22: return dict(kind='float', bits=a[0])
        if op == 21: return dict(kind='int', bits=a[0], signed=bool(a[1]))
        if op == 23: return dict(kind='vector', count=a[1], element=describe(a[0], seen))
        if op == 28:
            if a[1] not in constants: raise ValueError('Nonliteral interface array length')
            return dict(kind='array', count=constants[a[1]], element=describe(a[0], seen))
        if op == 32: return dict(kind='pointer', storage=a[0], element=describe(a[1], seen))
        if op == 30:
            return dict(kind='struct', decorations=decorations.get(identifier, []),
                        members=[dict(type=describe(t, seen), decorations=members.get((identifier, i), []))
                                 for i, t in enumerate(a)])
        raise ValueError('Unsupported interface type')

    result = []
    for identifier in interfaces:
        if identifier not in variables: raise ValueError('Missing interface variable')
        type_id, storage = variables[identifier]
        if storage not in (1, 3): continue  # SPIR-V 1.4 may list other globals.
        result.append(dict(id=identifier, storage='input' if storage == 1 else 'output',
                           decorations=decorations.get(identifier, []), type=describe(type_id)))
    return dict(stage=stage, declared_interfaces=result, limit_validation=False,
                static_usage_analysis=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    args = parser.parse_args()
    print(json.dumps(inventory(args.module.read_bytes()), indent=2))
