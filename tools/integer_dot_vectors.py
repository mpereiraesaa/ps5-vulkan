# SPDX-License-Identifier: GPL-3.0-or-later
"""Bit-exact runtime inputs for the owned integer-dot numerical witnesses.

All values are raw bits, independent of packed scalar carrier signedness.
The reference uses unbounded arithmetic. Saturating cases reject undefined
partial sums before clamping the final accumulator addition.
"""
from dataclasses import dataclass
import struct

from tools.integer_dot_spirv import Case

MASK = (1 << 32) - 1
INVOCATIONS = 128  # Two workgroups of 64 in integer_dot_spirv.module.


@dataclass(frozen=True)
class Sample:
    lhs: tuple[int, ...]
    rhs: tuple[int, ...]
    accumulator: int


def signed(bits: int, width: int) -> int:
    return bits - (1 << width) if bits & (1 << (width - 1)) else bits


def reference(case: Case, sample: Sample) -> int:
    """Return the exact 32 output bits; refuse ambiguous saturation inputs."""
    if case.mode == "float":
        raise ValueError("integer oracle requires integer operands")
    width = 8 if case.packed_types is not None else 32
    limit = (1 << width) - 1
    if (len(sample.lhs) != case.components or len(sample.rhs) != case.components or
            any(type(v) is not int or not 0 <= v <= limit for v in (*sample.lhs, *sample.rhs)) or
            type(sample.accumulator) is not int or not 0 <= sample.accumulator <= MASK):
        raise ValueError("sample does not match raw operand shape")
    lhs = [signed(v, width) if case.mode != "u" else v for v in sample.lhs]
    rhs = [signed(v, width) if case.mode == "s" else v for v in sample.rhs]
    products = [a * b for a, b in zip(lhs, rhs)]
    value = sum(products)
    if case.saturating:
        low, high = (0, MASK) if case.mode == "u" else (-(1 << 31), (1 << 31) - 1)
        # Do not depend on the implementation's reduction order. The original
        # CTS also excludes overflowing positive/negative partial dot sums.
        if sum(max(p, 0) for p in products) > high or sum(min(p, 0) for p in products) < low:
            raise ValueError("undefined saturating dot partial sum")
        accumulator = sample.accumulator if case.mode == "u" else signed(sample.accumulator, 32)
        value = min(high, max(low, value + accumulator))
    return value & MASK


def samples(case: Case) -> tuple[Sample, ...]:
    """Deterministic lane, sign, wrap and accumulator boundary probes."""
    if case.mode == "float":
        raise ValueError("integer dataset requires integer operands")
    width = 8 if case.packed_types is not None else 32
    mask = (1 << width) - 1
    rows = []
    for i in range(INVOCATIONS):
        if case.saturating:
            # Small products keep every partial sum defined even when the
            # accumulator causes saturation at either result boundary.
            a = [((i * 7 + lane * 13) % 31) - (15 if case.mode != "u" else 0)
                 for lane in range(case.components)]
            b = [((i * 11 + lane * 3) % 29) - (14 if case.mode == "s" else 0)
                 for lane in range(case.components)]
        else:
            patterns = (0, 1, mask, 1 << (width - 1), (1 << (width - 1)) - 1,
                        0x55 & mask, 0xaa & mask, 3)
            a = [patterns[(i + lane * 3) % len(patterns)] for lane in range(case.components)]
            b = [((i * 17 + lane * 29 + 1) & mask) ^
                 ((1 << (width - 1)) if (i + lane) % 3 == 0 else 0)
                 for lane in range(case.components)]
        # Each lane has a distinct isolated positive and negative/sign-bit
        # probe. A byte/lane reversal cannot hide behind a symmetric vector.
        if i < 2 * case.components:
            a = [0] * case.components
            a[i % case.components] = 1 if i < case.components else (mask if case.mode != "u" else 127)
            b = [lane + 1 for lane in range(case.components)]
        accumulators = (0, 1, MASK, 0x7fffffff, 0x80000000, 0x7ffffffe, 0x80000001, 17)
        sample = Sample(tuple(v & mask for v in a), tuple(v & mask for v in b), accumulators[i % 8])
        reference(case, sample)  # Fail closed before emitting a native fixture.
        rows.append(sample)
    return tuple(rows)


def buffers(case: Case) -> tuple[bytes, bytes, bytes, bytes]:
    """SSBO inputs at bindings 0/1/2 and bit-exact expected binding 3 bytes.

    Vector3 padding uses poison, not zero; it must never enter the dot product.
    Guards belong to the caller's allocation and are excluded from descriptors.
    """
    lhs, rhs, accumulators, expected = bytearray(), bytearray(), bytearray(), bytearray()
    for sample in samples(case):
        for output, values in ((lhs, sample.lhs), (rhs, sample.rhs)):
            raw = (bytes(values) if case.packed_types is not None else
                   struct.pack(f'<{case.components}I', *values))
            output.extend(raw + b'\xa5' * (case.stride - len(raw)))
        accumulators.extend(struct.pack('<I', sample.accumulator))
        expected.extend(struct.pack('<I', reference(case, sample)))
    return bytes(lhs), bytes(rhs), bytes(accumulators), bytes(expected)
