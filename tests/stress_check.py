#!/usr/bin/env python3
"""Decode fixtures many times with worker threads, each run a process of its own.

A race in the multithreaded path shows up only in some runs: a single decode
per fixture catches one rarely. This harness decodes every fixture once
single-threaded for the reference, then repeats it with worker threads,
several processes at a time. Every run must exit like the reference did,
within the timeout, with the same output (with --any-output, only exit like
it). Each run is a process of its own because a decoder opened and closed
repeatedly inside one process did not hit a race that a fresh process hits.

The runs alternate between two programs: edge264mvc_test, whose Y4M output
holds the samples only, and frame_info, which also prints the POC, decoding
and display order and flags of every view - two dependent views paired with
the wrong base views give the same samples. Build with `CPPFLAGS=-DNDEBUG` to
check what a release build does once an assertion would have stopped it, and
with `SANITIZE=address` to catch the memory errors that follow.
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


def decode(tool: str, exes: dict, fixture: Path, threads, timeout: float):
    """threads: "1" single-threaded, None auto, else a count"""
    env = dict(os.environ)
    env.pop("EDGE264MVC_THREADS", None)
    if tool == "test":
        # edge264mvc_test: -s single-threaded, -m with EDGE264MVC_THREADS or auto
        command = [str(exes[tool]), str(fixture), "-s" if threads == "1" else "-m", "-y", "-k", "-O"]
        if threads not in (None, "1"):
            env["EDGE264MVC_THREADS"] = threads
    else:
        # frame_info: EDGE264MVC_THREADS is n_threads, 0 being auto
        command = [str(exes[tool]), str(fixture)]
        env["EDGE264MVC_THREADS"] = "0" if threads is None else threads
    try:
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=timeout, check=False)
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
    parser.add_argument("--frame-info", default="./frame_info", type=Path)
    parser.add_argument("--runs", default=500, type=int, help="multithreaded runs per fixture")
    parser.add_argument("--jobs", default=os.cpu_count() or 4, type=int, help="runs at a time")
    parser.add_argument("--timeout", default=60.0, type=float)
    parser.add_argument("--any-output", action="store_true", help="hold the runs to the exit status only, for diagnosis")
    parser.add_argument("fixtures", nargs="+", type=Path)
    args = parser.parse_args()

    exes = {"test": args.exe.resolve(), "info": args.frame_info.resolve()}
    failures = 0
    for fixture in args.fixtures:
        refs = {tool: decode(tool, exes, fixture, "1", args.timeout) for tool in exes}
        broken = [tool for tool, (rc, _, _) in refs.items() if rc is None or rc < 0]
        if broken:
            print(f"FAIL {fixture.name} single-threaded: {describe(refs[broken[0]][2])}")
            failures += 1
            continue
        # every thread count with both programs, in turn
        runs = list(zip(range(args.runs), itertools.cycle(itertools.product(THREADS, exes))))
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda r: (r, decode(r[1][1], exes, fixture, r[1][0], args.timeout)), runs))
        bad = []
        for (i, (threads, tool)), (rc, h, err) in results:
            ref_rc, ref_hash, _ = refs[tool]
            if rc != ref_rc or (h != ref_hash and not args.any_output):
                bad.append((i, threads, tool, rc, ref_rc, err))
        for i, threads, tool, rc, ref_rc, err in bad[:5]:
            what = "different output" if rc == ref_rc else f"exit {rc} instead of {ref_rc}"
            program = "edge264mvc_test" if tool == "test" else "frame_info"
            print(f"FAIL {fixture.name} run {i + 1} {program} threads={threads or 'auto'}: {what}: {describe(err)}")
        print(f"{'FAIL' if bad else 'PASS'} {fixture.name}: {len(bad)} of {args.runs} multithreaded runs differ from the single-threaded one")
        failures += len(bad)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
