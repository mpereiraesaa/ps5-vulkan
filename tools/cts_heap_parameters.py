"""Select the application heap mode in a freshly linked native CTS ELF.

This adapts generated project metadata, not system code or the allocator.
The foundation's internal-memory mode ignores the application's heap size and
extension parameters. Keep the foundation untouched and fail on layout drift.
"""
import struct


def use_application_heap(image: bytes) -> bytes:
    if image[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected a little-endian ELF64 before SELF signing')
    # Size, parameter version, internal-memory mode. Exact foundation layout.
    signature = struct.pack('<QII', 0xa8, 14, 1)
    offset = image.find(signature)
    if offset < 0 or image.find(signature, offset + 1) >= 0:
        raise ValueError('expected exactly one foundation libc parameter block')
    # Validate all adjacent block headers and the existing unlimited/extendable
    # settings before touching the mode word. No hard-coded file offsets.
    cursor = offset
    for size in (0xa8, 0x38, 0x10, 0x78, 0xc0, 0x38):
        if cursor + size > len(image) or struct.unpack_from('<Q', image, cursor)[0] != size:
            raise ValueError('foundation parameter layout changed')
        cursor += size
    if image[cursor:cursor + 12] != struct.pack('<QI', 0xffffffffffffffff, 1):
        raise ValueError('expected foundation expandable heap settings')
    result = bytearray(image)
    struct.pack_into('<I', result, offset + 12, 0)
    return bytes(result)
