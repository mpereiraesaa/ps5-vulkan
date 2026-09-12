"""Bounded debugger observation of the already-launched ps5vk title only.

No breakpoints, register writes, memory patches or other-title operations.
Output contains private process addresses: retain it outside public history.
Attaching changes timing; an attached run cannot certify an unattached exit.
"""
import argparse
import json
from pathlib import Path
import socket
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "rehd_mods"))
from ps5debug import PS5Debug, Debugger


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    args = parser.parse_args()
    with PS5Debug(args.host, 744, 10) as dbg:
        selected = [p for p in dbg.get_process_list()
                    if p.name == "eboot.bin" and
                    dbg.get_process_info(p.pid).titleid == "PPSA99994"]
        if len(selected) != 1:
            raise SystemExit("Require exactly one running PPSA99994")
        pid = selected[0].pid
        maps = dbg.get_process_maps(pid)
        with Debugger(dbg) as observer:
            observer.attach(pid)
            print(json.dumps(dict(event="attached", title="PPSA99994", pid=pid)), flush=True)
            observer.cont()
            try:
                event = observer.wait_event(timeout=10)
            except socket.timeout:
                print(json.dumps(dict(event="observation-timeout", pid=pid)), flush=True)
                return
            registers = event.regs
            containing = [m for m in maps if m.start <= registers.rip < m.end]
            print(json.dumps(dict(event="debug-event", pid=pid, status=event.status,
                                  stop_signal=(event.status >> 8) & 0xff
                                  if event.status & 0xff == 0x7f else None,
                                  thread=event.tdname, lwpid=event.lwpid,
                                  rip=hex(registers.rip), rsp=hex(registers.rsp),
                                  rbp=hex(registers.rbp), trapno=registers.trapno,
                                  error=registers.err,
                                  syscall_registers={name: hex(getattr(registers, name))
                                                     for name in ("rax", "rdi", "rsi", "rdx", "r10", "r8", "r9")},
                                  mapping=[dict(name=m.name, offset=hex(registers.rip-m.start))
                                           for m in containing])), flush=True)


if __name__ == "__main__":
    main()
