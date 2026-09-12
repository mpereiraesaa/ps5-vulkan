#!/usr/bin/env python3
"""Embed a little-endian SPIR-V module as an auditable C uint32_t array."""
import argparse
from pathlib import Path
import struct

parser=argparse.ArgumentParser()
parser.add_argument("input",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("symbol")
args=parser.parse_args()
data=args.input.read_bytes()
if not data or len(data)%4:
    raise SystemExit("SPIR-V must be a non-empty uint32 word stream")
words=struct.unpack(f"<{len(data)//4}I",data)
lines=[f"static const uint32_t {args.symbol}[] = {{"]
for offset in range(0,len(words),8):
    lines.append("    "+",".join(f"0x{word:08x}u" for word in words[offset:offset+8])+",")
lines.append("};")
args.output.write_text("\n".join(lines)+"\n")
