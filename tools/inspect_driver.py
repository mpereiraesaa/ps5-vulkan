#!/usr/bin/env python3
"""Read-only private diagnostic of this title's audited FW12.02 driver."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "rehd_mods"))
from ps5debug import PS5Debug

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    args = parser.parse_args()
    with PS5Debug(args.host, 744, 15) as dbg:
        selected = []
        for proc in dbg.get_process_list():
            if proc.name != "eboot.bin":
                continue
            info = dbg.get_process_info(proc.pid)
            if info.titleid == "PPSA99994":
                selected.append(proc)
        if len(selected) != 1:
            raise SystemExit("Require exactly one PPSA99994 process")
        pid = selected[0].pid
        maps = dbg.get_process_maps(pid)
        code = [m for m in maps if "libSceAgcDriver" in m.name and "x" in m.perms]
        if len(code) != 1:
            raise SystemExit("Require exactly one executable AgcDriver mapping")
        base = code[0].start
        wrapper = dbg.read_memory(pid, base + 0x2960, 15)
        reference = (Path(__file__).resolve().parents[3] /
                     "research/gpu/dumps/game-libSceAgcDriver.sprx.bin").read_bytes()[0x2960:0x296f]
        if wrapper != reference:
            raise SystemExit("Driver wrapper differs from audited reference; no offset reads")
        queue = dbg.read_memory(pid, base + 0x228b8, 8)
        mode = dbg.read_memory(pid, base + 0x22910, 4)
        size, index = struct.unpack("<II", queue)
        # Local linker input describes this build's GOT slot. Only compare;
        # an artifact mismatch must never be mistaken for a proven binding.
        relocations = subprocess.check_output([
            "llvm-readelf-18", "-r", str(Path(__file__).resolve().parents[1] /
                                         "build/native/pie.elf")], text=True)
        slots = [int(line.split()[0], 16) for line in relocations.splitlines()
                 if "R_X86_64_GLOB_DAT" in line and "sceAgcDriverSubmitDcb +" in line]
        executable = [m for m in maps if m.name == "executable" and "x" in m.perms]
        if len(slots) != 1 or len(executable) != 1:
            raise SystemExit("Ambiguous local relocation or executable mapping")
        address = executable[0].start + slots[0]
        if not any(m.start <= address and address + 8 <= m.end and "r" in m.perms
                   for m in maps):
            raise SystemExit("GOT slot outside readable process mappings")
        got = struct.unpack("<Q", dbg.read_memory(pid, address, 8))[0]
        print(json.dumps({"title": "PPSA99994", "pid": pid,
            "submit_got_matches": got == base + 0x2960,
            "wrapper_matches": True, "wrapper_sha256": hashlib.sha256(wrapper).hexdigest(),
            "queue_size": size, "queue_index": index,
            "process_mode": struct.unpack("<I", mode)[0]}))

if __name__ == "__main__":
    main()
