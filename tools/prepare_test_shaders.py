#!/usr/bin/env python3
"""Compile owned GLSL compute fixtures for host compiler integration tests."""

from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/test-shaders"


def main():
    sources = {name: ROOT / f"experiments/compute/{name}.comp" for name in ("minimal", "xor")}
    targets = {name: OUTPUT / f"{name}.spv" for name in sources}
    if all(target.is_file() and target.stat().st_mtime >= sources[name].stat().st_mtime
           for name, target in targets.items()):
        return
    compiler = shutil.which("glslangValidator")
    if not compiler:
        raise SystemExit("glslangValidator is required to prepare compiler test shaders")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        target = targets[name]
        subprocess.run(
            [compiler, "-V", "--target-env", "vulkan1.0", str(source), "-o", str(target)],
            check=True,
        )


if __name__ == "__main__":
    main()
