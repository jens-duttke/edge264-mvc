#!python3
import datetime, json, matplotlib, matplotlib.pyplot as plt, sys
matplotlib.use("Agg")

# Draws the benchmark results as two charts, one single-threaded and one
# multithreaded, from a JSON matrix of architectures and decoders such as
# {"x86-64": {"edge264-mvc-GCC-1T": 3.8, "edge264-mvc-GCC-MT": 2.3, ...}, ...}.
# A name ending in -1T or -MT is one decoder timed single- or multithreaded; a
# name without the suffix is a decoder that only decodes single-threaded. Each
# chart has one panel per architecture, with horizontal bars named on the axis,
# so no legend is needed.
data = None
if len(sys.argv) == 4:
	try: data = json.loads(sys.argv[1])
	except: pass
if not data:
	print(f"Usage: {sys.argv[0]} <json> <single-threaded.svg> <multithreaded.svg>\n" +
		"data should be a matrix with named rows and columns encoded in JSON, like\n" +
		'{"x86-64":{"edge264-mvc-GCC-1T":3.8,"edge264-mvc-GCC-MT":2.3},"arm64":{...}}', file=sys.stderr)
	exit(1)

# why a decoder has no multithreaded bar, shown in red in its row
NO_MT_REASON = {"edge264-GCC": "hangs", "edge264-Clang": "hangs", "OpenH264": "not supported"}

def split(name):
	for s in ("-1T", "-MT"):
		if name.endswith(s):
			return name[:-3], s[1:]
	return name, "1T"

def label(base):
	if base.startswith("edge264-mvc-"):
		return f"edge264-mvc ({base[12:]})"
	if base.startswith("edge264-"):
		return f"edge264 ({base[8:]})"
	return base

def product(base):
	return label(base).split(" (")[0]

archs = list(data.keys())
decoders = []
for name in tuple(data.values())[0]:
	if split(name)[0] not in decoders:
		decoders.append(split(name)[0])
products = list(dict.fromkeys(product(d) for d in decoders))
# one color per product (the GCC and Clang builds of a decoder share it), and
# no red among them: red marks the missing multithreaded results
palette = [c for i, c in enumerate(matplotlib.colormaps["tab10"].colors) if i != 3]
color = {d: palette[products.index(product(d)) % len(palette)] for d in decoders}
values = {kind: {a: {} for a in archs} for kind in ("1T", "MT")}
for a, row in data.items():
	for name, v in row.items():
		base, kind = split(name)
		values[kind][a][base] = v
top = max(max(r.values()) for r in values["1T"].values() if r) # the same scale for both charts

def chart(kind, title, path):
	rows = list(range(len(decoders)))[::-1] # first decoder on top
	fig, axes = plt.subplots(1, len(archs), sharey=True, figsize=(4.2 * len(archs) + 1.6, 0.42 * len(decoders) + 1.2), layout="constrained")
	axes = axes if len(archs) > 1 else [axes]
	for ax, a in zip(axes, archs):
		for y, d in zip(rows, decoders):
			v = values[kind][a].get(d)
			if v is not None:
				ax.barh(y, v, 0.7, color=color[d], zorder=3)
				ax.text(v + top * 0.015, y, f"{v:.1f} s", va="center", fontsize=9, color="#333")
			else:
				ax.text(top * 0.015, y, "✗ " + NO_MT_REASON.get(d, "n/a"), va="center", fontsize=9, color="#d62728", fontweight="bold")
		ax.set_title(a, color="#555", fontsize=10)
		ax.set_xlim(0, top * 1.18)
		ax.set_ylim(-0.7, len(decoders) - 0.3)
		ax.tick_params(colors="#555", labelsize=9)
		ax.spines[["top", "right"]].set_visible(False)
		ax.spines[["left", "bottom"]].set_color("#999")
		ax.grid(axis="x", color="#ccc", linestyle="--", linewidth=0.7, zorder=0)
		ax.set_xlabel("Seconds (lower is better)", color="#555", fontsize=9)
	axes[0].set_yticks(rows, [label(d) for d in decoders])
	fig.suptitle(title, color="#444", fontsize=11)
	plt.savefig(path)
	plt.close(fig)

# the kind of chart in bold (mathtext, whose "-" would be a minus sign, hence the
# non-breaking hyphen U+2011)
date = datetime.datetime.today().strftime("%Y-%m-%d")
chart("1T", f"$\\mathbf{{Single‑threaded}}$ decoding time, measured on {date}", sys.argv[2])
chart("MT", f"$\\mathbf{{Multithreaded}}$ decoding time (all cores), measured on {date}", sys.argv[3])
