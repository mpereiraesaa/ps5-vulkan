"""Reproduce the host-only LLPC compute control; never deploy or submit."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from lab import lab_root

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_COMPILER = "666e7ec02a47d709f01c5230ea4cf3b2e379b49ff2cb7ecaca36a2f1b8c0f0a0"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    build = Path(os.environ.get("PS5VK_LLPC_BUILD", str(
        lab_root() / "third_party/amd-llpc/build-gfx1030"))).resolve()
    compiler = build / "llpc/amdllpc"
    if digest(compiler) != EXPECTED_COMPILER:
        raise SystemExit("Compiler differs from audited control; audit before changing pin")
    output = ROOT / "build/compute"
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="control-", dir=output))
    source = ROOT / "experiments/compute/minimal.comp"
    def invoke(*args):
        return subprocess.run([str(compiler), "-gfxip=10.1.3", *map(str, args)],
                              cwd=run, check=True, capture_output=True, text=True)
    invoke("-enable-pipeline-dump", f"-pipeline-dump-dir={run}",
           "-o=dump.elf", source)
    spirv, = run.glob("Shader_*.spv")
    for name in ("first.elf", "repeat.elf"):
        invoke(f"-o={name}", spirv)
    first = run / "first.elf"
    if first.read_bytes() != (run / "repeat.elf").read_bytes():
        raise SystemExit("Non-reproducible compiler output")
    notes = subprocess.check_output(
        [str(build / "llvm/bin/llvm-readobj"), "--notes", str(first)], text=True)
    disassembly = subprocess.check_output(
        [str(build / "llvm/bin/llvm-objdump"), "-d", "--mcpu=gfx1013", str(first)],
        text=True)
    for marker in ("amdgcn--amdpal--gfx1013", ".compute:", "_amdgpu_cs_main"):
        if marker not in notes:
            raise SystemExit(f"Missing metadata marker: {marker}")
    (run / "metadata.txt").write_text(notes)
    (run / "disassembly.txt").write_text(disassembly)
    record = {"scope": "host-compiler-only", "hardware_validated": False,
              "target": "gfx1013", "compiler_sha256": digest(compiler),
              "source_sha256": digest(source), "spirv_sha256": digest(spirv),
              "elf_sha256": digest(first), "repeat_identical": True}
    (run / "manifest.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))
    print(f"Private generated evidence: {run}")


if __name__ == "__main__":
    main()
