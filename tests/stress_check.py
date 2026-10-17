#!/usr/bin/env python3
"""Decode fixtures many times with worker threads, each run a process of its own.

A race in the multithreaded path shows up only in some runs: a single decode
per fixture, as in `check`, catches one rarely. This harness decodes every
fixture once single-threaded for the reference, then repeats it with worker
threads, several processes at a time. Every run must exit like the reference
did, within the timeout, with the same output (with --any-output, only exit
like it, for crafted fixtures whose output may differ). Each run is a process of its
own because a decoder opened and closed repeatedly inside one process did not
hit a race that a fresh process hits. Build with `CPPFLAGS=-DNDEBUG` to check
what a release build does once an assertion would have stopped it, and with
`SANITIZE=address` to catch the memory errors that follow.
"""

import argparse
import hashlib
import itertools
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


# auto (three workers per logical CPU), more workers than any fixture keeps busy -
# many of them asleep, where races at start and close show - the most used, and few
THREADS = (None, "64", "128", "2")


def decode(exe: Path, fixture: Path, mode: str, threads, timeout: float):
    env = dict(os.environ)
    env.pop("EDGE264MVC_THREADS", None)
    if threads is not None:
        env["EDGE264MVC_THREADS"] = threads
    try:
        completed = subprocess.run(
            [str(exe), str(fixture), mode, "-y", "-k", "-O"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return None, None, f"timed out after {timeout:g}s"
    return completed.returncode, hashlib.sha256(completed.stdout).hexdigest(), completed.stderr.decode(errors="replace")


def describe(stderr: str) -> str:
    lines = [l for l in stderr.splitlines() if "Assertion" in l or "ERROR:" in l or "SUMMARY:" in l or "timed out" in l]
    if lines:
        return " | ".join(lines[:3])
    rest = stderr.strip().splitlines()
    return rest[-1] if rest else "(no diagnostics)"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default="./edge264mvc_test", type=Path)
    parser.add_argument("--runs", default=500, type=int, help="multithreaded runs per fixture")
    parser.add_argument("--jobs", default=os.cpu_count() or 4, type=int, help="runs at a time")
    parser.add_argument("--timeout", default=60.0, type=float)
    parser.add_argument("--any-output", action="store_true",
        help="hold the runs to the exit status only, for crafted fixtures whose output may differ with threads")
    parser.add_argument("fixtures", nargs="+", type=Path)
    args = parser.parse_args()

    exe = args.exe.resolve()
    failures = 0
    for fixture in args.fixtures:
        ref_rc, ref_hash, ref_err = decode(exe, fixture, "-s", None, args.timeout)
        if ref_rc is None or ref_rc < 0:
            print(f"FAIL {fixture.name} single-threaded: {describe(ref_err)}")
            failures += 1
            continue
        runs = list(zip(range(args.runs), itertools.cycle(THREADS)))
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda r: (r, decode(exe, fixture, "-m", r[1], args.timeout)), runs))
        bad = [(i, t, rc, h, err) for (i, t), (rc, h, err) in results if rc != ref_rc or (h != ref_hash and not args.any_output)]
        for i, t, rc, h, err in bad[:5]:
            what = "different output" if rc == ref_rc else f"exit {rc} instead of {ref_rc}"
            print(f"FAIL {fixture.name} run {i + 1} threads={t or 'auto'}: {what}: {describe(err)}")
        print(f"{'FAIL' if bad else 'PASS'} {fixture.name}: {len(bad)} of {args.runs} multithreaded runs differ from the single-threaded one")
        failures += len(bad)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
