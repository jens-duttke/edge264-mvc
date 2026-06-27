#!python3
import json, os, re, sys
from shutil import which
from subprocess import run
from timeit import timeit

if not(len(sys.argv) == 3 and
       which("edge264_test-gcc") and
       which("edge264_test-clang") and
       which("ffmpeg") and
       which("avcdec") and
       which("h264dec")):
	print(f"Usage: {sys.argv[0]} <video.264> <nb_runs>\n" +
		"PATH must contain the paths to edge264_test-gcc, edge264_test-clang, ffmpeg,\n" +
		"avcdec and h264dec")
	exit(1)

ncores = os.cpu_count() or 1

# Every decoder that supports multithreading is timed both single-threaded (1T)
# and multithreaded (MT, all cores), so the comparison is fair at both ends:
# edge264 via -sby/-mby, ffmpeg via -threads 1 vs 0 (auto frame threading),
# avcdec via --num_cores 1 vs N. OpenH264's decoder has no multithreading, so it
# is timed once. All numbers are wall-clock seconds: edge264 prints "time:",
# ffmpeg's -benchmark prints rtime (real time; utime would sum per-thread CPU and
# hide the MT speedup), and avcdec is wrapped in a wall-clock timer. Pull values
# by name rather than a fixed offset so the parse stays robust.
#
# We report the MINIMUM across runs, not the median: on shared CI runners noise
# only ever slows a run down (contention from co-tenants, a cold first run), so
# the fastest run is the cleanest estimate of true decode time and is the metric
# least sensitive to runner variance. Every decoder is reduced the same way, so
# the comparison stays fair.
def edge264_time(binary, flags):
	out = run([binary, flags, sys.argv[1]], capture_output=True).stdout.decode("latin1")
	for line in out.splitlines():
		if line.startswith("time:"):
			return float(line.split(":", 1)[1].strip().rstrip("s"))
	return float("nan")

def ffmpeg_time(threads):
	err = run(["ffmpeg", "-nostdin", "-hide_banner", "-benchmark", "-threads", str(threads),
		"-c:v", "h264", "-i", sys.argv[1], "-f", "null", "-"], capture_output=True).stderr.decode("latin1")
	m = re.search(r"rtime=([0-9.]+)", err)
	return float(m.group(1)) if m else float("nan")

def libavc_time(cores):
	with open("test.cfg", "w") as f:
		f.write(f"--input {sys.argv[1]}\n--num_cores {cores}")
	return timeit(lambda: run("avcdec", capture_output=True), number=1)

def openh264_time():
	return float(run(["h264dec", sys.argv[1]], capture_output=True).stderr.split(b"\n")[6].split()[2])

cols = ["edge264-GCC-1T", "edge264-GCC-MT", "edge264-Clang-1T", "edge264-Clang-MT",
	"FFmpeg-1T", "FFmpeg-MT", "LibAVC-1T", "LibAVC-MT", "OpenH264"]
samples = {c: [] for c in cols}
for _ in range(int(sys.argv[2])):
	samples["edge264-GCC-1T"].append(edge264_time("edge264_test-gcc", "-sby"))
	samples["edge264-GCC-MT"].append(edge264_time("edge264_test-gcc", "-mby"))
	samples["edge264-Clang-1T"].append(edge264_time("edge264_test-clang", "-sby"))
	samples["edge264-Clang-MT"].append(edge264_time("edge264_test-clang", "-mby"))
	samples["FFmpeg-1T"].append(ffmpeg_time(1))
	samples["FFmpeg-MT"].append(ffmpeg_time(0))
	samples["LibAVC-1T"].append(libavc_time(1))
	samples["LibAVC-MT"].append(libavc_time(ncores))
	samples["OpenH264"].append(openh264_time())
print(json.dumps({c: round(min(samples[c]), 1) for c in cols}, separators=(",", ":")))
