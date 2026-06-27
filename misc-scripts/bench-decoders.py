#!python3
import json, sys
from shutil import which
from statistics import median
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
with open("test.cfg", "w") as f:
	f.write(f"--input {sys.argv[1]}\n--num_cores 1")

# edge264_test prints "time: X.XXXs"; -s forces single-thread decoding and -m
# multithreaded (auto-detected cores), so we time both to show the speedup. The
# other decoders are timed single-threaded (ffmpeg -threads 1, avcdec
# --num_cores 1) as the fair single-thread baseline. Pull the "time:" line by
# name rather than by a fixed offset so the parse is robust.
def edge264_time(binary, flags):
	out = run([binary, flags, sys.argv[1]], capture_output=True).stdout.decode("latin1")
	for line in out.splitlines():
		if line.startswith("time:"):
			return float(line.split(":", 1)[1].strip().rstrip("s"))
	return float("nan")

gcc_1t, gcc_mt, clang_1t, clang_mt, ffmpeg, libavc, openh264 = ([] for _ in range(7))
for _ in range(int(sys.argv[2])):
	gcc_1t.append(edge264_time("edge264_test-gcc", "-sby"))
	gcc_mt.append(edge264_time("edge264_test-gcc", "-mby"))
	clang_1t.append(edge264_time("edge264_test-clang", "-sby"))
	clang_mt.append(edge264_time("edge264_test-clang", "-mby"))
	ffmpeg.append(float(run(["ffmpeg", "-hide_banner", "-benchmark", "-threads", "1", "-c:v", "h264", "-i", sys.argv[1], "-f", "null", "-"], capture_output=True).stderr.split(b'\n')[-3][13:18]))
	libavc.append(timeit(lambda: run("avcdec", capture_output=True), number=1))
	openh264.append(float(run(["h264dec", sys.argv[1]], capture_output=True).stderr.split(b'\n')[6].split()[2]))
print(f'{{"edge264-GCC-1T":{median(gcc_1t):.1f},"edge264-GCC-MT":{median(gcc_mt):.1f},'
      f'"edge264-Clang-1T":{median(clang_1t):.1f},"edge264-Clang-MT":{median(clang_mt):.1f},'
      f'"FFmpeg":{median(ffmpeg):.1f},"LibAVC":{median(libavc):.1f},"OpenH264":{median(openh264):.1f}}}')
