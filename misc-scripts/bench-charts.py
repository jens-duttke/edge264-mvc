#!python3
import base64, datetime, json, matplotlib, matplotlib.colors as mcolors, matplotlib.pyplot as plt, numpy as np, sys
matplotlib.use("Agg")

# print usage if wrong number of inputs or ill-formed JSON
data = None
if len(sys.argv) == 3:
	try: data = json.loads(sys.argv[1])
	except: pass
if not data:
	print(f"Usage: {sys.argv[0]} <json> <output.svg>\n" +
		"data should be a matrix with named rows and columns encoded in JSON, like\n" +
		'{"x86":{"edge":0,"other":1},"arm64":{"edge":2,"other":3}}', file=sys.stderr)
	exit(1)
rnames = list(data.keys())
cnames = list(tuple(data.values())[0].keys())
d = datetime.datetime.today()

# generate output chart
x = np.arange(len(rnames))
width = 1 / (len(cnames) + 1)
fig, ax = plt.subplots(figsize=(max(6, 0.9 * len(cnames) + 1.5), 4), layout="constrained")
# Pair the single-/multi-thread bars of each decoder under one hue: 1T is a
# pastel tint, MT the same hue at full strength; a decoder without a 1T/MT split
# (e.g. OpenH264) uses its hue at full strength.
def base_kind(name):
	for s in ("-1T", "-MT"):
		if name.endswith(s):
			return name[:-3], s[1:]
	return name, "MT"

def tint(color, amount): # blend toward white; amount in [0,1], higher = lighter
	r, g, b = mcolors.to_rgb(color)
	return (r + (1 - r) * amount, g + (1 - g) * amount, b + (1 - b) * amount)

bases = []
for cname in cnames:
	b = base_kind(cname)[0]
	if b not in bases:
		bases.append(b)
palette = matplotlib.colormaps["tab10"].colors
base_color = {b: palette[i % len(palette)] for i, b in enumerate(bases)}

for c, cname in enumerate(cnames):
	b, kind = base_kind(cname)
	strong = base_color[b]
	color = tint(strong, 0.55) if kind == "1T" else strong
	rects = ax.bar(x + c * width, [r[cname] for r in data.values()], width * 0.9,
		label=cname, color=color, edgecolor=strong, linewidth=0.6, zorder=3)
	ax.bar_label(rects, fmt="{:.1f}", padding=3)
ax.set_xticks(x + 0.5 - width, rnames)
ax.set_ylabel("Seconds", color="#555", fontsize=10)
ax.set_title(d.strftime("Decoding time measured on %d/%m/%Y (lower is better)"), color="#555")
ax.set_ylim(0, 1.08 * max(max(r.values()) for r in data.values()))
ax.tick_params(colors="#555")
ax.spines[:].set_color("#555")
ax.grid(axis="y", color="#aaa", linestyle="--", linewidth=0.7, zorder=0)
ax.legend(facecolor="#222", edgecolor="#aaa", labelcolor="#fff", fontsize=10,
	loc="upper left", bbox_to_anchor=(1.01, 1.0))
plt.savefig(sys.argv[2])
