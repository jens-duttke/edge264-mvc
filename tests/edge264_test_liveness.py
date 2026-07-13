#!/usr/bin/env python3
"""Check that edge264_test's forced flush breaks a zero-progress stall."""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def run_regular(exe: Path, fixture: Path, mode: str, timeout: float) -> None:
    label = f"{fixture.name} {mode} regular"
    try:
        completed = subprocess.run(
            [str(exe), str(fixture), mode],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"{label} timed out after {timeout:g}s") from exc
    if completed.returncode != 0:
        raise RuntimeError(
            f"{label} exited {completed.returncode}\n"
            f"stderr:\n{completed.stderr.decode(errors='replace')}"
        )
    if completed.stderr:
        raise RuntimeError(
            f"{label} wrote unexpected diagnostics\n"
            f"stderr:\n{completed.stderr.decode(errors='replace')}"
        )
    output = ANSI.sub(b"", completed.stdout).splitlines()
    if not output or b"1 PASS, 0 UNSUPPORTED, 0 FAIL" not in output[-1]:
        raise RuntimeError(
            f"{label} did not finish as a passing decode\n"
            f"stdout:\n{completed.stdout.decode(errors='replace')}"
        )


def run_stdin(
    exe: Path, fixture: Path, frame: bytes, mode: str, timeout: float
) -> None:
    label = f"{fixture.name} {mode} stdin"
    try:
        completed = subprocess.run(
            [str(exe), "-", mode, "-o"],
            input=fixture.read_bytes(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"{label} timed out after {timeout:g}s") from exc
    if completed.returncode != 0:
        raise RuntimeError(
            f"{label} exited {completed.returncode}\n"
            f"stderr:\n{completed.stderr.decode(errors='replace')}"
        )
    expected = (
        b"YUV4MPEG2 W16 H16 F24000:1001 Ip A1:1 C420mpeg2\n"
        + (b"FRAME\n" + frame) * 16
    )
    if completed.stdout != expected:
        raise RuntimeError(
            f"{label} produced {len(completed.stdout)} bytes, expected {len(expected)}"
        )
    messages = [line for line in ANSI.sub(b"", completed.stderr).splitlines() if line]
    if not messages or b"1 PASS, 0 UNSUPPORTED, 0 FAIL" not in messages[-1]:
        raise RuntimeError(
            f"{label} did not finish as a passing decode\n"
            f"stderr:\n{completed.stderr.decode(errors='replace')}"
        )
    if any(
        b" PASS, " not in line or b" UNSUPPORTED, " not in line or b" FAIL" not in line
        for line in messages
    ):
        raise RuntimeError(
            f"{label} wrote unexpected diagnostics\n"
            f"stderr:\n{completed.stderr.decode(errors='replace')}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default="./edge264_test", type=Path)
    parser.add_argument("--timeout", default=10.0, type=float)
    parser.add_argument(
        "--fixture",
        default=Path("tests/liveness/cabac_all_incomplete.264"),
        type=Path,
    )
    args = parser.parse_args()

    exe = args.exe.resolve()
    with tempfile.TemporaryDirectory(prefix="edge264-test-liveness-") as directory:
        fixture = Path(directory) / args.fixture.name
        shutil.copyfile(args.fixture.resolve(), fixture)
        frame = bytes([128]) * (16 * 16 + 2 * 8 * 8)
        fixture.with_suffix(".yuv").write_bytes(frame * 16)
        for mode in ("-s", "-m"):
            run_regular(exe, fixture, mode, args.timeout)
            print(f"PASS {args.fixture.name} {mode} regular")
            run_stdin(exe, fixture, frame, mode, args.timeout)
            print(f"PASS {args.fixture.name} {mode} stdin")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        raise SystemExit(1)
