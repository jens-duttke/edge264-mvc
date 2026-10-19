#!/usr/bin/env python3
# Rewrite a CAVLC stream as x264 writes it (no scaling matrices, POC type 0) to
# explicit weighted prediction: every PPS gets weighted_pred_flag 1 and
# weighted_bipred_idc 1, and every P and B slice header a pred_weight_table whose
# weights vary with the reference index, so that the field macroblocks of an
# MBAFF frame, which take the weights of refIdx >> 1 (8.4.2.3), meet each of them
# twice. x264 and the JM encoder code no weighted prediction in MBAFF frames.
# The slice data is left as it is, so the pictures differ from what the encoder
# meant and drift, which does not matter for a decoder test. Usage:
#   python3 tests/gen_explicit_weights.py <in.264> <out.264>
# 2d-synthetic/mbaff_explicit_weights comes from it, see 2d-synthetic/README.md.
import sys

def split(data):
	i, out = 0, []
	starts = []
	while True:
		j = data.find(b'\x00\x00\x01', i)
		if j < 0:
			break
		starts.append(j + 3)
		i = j + 3
	for k, s in enumerate(starts):
		e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
		while e > s and data[e - 1] == 0:
			e -= 1
		out.append(data[s:e])
	return out

def unescape(b):
	return b.replace(b'\x00\x00\x03', b'\x00\x00')

def escape(b):
	out, zeros = bytearray(), 0
	for x in b:
		if zeros >= 2 and x <= 3:
			out.append(3)
			zeros = 0
		out.append(x)
		zeros = zeros + 1 if x == 0 else 0
	return bytes(out)

class R:
	def __init__(s, b): s.b, s.p = b, 0
	def u(s, n):
		v = 0
		for _ in range(n):
			v = v << 1 | (s.b[s.p >> 3] >> (7 - (s.p & 7)) & 1)
			s.p += 1
		return v
	def ue(s):
		z = 0
		while s.u(1) == 0:
			z += 1
		return (1 << z) - 1 + s.u(z)
	def se(s):
		v = s.ue()
		return (v + 1) // 2 if v & 1 else -(v // 2)

class W:
	def __init__(s): s.bits = []
	def u(s, n, v): s.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]
	def ue(s, v):
		v += 1
		n = v.bit_length()
		s.u(n - 1, 0)
		s.u(n, v)
	def se(s, v): s.ue(2 * v - 1 if v > 0 else -2 * v)
	def copy(s, r, n):
		for _ in range(n):
			s.bits.append(r.u(1))
	def bytes(s):
		b = s.bits + [0] * (-len(s.bits) % 8)
		return bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))

sps, pps = {}, {}
out = bytearray()
for nal in split(open(sys.argv[1], 'rb').read()):
	t = nal[0] & 0x1f
	rbsp = unescape(nal[1:])
	if t == 7:
		r = R(rbsp)
		prof = r.u(8); r.u(16); sid = r.ue()
		if prof in (100, 110, 122, 244):
			cf = r.ue()
			if cf == 3: r.u(1)
			r.ue(); r.ue(); r.u(1)
			assert r.u(1) == 0, 'scaling matrices'
		fn = r.ue() + 4
		assert r.ue() == 0, 'POC type'
		lsb = r.ue() + 4
		r.ue(); r.u(1); r.ue(); r.ue()
		fmo = r.u(1)
		sps[sid] = dict(fn=fn, lsb=lsb, fmo=fmo)
	elif t == 8:
		r = R(rbsp); w = W()
		pid = r.ue(); sid = r.ue(); w.ue(pid); w.ue(sid)
		ent = r.u(1); bfp = r.u(1); w.u(1, ent); w.u(1, bfp)
		assert ent == 0, 'CAVLC only'
		assert r.ue() == 0; w.ue(0)
		n0 = r.ue(); n1 = r.ue(); w.ue(n0); w.ue(n1)
		r.u(1); r.u(2); w.u(1, 1); w.u(2, 1)
		rest = len(rbsp) * 8 - r.p
		w.copy(r, rest)
		pps[pid] = dict(sid=sid, bfp=bfp, n=(n0 + 1, n1 + 1))
		rb = w.bytes()
		while rb and rb[-1] == 0:
			rb = rb[:-1]
		nal = nal[:1] + escape(rb)
	elif t in (1, 5):
		r = R(rbsp); w = W()
		fm = r.ue(); st = r.ue(); pid = r.ue()
		w.ue(fm); w.ue(st); w.ue(pid)
		P, S = pps[pid], sps[pps[pid]['sid']]
		w.copy(r, S['fn'])
		field = 0
		if not S['fmo']:
			field = r.u(1); w.u(1, field)
			if field: w.copy(r, 1)
		if t == 5: w.ue(r.ue())
		w.copy(r, S['lsb'])
		if P['bfp'] and not field: w.se(r.se())
		st %= 5
		if st in (0, 1):
			if st == 1: w.copy(r, 1)
			n = list(P['n'])
			o = r.u(1); w.u(1, o)
			if o:
				n[0] = r.ue() + 1; w.ue(n[0] - 1)
				if st == 1: n[1] = r.ue() + 1; w.ue(n[1] - 1)
			for l in range(st + 1):
				f = r.u(1); w.u(1, f)
				while f:
					op = r.ue(); w.ue(op)
					if op == 3: break
					w.ue(r.ue())
			# pred_weight_table
			w.ue(5); w.ue(4)
			for l in range(st + 1):
				for i in range(n[l]):
					k = (i + l) % 3
					w.u(1, k != 1)
					if k != 1:
						w.se([40, 0, 26][k]); w.se([-3, 0, 6][k] * (1 if l == 0 else -1))
					w.u(1, k != 0)
					if k != 0:
						w.se([0, 18, 14][k]); w.se([0, 4, -5][k])
						w.se([0, 13, 20][k]); w.se([0, -2, 3][k])
		w.copy(r, len(rbsp) * 8 - r.p)
		rb = w.bytes()
		while rb and rb[-1] == 0:
			rb = rb[:-1]
		nal = nal[:1] + escape(rb)
	out += b'\x00\x00\x00\x01' + nal
open(sys.argv[2], 'wb').write(out)
