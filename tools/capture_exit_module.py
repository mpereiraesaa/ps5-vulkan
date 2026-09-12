"""Read this title's libkernel executable mapping into ignored private storage."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "rehd_mods"))
from ps5debug import PS5Debug


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", required=True)
    a = p.parse_args()
    with PS5Debug(a.host, 744, 10) as dbg:
        selected = [x for x in dbg.get_process_list() if x.name == "eboot.bin"
                    and dbg.get_process_info(x.pid).titleid == "PPSA99994"]
        if len(selected) != 1:
            raise SystemExit("Require exactly one PPSA99994")
        pid = selected[0].pid
        mappings = [m for m in dbg.get_process_maps(pid)
                    if m.name == "libkernel.sprx" and "x" in m.perms]
        if len(mappings) != 1:
            raise SystemExit("Ambiguous libkernel text mapping")
        m = mappings[0]
        size = m.end-m.start
        if not 0x400 <= size <= 8*1024*1024:
            raise SystemExit("Unexpected text mapping size")
        data = dbg.read_memory(pid, m.start, size)
        if len(data) != size:
            raise SystemExit("Short read")
    digest = hashlib.sha256(data).hexdigest()
    root = Path(__file__).resolve().parents[1] / "private-captures/exit-module"
    root.mkdir(parents=True, exist_ok=True)
    binary = root / f"libkernel-text-{digest[:16]}.bin"
    if binary.exists():
        if binary.read_bytes() != data:
            raise SystemExit("Existing artifact mismatch")
    else:
        with binary.open("xb") as f:
            f.write(data)
    record = dict(title="PPSA99994", pid=pid, base=hex(m.start), size=size,
                  sha256=digest, path=str(binary), read_only_capture=True)
    with (root / f"capture-{pid}.json").open("x") as f:
        json.dump(record, f, indent=2)
    print(json.dumps(record))


if __name__ == "__main__":
    main()
