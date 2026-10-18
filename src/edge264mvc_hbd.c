/**
 * Reconstruction of pictures above 8 bits (9 and 10, as in the High 10 and
 * High 4:2:2 profiles), whose samples are 16 bits wide. The 8-bit routines are
 * vectorized for bytes, so each of them hands over to its counterpart here when
 * the bit depth is higher: plain C following the decoding processes of the
 * standard, for profiles where speed matters far less. The geometry is that of
 * 8 bits with strides and offsets in bytes, and the rows of Cb and Cr
 * alternating in the chroma planes.
 */
#include "edge264mvc_internal.h"

#define HBD_ROW(p, stride, y) ((uint16_t *)((uint8_t *)(p) + (ptrdiff_t)(y) * (ptrdiff_t)(stride)))



/**
 * Residual additions to a 4x4 block, from residuals r by rows, or from a
 * single residual for all samples.
 */
static void hbd_add4x4(uint8_t *p, size_t stride, const int16_t *r, int max) {
	for (int y = 0; y < 4; y++) {
		uint16_t *q = HBD_ROW(p, stride, y);
		for (int x = 0; x < 4; x++)
			q[x] = clip3(0, max, q[x] + r[y * 4 + x]);
	}
}

static void hbd_add_dc4x4(uint8_t *p, size_t stride, int r, int max) {
	for (int y = 0; y < 4; y++) {
		uint16_t *q = HBD_ROW(p, stride, y);
		for (int x = 0; x < 4; x++)
			q[x] = clip3(0, max, q[x] + r);
	}
}

/**
 * Inverse 8x8 transform (8.5.13.2) of the scaled coefficients d by rows, which
 * add_idct8x8 bounded so that no sum overflows, then addition to the samples.
 */
static void hbd_add_idct8x8(int32_t *d, uint8_t *p, size_t stride, int max) {
	for (int pass = 0; pass < 2; pass++) { // rows, then columns
		for (int i = 0; i < 8; i++) {
			int32_t *v = pass ? d + i : d + i * 8;
			int step = pass ? 8 : 1;
			int32_t v0 = v[0], v1 = v[step], v2 = v[step * 2], v3 = v[step * 3];
			int32_t v4 = v[step * 4], v5 = v[step * 5], v6 = v[step * 6], v7 = v[step * 7];
			int32_t a0 = v0 + v4, a4 = v0 - v4;
			int32_t a2 = (v2 >> 1) - v6, a6 = v2 + (v6 >> 1);
			int32_t b0 = a0 + a6, b2 = a4 + a2, b4 = a4 - a2, b6 = a0 - a6;
			int32_t a1 = -v3 + v5 - v7 - (v7 >> 1);
			int32_t a3 = v1 + v7 - v3 - (v3 >> 1);
			int32_t a5 = -v1 + v7 + v5 + (v5 >> 1);
			int32_t a7 = v3 + v5 + v1 + (v1 >> 1);
			int32_t b1 = a1 + (a7 >> 2), b7 = a7 - (a1 >> 2), b3 = a3 + (a5 >> 2), b5 = (a3 >> 2) - a5;
			v[0] = b0 + b7;
			v[step] = b2 + b5;
			v[step * 2] = b4 + b3;
			v[step * 3] = b6 + b1;
			v[step * 4] = b6 - b1;
			v[step * 5] = b4 - b3;
			v[step * 6] = b2 - b5;
			v[step * 7] = b0 - b7;
		}
	}
	for (int y = 0; y < 8; y++) {
		uint16_t *q = HBD_ROW(p, stride, y);
		for (int x = 0; x < 8; x++)
			q[x] = clip3(0, max, q[x] + ((d[y * 8 + x] + 32) >> 6));
	}
}



/**
 * Intra NxN prediction from the neighbouring samples top[-1..2N-1] and
 * left[-1..N-1] (both [-1] being p[-1, -1]), as in 8.3.1.2 (N = 4) and 8.3.2.2
 * (N = 8, from the filtered samples), where the modes are written alike.
 */
enum { HBD_V, HBD_H, HBD_DC, HBD_DDL, HBD_DDR, HBD_VR, HBD_HD, HBD_VL, HBD_HU };
static void hbd_predNxN(uint16_t *pred, int N, const int *top, const int *left, int mode, int availA, int availB, int max) {
	#define T(i) top[i]
	#define L(i) left[i]
	int last = 2 * N - 1, zmax = 2 * N - 3;
	int dc = (max + 1) >> 1;
	if (mode == HBD_DC) {
		int sumT = 0, sumL = 0;
		for (int i = 0; i < N; i++)
			sumT += T(i), sumL += L(i);
		int sh = N == 4 ? 2 : 3;
		dc = availA && availB ? (sumT + sumL + N) >> (sh + 1) : availB ? (sumT + (N >> 1)) >> sh : availA ? (sumL + (N >> 1)) >> sh : dc;
	}
	for (int y = 0; y < N; y++) {
		for (int x = 0; x < N; x++) {
			int v;
			switch (mode) {
			case HBD_V: v = T(x); break;
			case HBD_H: v = L(y); break;
			case HBD_DC: v = dc; break;
			case HBD_DDL:
				v = x == N - 1 && y == N - 1 ? (T(last - 1) + 3 * T(last) + 2) >> 2 : (T(x + y) + 2 * T(x + y + 1) + T(x + y + 2) + 2) >> 2;
				break;
			case HBD_DDR:
				v = x > y ? (T(x - y - 2) + 2 * T(x - y - 1) + T(x - y) + 2) >> 2 :
					x < y ? (L(y - x - 2) + 2 * L(y - x - 1) + L(y - x) + 2) >> 2 :
					(T(0) + 2 * T(-1) + L(0) + 2) >> 2;
				break;
			case HBD_VR: {
				int z = 2 * x - y;
				v = z >= 0 && !(z & 1) ? (T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 1) >> 1 :
					z > 0 ? (T(x - (y >> 1) - 2) + 2 * T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 2) >> 2 :
					z == -1 ? (L(0) + 2 * L(-1) + T(0) + 2) >> 2 :
					(L(y - 2 * x - 1) + 2 * L(y - 2 * x - 2) + L(y - 2 * x - 3) + 2) >> 2;
				} break;
			case HBD_HD: {
				int z = 2 * y - x;
				v = z >= 0 && !(z & 1) ? (L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 1) >> 1 :
					z > 0 ? (L(y - (x >> 1) - 2) + 2 * L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 2) >> 2 :
					z == -1 ? (L(0) + 2 * L(-1) + T(0) + 2) >> 2 :
					(T(x - 2 * y - 1) + 2 * T(x - 2 * y - 2) + T(x - 2 * y - 3) + 2) >> 2;
				} break;
			case HBD_VL:
				v = !(y & 1) ? (T(x + (y >> 1)) + T(x + (y >> 1) + 1) + 1) >> 1 :
					(T(x + (y >> 1)) + 2 * T(x + (y >> 1) + 1) + T(x + (y >> 1) + 2) + 2) >> 2;
				break;
			default: { // HBD_HU
				int z = x + 2 * y;
				v = z > zmax ? L(N - 1) :
					z == zmax ? (L(N - 2) + 3 * L(N - 1) + 2) >> 2 :
					!(z & 1) ? (L(y + (x >> 1)) + L(y + (x >> 1) + 1) + 1) >> 1 :
					(L(y + (x >> 1)) + 2 * L(y + (x >> 1) + 1) + L(y + (x >> 1) + 2) + 2) >> 2;
				} break;
			}
			pred[y * N + x] = v;
		}
	}
	#undef T
	#undef L
}

/**
 * Each mode of enum Intra4x4Modes and Intra8x8Modes as {prediction, then the
 * availability of A, B, C and D}, where only the neighbours that the mode
 * reads count (a variant of the enum exists wherever their absence matters).
 */
static const int8_t hbd_modes4x4[][5] = {
	[I4x4_V_8] = {HBD_V, 0, 1, 0, 0}, [I4x4_H_8] = {HBD_H, 1, 0, 0, 0},
	[I4x4_DC_8] = {HBD_DC, 1, 1, 0, 0}, [I4x4_DC_A_8] = {HBD_DC, 0, 1, 0, 0},
	[I4x4_DC_B_8] = {HBD_DC, 1, 0, 0, 0}, [I4x4_DC_AB_8] = {HBD_DC, 0, 0, 0, 0},
	[I4x4_DDL_8] = {HBD_DDL, 0, 1, 1, 0}, [I4x4_DDL_C_8] = {HBD_DDL, 0, 1, 0, 0},
	[I4x4_DDR_8] = {HBD_DDR, 1, 1, 0, 1}, [I4x4_VR_8] = {HBD_VR, 1, 1, 0, 1},
	[I4x4_HD_8] = {HBD_HD, 1, 1, 0, 1}, [I4x4_VL_8] = {HBD_VL, 0, 1, 1, 0},
	[I4x4_VL_C_8] = {HBD_VL, 0, 1, 0, 0}, [I4x4_HU_8] = {HBD_HU, 1, 0, 0, 0},
};
static const int8_t hbd_modes8x8[][5] = {
	[I8x8_V_8] = {HBD_V, 0, 1, 1, 1}, [I8x8_V_C_8] = {HBD_V, 0, 1, 0, 1},
	[I8x8_V_D_8] = {HBD_V, 0, 1, 1, 0}, [I8x8_V_CD_8] = {HBD_V, 0, 1, 0, 0},
	[I8x8_H_8] = {HBD_H, 1, 0, 0, 1}, [I8x8_H_D_8] = {HBD_H, 1, 0, 0, 0},
	[I8x8_DC_8] = {HBD_DC, 1, 1, 1, 1}, [I8x8_DC_A_8] = {HBD_DC, 0, 1, 1, 1},
	[I8x8_DC_AC_8] = {HBD_DC, 0, 1, 0, 1}, [I8x8_DC_AD_8] = {HBD_DC, 0, 1, 1, 0},
	[I8x8_DC_ACD_8] = {HBD_DC, 0, 1, 0, 0}, [I8x8_DC_B_8] = {HBD_DC, 1, 0, 0, 1},
	[I8x8_DC_BD_8] = {HBD_DC, 1, 0, 0, 0}, [I8x8_DC_C_8] = {HBD_DC, 1, 1, 0, 1},
	[I8x8_DC_D_8] = {HBD_DC, 1, 1, 1, 0}, [I8x8_DC_CD_8] = {HBD_DC, 1, 1, 0, 0},
	[I8x8_DC_AB_8] = {HBD_DC, 0, 0, 0, 0},
	[I8x8_DDL_8] = {HBD_DDL, 0, 1, 1, 1}, [I8x8_DDL_C_8] = {HBD_DDL, 0, 1, 0, 1},
	[I8x8_DDL_D_8] = {HBD_DDL, 0, 1, 1, 0}, [I8x8_DDL_CD_8] = {HBD_DDL, 0, 1, 0, 0},
	[I8x8_DDR_8] = {HBD_DDR, 1, 1, 1, 1}, [I8x8_DDR_C_8] = {HBD_DDR, 1, 1, 0, 1},
	[I8x8_VR_8] = {HBD_VR, 1, 1, 1, 1}, [I8x8_VR_C_8] = {HBD_VR, 1, 1, 0, 1},
	[I8x8_HD_8] = {HBD_HD, 1, 1, 0, 1},
	[I8x8_VL_8] = {HBD_VL, 0, 1, 1, 1}, [I8x8_VL_C_8] = {HBD_VL, 0, 1, 0, 1},
	[I8x8_VL_D_8] = {HBD_VL, 0, 1, 1, 0}, [I8x8_VL_CD_8] = {HBD_VL, 0, 1, 0, 0},
	[I8x8_HU_8] = {HBD_HU, 1, 0, 0, 1}, [I8x8_HU_D_8] = {HBD_HU, 1, 0, 0, 0},
};

static void hbd_intra4x4(uint8_t *p, size_t stride, int mode, int max) {
	const int8_t *m = hbd_modes4x4[mode];
	int t[9] = {}, l[5] = {}, *top = t + 1, *left = l + 1;
	const uint16_t *above = HBD_ROW(p, stride, -1);
	if (m[2]) {
		for (int i = 0; i < 8; i++) // p[x, -1] with x = 4..7 substituted by p[3, -1] without C (8.3.1.2)
			top[i] = above[i < 4 || m[3] ? i : 3];
	}
	if (m[1]) {
		for (int i = 0; i < 4; i++)
			left[i] = HBD_ROW(p, stride, i)[-1];
	}
	if (m[4])
		top[-1] = left[-1] = above[-1];
	uint16_t pred[16];
	hbd_predNxN(pred, 4, top, left, m[0], m[1], m[2], max);
	for (int y = 0; y < 4; y++)
		memcpy(HBD_ROW(p, stride, y), pred + y * 4, 8);
}

/**
 * Reference sample filtering of Intra_8x8 (8.3.2.2.1), then prediction.
 */
static void hbd_intra8x8(uint8_t *p, size_t stride, int mode, int max) {
	const int8_t *m = hbd_modes8x8[mode];
	int availA = m[1], availB = m[2], availC = m[3], availD = m[4];
	int t[17] = {}, l[9] = {}, *top = t + 1, *left = l + 1; // unfiltered
	const uint16_t *above = HBD_ROW(p, stride, -1);
	if (availB) {
		for (int i = 0; i < 16; i++)
			top[i] = above[i < 8 || availC ? i : 7];
	}
	if (availA) {
		for (int i = 0; i < 8; i++)
			left[i] = HBD_ROW(p, stride, i)[-1];
	}
	if (availD)
		top[-1] = left[-1] = above[-1];
	int ft[17] = {}, fl[9] = {}, *ftop = ft + 1, *fleft = fl + 1;
	if (availB) {
		ftop[0] = availD ? (top[-1] + 2 * top[0] + top[1] + 2) >> 2 : (3 * top[0] + top[1] + 2) >> 2;
		for (int x = 1; x < 15; x++)
			ftop[x] = (top[x - 1] + 2 * top[x] + top[x + 1] + 2) >> 2;
		ftop[15] = (top[14] + 3 * top[15] + 2) >> 2;
	}
	if (availD) {
		ftop[-1] = fleft[-1] = availB && availA ? (top[0] + 2 * top[-1] + left[0] + 2) >> 2 :
			availB ? (3 * top[-1] + top[0] + 2) >> 2 : availA ? (3 * top[-1] + left[0] + 2) >> 2 : top[-1];
	}
	if (availA) {
		fleft[0] = availD ? (left[-1] + 2 * left[0] + left[1] + 2) >> 2 : (3 * left[0] + left[1] + 2) >> 2;
		for (int y = 1; y < 7; y++)
			fleft[y] = (left[y - 1] + 2 * left[y] + left[y + 1] + 2) >> 2;
		fleft[7] = (left[6] + 3 * left[7] + 2) >> 2;
	}
	uint16_t pred[64];
	hbd_predNxN(pred, 8, ftop, fleft, m[0], availA, availB, max);
	for (int y = 0; y < 8; y++)
		memcpy(HBD_ROW(p, stride, y), pred + y * 8, 16);
}

/**
 * Intra 16x16 prediction (8.3.3), with I16x16_DC_A_8 lacking A and
 * I16x16_DC_B_8 lacking B.
 */
static void hbd_intra16x16(uint8_t *p, size_t stride, int mode, int max) {
	const uint16_t *above = HBD_ROW(p, stride, -1);
	#define LEFT(y) HBD_ROW(p, stride, y)[-1]
	int dc = (max + 1) >> 1;
	int H = 0, V = 0, a = 0, b = 0, c = 0;
	if (mode == I16x16_P_8) { // (8-122 to 8-127)
		for (int i = 0; i < 8; i++) {
			H += (i + 1) * (above[8 + i] - above[6 - i]);
			V += (i + 1) * (LEFT(8 + i) - LEFT(6 - i));
		}
		a = 16 * (LEFT(15) + above[15]), b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
	} else if (mode != I16x16_V_8 && mode != I16x16_H_8) {
		int availA = mode == I16x16_DC_8 || mode == I16x16_DC_B_8, availB = mode == I16x16_DC_8 || mode == I16x16_DC_A_8;
		int sumT = 0, sumL = 0;
		for (int i = 0; i < 16; i++) {
			sumT += availB ? above[i] : 0;
			sumL += availA ? LEFT(i) : 0;
		}
		dc = availA && availB ? (sumT + sumL + 16) >> 5 : availB ? (sumT + 8) >> 4 : availA ? (sumL + 8) >> 4 : dc;
	}
	for (int y = 0; y < 16; y++) {
		uint16_t *q = HBD_ROW(p, stride, y);
		int l = mode == I16x16_H_8 ? LEFT(y) : 0;
		for (int x = 0; x < 16; x++) {
			q[x] = mode == I16x16_V_8 ? above[x] : mode == I16x16_H_8 ? l : mode == I16x16_P_8 ?
				clip3(0, max, (a + b * (x - 7) + c * (y - 7) + 16) >> 5) : dc;
		}
	}
	#undef LEFT
}

/**
 * Intra chroma prediction (8.3.4) of both components, 8 wide and h high (8 for
 * 4:2:0, 16 for 4:2:2), with the rows of a component at row and Cr at cr from Cb.
 */
static void hbd_intraChroma(uint8_t *p, size_t cr, size_t row, int mode, int max, int h) {
	int availA = mode != IC8x8_DC_A_8 && mode != IC8x8_DC_AB_8;
	int availB = mode != IC8x8_DC_B_8 && mode != IC8x8_DC_AB_8;
	int yCF = h == 16 ? 4 : 0;
	for (int iCbCr = 0; iCbCr < 2; iCbCr++) {
		uint8_t *s = p + iCbCr * cr; // row y of the component at s + y * row
		const uint16_t *top = HBD_ROW(s, row, -1);
		#define LEFT(y) HBD_ROW(s, row, y)[-1]
		if (mode == IC8x8_H_8 || mode == IC8x8_V_8) {
			for (int y = 0; y < h; y++) {
				uint16_t *q = HBD_ROW(s, row, y);
				for (int x = 0; x < 8; x++)
					q[x] = mode == IC8x8_V_8 ? top[x] : LEFT(y);
			}
		} else if (mode == IC8x8_P_8) { // (8-144 to 8-149) with xCF = 0
			int H = 0, V = 0;
			for (int x = 0; x < 4; x++)
				H += (x + 1) * (top[4 + x] - top[2 - x]);
			for (int y = 0; y < 4 + yCF; y++)
				V += (y + 1) * (LEFT(4 + yCF + y) - LEFT(2 + yCF - y)); // LEFT(-1) is p[-1, -1]
			int a = 16 * (LEFT(h - 1) + top[7]);
			int b = (34 * H + 32) >> 6;
			int c = ((34 - 29 * (h == 16)) * V + 32) >> 6;
			for (int y = 0; y < h; y++) {
				uint16_t *q = HBD_ROW(s, row, y);
				for (int x = 0; x < 8; x++)
					q[x] = clip3(0, max, (a + b * (x - 3) + c * (y - 3 - yCF) + 16) >> 5);
			}
		} else { // DC of each 4x4 block from the neighbours of its row or column (8.3.4.1 to 8.3.4.3)
			for (int blk = 0; blk < h / 2; blk++) {
				int xO = (blk & 1) * 4, yO = (blk >> 1) * 4;
				int sumT = availB ? top[xO] + top[xO + 1] + top[xO + 2] + top[xO + 3] : 0;
				int sumL = availA ? LEFT(yO) + LEFT(yO + 1) + LEFT(yO + 2) + LEFT(yO + 3) : 0;
				int dc = (max + 1) >> 1;
				if ((xO == 0) == (yO == 0))
					dc = availA && availB ? (sumT + sumL + 4) >> 3 : availA ? (sumL + 2) >> 2 : availB ? (sumT + 2) >> 2 : dc;
				else if (xO > 0)
					dc = availB ? (sumT + 2) >> 2 : availA ? (sumL + 2) >> 2 : dc;
				else
					dc = availA ? (sumL + 2) >> 2 : availB ? (sumT + 2) >> 2 : dc;
				for (int y = yO; y < yO + 4; y++) {
					uint16_t *q = HBD_ROW(s, row, y);
					for (int x = xO; x < xO + 4; x++)
						q[x] = dc;
				}
			}
		}
		#undef LEFT
	}
}



/**
 * Inter prediction (8.4.2.2) and weighted prediction (8.4.2.3) of one
 * partition, with the choices of decode_inter, which calls it once the
 * reference rows are final: the first list of a bipredicted partition writes
 * its prediction alone, which the second list then blends in.
 */
static noinline void hbd_decode_inter(Edge264MvcContext *ctx, int i, int w, int h) {
	int mvx = mb->mvs[i * 2], mvy = mb->mvs[i * 2 + 1];
	int i8x8 = i >> 2, i4x4 = i & 15;
	const uint8_t *ref = ctx->t.samples_buffers[mb->refPic[i8x8]];
	int c422 = ctx->t.ChromaArrayType == 2;
	int W = ctx->t.pic_width_in_mbs * 16, Hgt = ctx->t.pic_height_in_mbs * 16;
	int xAL = ctx->mbx * 16 + x444[i4x4], yAL = ctx->mby * 16 + y444[i4x4];
	int bd8 = __builtin_ctz(ctx->t.samples_clip[0][0] + 1) - 8; // BitDepth - 8, equal for luma and chroma

	// blend 0 = copy, 1 = average, 2 = weighted single list, 3 = weighted pair
	int refIdx = mb->refIdx[i8x8], refIdxX = mb->refIdx[i8x8 ^ 4];
	int blend = 0, logWD[3] = {}, w0[3] = {}, w1[3] = {}, o0[3] = {}, o1[3] = {};
	int pair = refIdxX >= 0, second = i8x8 >= 4;
	if (ctx->t.pps.weighted_bipred_idc != 1) {
		if (pair && second) {
			if (ctx->t.pps.weighted_bipred_idc == 0) {
				blend = 1;
			} else { // implicit (8-277, 8-282, 8-283)
				int W1 = ctx->implicit_weights[refIdxX][refIdx] - 64;
				blend = 3;
				for (int c = 0; c < 3; c++)
					logWD[c] = 5, w0[c] = 64 - W1, w1[c] = W1;
			}
		}
	} else if (!pair) { // explicit, single list, with the offsets scaled to the bit depth (8-291, 8-296)
		int r = refIdx + (i8x8 & 4) * 8;
		blend = 2;
		for (int c = 0; c < 3; c++) {
			logWD[c] = c ? ctx->t.chroma_log2_weight_denom : ctx->t.luma_log2_weight_denom;
			w0[c] = ctx->t.explicit_weights[c][r];
			o0[c] = ctx->t.explicit_offsets[c][r] * (1 << bd8);
		}
	} else if (second) { // explicit, pair
		blend = 3;
		for (int c = 0; c < 3; c++) {
			logWD[c] = c ? ctx->t.chroma_log2_weight_denom : ctx->t.luma_log2_weight_denom;
			w0[c] = ctx->t.explicit_weights[c][refIdxX];
			w1[c] = ctx->t.explicit_weights[c][refIdx + 32];
			o0[c] = ctx->t.explicit_offsets[c][refIdxX] * (1 << bd8);
			o1[c] = ctx->t.explicit_offsets[c][refIdx + 32] * (1 << bd8);
		}
	}
	#define BLEND(q, v, c, max) (blend == 0 ? (v) : blend == 1 ? ((q) + (v) + 1) >> 1 : blend == 2 ?\
		clip3(0, max, (logWD[c] >= 1 ? ((v) * w0[c] + (1 << (logWD[c] - 1))) >> logWD[c] : (v) * w0[c]) + o0[c]) :\
		clip3(0, max, (((q) * w0[c] + (v) * w1[c] + (1 << logWD[c])) >> (logWD[c] + 1)) + ((o0[c] + o1[c] + 1) >> 1)))

	// luma (8.4.2.2.1), from the positions clamped into the picture
	int maxY = ctx->t.samples_clip[0][0];
	size_t sY = ctx->t.stride[0];
	#define REFY(x, y) HBD_ROW(ref, sY, clip3(0, Hgt - 1, y))[clip3(0, W - 1, x)]
	#define TAP6(a, b, c, d, e, f) ((a) - 5 * (b) + 20 * (c) + 20 * (d) - 5 * (e) + (f))
	#define B1(x, y) TAP6(REFY((x) - 2, y), REFY((x) - 1, y), REFY(x, y), REFY((x) + 1, y), REFY((x) + 2, y), REFY((x) + 3, y))
	#define H1(x, y) TAP6(REFY(x, (y) - 2), REFY(x, (y) - 1), REFY(x, y), REFY(x, (y) + 1), REFY(x, (y) + 2), REFY(x, (y) + 3))
	int frac = (mvy & 3) * 4 + (mvx & 3);
	int need_b = 0xee & 1 << frac, need_h = 0x3330 & 1 << frac, need_j = 0x4e40 & 1 << frac;
	int need_s = 0xe000 & 1 << frac, need_m = 0x8880 & 1 << frac;
	uint8_t *dstY = ctx->samples_mb[0] + y444[i4x4] * sY + (x444[i4x4] << 1);
	for (int y = 0; y < h; y++) {
		uint16_t *q = HBD_ROW(dstY, sY, y);
		for (int x = 0; x < w; x++) {
			int xI = xAL + (mvx >> 2) + x, yI = yAL + (mvy >> 2) + y;
			int G = REFY(xI, yI), b = 0, hh = 0, j = 0, s = 0, m = 0, v;
			if (need_b)
				b = clip3(0, maxY, (B1(xI, yI) + 16) >> 5);
			if (need_h)
				hh = clip3(0, maxY, (H1(xI, yI) + 16) >> 5);
			if (need_j)
				j = clip3(0, maxY, (TAP6(B1(xI, yI - 2), B1(xI, yI - 1), B1(xI, yI), B1(xI, yI + 1), B1(xI, yI + 2), B1(xI, yI + 3)) + 512) >> 10);
			if (need_s)
				s = clip3(0, maxY, (B1(xI, yI + 1) + 16) >> 5);
			if (need_m)
				m = clip3(0, maxY, (H1(xI + 1, yI) + 16) >> 5);
			switch (frac) { // Table 8-12, by yFracL * 4 + xFracL
			case 0: v = G; break;
			case 1: v = (G + b + 1) >> 1; break;
			case 2: v = b; break;
			case 3: v = (b + REFY(xI + 1, yI) + 1) >> 1; break;
			case 4: v = (G + hh + 1) >> 1; break;
			case 5: v = (b + hh + 1) >> 1; break;
			case 6: v = (b + j + 1) >> 1; break;
			case 7: v = (b + m + 1) >> 1; break;
			case 8: v = hh; break;
			case 9: v = (hh + j + 1) >> 1; break;
			case 10: v = j; break;
			case 11: v = (j + m + 1) >> 1; break;
			case 12: v = (hh + REFY(xI, yI + 1) + 1) >> 1; break;
			case 13: v = (hh + s + 1) >> 1; break;
			case 14: v = (j + s + 1) >> 1; break;
			default: v = (s + m + 1) >> 1; break;
			}
			q[x] = BLEND(q[x], v, 0, maxY);
		}
	}
	#undef REFY
	#undef TAP6
	#undef B1
	#undef H1

	// chroma (8.4.2.2.2), with mvCLX equal to mvLX in frames (8.4.1.4)
	if (ctx->t.ChromaArrayType == 0)
		return;
	int maxC = ctx->t.samples_clip[1][0];
	size_t sC = ctx->t.stride_Cr, row = ctx->t.stride[1]; // from a Cb row to the Cr row of the same y, and to the next row
	int WC = W >> 1, HC = c422 ? Hgt : Hgt >> 1;
	int wC = w >> 1, hC = c422 ? h : h >> 1;
	// in 4:2:0 fields, a reference field of the other parity has its chroma
	// samples a quarter sample off vertically (Table 8-10)
	int mvCy = ctx->t.field_pic_flag && !c422 ? mvy + (ctx->t.bottom_field_flag - (mb->refPic[i8x8] >= FIELD_ID)) * 2 : mvy;
	int xIntC = (xAL >> 1) + (mvx >> 3), xFracC = mvx & 7;
	int yIntC = c422 ? yAL + (mvy >> 2) : (yAL >> 1) + (mvCy >> 3);
	int yFracC = c422 ? (mvy & 3) << 1 : mvCy & 7;
	const uint8_t *refC = ref + PLANE_C(&ctx->t, mb->refPic[i8x8]);
	uint8_t *dstC = ctx->samples_mb[1] + (y444[i4x4] >> !c422) * ctx->t.stride[1] + (x444[i4x4] >> 1 << 1);
	for (int iCbCr = 0; iCbCr < 2; iCbCr++) {
		#define REFC(x, y) HBD_ROW(refC + iCbCr * sC, row, clip3(0, HC - 1, y))[clip3(0, WC - 1, x)]
		for (int y = 0; y < hC; y++) {
			uint16_t *q = HBD_ROW(dstC + iCbCr * sC, row, y);
			for (int x = 0; x < wC; x++) {
				int xI = xIntC + x, yI = yIntC + y;
				int v = ((8 - xFracC) * (8 - yFracC) * REFC(xI, yI) + xFracC * (8 - yFracC) * REFC(xI + 1, yI) +
					(8 - xFracC) * yFracC * REFC(xI, yI + 1) + xFracC * yFracC * REFC(xI + 1, yI + 1) + 32) >> 6; // (8-270)
				q[x] = BLEND(q[x], v, 1 + iCbCr, maxC);
			}
		}
		#undef REFC
	}
	#undef BLEND
}



/**
 * Deblocking of a macroblock (8.7), with the alpha, beta and tC0 that
 * deblock_mb looked up in the tables of 8 bits, which scale by
 * 1 << (BitDepth - 8) (8-456 to 8-462).
 */
static void hbd_filter(uint16_t *s, ptrdiff_t across, int bS4, int tC0, int alpha, int beta, int chroma, int max) {
	int p0 = s[-across], p1 = s[-2 * across], q0 = s[0], q1 = s[across];
	if (abs(p0 - q0) >= alpha || abs(p1 - p0) >= beta || abs(q1 - q0) >= beta) // (8-460)
		return;
	if (chroma) {
		if (bS4) { // (8.7.2.4)
			s[-across] = (2 * p1 + p0 + q1 + 2) >> 2;
			s[0] = (2 * q1 + q0 + p1 + 2) >> 2;
		} else { // (8-466 to 8-469)
			int tC = tC0 + 1;
			int delta = clip3(-tC, tC, ((q0 - p0) * 4 + p1 - q1 + 4) >> 3);
			s[-across] = clip3(0, max, p0 + delta);
			s[0] = clip3(0, max, q0 - delta);
		}
		return;
	}
	int p2 = s[-3 * across], q2 = s[2 * across];
	int ap = abs(p2 - p0) < beta, aq = abs(q2 - q0) < beta;
	if (bS4) { // (8-476 to 8-489)
		int p3 = s[-4 * across], q3 = s[3 * across];
		int strong = abs(p0 - q0) < (alpha >> 2) + 2;
		if (ap && strong) {
			s[-across] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
			s[-2 * across] = (p2 + p1 + p0 + q0 + 2) >> 2;
			s[-3 * across] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
		} else {
			s[-across] = (2 * p1 + p0 + q1 + 2) >> 2;
		}
		if (aq && strong) {
			s[0] = (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3;
			s[across] = (p0 + q0 + q1 + q2 + 2) >> 2;
			s[2 * across] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
		} else {
			s[0] = (2 * q1 + q0 + p1 + 2) >> 2;
		}
	} else { // (8-465 to 8-475)
		int tC = tC0 + ap + aq;
		int delta = clip3(-tC, tC, ((q0 - p0) * 4 + p1 - q1 + 4) >> 3);
		s[-across] = clip3(0, max, p0 + delta);
		s[0] = clip3(0, max, q0 - delta);
		if (ap)
			s[-2 * across] = p1 + clip3(-tC0, tC0, (p2 + ((p0 + q0 + 1) >> 1) - 2 * p1) >> 1);
		if (aq)
			s[across] = q1 + clip3(-tC0, tC0, (q2 + ((p0 + q0 + 1) >> 1) - 2 * q1) >> 1);
	}
}

static noinline void hbd_deblock_mb(Edge264MvcContext *ctx) {
	int bd8 = __builtin_ctz(ctx->t.samples_clip[0][0] + 1) - 8;
	for (int plane = 0; plane < (ctx->t.ChromaArrayType ? 3 : 1); plane++) {
		int chroma = plane > 0;
		int max = ctx->t.samples_clip[plane][0];
		size_t stride = ctx->t.stride[chroma]; // from a row to the next of the same component
		uint8_t *base = ctx->samples_mb[plane];
		int h = chroma ? ctx->t.mb_height_C : 16;
		for (int dir = 0; dir < 2; dir++) { // vertical edges, then horizontal ones
			int n_edges = !chroma ? 4 : dir == 0 ? 2 : h / 4;
			for (int e = 0; e < n_edges; e++) {
				int bS4 = 0, idx = plane; // internal edges
				if (e == 0) {
					if (!(mb->filter_edges & (1 << dir)))
						continue;
					idx = (dir ? 12 : 8) + plane;
					// the horizontal edges of field pictures take bS 3 (8.7.2.1)
					bS4 = !((dir ? mbB : mbA)->mbIsInterFlag & mb->mbIsInterFlag) && !(dir & ctx->t.field_pic_flag);
				} else if (!chroma && (e & 1) && mb->f.transform_size_8x8_flag) {
					continue;
				}
				int alpha = ctx->alpha[idx] << bd8, beta = ctx->beta[idx] << bd8;

				// tC0 for each 4 luma samples of the edge (see deblock_mb), the
				// chroma edges taking those of the luma edges at the same positions
				const int8_t *tC0;
				if (!chroma)
					tC0 = (const int8_t *)&ctx->tC0_s[dir * 4 + e];
				else if (dir == 0)
					tC0 = (const int8_t *)&ctx->tC0_l[4 + e] + (plane - 1) * 4;
				else if (h == 16) // 4:2:2, at the luma edges e, f, g and h
					tC0 = (const int8_t *)(e & 1 ? &ctx->tC0_fh_l[e >> 1] : &ctx->tC0_l[6 + (e >> 1)]) + (plane - 1) * 4;
				else
					tC0 = (const int8_t *)&ctx->tC0_l[6 + e] + (plane - 1) * 4;
				int len = dir ? (chroma ? 8 : 16) : h;
				for (int k = 0; k < len; k++) {
					int t = bS4 ? 0 : tC0[!chroma ? k >> 2 : dir ? k >> 1 : k / (h / 4)]; // unused by bS 4
					if (t < 0) // bS 0
						continue;
					uint16_t *s = dir ? HBD_ROW(base, stride, e * 4) + k : HBD_ROW(base, stride, k) + e * 4;
					hbd_filter(s, dir ? (ptrdiff_t)stride / 2 : 1, bS4, t << bd8, alpha, beta, chroma, max);
				}
			}
		}
	}
	mb->filter_edges = 0; // prevent redundant deblocking with deblock_idc==2 and ASO
}
