#!/usr/bin/env python3
"""Compare regular-file, stdin, and FIFO input in edge264_test."""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import List, NamedTuple, Optional, Sequence, Tuple


SMALL_CHUNKS = (1, 2, 1, 3, 5, 8, 13, 2, 4, 7, 11, 17)


class Fixture(NamedTuple):
    path: Path
    frames: int
    fifo_chunks: Tuple[int, ...]
    pause_after: int
    three_byte: bool
    distinct_views: bool


FIXTURES = (
    Fixture(
        Path("tests/conformance/mvc-synthetic/mvc_interview_reflist.264"),
        2,
        SMALL_CHUNKS,
        0,
        True,
        True,
    ),
    Fixture(
        Path("tests/conformance/mvc-synthetic/mvc_same_poc_pairing.264"),
        320,
        (4096, 1) + SMALL_CHUNKS,
        1,
        False,
        False,
    ),
)
EMPTY_NAL_FIXTURE = FIXTURES[0]
MODES = (("-o", 16), ("-O", 32))
THREADS = (((), "auto"), (("-s",), "single"))


def run_capture(
    argv: Sequence[str],
    stdin: Optional[bytes],
    timeout: float,
    label: str,
) -> bytes:
    try:
        completed = subprocess.run(
            argv,
            input=stdin,
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
    return completed.stdout


def inspect_y4m(data: bytes, width: int, frames: int, label: str) -> Tuple[int, int]:
    newline = data.find(b"\n")
    if newline < 0:
        raise RuntimeError(f"{label} did not produce a Y4M header")
    header = data[:newline].split()
    if not header or header[0] != b"YUV4MPEG2":
        raise RuntimeError(f"{label} did not produce a Y4M stream")
    fields = {part[:1]: part[1:] for part in header[1:]}
    try:
        actual_width = int(fields[b"W"])
        height = int(fields[b"H"])
    except (KeyError, ValueError) as exc:
        raise RuntimeError(f"{label} has an invalid Y4M header") from exc
    if (actual_width, height) != (width, 16):
        raise RuntimeError(
            f"{label} dimensions are {actual_width}x{height}, expected {width}x16"
        )
    frame_size = actual_width * height * 3 // 2
    offset = newline + 1
    actual_frames = 0
    while offset < len(data):
        if data[offset : offset + 6] != b"FRAME\n":
            raise RuntimeError(f"{label} has an invalid frame marker")
        offset += 6 + frame_size
        if offset > len(data):
            raise RuntimeError(f"{label} has a truncated frame")
        actual_frames += 1
    if actual_frames != frames:
        raise RuntimeError(
            f"{label} has {actual_frames} frames, expected {frames}"
        )
    return actual_width, height


def assert_distinct_views(data: bytes, width: int, height: int, label: str) -> None:
    newline = data.find(b"\n")
    if newline < 0:
        raise RuntimeError(f"{label} did not produce a Y4M header")
    if width % 2 != 0:
        raise RuntimeError(f"{label} has an odd side-by-side width")
    view_width = width // 2
    frame_size = width * height * 3 // 2
    offset = newline + 1
    while offset < len(data):
        offset += 6
        luma = data[offset : offset + width * height]
        if any(
            luma[row : row + view_width] != luma[row + view_width : row + width]
            for row in range(0, len(luma), width)
        ):
            return
        offset += frame_size
    raise RuntimeError(f"{label} did not contain distinct MVC views")


def edge264_argv(
    exe: Path,
    source: str,
    mode: str,
    thread_args: Sequence[str],
) -> List[str]:
    return [str(exe), source, mode, "-k", "-y", *thread_args]


def run_fifo(
    exe: Path,
    fixture: Path,
    mode: str,
    thread_args: Sequence[str],
    chunks: Tuple[int, ...],
    pause_after: int,
    timeout: float,
    label: str,
) -> bytes:
    with tempfile.TemporaryDirectory(prefix="edge264-stream-input-") as directory:
        fifo = Path(directory) / f"{fixture.stem}.264"
        os.mkfifo(fifo)
        process = subprocess.Popen(
            edge264_argv(exe, str(fifo), mode, thread_args),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        writer_errors: List[BaseException] = []

        def writer() -> None:
            try:
                data = fixture.read_bytes()
                with fifo.open("wb", buffering=0) as pipe:
                    offset = 0
                    index = 0
                    while offset < len(data):
                        size = chunks[index % len(chunks)]
                        pipe.write(data[offset : offset + size])
                        offset += size
                        index += 1
                        if index == pause_after:
                            # The next byte completes a start code at offset 4094.
                            time.sleep(0.005)
            except BaseException as exc:
                writer_errors.append(exc)

        thread = threading.Thread(target=writer, name="edge264-fifo-writer", daemon=True)
        thread.start()
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            process.kill()
            stdout, stderr = process.communicate()
            thread.join(timeout=1)
            raise RuntimeError(
                f"{label} timed out after {timeout:g}s\n"
                f"partial stdout: {len(stdout)} bytes\n"
                f"stderr:\n{stderr.decode(errors='replace')}"
            ) from exc
        thread.join(timeout=1)
        if thread.is_alive():
            raise RuntimeError(f"{label} FIFO writer did not finish")
        if process.returncode != 0:
            raise RuntimeError(
                f"{label} exited {process.returncode}\n"
                f"stderr:\n{stderr.decode(errors='replace')}"
            )
        if writer_errors:
            raise RuntimeError(f"{label} FIFO writer failed: {writer_errors[0]!r}")
        return stdout


def check_empty_nal(exe: Path, fixture: Path, timeout: float) -> None:
    label = f"{fixture.name} empty NAL"
    try:
        completed = subprocess.run(
            edge264_argv(exe, "-", "-O", ("-s",)),
            input=b"\x00\x00\x01" + fixture.read_bytes(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"{label} timed out after {timeout:g}s") from exc
    if completed.stdout:
        raise RuntimeError(f"{label} produced output instead of rejecting the stream")
    clean_stderr = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", completed.stderr)
    if not clean_stderr.splitlines() or b"1 FAIL" not in clean_stderr.splitlines()[-1]:
        raise RuntimeError(
            f"{label} was not rejected as invalid\n"
            f"stderr:\n{completed.stderr.decode(errors='replace')}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default="./edge264_test", type=Path)
    parser.add_argument("--timeout", default=15.0, type=float)
    args = parser.parse_args()

    exe = args.exe.resolve()
    have_fifo = hasattr(os, "mkfifo")
    check_empty_nal(exe, EMPTY_NAL_FIXTURE.path.resolve(), args.timeout)
    for fixture_spec in FIXTURES:
        fixture = fixture_spec.path.resolve()
        data = fixture.read_bytes()
        for mode, width in MODES:
            for thread_args, thread_name in THREADS:
                label = f"{fixture.name} {mode} {thread_name}"
                regular = run_capture(
                    edge264_argv(exe, str(fixture), mode, thread_args),
                    None,
                    args.timeout,
                    f"{label} regular",
                )
                stdin = run_capture(
                    edge264_argv(exe, "-", mode, thread_args),
                    data,
                    args.timeout,
                    f"{label} stdin",
                )
                dimensions = inspect_y4m(
                    regular,
                    width,
                    fixture_spec.frames,
                    f"{label} regular",
                )
                if stdin != regular:
                    raise RuntimeError(f"{label} stdin differs from regular-file output")
                if fixture_spec.three_byte:
                    three_byte = data.replace(b"\x00\x00\x00\x01", b"\x00\x00\x01")
                    stdin_three_byte = run_capture(
                        edge264_argv(exe, "-", mode, thread_args),
                        three_byte,
                        args.timeout,
                        f"{label} three-byte stdin",
                    )
                    if stdin_three_byte != regular:
                        raise RuntimeError(
                            f"{label} three-byte stdin differs from regular-file output"
                        )
                if have_fifo:
                    fifo = run_fifo(
                        exe,
                        fixture,
                        mode,
                        thread_args,
                        fixture_spec.fifo_chunks,
                        fixture_spec.pause_after,
                        args.timeout,
                        f"{label} FIFO",
                    )
                    if fifo != regular:
                        raise RuntimeError(f"{label} FIFO differs from regular-file output")
                if fixture_spec.distinct_views and mode == "-O":
                    assert_distinct_views(regular, *dimensions, label)
                print(f"PASS {label}: {len(regular)} bytes")
    if not have_fifo:
        print("SKIP FIFO input: os.mkfifo is unavailable")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        raise SystemExit(1)
