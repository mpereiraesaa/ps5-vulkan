#!/usr/bin/env python3
"""Run the tests/ unittest modules in parallel, one process per module.

Equivalent to `python -m unittest discover -s tests`, but modules run
concurrently. Modules that build into shared output trees (dist-sdk, the
native consumer, build/graphics) keep one serial lane, so they never race each
other. Each module's output is printed as one block when it finishes, and the
run fails if any module fails.

Observed durations are cached in build/python-test-times.json so the slowest
modules start first on the next run.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
TIMES = ROOT / "build/python-test-times.json"

# Modules that build into, or link against, fixed shared paths (dist-sdk,
# examples/native_consumer/build, build/graphics, dist-upstream-cts,
# build/tests/test_cts_host). They run one at a time, in this order, in a
# single lane alongside the parallel ones. A new module that touches any of
# these trees, directly or through a tool it runs, belongs here.
SERIAL = (
    "test_build_sdk_identity",
    "test_consumer_isolation",
    "test_consumer_resource_abi",
    "test_cts_runner",
    "test_dxvk_probe",
    "test_sdk_archive",
    "test_tess_sdk_profile",
    "test_native_diagnostic_options",
    "test_graphics_control_generator",
    "test_license_policy",
    "test_lab",
    "test_tess_reference_assets",
    "test_upstream_runner",
    "test_upstream_tessellation_registration",
    "test_verify_geometry",
)

SUMMARY = re.compile(r"^Ran (\d+) tests? in", re.M)
SKIPS = re.compile(r"skipped=(\d+)")


def run_module(module: str, verbose: bool, tests: Path = TESTS) -> tuple[str, int, str, float]:
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(filter(None, [str(tests), env.get("PYTHONPATH")]))
    args = [sys.executable, "-m", "unittest"] + (["-v"] if verbose else []) + [module]
    started = time.monotonic()
    # Tests never read stdin; a tool that would (llvm-mc -mcpu=help) must see
    # EOF instead of blocking on whatever terminal or pipe launched the run.
    proc = subprocess.run(args, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return module, proc.returncode, proc.stdout, time.monotonic() - started


def run_serial(modules: list[str], verbose: bool,
               tests: Path = TESTS) -> list[tuple[str, int, str, float]]:
    return [run_module(module, verbose, tests) for module in modules]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-j", "--jobs", type=int,
                        default=int(os.environ.get("PYTHON_TEST_JOBS", os.cpu_count() or 1)))
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--start-dir", type=Path, default=TESTS)
    parser.add_argument("--times", type=Path, default=TIMES)
    parser.add_argument("modules", nargs="*", help="module names (default: every test_*.py)")
    args = parser.parse_args(argv)

    tests = args.start_dir
    modules = args.modules or sorted(p.stem for p in tests.glob("test_*.py"))
    try:
        times = json.loads(args.times.read_text())
    except (OSError, ValueError):
        times = {}
    serial = [m for m in SERIAL if m in modules]
    parallel = sorted((m for m in modules if m not in SERIAL),
                      key=lambda m: times.get(m, 0.0), reverse=True)

    results = []
    failed = []
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(run_serial, serial, args.verbose, tests)] if serial else []
        futures += [pool.submit(run_serial, [m], args.verbose, tests) for m in parallel]
        for future in as_completed(futures):
            for module, code, output, seconds in future.result():
                results.append((module, code, output, seconds))
                status = "ok" if code == 0 else "FAILED"
                print(f"==== {module} ({seconds:.1f}s) {status}", flush=True)
                if args.verbose or code != 0:
                    print(output, end="" if output.endswith("\n") else "\n", flush=True)
                if code != 0:
                    failed.append(module)

    args.times.parent.mkdir(parents=True, exist_ok=True)
    times.update({module: round(seconds, 2) for module, _, _, seconds in results})
    args.times.write_text(json.dumps(times, indent=1, sort_keys=True) + "\n")

    total = sum(int(m.group(1)) for _, _, out, _ in results for m in SUMMARY.finditer(out))
    skipped = sum(int(m.group(1)) for _, _, out, _ in results for m in SKIPS.finditer(out))
    elapsed = time.monotonic() - started
    print(f"\nRan {total} tests in {len(results)} modules in {elapsed:.1f}s "
          f"(jobs={args.jobs}, skipped={skipped})")
    if failed:
        print("FAILED modules: " + ", ".join(sorted(failed)))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
