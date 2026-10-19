#include "edge264mvc_internal.h"

// This file is compiled twice: once for CAVLC and once for CABAC
#undef CAFUNC
#undef CACALL
#undef CAJUMP
#undef CACOND
#ifndef CABAC
	#define CABAC 0
#endif
#if !CABAC
	#define CAFUNC(f, ...) f ## _cavlc(Edge264MvcContext *ctx, ## __VA_ARGS__)
	#define CACALL(f, ...) f ## _cavlc(ctx, ## __VA_ARGS__)
	#define CAJUMP(f, ...) { f ## _cavlc(ctx, ## __VA_ARGS__); return; }
	#define CACOND(cavlc, cabac) cavlc
#else
	#define CAFUNC(f, ...) f ## _cabac(Edge264MvcContext *ctx, ## __VA_ARGS__)
	#define CACALL(f, ...) f ## _cabac(ctx, ## __VA_ARGS__)
	#define CAJUMP(f, ...) { f ## _cabac(ctx, ## __VA_ARGS__); return; }
	#define CACOND(cavlc, cabac) cabac
	
	static const i8x16 sig_inc_8x8[2][4] = {{
		{ 0,  1,  2,  3,  4,  5,  5,  4,  4,  3,  3,  4,  4,  4,  5,  5},
		{ 4,  4,  4,  4,  3,  3,  6,  7,  7,  7,  8,  9, 10,  9,  8,  7},
		{ 7,  6, 11, 12, 13, 11,  6,  7,  8,  9, 14, 10,  9,  8,  6, 11},
		{12, 13, 11,  6,  9, 14, 10,  9, 11, 12, 13, 11, 14, 10, 12,  0},
		}, {
		{ 0,  1,  1,  2,  2,  3,  3,  4,  5,  6,  7,  7,  7,  8,  4,  5},
		{ 6,  9, 10, 10,  8, 11, 12, 11,  9,  9, 10, 10,  8, 11, 12, 11},
		{ 9,  9, 10, 10,  8, 11, 12, 11,  9,  9, 10, 10,  8, 13, 13,  9},
		{ 9, 10, 10,  8, 13, 13,  9,  9, 10, 10, 14, 14, 14, 14, 14,  0},
	}};
	static const i8x16 last_inc_8x8[4] = {
		{0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
		{2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
		{3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4},
		{5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8},
	};
	static const i8x8 sig_inc_chromaDC_422 = {0, 0, 1, 1, 2, 2, 2};
	
	static const i16x4 ctxIdxOffsets_16x16DC[3][2] = {
		{{85, 105, 166, 227}, {85, 277, 338, 227}}, // ctxBlockCat==0
		{{460, 484, 572, 952}, {460, 776, 864, 952}}, // ctxBlockCat==6
		{{472, 528, 616, 982}, {472, 820, 908, 982}}, // ctxBlockCat==10
	};
	static const i16x4 ctxIdxOffsets_16x16AC[3][2] = {
		{{89, 119, 180, 237}, {89, 291, 352, 237}}, // ctxBlockCat==1
		{{464, 498, 586, 962}, {464, 790, 878, 962}}, // ctxBlockCat==7
		{{476, 542, 630, 992}, {476, 834, 922, 992}}, // ctxBlockCat==11
	};
	static const i16x4 ctxIdxOffsets_chromaDC[2] =
		{{97, 149, 210, 257}, {97, 321, 382, 257}}; // ctxBlockCat==3
	static const i16x4 ctxIdxOffsets_chromaAC[2] =
		{{101, 151, 212, 266}, {101, 323, 384, 266}}; // ctxBlockCat==4
	static const i16x4 ctxIdxOffsets_4x4[3][2] = {
		{{93, 134, 195, 247}, {93, 306, 367, 247}}, // ctxBlockCat==2
		{{468, 528, 616, 972}, {468, 805, 893, 972}}, // ctxBlockCat==8
		{{480, 557, 645, 1002}, {480, 849, 937, 1002}}, // ctxBlockCat==12
	};
	static const i16x4 ctxIdxOffsets_8x8[3][2] = {
		{{1012, 402, 417, 426}, {1012, 436, 451, 426}}, // ctxBlockCat==5
		{{1016, 660, 690, 708}, {1016, 675, 699, 708}}, // ctxBlockCat==9
		{{1020, 718, 748, 766}, {1020, 733, 757, 766}}, // ctxBlockCat==13
	};
#endif



/**
 * Parse a CAVLC residual block (9.2).
 * 
 * There are 2 entry points here:
 * _ parse_residual_block_4x4_cavlc which mirrors signature of
 *   parse_residual_coeffs_cavlc to tail jump to it
 * _ parse_residual_block_2x2_cavlc which is inlined but contains a call to
 *   parse_residual_coeffs_cavlc
 * 
 * For coeff_token/total_zeros/run_before, while the spec is quite convoluted
 * level_prefix+level_suffix just look like Exp-Golomb codes, so we need only
 * compute a code length (v) and an offset to add to the input bits.
 */
#if !CABAC
	static inline int parse_total_zeros(Edge264MvcContext *ctx, int endIdx, int TotalCoeff) {
		static const uint8_t codes[27][9 * 4] = { // [tzVlcIndex][leadingZeroBits][suffix]
			// 2x2 blocks
			{16, 16, 16, 16, 33, 33, 33, 33, 50, 50, 50, 50, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51},
			{16, 16, 16, 16, 33, 33, 33, 33, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34},
			{16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17},
			{}, // 2x4 blocks
			{16, 16, 16, 16, 49, 49, 50, 50, 67, 67, 68, 68, 69, 69, 69, 69, 86, 86, 86, 86, 87, 87, 87, 87, 87, 87, 87, 87, 87, 87, 87, 87, 87, 87, 87, 87},
			{51, 52, 53, 54, 33, 33, 33, 33, 50, 50, 50, 50, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48},
			{35, 35, 52, 53, 34, 34, 34, 34, 49, 49, 49, 49, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48},
			{35, 35, 48, 52, 34, 34, 34, 34, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33},
			{34, 34, 35, 35, 33, 33, 33, 33, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32},
			{18, 18, 18, 18, 33, 33, 33, 33, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32},
			{17, 17, 17, 17, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16},
			{}, // 4x4 blocks
			{16, 16, 16, 16, 50, 50, 49, 49, 68, 68, 67, 67, 86, 86, 85, 85, 104, 104, 103, 103, 122, 122, 121, 121, 140, 140, 139, 139, 158, 158, 157, 157, 159, 159, 159, 159},
			{51, 50, 49, 48, 70, 69, 52, 52, 72, 72, 71, 71, 90, 90, 89, 89, 108, 108, 107, 107, 109, 109, 109, 109, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110, 110},
			{54, 51, 50, 49, 68, 64, 55, 55, 72, 72, 69, 69, 90, 90, 89, 89, 92, 92, 92, 92, 107, 107, 107, 107, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109, 109},
			{54, 53, 52, 49, 67, 66, 56, 56, 73, 73, 71, 71, 90, 90, 80, 80, 91, 91, 91, 91, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92, 92},
			{54, 53, 52, 51, 65, 64, 55, 55, 72, 72, 66, 66, 74, 74, 74, 74, 89, 89, 89, 89, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91, 91},
			{53, 52, 51, 50, 55, 55, 54, 54, 57, 57, 57, 57, 72, 72, 72, 72, 81, 81, 81, 81, 96, 96, 96, 96, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106, 106},
			{51, 50, 37, 37, 54, 54, 52, 52, 56, 56, 56, 56, 71, 71, 71, 71, 81, 81, 81, 81, 96, 96, 96, 96, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 105},
			{37, 37, 36, 36, 54, 54, 51, 51, 55, 55, 55, 55, 65, 65, 65, 65, 82, 82, 82, 82, 96, 96, 96, 96, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104},
			{36, 36, 35, 35, 38, 38, 38, 38, 53, 53, 53, 53, 66, 66, 66, 66, 87, 87, 87, 87, 96, 96, 96, 96, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97, 97},
			{36, 36, 35, 35, 37, 37, 37, 37, 50, 50, 50, 50, 70, 70, 70, 70, 80, 80, 80, 80, 81, 81, 81, 81, 81, 81, 81, 81, 81, 81, 81, 81, 81, 81, 81, 81},
			{20, 20, 20, 20, 51, 51, 53, 53, 50, 50, 50, 50, 65, 65, 65, 65, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64},
			{19, 19, 19, 19, 34, 34, 34, 34, 52, 52, 52, 52, 65, 65, 65, 65, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64},
			{18, 18, 18, 18, 35, 35, 35, 35, 49, 49, 49, 49, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48},
			{18, 18, 18, 18, 33, 33, 33, 33, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32},
			{17, 17, 17, 17, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16},
		};
		int leadingZeroBits = clz(ctx->t.gb.msb_cache | (size_t)1 << (SIZE_BIT - 9));
		int suffix = ctx->t.gb.msb_cache >> ((leadingZeroBits + 2) ^ (SIZE_BIT - 1)) & 3;
		assert(endIdx + TotalCoeff - 4 < 27 && leadingZeroBits * 4 + suffix < 36);
		int code = codes[endIdx + TotalCoeff - 4][leadingZeroBits * 4 + suffix];
		int v = code >> 4;
		ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, v);
		ctx->t.gb.lsb_cache <<= v;
		return code & 15;
	}
	
	static noinline int parse_residual_coeffs_cavlc(Edge264MvcContext *ctx, int startIdx, int endIdx, int TrailingOnes, int TotalCoeff) {
		// parse all level values from end to start
		int32_t level[16];
		size_t signs = ~ctx->t.gb.msb_cache;
		level[0] = (signs >> (SIZE_BIT - 2) & 2) - 1;
		level[1] = (signs >> (SIZE_BIT - 3) & 2) - 1;
		level[2] = (signs >> (SIZE_BIT - 4) & 2) - 1;
		ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, TrailingOnes);
		ctx->t.gb.lsb_cache <<= TrailingOnes;
		for (int i = TrailingOnes, suffixLength = 1, v, offset; i < TotalCoeff; i++) {
			int level_prefix = clz(ctx->t.gb.msb_cache | (size_t)1 << (SIZE_BIT - 26)); // limit given in 9.2.2.1
			if (level_prefix >= 15) {
				v = level_prefix * 2 - 2;
				offset = (15 << suffixLength) - 4096;
			} else if (i > TrailingOnes || (TotalCoeff > 10 && TrailingOnes < 3)) {
				v = level_prefix + suffixLength + 1;
				offset = (level_prefix - 1) * (1 << suffixLength); // level_prefix may be 0
			} else if (level_prefix < 14) {
				v = level_prefix + 1;
				offset = level_prefix - 1;
			} else {
				v = 19;
				offset = -2;
			}
			#if SIZE_BIT == 32
				v -= level_prefix;
				ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, level_prefix);
				if (!(ctx->t.gb.lsb_cache <<= level_prefix))
					refill(&ctx->t.gb, 0);
			#endif
			int levelCode = get_uv(&ctx->t.gb, v) + offset;
			levelCode += (i == TrailingOnes && TrailingOnes < 3) * 2;
			level[i] = (levelCode % 2) ? (-levelCode - 1) >> 1 : (levelCode + 2) >> 1;
			suffixLength = min(suffixLength + (levelCode >= (3 << suffixLength)), 6);
		}
		
		// store level values at proper positions in memory
		int zerosLeft = 0;
		if (TotalCoeff <= endIdx - startIdx)
			zerosLeft = parse_total_zeros(ctx, endIdx, TotalCoeff);
		int8_t *scan = ctx->scan + startIdx + zerosLeft + TotalCoeff - 1;
		ctx->c[*scan] = level[0];
		for (int i = 1, v, run_before; i < TotalCoeff; i++) {
			scan--;
			if (zerosLeft > 0) {
				int threeBits = ctx->t.gb.msb_cache >> (SIZE_BIT - 3);
				if (zerosLeft <= 6 || threeBits > 0) {
					static int8_t run_before_codes[7][8] = {
						{9, 9, 9, 9, 8, 8, 8, 8},
						{18, 18, 17, 17, 8, 8, 8, 8},
						{19, 19, 18, 18, 17, 17, 16, 16},
						{28, 27, 18, 18, 17, 17, 16, 16},
						{29, 28, 27, 26, 17, 17, 16, 16},
						{25, 26, 28, 27, 30, 29, 16, 16},
						{0, 30, 29, 28, 27, 26, 25, 24}};
					int code = run_before_codes[min(zerosLeft, 7) - 1][threeBits];
					v = code >> 3;
					run_before = code & 7;
				} else {
					v = clz(ctx->t.gb.msb_cache | (size_t)1 << (SIZE_BIT - 11)) + 1; // at most 10 zeros in a valid code, and never clz(0)
					run_before = min(v + 3, zerosLeft);
				}
				scan -= run_before;
				zerosLeft -= run_before;
				ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, v);
				ctx->t.gb.lsb_cache <<= v;
			}
			ctx->c[*scan] = level[i];
		}
		for (int i = startIdx; i <= endIdx; i++)
			log_mb(ctx, (i < endIdx) ? "%d," : "%d]}\n", ctx->c[ctx->scan[i]]);
		
		// The post-level section (trailing_ones_sign_flags + total_zeros +
		// run_before) can consume more than 31 bits across its multiple
		// run_before reads; 31 is the old 32-bit-cache bound. Delaying the refill
		// to here is safe because the SIZE_BIT == 64 cache absorbs the true
		// worst case (a 32-bit build would need to re-check it).
		return ctx->t.gb.lsb_cache ? 1 : refill(&ctx->t.gb, 1);
	}

	static noinline int parse_residual_block_4x4_cavlc(Edge264MvcContext *ctx, int startIdx, int i4x4, int nA, int nB) {
		static const uint8_t nC_offset[8] = {184, 184, 80, 80, 0, 0, 0, 0};
		static const int16_t tokens[38 * 8] = {
			543, 539, 535, 531, 527, 522, 517, 512, // 4 <= nC < 8
			661, 662, 657, 658, 653, 675, 654, 649,
			780, 798, 797, 776, 807, 794, 793, 772,
			924, 920, 934, 916, 939, 930, 929, 912,
			1075, 1070, 1065, 1060, 1071, 1066, 1061, 1056,
			1200, 1206, 1201, 1196, 1207, 1202, 1197, 1192,
			1341, 1336, 1339, 1338, 1337, 1332, 1205, 1205,
			1345, 1345, 1340, 1340, 1343, 1343, 1342, 1342,
			1347, 1347, 1347, 1347, 1346, 1346, 1346, 1346,
			1344, 1344, 1344, 1344, 1344, 1344, 1344, 1344,
			261, 261, 261, 261, 256, 256, 256, 256, // 2 <= cN < 4
			531, 531, 527, 527, 394, 394, 394, 394,
			795, 782, 781, 772, 663, 663, 649, 649,
			799, 799, 786, 786, 785, 785, 776, 776,
			931, 931, 918, 918, 917, 917, 908, 908,
			1044, 1044, 1050, 1050, 1049, 1049, 1040, 1040,
			1191, 1191, 1182, 1182, 1181, 1181, 1176, 1176,
			1455, 1446, 1445, 1440, 1451, 1442, 1441, 1436,
			1580, 1582, 1581, 1576, 1587, 1578, 1577, 1572,
			1723, 1718, 1717, 1716, 1719, 1714, 1713, 1712,
			1853, 1852, 1854, 1849, 1722, 1722, 1720, 1720,
			1859, 1859, 1858, 1858, 1857, 1857, 1856, 1856,
			1727, 1727, 1727, 1727, 1727, 1727, 1727, 1727,
			128, 128, 128, 128, 128, 128, 128, 128, // 0 <= nC < 2
			261, 261, 261, 261, 261, 261, 261, 261,
			394, 394, 394, 394, 394, 394, 394, 394,
			777, 777, 772, 772, 655, 655, 655, 655,
			919, 919, 910, 910, 787, 787, 787, 787,
			1051, 1051, 1042, 1042, 1037, 1037, 1032, 1032,
			1183, 1183, 1174, 1174, 1169, 1169, 1164, 1164,
			1315, 1315, 1306, 1306, 1301, 1301, 1296, 1296,
			1447, 1447, 1438, 1438, 1433, 1433, 1428, 1428,
			1696, 1702, 1697, 1692, 1707, 1698, 1693, 1688,
			1843, 1838, 1833, 1832, 1839, 1834, 1829, 1828,
			1979, 1974, 1969, 1968, 1975, 1970, 1965, 1964,
			2115, 2110, 2109, 2104, 2111, 2106, 2105, 2100,
			2112, 2112, 2114, 2114, 2113, 2113, 2108, 2108,
			1973, 1973, 1973, 1973, 1973, 1973, 1973, 1973,
		};
		
		int sum = nA + nB;
		int nC = !(ctx->unavail4x4[i4x4] & 3) ? (sum + 1) >> 1 : sum;
		int coeff_token, v;
		if (__builtin_expect(nC < 8, 1)) {
			int leadingZeroBits = clz(ctx->t.gb.msb_cache | (size_t)1 << (SIZE_BIT - 15));
			unsigned suffix = ctx->t.gb.msb_cache >> (SIZE_BIT - 4 - leadingZeroBits) & 7;
			assert(nC >= 0 && nC_offset[nC] + leadingZeroBits * 8 + suffix < 304);
			int token = tokens[nC_offset[nC] + leadingZeroBits * 8 + suffix];
			coeff_token = token & 127;
			v = token >> 7;
		} else {
			coeff_token = (ctx->t.gb.msb_cache >> (SIZE_BIT - 6)) + 4;
			if (coeff_token == 7)
				coeff_token = 0;
			v = 6;
		}
		ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, v);
		ctx->t.gb.lsb_cache <<= v;
		if (!ctx->t.gb.lsb_cache)
			refill(&ctx->t.gb, 0);
		if (coeff_token) {
			mbc->nC[i4x4] = coeff_token >> 2;
			log_mb(ctx, "%s- {nC: %u, c: [", ctx->log_indent, nC);
			return parse_residual_coeffs_cavlc(ctx, startIdx, 15, coeff_token & 3, coeff_token >> 2);
		} else {
			log_mb(ctx, "%s- {nC: %u}\n", ctx->log_indent, nC);
			return 0;
		}
	}
	
	// 4:2:0 is best handled separately due to the open-ended 0000000 code and 3 bit suffixes
	static void parse_residual_block_2x2_cavlc(Edge264MvcContext *ctx) {
		static const int16_t tokens[8 * 4] = {
			133, 133, 133, 133,
			256, 256, 256, 256,
			394, 394, 394, 394,
			776, 783, 777, 772,
			784, 784, 780, 780,
			910, 910, 909, 909,
			1042, 1042, 1041, 1041,
			915, 915, 915, 915,
		};
		int leadingZeroBits = clz(ctx->t.gb.msb_cache | (size_t)1 << (SIZE_BIT - 8));
		unsigned suffix = ctx->t.gb.msb_cache >> (SIZE_BIT - 3 - leadingZeroBits) & 3;
		int token = tokens[leadingZeroBits * 4 + suffix];
		int coeff_token = token & 127;
		int v = token >> 7;
		ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, v);
		if (!(ctx->t.gb.lsb_cache <<= v))
			refill(&ctx->t.gb, 0);
		if (coeff_token) {
			log_mb(ctx, "%s- {nC: -1, c: [", ctx->log_indent);
			parse_residual_coeffs_cavlc(ctx, 0, 3, coeff_token & 3, coeff_token >> 2);
		} else {
			log_mb(ctx, "%s- {nC: -1}\n", ctx->log_indent);
		}
	}
	
	// 4:2:2 chroma DC has codes of its own (nC == -2 in Table 9-5), matched one
	// by one as {code aligned on 13 bits, length, TotalCoeff << 2 | TrailingOnes}
	static void parse_residual_block_2x4_cavlc(Edge264MvcContext *ctx) {
		static const uint32_t codes[30] = {
			0x10000100, 0x3c00704, 0x8000205, 0x3800708, 0x3400709, 0x400030a, 0x70090c, 0x300070d,
			0x2c0070e, 0x100050f, 0x600910, 0x500911, 0x2800712, 0x800613, 0x380a14, 0x300a15,
			0x400916, 0x2400717, 0x1c0b18, 0x180b19, 0x280a1a, 0x200071b, 0xe0c1c, 0xc0c1d,
			0x140b1e, 0x200a1f, 0x70d20, 0xa0c21, 0x80c22, 0x100b23,
		};
		unsigned bits = ctx->t.gb.msb_cache >> (SIZE_BIT - 13);
		int coeff_token = 0, v = 13; // no code matches 13 zeros, which a valid stream never holds
		for (int i = 0; i < 30; i++) {
			int len = codes[i] >> 8 & 15;
			if (!((bits ^ codes[i] >> 16) >> (13 - len))) {
				coeff_token = codes[i] & 255;
				v = len;
				break;
			}
		}
		ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, v);
		if (!(ctx->t.gb.lsb_cache <<= v))
			refill(&ctx->t.gb, 0);
		if (coeff_token) {
			log_mb(ctx, "%s- {nC: -2, c: [", ctx->log_indent);
			parse_residual_coeffs_cavlc(ctx, 0, 7, coeff_token & 3, coeff_token >> 2);
		} else {
			log_mb(ctx, "%s- {nC: -2}\n", ctx->log_indent);
		}
	}
#endif



/**
 * Parse a CABAC residual block after coded_block_flag (9.3.2.3).
 * 
 * There are 2 entry points which tail jump to parse_residual_coeffs_cabac:
 * _ parse_residual_block_cabac for 4x4 and 2x2 blocks
 *   (for which ctxIdxInc = LevelListIdx in spec's formula 9-21)
 * _ parse_residual_block_8x8_cabac for 8x8 and 2x4 blocks
 *   (with additional array accesses for ctxIdxInc)
 * 
 * Here, all bypass bits can be extracted using a binary division (!!).
 * coeff_abs_level expects at most 2^(7+14)-14, i.e 41 bits as Exp-Golomb, so
 * we can get all of them on 64 bit machines.
 * While residual coefficients are not too critical to optimize yet tricky to
 * maintain, this function is designed to be simple and compact.
 */
#if CABAC
	static noinline void parse_residual_coeffs_cabac(Edge264MvcContext * restrict ctx, uint64_t significant_coeff_flags, size_t range, size_t offset) {
		// Now loop on set bits to parse all non-zero coefficients.
		int ctxIdx0 = ctx->ctxIdxOffsets[3] + 1;
		int ctxIdx1 = ctx->ctxIdxOffsets[3] + 5;
		do {
			int coeff_level;
			if (!get_ae_regs(ctx, ctxIdx0, &range, &offset)) {
				static const int8_t trans[5] = {0, 2, 3, 4, 4};
				ctxIdx0 = ctx->ctxIdxOffsets[3] + trans[ctxIdx0 - ctx->ctxIdxOffsets[3]];
				coeff_level = get_bypass_regs(ctx, &range, &offset) ? -1 : 1;
			} else {
				coeff_level = 2;
				while (get_ae_regs(ctx, ctxIdx1, &range, &offset) && ++coeff_level != 15)
					continue;
				#if SIZE_BIT == 32
					if (coeff_level == 15) {
						// the largest value to encode is -2^(14+7)+14, for which k=20 (see 9.3.2.3)
						int k = 0;
						while (get_bypass_regs(ctx, &range, &offset) && k < 20)
							k++;
						coeff_level = 1;
						while (k--)
							coeff_level += coeff_level + get_bypass_regs(ctx, &range, &offset);
						coeff_level += 14;
					}
					coeff_level = get_bypass_regs(ctx, &range, &offset) ? -coeff_level : coeff_level;
				#elif SIZE_BIT == 64
					if (coeff_level != 15) {
						coeff_level = get_bypass_regs(ctx, &range, &offset) ? -coeff_level : coeff_level;
					} else {
						// we need at least 51 bits in offset to get 42 bits with a division by 9 bits
						ctx->t.gb.range = range;
						ctx->t.gb.offset = offset;
						int zeros = clz(ctx->t.gb.range);
						if (zeros > 64 - 51)
							zeros = renorm_bits(ctx, zeros);
						size_t range42 = ctx->t.gb.range >> 42;
						size_t quo = ctx->t.gb.offset / range42; // contains 42 bypass bits in lsb
						size_t rem = ctx->t.gb.offset % range42;
						int k = clz(~quo << (64 - 42) | (size_t)1 << (64 - 21));
						int unused = 42 - k * 2 - 2;
						coeff_level = 14 + (1 << k) + ((unsigned)(quo >> unused >> 1) & ((1 << k) - 1));
						coeff_level = (quo & (size_t)1 << unused) ? -coeff_level : coeff_level;
						offset = (quo & (((size_t)1 << unused) - 1)) * range42 + rem;
						range = range42 << unused;
					}
				#endif
				ctxIdx0 = ctx->ctxIdxOffsets[3];
				ctxIdx1 = ctxIdx0 + ctx->coeff_abs_inc[ctxIdx1 - ctxIdx0 - 5];
			}
		
			// store in transposed scan position
			int i = 63 - __builtin_clzll(significant_coeff_flags);
			ctx->c[ctx->scan[i]] = coeff_level;
			significant_coeff_flags &= ~((uint64_t)1 << i);
		} while (significant_coeff_flags != 0);
		ctx->t.gb.range = range;
		ctx->t.gb.offset = offset;
	}
	
	static noinline void parse_residual_block_8x8_cabac(Edge264MvcContext * restrict ctx, int startIdx, int endIdx) {
		uint64_t significant_coeff_flags = 0;
		size_t range = ctx->t.gb.range;
		size_t offset = ctx->t.gb.offset;
		int i = startIdx;
		do {
			if (get_ae_regs(ctx, ctx->ctxIdxOffsets[1] + ctx->sig_inc[i], &range, &offset)) {
				significant_coeff_flags |= (uint64_t)1 << i;
				if (get_ae_regs(ctx, ctx->ctxIdxOffsets[2] + ctx->last_inc[i], &range, &offset))
					break;
			}
		} while (++i < endIdx);
		significant_coeff_flags |= (uint64_t)1 << i;
		parse_residual_coeffs_cabac(ctx, significant_coeff_flags, range, offset);
	}
	
	static noinline void parse_residual_block_cabac(Edge264MvcContext * restrict ctx, int startIdx, int endIdx) {
		// we could preload all states in a vector, but cache misses aren't an issue here
		unsigned significant_coeff_flags = 0;
		size_t range = ctx->t.gb.range;
		size_t offset = ctx->t.gb.offset;
		int i = startIdx;
		do {
			if (get_ae_regs(ctx, ctx->ctxIdxOffsets[1] + i, &range, &offset)) {
				significant_coeff_flags |= 1 << i;
				if (get_ae_regs(ctx, ctx->ctxIdxOffsets[2] + i, &range, &offset))
					break;
			}
		} while (++i < endIdx);
		significant_coeff_flags |= 1 << i;
		parse_residual_coeffs_cabac(ctx, significant_coeff_flags, range, offset);
	}
#endif



/**
 * As its name says, parses mb_qp_delta (9.3.2.7 and 9.3.3.1.1.5).
 */
static inline void CAFUNC(parse_mb_qp_delta)
{
	#if !CABAC
		// QP'Y wraps modulo 52 + QpBdOffsetY (7-37)
		int range = 52 + ctx->t.QpBdOffset_Y;
		int mb_qp_delta = get_se16(&ctx->t.gb, -(26 + ctx->t.QpBdOffset_Y / 2), 25 + ctx->t.QpBdOffset_Y / 2);
		if (mb_qp_delta) {
			int sum = ctx->t.QP[0] + mb_qp_delta;
			int QP_Y = (sum < 0) ? sum + range : (sum >= range) ? sum - range : sum;
			mb->QP_s = ctx->t.QP_s = (i8x4){QP_Y, ctx->QP_C[0][QP_Y], ctx->QP_C[1][QP_Y], ctx->t.QP[3]};
		}
	#else
		int mb_qp_delta_nz = get_ae(ctx, 60 + ctx->mb_qp_delta_nz);
		ctx->mb_qp_delta_nz = mb_qp_delta_nz;
		int mb_qp_delta = 0;
		if (mb_qp_delta_nz) {
			int range = 52 + ctx->t.QpBdOffset_Y; // QP'Y wraps modulo it (7-37)
			unsigned count = 1, ctxIdx = 62;
			while (get_ae(ctx, ctxIdx) && count < range)
				count++, ctxIdx = 63;
			mb_qp_delta = count & 1 ? count / 2 + 1 : -(count / 2);
			int sum = ctx->t.QP[0] + mb_qp_delta;
			int QP_Y = (sum < 0) ? sum + range : (sum >= range) ? sum - range : sum;
			mb->QP_s = ctx->t.QP_s = (i8x4){QP_Y, ctx->QP_C[0][QP_Y], ctx->QP_C[1][QP_Y], ctx->t.QP[3]};
		}
	#endif
	log_mb(ctx, "%smb_qp_delta: %d\n", ctx->log_indent, mb_qp_delta);
}



/**
 * The P macroblocks of SP slices keep the levels of their 4x4 blocks for
 * decode_sp_mb, which runs once the macroblock is parsed and predicted.
 */
#if !CABAC
	static always_inline int keep_sp_levels(Edge264MvcContext *ctx, int blk) {
		if (!(ctx->t.sp_slice & mb->mbIsInterFlag))
			return 0;
		memcpy(ctx->sp_levels[blk], ctx->c, sizeof(ctx->sp_levels[blk]));
		ctx->c_v[0] = ctx->c_v[1] = ctx->c_v[2] = ctx->c_v[3] = (i8x16){};
		return 1;
	}
#endif



/**
 * Parsing for chroma 4:2:2 (7.3.5.3.3), where each component has a 2x4 block
 * of DC coefficients (8.5.11) and eight 4x4 AC blocks, two per row. The DC
 * coefficients of Cb are parsed to ctx->c[0..7] and those of Cr to
 * ctx->c[8..15], then transformed to ctx->c[16..31], one per AC block, as
 * the DC values of add_idct4x4 and add_dc4x4.
 */
static noinline void CAFUNC(parse_chroma422_residual)
{
	if (!mb->f.CodedBlockPatternChromaDC)
		return;
	for (int iCbCr = 0; iCbCr < 2; iCbCr++) {
		ctx->scan_v[0] = iCbCr ? (i8x16){8, 9, 10, 11, 12, 13, 14, 15} : (i8x16){0, 1, 2, 3, 4, 5, 6, 7};
		#if !CABAC
			parse_residual_block_2x4_cavlc(ctx);
		#else
			ctx->ctxIdxOffsets_l = ctxIdxOffsets_chromaDC[ctx->t.field_pic_flag];
			ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 8};
			ctx->sig_inc_l = ctx->last_inc_l = sig_inc_chromaDC_422; // Min(numDecodAbsLevel / NumC8x8, 2) in 9.3.3.1.3
			if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->inc.coded_block_flags_16x16[1 + iCbCr])) {
				mb->f.coded_block_flags_16x16[1 + iCbCr] = 1;
				parse_residual_block_8x8_cabac(ctx, 0, 7);
			}
		#endif
	}
	transform_dc2x4(ctx);
	
	// Sixteen 4x4 AC blocks for the Cb/Cr components, or their DC values alone
	#if CABAC
		ctx->ctxIdxOffsets_l = ctxIdxOffsets_chromaAC[ctx->t.field_pic_flag];
		ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 9, 9};
	#endif
	ctx->scan_v[0] = scan_4x4[ctx->t.field_pic_flag];
	for (int i4x4 = 0; i4x4 < 16; i4x4++) {
		int iYCbCr = 1 + (i4x4 >> 3);
		uint8_t *samples = ctx->samples_mb[iYCbCr] + (i4x4 >> 1 & 3) * 4 * ctx->t.stride[1] + ((i4x4 & 1) * 4 << ctx->t.px_shift);
		if (mb->f.CodedBlockPatternChromaAC) {
			int nA = *((int8_t *)mbc->nC + 16 + ctx->ACbCr_int8[i4x4]);
			int nB = *((int8_t *)mbc->nC + 16 + ctx->BCbCr_int8[i4x4]);
			#if !CABAC
				if (parse_residual_block_4x4_cavlc(ctx, 1, 16 + i4x4, nA, nB)) {
					add_idct4x4(ctx, iYCbCr, i4x4, samples);
					continue;
				}
			#else
				if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->nC_inc[1][i4x4] + nA + nB * 2)) {
					mbc->nC[16 + i4x4] = 1;
					parse_residual_block_cabac(ctx, 1, 15);
					add_idct4x4(ctx, iYCbCr, i4x4, samples);
					continue;
				}
			#endif
		}
		add_dc4x4(ctx, iYCbCr, i4x4, samples);
	}
	ctx->c_v[4] = ctx->c_v[5] = ctx->c_v[6] = ctx->c_v[7] = (i8x16){};
}



/**
 * Parsing for chroma 4:2:0 and 4:2:2 is put in a separate function to be
 * tail-called from parse_NxN_residual and parse_Intra16x16_residual.
 */
static noinline void CAFUNC(parse_chroma_residual)
{
	if (ctx->t.ChromaArrayType == 2)
		CAJUMP(parse_chroma422_residual);
	// As in Intra16x16, DC blocks are parsed to ctx->c[0..15], then transformed to ctx->c[16..31]
	if (mb->f.CodedBlockPatternChromaDC) { // valid also for 4:0:0
		#if !CABAC
			ctx->scan_s = (i8x4){0, 4, 2, 6};
			parse_residual_block_2x2_cavlc(ctx);
			ctx->scan_s = (i8x4){1, 5, 3, 7};
			parse_residual_block_2x2_cavlc(ctx);
		#else
			ctx->ctxIdxOffsets_l = ctxIdxOffsets_chromaDC[ctx->t.field_pic_flag]; // FIXME 4:2:2
			ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 8};
			if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->inc.coded_block_flags_16x16[1])) {
				mb->f.coded_block_flags_16x16[1] = 1;
				ctx->scan_s = (i8x4){0, 4, 2, 6};
				parse_residual_block_cabac(ctx, 0, 3);
			}
			if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->inc.coded_block_flags_16x16[2])) {
				mb->f.coded_block_flags_16x16[2] = 1;
				ctx->scan_s = (i8x4){1, 5, 3, 7};
				parse_residual_block_cabac(ctx, 0, 3);
			}
		#endif
		if (ctx->t.sp_slice & mb->mbIsInterFlag) {
			memcpy(ctx->sp_levels_dc, ctx->c, sizeof(ctx->sp_levels_dc));
			ctx->c_v[0] = ctx->c_v[1] = (i8x16){};
		} else {
			transform_dc2x2(ctx);
		}
		
		// Eight or sixteen 4x4 AC blocks for the Cb/Cr components
		if (mb->f.CodedBlockPatternChromaAC) {
			#if CABAC
				ctx->ctxIdxOffsets_l = ctxIdxOffsets_chromaAC[ctx->t.field_pic_flag];
				ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 9, 9};
			#endif
			ctx->scan_v[0] = scan_4x4[ctx->t.field_pic_flag];
			for (int i4x4 = 0; i4x4 < 8; i4x4++) {
				int iYCbCr = 1 + (i4x4 >> 2);
				uint8_t *samples = ctx->samples_mb[iYCbCr] + y420[i4x4] * ctx->t.stride[1] + (x420[i4x4] << ctx->t.px_shift);
				int nA = *((int8_t *)mbc->nC + 16 + ctx->ACbCr_int8[i4x4]);
				int nB = *((int8_t *)mbc->nC + 16 + ctx->BCbCr_int8[i4x4]);
				#if !CABAC
					if (parse_residual_block_4x4_cavlc(ctx, 1, 16 + i4x4, nA, nB)) {
						if (!keep_sp_levels(ctx, 16 + i4x4))
							add_idct4x4(ctx, iYCbCr, i4x4, samples);
					} else if (!(ctx->t.sp_slice & mb->mbIsInterFlag)) {
						add_dc4x4(ctx, iYCbCr, i4x4, samples);
					}
				#else
					if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->nC_inc[1][i4x4] + nA + nB * 2)) {
						mbc->nC[16 + i4x4] = 1;
						parse_residual_block_cabac(ctx, 1, 15);
						if (!keep_sp_levels(ctx, 16 + i4x4))
							add_idct4x4(ctx, iYCbCr, i4x4, samples);
					} else if (!(ctx->t.sp_slice & mb->mbIsInterFlag)) {
						add_dc4x4(ctx, iYCbCr, i4x4, samples);
					}
				#endif
			}
		}
		ctx->c_v[4] = ctx->c_v[5] = (i8x16){};
	}
}



/**
 * Intra16x16 residual blocks have so many differences with Intra4x4 that they
 * deserve their own function.
 */
static noinline void CAFUNC(parse_Intra16x16_residual)
{
	CACALL(parse_mb_qp_delta);
	log_mb(ctx, "%scoeffLevels:\n", ctx->log_indent);
	
	// Both AC and DC coefficients are initially parsed to ctx->c[0..15]
	ctx->scan_v[0] = scan_4x4[ctx->t.field_pic_flag];
	for (int iYCbCr = 0; iYCbCr < 3; iYCbCr++) {
		
		// Parse a DC block, then transform it to ctx->c[16..31]
		#if !CABAC
			if (parse_residual_block_4x4_cavlc(ctx, 0, 0,
				(mbA == &unavail_mb ? &unavail_mbc : mbc - 1)->nC[iYCbCr * 16 + 5],
				(mbB == &unavail_mb ? &unavail_mbc : mbc + 1)->nC[iYCbCr * 16 + 10])) {
				mbc->nC[0] = 0;
				transform_dc4x4(ctx, iYCbCr);
			}
		#else
			ctx->ctxIdxOffsets_l = ctxIdxOffsets_16x16DC[iYCbCr][ctx->t.field_pic_flag];
			if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->inc.coded_block_flags_16x16[iYCbCr])) {
				mb->f.coded_block_flags_16x16[iYCbCr] = 1;
				ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 9, 9};
				parse_residual_block_cabac(ctx, 0, 15);
				transform_dc4x4(ctx, iYCbCr);
			}
		#endif
		
		// All AC blocks pick a DC coeff, then go to ctx->c[1..15]
		if (mb->bits[0] & 1 << 5) {
			#if CABAC
				ctx->ctxIdxOffsets_l = ctxIdxOffsets_16x16AC[iYCbCr][ctx->t.field_pic_flag];
				ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 9, 9};
			#endif
			for (int i4x4 = 0; i4x4 < 16; i4x4++) {
				uint8_t *samples = ctx->samples_mb[iYCbCr] + y444[i4x4] * ctx->t.stride[iYCbCr] + (x444[i4x4] << ctx->t.px_shift);
				int nA = *((int8_t *)mbc->nC + iYCbCr * 16 + ctx->A4x4_int8[i4x4]);
				int nB = *((int8_t *)mbc->nC + iYCbCr * 16 + ctx->B4x4_int8[i4x4]);
				#if !CABAC
					if (parse_residual_block_4x4_cavlc(ctx, 1, iYCbCr * 16 + i4x4, nA, nB))
						add_idct4x4(ctx, iYCbCr, i4x4, samples);
					else
						add_dc4x4(ctx, iYCbCr, i4x4, samples);
				#else
					if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->nC_inc[iYCbCr][i4x4] + nA + nB * 2)) {
						mbc->nC[iYCbCr * 16 + i4x4] = 1;
						parse_residual_block_cabac(ctx, 1, 15);
						add_idct4x4(ctx, iYCbCr, i4x4, samples);
					} else {
						add_dc4x4(ctx, iYCbCr, i4x4, samples);
					}
				#endif
			}
		}
		ctx->c_v[4] = ctx->c_v[5] = ctx->c_v[6] = ctx->c_v[7] = (i32x4){};
		
		// here is how we share the decoding of luma coefficients with 4:4:4 modes
		if (ctx->t.ChromaArrayType <3)
			CAJUMP(parse_chroma_residual);
	}
}



/**
 * This block is dedicated to the parsing of Intra_NxN and Inter_NxN residual
 * blocks, since they share much in common.
 */
static noinline void CAFUNC(parse_NxN_residual)
{
	static const int8_t Intra4x4Modes[9][16] = {
		{I4x4_V_8  , I4x4_V_8   , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_V_8   , I4x4_V_8   , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_V_8   , I4x4_V_8   , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_V_8   , I4x4_V_8   , I4x4_DC_AB_8, I4x4_DC_AB_8},
		{I4x4_H_8  , I4x4_DC_AB_8, I4x4_H_8   , I4x4_DC_AB_8, I4x4_H_8   , I4x4_DC_AB_8, I4x4_H_8   , I4x4_DC_AB_8, I4x4_H_8   , I4x4_DC_AB_8, I4x4_H_8   , I4x4_DC_AB_8, I4x4_H_8   , I4x4_DC_AB_8, I4x4_H_8   , I4x4_DC_AB_8},
		{I4x4_DC_8 , I4x4_DC_A_8 , I4x4_DC_B_8 , I4x4_DC_AB_8, I4x4_DC_8  , I4x4_DC_A_8 , I4x4_DC_B_8 , I4x4_DC_AB_8, I4x4_DC_8  , I4x4_DC_A_8 , I4x4_DC_B_8 , I4x4_DC_AB_8, I4x4_DC_8  , I4x4_DC_A_8 , I4x4_DC_B_8 , I4x4_DC_AB_8},
		{I4x4_DDL_8, I4x4_DDL_8 , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DDL_C_8, I4x4_DDL_C_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DDL_8 , I4x4_DDL_8 , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DDL_C_8, I4x4_DDL_C_8, I4x4_DC_AB_8, I4x4_DC_AB_8},
		{I4x4_DDR_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DDR_8 , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8},
		{I4x4_VR_8 , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_VR_8  , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8},
		{I4x4_HD_8 , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_HD_8  , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_DC_AB_8},
		{I4x4_VL_8 , I4x4_VL_8  , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_VL_C_8 , I4x4_VL_C_8 , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_VL_8  , I4x4_VL_8  , I4x4_DC_AB_8, I4x4_DC_AB_8, I4x4_VL_C_8 , I4x4_VL_C_8 , I4x4_DC_AB_8, I4x4_DC_AB_8},
		{I4x4_HU_8 , I4x4_DC_AB_8, I4x4_HU_8  , I4x4_DC_AB_8, I4x4_HU_8  , I4x4_DC_AB_8, I4x4_HU_8  , I4x4_DC_AB_8, I4x4_HU_8  , I4x4_DC_AB_8, I4x4_HU_8  , I4x4_DC_AB_8, I4x4_HU_8  , I4x4_DC_AB_8, I4x4_HU_8  , I4x4_DC_AB_8},
	};
	static const int8_t Intra8x8Modes[9][16] = {
		{I8x8_V_8  , I8x8_V_8    , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_V_C_8  , I8x8_V_C_8  , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_V_D_8  , I8x8_V_D_8  , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_V_CD_8  , I8x8_V_CD_8  , I8x8_DC_AB_8, I8x8_DC_AB_8},
		{I8x8_H_8  , I8x8_DC_AB_8, I8x8_H_8    , I8x8_DC_AB_8, I8x8_H_8    , I8x8_DC_AB_8, I8x8_H_8    , I8x8_DC_AB_8, I8x8_H_D_8  , I8x8_DC_AB_8, I8x8_H_D_8  , I8x8_DC_AB_8, I8x8_H_D_8   , I8x8_DC_AB_8 , I8x8_H_D_8  , I8x8_DC_AB_8},
		{I8x8_DC_8 , I8x8_DC_A_8 , I8x8_DC_B_8 , I8x8_DC_AB_8, I8x8_DC_C_8 , I8x8_DC_AC_8, I8x8_DC_B_8 , I8x8_DC_AB_8, I8x8_DC_D_8 , I8x8_DC_AD_8, I8x8_DC_BD_8, I8x8_DC_AB_8, I8x8_DC_CD_8 , I8x8_DC_ACD_8, I8x8_DC_BD_8, I8x8_DC_AB_8},
		{I8x8_DDL_8, I8x8_DDL_8  , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DDL_C_8, I8x8_DDL_C_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DDL_D_8, I8x8_DDL_D_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DDL_CD_8, I8x8_DDL_CD_8, I8x8_DC_AB_8, I8x8_DC_AB_8},
		{I8x8_DDR_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DDR_C_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8 , I8x8_DC_AB_8 , I8x8_DC_AB_8, I8x8_DC_AB_8},
		{I8x8_VR_8 , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_VR_C_8 , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8 , I8x8_DC_AB_8 , I8x8_DC_AB_8, I8x8_DC_AB_8},
		{I8x8_HD_8 , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_HD_8   , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_DC_AB_8 , I8x8_DC_AB_8 , I8x8_DC_AB_8, I8x8_DC_AB_8},
		{I8x8_VL_8 , I8x8_VL_8   , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_VL_C_8 , I8x8_VL_C_8 , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_VL_D_8 , I8x8_VL_D_8 , I8x8_DC_AB_8, I8x8_DC_AB_8, I8x8_VL_CD_8 , I8x8_VL_CD_8 , I8x8_DC_AB_8, I8x8_DC_AB_8},
		{I8x8_HU_8 , I8x8_DC_AB_8, I8x8_HU_8   , I8x8_DC_AB_8, I8x8_HU_8   , I8x8_DC_AB_8, I8x8_HU_8   , I8x8_DC_AB_8, I8x8_HU_D_8 , I8x8_DC_AB_8, I8x8_HU_D_8 , I8x8_DC_AB_8, I8x8_HU_D_8  , I8x8_DC_AB_8 , I8x8_HU_D_8 , I8x8_DC_AB_8},
	};
	
	if (mb->f.CodedBlockPatternChromaDC | (mb->bits[0] & 0xac)) {
		CACALL(parse_mb_qp_delta);
		log_mb(ctx, "%scoeffLevels:\n", ctx->log_indent);
	}
	#if CABAC
		else
			ctx->mb_qp_delta_nz = 0;
	#endif
	
	// next few blocks will share many parameters, so we cache them
	for (int iYCbCr = 0; iYCbCr < 3; iYCbCr++) {
		if (!mb->f.transform_size_8x8_flag) {
			#if CABAC
				ctx->ctxIdxOffsets_l = ctxIdxOffsets_4x4[iYCbCr][ctx->t.field_pic_flag];
				ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 9, 9};
			#endif
			ctx->scan_v[0] = scan_4x4[ctx->t.field_pic_flag];
			
			// Decoding directly follows parsing to avoid duplicate loops.
			for (int i4x4 = 0; i4x4 < 16; i4x4++) {
				size_t stride = ctx->t.stride[iYCbCr];
				uint8_t *samples = ctx->samples_mb[iYCbCr] + y444[i4x4] * stride + (x444[i4x4] << ctx->t.px_shift);
				if (!mb->mbIsInterFlag)
					decode_intra4x4(samples, stride, Intra4x4Modes[mbc->Intra4x4PredMode[i4x4]][ctx->unavail_intra[i4x4]], ctx->t.samples_clip_v[iYCbCr]);
				if (mb->bits[0] & 1 << bit8x8[i4x4 >> 2]) {
					int nA = *((int8_t *)mbc->nC + iYCbCr * 16 + ctx->A4x4_int8[i4x4]);
					int nB = *((int8_t *)mbc->nC + iYCbCr * 16 + ctx->B4x4_int8[i4x4]);
					// DC blocks are marginal here (about 16%) so we do not handle them separately
					#if !CABAC
						if (parse_residual_block_4x4_cavlc(ctx, 0, iYCbCr * 16 + i4x4, nA, nB) && !keep_sp_levels(ctx, i4x4))
							add_idct4x4(ctx, iYCbCr, -1, samples); // FIXME 4:4:4
					#else
						if (get_ae(ctx, ctx->ctxIdxOffsets[0] + ctx->nC_inc[iYCbCr][i4x4] + nA + nB * 2)) {
							mbc->nC[iYCbCr * 16 + i4x4] = 1;
							parse_residual_block_cabac(ctx, 0, 15);
							if (!keep_sp_levels(ctx, i4x4))
								add_idct4x4(ctx, iYCbCr, -1, samples); // FIXME 4:4:4
						}
					#endif
				}
			}
		} else {
			#if CABAC
				ctx->ctxIdxOffsets_l = ctxIdxOffsets_8x8[iYCbCr][ctx->t.field_pic_flag];
				ctx->coeff_abs_inc_l = (i8x8){6, 7, 8, 9, 9};
				ctx->sig_inc_v[0] = sig_inc_8x8[ctx->t.field_pic_flag][0];
				ctx->last_inc_v[0] = last_inc_8x8[0];
				ctx->scan_v[0] = scan_8x8_cabac[ctx->t.field_pic_flag][0];
			#endif
			
			for (int i8x8 = 0; i8x8 < 4; i8x8++) {
				size_t stride = ctx->t.stride[iYCbCr];
				uint8_t *samples = ctx->samples_mb[iYCbCr] + y444[i8x8 * 4] * stride + (x444[i8x8 * 4] << ctx->t.px_shift);
				if (!mb->mbIsInterFlag)
					decode_intra8x8(samples, stride, Intra8x8Modes[mbc->Intra4x4PredMode[i8x8 * 4]][ctx->unavail_intra[i8x8 * 5]], ctx->t.samples_clip_v[iYCbCr]);
				if (mb->bits[0] & 1 << bit8x8[i8x8]) {
					#if !CABAC
						for (int i4x4 = 0; i4x4 < 4; i4x4++) {
							ctx->scan_v[0] = scan_8x8_cavlc[ctx->t.field_pic_flag][i4x4];
							int nA = *((int8_t *)mbc->nC + iYCbCr * 16 + ctx->A4x4_int8[i8x8 * 4 + i4x4]);
							int nB = *((int8_t *)mbc->nC + iYCbCr * 16 + ctx->B4x4_int8[i8x8 * 4 + i4x4]);
							parse_residual_block_4x4_cavlc(ctx, 0, iYCbCr * 16 + i8x8 * 4 + i4x4, nA, nB);
						}
						add_idct8x8(ctx, iYCbCr, samples);
					#else
						if (ctx->t.ChromaArrayType < 3 || get_ae(ctx, ctx->ctxIdxOffsets[0] + (mb->bits[1] >> inc8x8[iYCbCr * 4 + i8x8] & 3))) {
							mb->bits[1] |= 1 << bit8x8[iYCbCr * 4 + i8x8];
							mbc->nC_s[iYCbCr * 4 + i8x8] = 0x01010101;
							parse_residual_block_8x8_cabac(ctx, 0, 63);
							add_idct8x8(ctx, iYCbCr, samples);
						}
					#endif
				}
			}
		}
		
		// nice optimisation for 4:4:4 modes
		if (ctx->t.ChromaArrayType <3)
			CAJUMP(parse_chroma_residual);
	}
}



/**
 * Parses CodedBlockPatternLuma/Chroma (9.3.2.6 and 9.3.3.1.1.4).
 *
 * As with mb_qp_delta, coded_block_pattern is parsed in two distinct code
 * paths, thus put in a distinct function.
 */
static always_inline void CAFUNC(parse_coded_block_pattern, const uint8_t *map_me)
{
	// Luma prefix
	#if !CABAC
		// Table 9-4 has 48 codes with chroma (1 or 2), and 16 more without
		int cbp = (ctx->t.ChromaArrayType == 1 || ctx->t.ChromaArrayType == 2) ?
			map_me[get_ue16(&ctx->t.gb, 47)] : map_me[48 + get_ue16(&ctx->t.gb, 15)];
		mb->bits[0] |= cbp & 0xac;
	#else
		int bits = mb->bits[0];
		bits |= get_ae(ctx, 76 - (bits      & 3)) << 5;
		bits |= get_ae(ctx, 76 - (bits >> 5 & 3)) << 3;
		bits |= get_ae(ctx, 76 - (bits >> 4 & 3)) << 2;
		bits |= get_ae(ctx, 76 - (bits >> 2 & 3)) << 7;
		mb->bits[0] = bits;
	#endif
	
	// Chroma suffix
	if ((ctx->t.ChromaArrayType == 1 || ctx->t.ChromaArrayType == 2) &&
		CACOND(cbp & 3, get_ae(ctx, 77 + ctx->inc.CodedBlockPatternChromaDC)))
	{
		mb->f.CodedBlockPatternChromaDC = 1;
		mb->f.CodedBlockPatternChromaAC = CACOND(cbp >> 1 & 1, get_ae(ctx, 81 + ctx->inc.CodedBlockPatternChromaAC));
	}
	
	log_mb(ctx, "%scoded_block_pattern: %u\n", ctx->log_indent,
		(mb->bits[0] >> 5 & 1) | (mb->bits[0] >> 2 & 2) | (mb->bits[0] & 4) | (mb->bits[0] >> 4 & 8) |
		(mb->f.CodedBlockPatternChromaDC + mb->f.CodedBlockPatternChromaAC) << 4);
}



#if !CABAC
/**
 * Intra chroma prediction of a macroblock of a field picture, whose rows of Cb
 * and Cr do not alternate at a single stride as decode_intraChroma takes them:
 * the available neighbouring samples are copied around a block of such rows,
 * which the prediction fills and which is copied back.
 */
static noinline void decode_intraChroma_field(Edge264MvcContext *ctx, int mode) {
	int h = ctx->t.mb_height_C;
	uint8_t *p = ctx->samples_mb[1];
	ssize_t row = ctx->t.stride[1], cr = ctx->t.stride_Cr;
	if (ctx->t.px_shift) {
		hbd_intraChroma(p, cr, row, mode, ctx->t.samples_clip[1][0], h);
		return;
	}
	uint8_t buf[36 * 16] __attribute__((aligned(16))) = {};
	uint8_t *b = buf + 48; // rows of Cb at b + y * 32 and of Cr 16 bytes after
	for (int c = 0; c < 2; c++) {
		uint8_t *s = p + c * cr, *d = b + c * 16;
		if (!(ctx->unavail16x16 & 2))
			memcpy(d - 32, s - row, 8);
		if (!(ctx->unavail16x16 & 8))
			d[-33] = s[-row - 1];
		for (int y = 0; y < h && !(ctx->unavail16x16 & 1); y++)
			d[y * 32 - 1] = s[y * row - 1];
	}
	if (h == 8)
		decode_intraChroma(b, 16, mode, ctx->t.samples_clip_v[1]);
	else
		decode_intraChroma422(b, 16, mode, ctx->t.samples_clip_v[1]);
	for (int y = 0; y < h; y++) {
		memcpy(p + y * row, b + y * 32, 8);
		memcpy(p + cr + y * row, b + y * 32 + 16, 8);
	}
}
#endif



#if !CABAC
enum { MBAFF_A, MBAFF_B, MBAFF_C, MBAFF_D, MBAFF_CUR };
static int mbaff_locate(Edge264MvcContext *ctx, int nb, int yN, int maxH, int *bottom, int *yM);
static always_inline const Edge264MvcMacroblock *mbaff_located_mb(Edge264MvcContext *ctx, int pair, int bottom);

/**
 * With constrained_intra_pred_flag, the samples left of a macroblock of an
 * MBAFF frame may come from both macroblocks of the left pair, an Intra and
 * an Inter one, so their availability is set per row of samples: for the 4x4
 * or 8x8 blocks of the left column (A, and D from the row above them), and
 * for Intra16x16 and chroma prediction, which need them all.
 */
static noinline void mbaff_constrained_intra(Edge264MvcContext *ctx, int transform_size_8x8_flag) {
	static const int8_t blk4x4[4] = {0, 2, 8, 10}, blk8x8[2] = {0, 10};
	int inter[17] = {}, bot, yM, pair; // rows -1..15 left of the macroblock
	for (int y = -1; y < 16; y++) {
		if ((pair = mbaff_locate(ctx, y < 0 ? MBAFF_D : MBAFF_A, y, 16, &bot, &yM)) >= 0)
			inter[y + 1] = mbaff_located_mb(ctx, pair, bot)->mbIsInterFlag;
	}
	int inter16x16 = 0;
	for (int y = 1; y <= 16; y++)
		inter16x16 |= inter[y];
	int hC = ctx->t.mb_height_C, halves = 0;
	for (int y = 0; y < hC; y++) {
		if ((pair = mbaff_locate(ctx, MBAFF_A, y, hC, &bot, &yM)) >= 0)
			halves |= mbaff_located_mb(ctx, pair, bot)->mbIsInterFlag << (y >= hC / 2);
	}
	ctx->mbaff_left_halves = halves;
	int interBCD = mbB->mbIsInterFlag << 1 | mbC->mbIsInterFlag << 2 | mbD->mbIsInterFlag << 3;
	ctx->unavail_intra_v = block_unavailability[ctx->unavail16x16 | interBCD];
	ctx->unavail_intra16x16 = ctx->unavail_intra[0] | inter16x16;
	int n = transform_size_8x8_flag ? 2 : 4, rows = 16 / n;
	for (int i = 0; i < n; i++) {
		int b = transform_size_8x8_flag ? blk8x8[i] : blk4x4[i], a = 0;
		for (int y = 0; y < rows; y++)
			a |= inter[1 + i * rows + y];
		ctx->unavail_intra[b] |= a | inter[i * rows] << 3;
	}
}

/**
 * Chroma DC prediction (8.3.4.1 to 8.3.4.3) when only one half of the samples
 * left of the macroblock is available (see mbaff_constrained_intra), each
 * 4x4 block taking those of its rows.
 */
static noinline void mbaff_chroma_dc(Edge264MvcContext *ctx) {
	int hC = ctx->t.mb_height_C, px = ctx->t.px_shift;
	int availB = !(ctx->unavail_intra16x16 & 2);
	int availA[2] = {!(ctx->unavail16x16 & 1) && !(ctx->mbaff_left_halves & 1), !(ctx->unavail16x16 & 1) && !(ctx->mbaff_left_halves & 2)};
	ssize_t stride = ctx->t.stride[1];
	for (int c = 1; c < 3; c++) {
		uint8_t *s = ctx->samples_mb[c];
		#define SAMPLE(x, y) (px ? ((uint16_t *)(s + (y) * stride))[x] : s[(y) * stride + (x)])
		int dc[4][2];
		for (int yO = 0; yO < hC; yO += 4) {
			for (int xO = 0; xO < 8; xO += 4) {
				int sumT = 0, sumL = 0, a = availA[yO >= hC / 2];
				for (int i = 0; i < 4; i++) {
					sumT += availB ? SAMPLE(xO + i, -1) : 0;
					sumL += a ? SAMPLE(-1, yO + i) : 0;
				}
				int v = 1 << (ctz(ctx->t.samples_clip[c][0] + 1) - 1);
				if ((xO == 0) == (yO == 0)) {
					v = a && availB ? (sumT + sumL + 4) >> 3 : a ? (sumL + 2) >> 2 : availB ? (sumT + 2) >> 2 : v;
				} else if (yO == 0) {
					v = availB ? (sumT + 2) >> 2 : a ? (sumL + 2) >> 2 : v;
				} else {
					v = a ? (sumL + 2) >> 2 : availB ? (sumT + 2) >> 2 : v;
				}
				dc[yO >> 2][xO >> 2] = v;
			}
		}
		for (int y = 0; y < hC; y++) {
			for (int x = 0; x < 8; x++) {
				if (px)
					((uint16_t *)(s + y * stride))[x] = dc[y >> 2][x >> 2];
				else
					s[y * stride + x] = dc[y >> 2][x >> 2];
			}
		}
		#undef SAMPLE
	}
}
#endif



/**
 * Parses intra_chroma_pred_mode (9.3.2.2 and 9.3.3.1.1.8).
 */
static inline void CAFUNC(parse_intra_chroma_pred_mode)
{
	// indexed with the availability of A, B and D (bit 2), since Plane prediction
	// also reads p[-1,-1] (8.3.4.4): without D it falls back to DC from A and B,
	// as it does without A or B
	static const int8_t IntraChromaModes[4][8] = {
		{IC8x8_DC_8, IC8x8_DC_A_8, IC8x8_DC_B_8, IC8x8_DC_AB_8, IC8x8_DC_8, IC8x8_DC_A_8, IC8x8_DC_B_8, IC8x8_DC_AB_8},
		{IC8x8_H_8 , IC8x8_DC_A_8, IC8x8_H_8  , IC8x8_DC_AB_8, IC8x8_H_8 , IC8x8_DC_A_8, IC8x8_H_8  , IC8x8_DC_AB_8},
		{IC8x8_V_8 , IC8x8_V_8  , IC8x8_DC_B_8, IC8x8_DC_AB_8, IC8x8_V_8 , IC8x8_V_8  , IC8x8_DC_B_8, IC8x8_DC_AB_8},
		{IC8x8_P_8 , IC8x8_DC_A_8, IC8x8_DC_B_8, IC8x8_DC_AB_8, IC8x8_DC_8, IC8x8_DC_A_8, IC8x8_DC_B_8, IC8x8_DC_AB_8},
	};
	
	// Do not optimise too hard to keep the code understandable here.
	int type = ctx->t.ChromaArrayType;
	if (type == 1 || type == 2) {
		#if !CABAC
			int mode = get_ue16(&ctx->t.gb, 3);
		#else
			int ctxIdx = 64 + ctx->inc.intra_chroma_pred_mode_non_zero;
			int mode = 0;
			while (mode <3 && get_ae(ctx, ctxIdx))
				mode++, ctxIdx = 67;
			mb->f.intra_chroma_pred_mode_non_zero = (mode > 0);
		#endif
		log_mb(ctx, "%sintra_chroma_pred_mode: %u\n", ctx->log_indent, mode);
		int ic_mode = IntraChromaModes[mode][(ctx->unavail_intra16x16 & 3) | (ctx->unavail_intra16x16 >> 1 & 4)];
		if (__builtin_expect(ctx->t.MbaffFrameFlag & ctx->t.pps.constrained_intra_pred_flag, 0) && mode == 0 &&
			!(ctx->unavail16x16 & 1) && ctx->mbaff_left_halves && ctx->mbaff_left_halves != 3)
			mbaff_chroma_dc(ctx);
		else if (__builtin_expect(ctx->t.field_pic_flag, 0))
			decode_intraChroma_field(ctx, ic_mode);
		else if (type == 1)
			decode_intraChroma(ctx->samples_mb[1], ctx->t.stride[1] >> 1, ic_mode, ctx->t.samples_clip_v[1]);
		else
			decode_intraChroma422(ctx->samples_mb[1], ctx->t.stride[1] >> 1, ic_mode, ctx->t.samples_clip_v[1]);
	}
}



/**
 * Parses prev_intraNxN_pred_mode_flag and rem_intraNxN_pred_mode, and returns
 * the given intra_pred_mode (7.3.5.1, 7.4.5.1, 8.3.1.1 and table 9-34).
 */
static inline int CAFUNC(parse_intraNxN_pred_mode, int luma4x4BlkIdx)
{
	// dcPredModePredictedFlag is enforced by putting -2
	int intraMxMPredModeA = *((int8_t *)mbc->Intra4x4PredMode + ctx->A4x4_int8[luma4x4BlkIdx]);
	int intraMxMPredModeB = *((int8_t *)mbc->Intra4x4PredMode + ctx->B4x4_int8[luma4x4BlkIdx]);
	int mode = abs(min(intraMxMPredModeA, intraMxMPredModeB));
	int rem_intra_pred_mode = -1;
	if (CACOND(!get_u1(&ctx->t.gb), !get_ae(ctx, 68))) {
		#if !CABAC
			rem_intra_pred_mode = get_uv(&ctx->t.gb, 3);
		#else
			rem_intra_pred_mode = get_ae(ctx, 69);
			rem_intra_pred_mode += get_ae(ctx, 69) * 2;
			rem_intra_pred_mode += get_ae(ctx, 69) * 4;
		#endif
		mode = rem_intra_pred_mode + (rem_intra_pred_mode >= mode);
	}
	log_mb(ctx, luma4x4BlkIdx ? ",%d" : "%d", rem_intra_pred_mode);
	return mode;
}






/**
 * This function parses the syntax elements mb_type, transform_size_8x8_flag,
 * intraNxN_pred_mode (from function), intra_chroma_pred_mode (from function),
 * coded_block_pattern (from function) and PCM stuff for the current Intra
 * macroblock. It proceeds to residual decoding through tail calls.
 *
 * In Intra4x4PredMode the special value -2 is used by unavailable blocks and
 * Inter blocks with constrained_intra_pred_flag, to account for
 * dcPredModePredictedFlag.
 */
static noinline void CAFUNC(parse_I_mb, int mb_type_or_ctxIdx)
{
	static const uint8_t me_intra[64] = {174, 173, 172, 0, 45, 169, 165, 141, 44, 168, 164, 140, 46, 170, 166, 142, 1, 40, 36, 136, 132, 41, 37, 137, 133, 42, 38, 138, 134, 32, 8, 4, 128, 33, 9, 5, 129, 12, 160, 13, 161, 2, 34, 10, 6, 130, 14, 162,
		172, 0, 44, 168, 164, 140, 40, 36, 136, 132, 32, 8, 4, 128, 12, 160};
	
	// Intra-specific initialisations
	#if CABAC
		ctx->nC_inc_v[0] = ctx->nC_inc_v[1] = ctx->nC_inc_v[2] = (i8x16){};
		// the chroma blocks on the left and top edges, in 2x2 (4:2:0) or 2x4 (4:2:2) per component
		int c422 = ctx->t.ChromaArrayType == 2;
		if (ctx->unavail4x4[0] & 1) {
			mb->bits[1] |= 0x111111;
			ctx->nC_inc_v[0] += (i8x16){1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0};
			ctx->nC_inc_v[1] += c422 ? (i8x16){1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0} : (i8x16){1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
			ctx->inc.coded_block_flags_16x16_s |= 0x010101;
		}
		if (ctx->unavail4x4[0] & 2) {
			mb->bits[1] |= 0x424242;
			ctx->nC_inc_v[0] += (i8x16){2, 2, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
			ctx->nC_inc_v[1] += c422 ? (i8x16){2, 2, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0} : (i8x16){2, 2, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
			ctx->inc.coded_block_flags_16x16_s |= 0x020202;
		}
	#endif
	mb->mbIsInterFlag = 0;
	mb->f.inter_eqs_s = little_endian32(0x1b5fbbff);
	// With constrained_intra_pred_flag, intra prediction takes no samples from
	// Inter macroblocks (8.3.1.2, 8.3.2.2, 8.3.3, 8.3.4), unlike the other
	// processes that read the neighbours, so it gets an availability of its own.
	ctx->unavail_intra_v = ctx->unavail4x4_v[0];
	if (ctx->t.pps.constrained_intra_pred_flag) {
		int inter = mbA->mbIsInterFlag | mbB->mbIsInterFlag << 1 | mbC->mbIsInterFlag << 2 | mbD->mbIsInterFlag << 3;
		ctx->unavail_intra_v = block_unavailability[ctx->unavail16x16 | inter];
	}
	ctx->unavail_intra16x16 = ctx->unavail_intra[0];
	mb->refIdx_l = -1;
	mb->refPic_l = -1;
	mb->mvs_v[0] = mb->mvs_v[1] = mb->mvs_v[2] = mb->mvs_v[3] = mb->mvs_v[4] = mb->mvs_v[5] = mb->mvs_v[6] = mb->mvs_v[7] = (i16x8){};
	
	// I_NxN
	if (CACOND(mb_type_or_ctxIdx == 0, !get_ae(ctx, mb_type_or_ctxIdx))) {
		#if CABAC
			mb->f.mb_type_I_NxN = 1;
			log_mb(ctx, (mb_type_or_ctxIdx == 17) ? "%smb_type: 5\n" : // in P slice
				(mb_type_or_ctxIdx == 32) ? "%smb_type: 23\n" : // in B slice
				"%smb_type: 0\n", ctx->log_indent); // in I slice
		#endif
		
		// 7.3.5, 7.4.5, 9.3.3.1.1.10 and table 9-34
		int transform_size_8x8_flag = 0;
		if (ctx->t.pps.transform_8x8_mode_flag) {
			transform_size_8x8_flag = CACOND(get_u1(&ctx->t.gb), get_ae(ctx, 399 + ctx->inc.transform_size_8x8_flag));
			log_mb(ctx, "%stransform_size_8x8_flag: %u\n", ctx->log_indent, transform_size_8x8_flag);
		}
		
		if (__builtin_expect(ctx->t.MbaffFrameFlag & ctx->t.pps.constrained_intra_pred_flag, 0))
			mbaff_constrained_intra(ctx, transform_size_8x8_flag);
		mbc->Intra4x4PredMode_v = (i8x16){-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2}; // default values when A/B is unavailable
		if (transform_size_8x8_flag) {
			mb->f.transform_size_8x8_flag = transform_size_8x8_flag;
			log_mb(ctx, "%srem_intra8x8_pred_modes: [", ctx->log_indent);
			for (int i = 0; i < 4; i++)
				mbc->Intra4x4PredMode_s[i] = CACALL(parse_intraNxN_pred_mode, i * 4) * 0x01010101;
			log_mb(ctx, "]\n%sIntra8x8PredModes: [%u,%u,%u,%u]\n", ctx->log_indent,
				mbc->Intra4x4PredMode[0], mbc->Intra4x4PredMode[4], mbc->Intra4x4PredMode[8], mbc->Intra4x4PredMode[12]);
		} else {
			log_mb(ctx, "%srem_intra4x4_pred_modes: [", ctx->log_indent);
			for (int i = 0; i < 16; i++)
				mbc->Intra4x4PredMode[i] = CACALL(parse_intraNxN_pred_mode, i);
			log_mb(ctx, "]\n%sIntra4x4PredModes: [%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]\n", ctx->log_indent,
				mbc->Intra4x4PredMode[0], mbc->Intra4x4PredMode[1], mbc->Intra4x4PredMode[2], mbc->Intra4x4PredMode[3],
				mbc->Intra4x4PredMode[4], mbc->Intra4x4PredMode[5], mbc->Intra4x4PredMode[6], mbc->Intra4x4PredMode[7],
				mbc->Intra4x4PredMode[8], mbc->Intra4x4PredMode[9], mbc->Intra4x4PredMode[10], mbc->Intra4x4PredMode[11],
				mbc->Intra4x4PredMode[12], mbc->Intra4x4PredMode[13], mbc->Intra4x4PredMode[14], mbc->Intra4x4PredMode[15]);
		}
		
		CACALL(parse_intra_chroma_pred_mode);
		CACALL(parse_coded_block_pattern, me_intra);
		CAJUMP(parse_NxN_residual);
	
	// Intra_16x16
	} else if (__builtin_expect(CACOND(mb_type_or_ctxIdx < 25, !cabac_terminate(ctx)), 1)) {
		#if !CABAC
			int mb_type = mb_type_or_ctxIdx - 1;
			mb->bits[0] = mb_type > 11 ? 0xac : 0; // zeroes ref_idx_nz as byproduct
			mb_type = mb_type > 11 ? mb_type - 12 : mb_type;
			mb->f.CodedBlockPatternChromaDC = mb_type > 3;
			mb->f.CodedBlockPatternChromaAC = mb_type >> 3;
			int mode = mb_type & 3;
		#else
			int ctxIdx = max(mb_type_or_ctxIdx, 5);
			mb->bits[0] = get_ae(ctx, ctxIdx + 1) ? 0xac : 0; // zeroes ref_idx_nz as byproduct
			int CodedBlockPatternChromaDC = get_ae(ctx, ctxIdx + 2);
			ctxIdx = max(ctxIdx, 6);
			if (CodedBlockPatternChromaDC) {
				mb->f.CodedBlockPatternChromaDC = 1;
				mb->f.CodedBlockPatternChromaAC = get_ae(ctx, ctxIdx + 2);
			}
			int mode = get_ae(ctx, ctxIdx + 3) << 1;
			mode += get_ae(ctx, max(ctxIdx + 3, 10));
			log_mb(ctx, "%smb_type: %u\n", ctx->log_indent, (mb->bits[0] & 12) +
				(mb->f.CodedBlockPatternChromaDC + mb->f.CodedBlockPatternChromaAC) * 4 +
				mode + (ctxIdx == 17 ? 6 : ctxIdx == 32 ? 24 : 1));
		#endif
		// a mb_type coding chroma blocks is not allowed in 4:0:0 (7.4.5), which
		// has none to parse, as damaged streams code them (as the JM reference
		// decoder does, the bits are ignored)
		if (ctx->t.ChromaArrayType == 0)
			mb->f.CodedBlockPatternChromaDC = mb->f.CodedBlockPatternChromaAC = 0;

		// decode the samples before parsing residuals
		// indexed with the availability of A, B and D (bit 2), as Plane prediction
		// also reads p[-1,-1] (8.3.3.4), falling back to DC like the chroma modes
		static const int8_t Intra16x16Modes[4][8] = {
			{I16x16_V_8 , I16x16_V_8  , I16x16_DC_B_8, I16x16_DC_AB_8, I16x16_V_8 , I16x16_V_8  , I16x16_DC_B_8, I16x16_DC_AB_8},
			{I16x16_H_8 , I16x16_DC_A_8, I16x16_H_8  , I16x16_DC_AB_8, I16x16_H_8 , I16x16_DC_A_8, I16x16_H_8  , I16x16_DC_AB_8},
			{I16x16_DC_8, I16x16_DC_A_8, I16x16_DC_B_8, I16x16_DC_AB_8, I16x16_DC_8, I16x16_DC_A_8, I16x16_DC_B_8, I16x16_DC_AB_8},
			{I16x16_P_8 , I16x16_DC_A_8, I16x16_DC_B_8, I16x16_DC_AB_8, I16x16_DC_8, I16x16_DC_A_8, I16x16_DC_B_8, I16x16_DC_AB_8},
		};
		mbc->Intra4x4PredMode_v = (i8x16){2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
		if (__builtin_expect(ctx->t.MbaffFrameFlag & ctx->t.pps.constrained_intra_pred_flag, 0))
			mbaff_constrained_intra(ctx, 0);
		decode_intra16x16(ctx->samples_mb[0], ctx->t.stride[0], Intra16x16Modes[mode][(ctx->unavail_intra16x16 & 3) | (ctx->unavail_intra16x16 >> 1 & 4)], ctx->t.samples_clip_v[0]); // FIXME 4:4:4
		CACALL(parse_intra_chroma_pred_mode);
		CAJUMP(parse_Intra16x16_residual);
		
	// I_PCM
	} else {
		#if !CABAC
			unsigned bits = (SIZE_BIT - 1 - ctz(ctx->t.gb.lsb_cache)) & 7;
			ctx->t.gb.msb_cache = shld(ctx->t.gb.lsb_cache, ctx->t.gb.msb_cache, bits);
			ctx->t.gb.lsb_cache = ctx->t.gb.lsb_cache << bits;
		#else
			log_mb(ctx, (mb_type_or_ctxIdx == 17) ? "%smb_type: 30\n" :
				(mb_type_or_ctxIdx == 32) ? "%smb_type: 48\n" :
				"%smb_type: 25\n", ctx->log_indent);
		#endif
		
		ctx->mb_qp_delta_nz = 0;
		mb->f.v |= flags_twice.v; // ChromaDC, ChromaAC and flags_16x16, just what we need :)
		int QP0 = ctx->t.QpBdOffset_Y; // QP'Y of QPY 0, which deblocking takes for I_PCM (8.7.2.2)
		mb->QP_s = (i8x4){QP0, ctx->QP_C[0][QP0], ctx->QP_C[1][QP0], ctx->t.QP[3]};
		mb->bits_l = (uint64_t)(i32x2){0xac, 0xacacac}; // FIXME 4:2:2
		mbc->nC_v[0] = mbc->nC_v[1] = mbc->nC_v[2] = CACOND(
			((i8x16){16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16}),
			((i8x16){1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));
		mbc->Intra4x4PredMode_v = (i8x16){2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
		
		// PCM is so rare that it should be compact rather than fast
		int MbWidth = 16, y = 16;
		log_mb(ctx, "%spcm_samples:\n"
			"%s  bits_Y: %u\n"
			"%s  bits_C: %u\n",
			ctx->log_indent,
			ctx->log_indent, ctz(ctx->t.samples_clip[0][0] + 1),
			ctx->log_indent, ctz(ctx->t.samples_clip[1][0] + 1));
		for (int iYCbCr = 0; iYCbCr < 3; iYCbCr++) {
			log_mb(ctx, "%s  %s: [", ctx->log_indent, (const char *)"Y\0\0Cb\0Cr" + iYCbCr * 3);
			int BitDepth = ctz(ctx->t.samples_clip[iYCbCr][0] + 1);
			for (uint8_t *p = ctx->samples_mb[iYCbCr]; y-- > 0; p += ctx->t.stride[iYCbCr]) {
				if (BitDepth == 8) {
					((uint32_t *)p)[0] = big_endian32(get_uv(&ctx->t.gb, 32));
					((uint32_t *)p)[1] = big_endian32(get_uv(&ctx->t.gb, 32));
					log_mb(ctx, "%u,%u,%u,%u,%u,%u,%u,%u,", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					if (MbWidth == 16) {
						((uint32_t *)p)[2] = big_endian32(get_uv(&ctx->t.gb, 32));
						((uint32_t *)p)[3] = big_endian32(get_uv(&ctx->t.gb, 32));
						log_mb(ctx, "%u,%u,%u,%u,%u,%u,%u,%u,", p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
					}
				} else for (int x = 0; x < MbWidth; x++) {
					((uint16_t *)p)[x] = get_uv(&ctx->t.gb, BitDepth);
					log_mb(ctx, "%u,", ((uint16_t *)p)[x]);
				}
			}
			log_mb(ctx, "]\n");
			MbWidth = (ctx->t.ChromaArrayType < 3) ? 8 : 16;
			y = (int8_t[4]){0, 8, 16, 16}[ctx->t.ChromaArrayType];
		}
		#if CABAC
			cabac_start(ctx);
		#endif
	}
}



/**
 * This function is the entry point to residual parsing in Inter macroblocks.
 * It parses coded_block_pattern and transform_size_8x8_flag, that are parsed
 * in different orders than Intra macroblocks.
 */
static noinline void CAFUNC(parse_inter_residual)
{
	static const uint8_t me_inter[64] = {0, 1, 32, 8, 4, 128, 2, 40, 36, 136, 132, 172, 174, 44, 168, 164, 140, 12, 160, 173, 42, 38, 138, 134, 34, 10, 6, 130, 46, 170, 166, 142, 33, 9, 5, 129, 41, 37, 137, 133, 45, 169, 165, 141, 13, 161, 14, 162,
		0, 32, 8, 4, 128, 40, 36, 136, 132, 172, 44, 168, 164, 140, 12, 160};
	CACALL(parse_coded_block_pattern, me_inter);
	
	if ((mb->bits[0] & 0xac) && ctx->transform_8x8_mode_flag) {
		mb->f.transform_size_8x8_flag = CACOND(get_u1(&ctx->t.gb), get_ae(ctx, 399 + ctx->inc.transform_size_8x8_flag));
		log_mb(ctx, "%stransform_size_8x8_flag: %u\n", ctx->log_indent, mb->f.transform_size_8x8_flag);
	}
	
	#if CABAC
		ctx->nC_inc_v[0] = ctx->nC_inc_v[1] = ctx->nC_inc_v[2] = (i8x16){};
	#endif
	CAJUMP(parse_NxN_residual);
}



/**
 * Parse both components of a motion vector (7.3.5.1, 7.4.5.1, 9.3.2.3,
 * 9.3.3.1.1.7 and tables 9-34 and 9-39).
 * 
 * As with residual coefficients in CABAC, bypass bits can be extracted all at
 * once using a binary division. The longest mvd value is -2^15, i.e 27 bits as
 * Exp-Golomb, so we need a single division on 64-bit machines and two on
 * 32-bit machines.
 */
#if !CABAC
	static inline i16x8 parse_mvd_pair_cavlc(Edge264MvcContext *ctx, const uint8_t *absMvd_lx, int i4x4) {
		int x = get_se32(&ctx->t.gb, -32768, 32767);
		int y = get_se32(&ctx->t.gb, -32768, 32767);
		log_mb(ctx, "[%d,%d],", x, y);
		return (i16x8){x, y};
	}
#else
	static noinline i16x8 parse_mvd_pair_cabac(Edge264MvcContext *ctx, const uint8_t *absMvd_lx, int i4x4) {
		i16x8 res;
		for (int ctxBase = 40, i = 0;;) {
			int sum = absMvd_lx[ctx->absMvd_A[i4x4] + i] + absMvd_lx[ctx->absMvd_B[i4x4] + i];
			int ctxIdx = ctxBase + (sum >= 3) + (sum > 32);
			int mvd = 0;
			ctxBase += 3;
			while (mvd != 9 && get_ae_inline(ctx, ctxIdx))
				ctxIdx = ctxBase + min(mvd++, 3);
			if (mvd == 9) {
				#if SIZE_BIT == 32
					// we need at least 21 bits in offset to get 12 bypass bits (max prefix size)
					int zeros = clz(ctx->t.gb.range);
					if (zeros > 32 - 21)
						zeros = renorm_bits(ctx, zeros);
					unsigned range = ctx->t.gb.range >> (32 - 9 - zeros); // maximum shift to get as many bypass bits as possible
					unsigned quo = ctx->t.gb.offset / range; // contains 32-9-zeros bypass bits in lsb
					unsigned rem = ctx->t.gb.offset % range;
					int k = 3 + clz(~quo << (zeros + 9) | 1 << (32 - 12));
					int unused = 32 - 9 - zeros - k * 2 + 1;
					if (__builtin_expect(unused < 0, 0)) {
						unsigned ref = get_bytes(&ctx->t.gb, 3);
						unsigned div = shld(ref, rem, -unused);
						quo = shld((div / range) << (32 + unused), quo, -unused);
						rem = shld(ref << -unused, div % range, 24 + unused);
						range <<= 24 + unused;
						unused = 0;
					}
					mvd = 1 + (1 << k) + (quo >> unused >> 1 & ((1 << k) - 1));
					mvd = (quo & 1 << unused) ? -mvd : mvd;
					ctx->t.gb.offset = (quo & ((1 << unused) - 1)) * range + rem;
					ctx->t.gb.range = range << unused;
				#else
					// we need at least 36 bits in offset to get 27 bypass bits (max code size)
					int zeros = clz(ctx->t.gb.range);
					if (zeros > 64 - 36)
						zeros = renorm_bits(ctx, zeros);
					size_t range = ctx->t.gb.range >> 27; // shift to get exactly 27 bypass bits
					size_t quo = ctx->t.gb.offset / range; // contains 27 bypass bits in lsb
					size_t rem = ctx->t.gb.offset % range;
					int k = 3 + clz(~quo << (64 - 27) | (size_t)1 << (64 - 12));
					int unused = 27 - k * 2 + 1;
					mvd = 1 + (1 << k) + ((unsigned)(quo >> unused >> 1) & ((1 << k) - 1));
					mvd = (quo & (size_t)1 << unused) ? -mvd : mvd;
					ctx->t.gb.offset = (quo & (((size_t)1 << unused) - 1)) * range + rem;
					ctx->t.gb.range = range << unused;
				#endif
			} else if (mvd > 0) {
				mvd = get_bypass(ctx) ? -mvd : mvd;
			}
			
			if (++i == 2) {
				res[1] = mvd;
				log_mb(ctx, "[%d,%d],", res[0], res[1]);
				return res;
			}
			ctxBase = 47;
			res = (i16x8){mvd};
		}
	}
#endif



/**
 * Parse all ref_idx_lx in a given macroblock (9.3.3.1.1.6).
 * 
 * f is a bitmask for indices of symbols that should be parsed. These values
 * are then broadcast to other positions according to the inferred block
 * shapes, unless bit 8 is set (to signal Direct_8x8).
 * This function also clips all values to valid ones.
 * 
 * This function may be safely used as noinline since it is never called in the
 * middle of a stateful computation.
 */
static noinline void CAFUNC(parse_ref_idx, unsigned f) {
	u8x16 v = {f, f, f, f, f, f, f, f, f, f, f, f, f, f, f, f};
	u8x16 bits = {1, 2, 4, 8, 16, 32, 64, 128};
	mb->refIdx_l = ((i64x2){mb->refIdx_l} & ~(i64x2)((v & bits) == bits))[0]; // set to 0 if parsed
	log_mb(ctx, "%sref_idx: {", ctx->log_indent);
	for (unsigned u = f & ctx->num_ref_idx_mask; u; u &= u - 1) {
		int i = __builtin_ctz(u);
		int ref_idx = 0;
		#if !CABAC
			if (ctx->clip_ref_idx[i] == 1)
				ref_idx = get_u1(&ctx->t.gb) ^ 1;
			else
				ref_idx = get_ue16(&ctx->t.gb, ctx->clip_ref_idx[i]);
		#else
			if (get_ae(ctx, 54 + (mb->bits[0] >> inc8x8[4 + i] & 3))) {
				ref_idx = 1;
				mb->bits[0] |= 1 << bit8x8[4 + i];
				int ctxIdx = 58;
				while (ref_idx < 32 && get_ae(ctx, ctxIdx))
					ref_idx++, ctxIdx = 59;
			}
		#endif
		mb->refIdx[i] = ref_idx;
		log_mb(ctx, "\"%u\":%u,", i, ref_idx);
	}
	log_mb(ctx, "}\n");
	
	// clip and broadcast the values
	i8x16 refIdx_v = (i64x2){mb->refIdx_l};
	#if CABAC
		refIdx_v = min8(refIdx_v, (i64x2){(int64_t)ctx->clip_ref_idx_v});
	#endif
	if (!(f & 0x122)) { // 16xN
		refIdx_v = shuffle(refIdx_v, ((i8x16){0, 0, 2, 2, 4, 4, 6, 6, -1, -1, -1, -1, -1, -1, -1, -1}));
		#if CABAC
			mb->bits[0] |= (mb->bits[0] >> 2 & 0x080800) | (mb->bits[0] << 5 & 0x808000);
		#endif
	}
	if (!(f & 0x144)) { // Nx16
		refIdx_v = shuffle(refIdx_v, ((i8x16){0, 1, 0, 1, 4, 5, 4, 5, -1, -1, -1, -1, -1, -1, -1, -1}));
		#if CABAC
			mb->bits[0] |= (mb->bits[0] >> 3 & 0x040400) | (mb->bits[0] << 4 & 0x808000);
		#endif
	}
	mb->refIdx_l = ((i64x2)refIdx_v)[0];
	
	// compute reference picture numbers
	mb->refPic_s[0] = ((i32x4)shufflen(ctx->t.RefPicList_v[0], refIdx_v))[0];
	mb->refPic_s[1] = ((i32x4)shufflen(ctx->t.RefPicList_v[2], refIdx_v))[1];
}



/**
 * Parse sub_mb_type in a B macroblock.
 * 
 * This function is distinct from parse_B_mb to allow different inlinings.
 * For each 8x8 block we test for B_Direct_8x8, otherwise we extract a
 * bitstring and initialize the loop flags with it. Values for CABAC are:
 * _ 0 = B_Bi_8x8
 * _ 1 = B_L0_8x4
 * _ 2 = B_L0_4x8
 * _ 3 = B_L1_8x4
 * _ 4 = B_L0_8x8
 * _ 5 = B_L1_8x8
 * _ 6 = B_L1_4x4
 * _ 7 = B_Bi_4x4
 * _ 8 = B_L1_4x8
 * _ 9 = B_Bi_8x4
 * _ 10 = B_Bi_4x8
 * _ 11 = B_L0_4x4
 */
static void CAFUNC(parse_B_sub_mb) {
	
	// initializations for sub_mb_type
	log_mb(ctx, "%ssub_mb_types: [", ctx->log_indent);
	unsigned mvd_flags = 0;
	mb->f.inter_eqs_s = 0;
	for (int i8x8 = 0; i8x8 < 4; i8x8++) {
		int i4x4 = i8x8 * 4;
		#if !CABAC
			int sub_mb_type = get_ue16(&ctx->t.gb, 12);
			log_mb(ctx, (i8x8 < 3) ? "%u," : "%u]\n", sub_mb_type);
		#endif
		if (CACOND(sub_mb_type == 0, !get_ae(ctx, 36))) { // B_Direct_8x8
			mb->f.inter_eqs[i8x8] = 0x1b;
			if (!ctx->t.direct_8x8_inference_flag)
				ctx->transform_8x8_mode_flag = 0;
			#if CABAC
				log_mb(ctx, (i8x8 < 3) ? "0," : "0]\n");
			#endif
		} else {
			#if !CABAC
				static const uint32_t sub_mb_type2flags[13] = {0, 0x00001, 0x10000,
					0x10001, 0x00005, 0x00003, 0x50000, 0x30000, 0x50005, 0x30003,
					0x0000f, 0xf0000, 0xf000f};
				static const uint8_t sub_mb_type2eqs[13] = {0, 0x1b, 0x1b, 0x1b,
					0x11, 0x0a, 0x11, 0x0a, 0x11, 0x0a, 0, 0, 0};
				mvd_flags |= sub_mb_type2flags[sub_mb_type] << i4x4;
				mb->f.inter_eqs[i8x8] = sub_mb_type2eqs[sub_mb_type];
			#else
				int sub = 2;
				if (!get_ae(ctx, 37) || (sub = get_ae(ctx, 38),
					sub += sub + get_ae(ctx, 39),
					sub += sub + get_ae(ctx, 39), sub - 4 < 2u))
				{
					sub += sub + get_ae(ctx, 39);
				}
				static const uint32_t sub2flags[12] = {0x10001, 0x00005, 0x00003, 0x50000,
					0x00001, 0x10000, 0xf0000, 0xf000f, 0x30000, 0x50005, 0x30003, 0x0000f};
				static const uint8_t sub2eqs[12] = {0x1b, 0x11, 0x0a, 0x11, 0x1b,
					0x1b, 0, 0, 0x0a, 0x11, 0x0a, 0};
				mvd_flags |= sub2flags[sub] << i4x4;
				mb->f.inter_eqs[i8x8] = sub2eqs[sub];
				static const uint8_t sub2mb_type[13] = {3, 4, 5, 6, 1, 2, 11, 12, 7, 8, 9, 10, 0};
				log_mb(ctx, (i8x8 < 3) ? "%u," : "%u]\n", sub2mb_type[sub]);
			#endif
			if (CACOND(0x015f & 1 << sub_mb_type, 0x23b & 1 << sub)) { // 8xN
				ctx->unavail4x4[i4x4] = (ctx->unavail4x4[i4x4] & 11) | (ctx->unavail4x4[i4x4 + 1] & 4);
				ctx->unavail4x4[i4x4 + 2] |= 4;
				ctx->refIdx4x4_C[i4x4] = 0x0d63 >> i4x4 & 15;
				ctx->mvs_C[i4x4] = ctx->mvs_C[i4x4 + 1];
				if (CACOND(0x1ff0 & 1 << sub_mb_type, 0xfce & 1 << sub))
					ctx->transform_8x8_mode_flag = 0;
			} else { // 4xN
				ctx->refIdx4x4_C[i4x4] = 0xdc32 >> i4x4 & 15;
				ctx->mvs_C[i4x4] = ctx->mvs_B[i4x4 + 1];
				ctx->transform_8x8_mode_flag = 0;
			}
		}
	}
	
	// initialize direct prediction then parse all ref_idx values
	unsigned direct_flags = (~((mvd_flags & 0xffff) | mvd_flags >> 16) & 0x1111) * 0xf000f;
	if (direct_flags != 0)
		decode_direct_mv_pred(ctx, direct_flags);
	if (mvd_flags == 0) // yes there are streams that use 4 Direct8x8 instead of one Direct16x16
		CAJUMP(parse_inter_residual);
	CACALL(parse_ref_idx, 0x100 | mvd_flags2ref_idx(mvd_flags));
	
	// load neighbouring refIdx values and shuffle them into A/B/C/D
	i8x16 BC = (i64x2){(int64_t)mbB->refIdx_l, (int64_t)mbC->refIdx_l};
	i8x16 Ar = (i64x2){(int64_t)mbA->refIdx_l, (int64_t)mb->refIdx_l};
	i8x16 BCAr0 = unziplo32(BC, Ar);
	i8x16 BCAr1 = unziphi32(BC, Ar);
	i8x16 r0 = shuffle(BCAr0, ((i8x16){12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15}));
	i8x16 r1 = shuffle(BCAr1, ((i8x16){12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15}));
	i8x16 A0 = shuffle(BCAr0, ((i8x16){9, 12, 9, 12, 12, 13, 12, 13, 11, 14, 11, 14, 14, 15, 14, 15}));
	i8x16 A1 = shuffle(BCAr1, ((i8x16){9, 12, 9, 12, 12, 13, 12, 13, 11, 14, 11, 14, 14, 15, 14, 15}));
	i8x16 B0 = shuffle(BCAr0, ((i8x16){2, 2, 12, 12, 3, 3, 13, 13, 12, 12, 14, 14, 13, 13, 15, 15}));
	i8x16 B1 = shuffle(BCAr1, ((i8x16){2, 2, 12, 12, 3, 3, 13, 13, 12, 12, 14, 14, 13, 13, 15, 15}));
	i8x16 C0 = shuffle(BCAr0, ctx->refIdx4x4_C_v);
	i8x16 C1 = shuffle(BCAr1, ctx->refIdx4x4_C_v);
	i8x16 D0 = shuffle(BCAr0, ((i8x16){-1, 2, 9, 12, 2, 3, 12, 13, 9, 12, 11, 14, 12, 13, 14, 15}));
	i8x16 D1 = shuffle(BCAr1, ((i8x16){-1, 2, 9, 12, 2, 3, 12, 13, 9, 12, 11, 14, 12, 13, 14, 15}));
	D0[0] = mbD->refIdx[3];
	D1[0] = mbD->refIdx[7];
	if (__builtin_expect(ctx->t.MbaffFrameFlag, 0)) { // the D locations of the left column (see mbaff_neighbours)
		const Edge264MvcMacroblock *AD = &ctx->mbaff_nb[5];
		D0[2] = AD->refIdx[0], D0[8] = AD->refIdx[1], D0[10] = AD->refIdx[2];
		D1[2] = AD->refIdx[4], D1[8] = AD->refIdx[5], D1[10] = AD->refIdx[6];
		A0[2] = ctx->mbaff_refIdxA[0], A0[10] = ctx->mbaff_refIdxA[1];
		A1[2] = ctx->mbaff_refIdxA[2], A1[10] = ctx->mbaff_refIdxA[3];
	}
	
	// combine them into a vector of 4-bit equality masks
	union { int8_t q[32]; i8x16 v[2]; } refIdx4x4_eq;
	i8x16 u = ctx->unavail4x4_v[0];
	i8x16 uC = u & 4;
	refIdx4x4_eq.v[0] = (uC - ifelse_mask(uC==4, r0==D0, r0==C0) * 2 - (r0==B0)) * 2 - (r0==A0 | u==14);
	refIdx4x4_eq.v[1] = (uC - ifelse_mask(uC==4, r1==D1, r1==C1) * 2 - (r1==B1)) * 2 - (r1==A1 | u==14);
	
	// loop on mvs
	log_mb(ctx, "%smvds: [", ctx->log_indent);
	do {
		int i = __builtin_ctz(mvd_flags);
		int i4x4 = i & 15;
		mb->mvs_s[i] = 0; // value pointed to when A/B/C/D are unavailable
		uint8_t *absMvd_p = mbc->absMvd + (i & 16) * 2;
		i16x8 mvd = CACALL(parse_mvd_pair, absMvd_p, i4x4);
		
		// branch on equality mask
		int32_t *mvs_p = mb->mvs_s + (i & 16);
		i16x8 mvp;
		int eq = refIdx4x4_eq.q[i];
		int mvs_DC = eq & 8 ? ctx->mvs_D[i4x4] : ctx->mvs_C[i4x4];
		if (__builtin_expect(0xe9e9 >> eq & 1, 1)) {
			i16x8 mvA = (i32x4){mvs_p[ctx->mvs_A[i4x4]]};
			i16x8 mvB = (i32x4){mvs_p[ctx->mvs_B[i4x4]]};
			i16x8 mvDC = (i32x4){mvs_p[mvs_DC]};
			mvp = median16(mvA, mvB, mvDC);
		} else {
			int mvs_AB = eq & 1 ? ctx->mvs_A[i4x4] : ctx->mvs_B[i4x4];
			mvp = (i32x4){mvs_p[eq & 4 ? mvs_DC : mvs_AB]};
		}
		
		// broadcast absMvd and mvs to memory then call decoding
		static const int8_t masks[16] = {0, 15, 10, 5, 12, 3, 0, 0, 8, 0, 0, 0, 4, 0, 2, 1};
		static const int8_t widths[16] = {0, 8, 4, 4, 8, 8, 0, 0, 4, 0, 0, 0, 4, 0, 4, 4};
		static const int8_t heights[16] = {0, 8, 8, 8, 4, 4, 0, 0, 4, 0, 0, 0, 4, 0, 4, 4};
		int type = mvd_flags >> (i & -4) & 15;
		int m = masks[type];
		int i8x8 = i >> 2;
		i16x8 bits = {1, 2, 4, 8};
		i16x8 absMvd_mask = ((i16x8){m, m, m, m, m, m, m, m} & bits) == bits;
		i16x8 absMvd_old = (i64x2){mbc->absMvd_l[i8x8]};
		i16x8 mvs_mask = ziplo16(absMvd_mask, absMvd_mask);
		i32x4 mv = mvp + mvd;
		i16x8 mvs = broadcast32(mv, 0);
		mbc->absMvd_l[i8x8] = ((i64x2)ifelse_mask(absMvd_mask, pack_absMvd(mvd), absMvd_old))[0];
		mb->mvs_v[i8x8] = ifelse_mask(mvs_mask, mvs, mb->mvs_v[i8x8]);
		decode_inter(ctx, i, widths[type], heights[type]);
	} while (mvd_flags &= mvd_flags - 1);
	log_mb(ctx, "]\n");
	CAJUMP(parse_inter_residual);
}



/**
 * Parse mb_skip_flag and mb_type in a B macroblock.
 * 
 * Quick tests are done for B_Skip and B_Direct_16x16, otherwise we extract a
 * bitstring and use it to branch to further parsing. Values for CABAC are:
 * _ 0 = B_Bi_16x16
 * _ 1 = B_L0_L0_16x8
 * _ 2 = B_L0_L0_8x16
 * _ 3 = B_L1_L1_16x8
 * _ 4 = B_L1_L1_8x16
 * _ 5 = B_L0_L1_16x8
 * _ 6 = B_L0_L1_8x16
 * _ 7 = B_L1_L0_16x8
 * _ 8 = B_L0_16x16
 * _ 9 = B_L1_16x16
 * _ 13 = Intra
 * _ 14 = B_L1_L0_8x16
 * _ 15 = B_8x8
 * _ 16 = B_L0_Bi_16x8
 * _ 17 = B_L0_Bi_8x16
 * _ 18 = B_L1_Bi_16x8
 * _ 19 = B_L1_Bi_8x16
 * _ 20 = B_Bi_L0_16x8
 * _ 21 = B_Bi_L0_8x16
 * _ 22 = B_Bi_L1_16x8
 * _ 23 = B_Bi_L1_8x16
 * _ 24 = B_Bi_Bi_16x8
 * _ 25 = B_Bi_Bi_8x16
 */
static void CAFUNC(parse_B_mb)
{
	// Inter initializations, with constrained_intra_pred_flag sending the intra
	// modes of the neighbours to DC (dcPredModePredictedFlag, 8.3.1.1)
	mb->mbIsInterFlag = 1;
	mbc->Intra4x4PredMode_v = set8(ctx->t.pps.constrained_intra_pred_flag ? -2 : 2);
	#if CABAC
		mbc->absMvd_v[0] = mbc->absMvd_v[1] = mbc->absMvd_v[2] = mbc->absMvd_v[3] = (i8x16){};
	#endif
	mb->mvs_v[0] = mb->mvs_v[1] = mb->mvs_v[2] = mb->mvs_v[3] = mb->mvs_v[4] = mb->mvs_v[5] = mb->mvs_v[6] = mb->mvs_v[7] = (i16x8){};
	
	// parse mb_skip_run/flag
	#if !CABAC
		if (ctx->mb_skip_run < 0) {
			ctx->mb_skip_run = get_ue32(&ctx->t.gb, 139264);
			log_mb(ctx, "%smb_skip_run: %u\n", ctx->log_indent, ctx->mb_skip_run);
		}
		int mb_skip_flag = ctx->mb_skip_run-- > 0;
	#else
		// (an MBAFF frame may have parsed it ahead, see parse_slice_data_mbaff)
		int mb_skip_flag = ctx->mbaff_skip >= 0 ? ctx->mbaff_skip : get_ae(ctx, 26 - ctx->inc.mb_skip_flag);
		log_mb(ctx, "%smb_skip_flag: %u\n", ctx->log_indent, mb_skip_flag);
	#endif
	
	// earliest handling for B_Skip
	if (mb_skip_flag) {
		#if CABAC
			mb->f.mb_skip_flag = 1;
			mb->f.mb_type_B_Direct = 1;
			ctx->mb_qp_delta_nz = 0;
		#endif
		mb->f.inter_eqs_s = 0;
		decode_direct_mv_pred(ctx, 0xffffffff);
		return;
	}
		
	// B_Direct_16x16
	#if !CABAC
		int mb_type = get_ue16(&ctx->t.gb, 48);
	#endif
	if (CACOND(mb_type == 0, !get_ae(ctx, 29 - ctx->inc.mb_type_B_Direct))) {
		log_mb(ctx, "%smb_type: 0\n", ctx->log_indent);
		#if CABAC
			mb->f.mb_type_B_Direct = 1;
		#endif
		ctx->transform_8x8_mode_flag = ctx->t.pps.transform_8x8_mode_flag & ctx->t.direct_8x8_inference_flag;
		mb->f.inter_eqs_s = 0;
		decode_direct_mv_pred(ctx, 0xffffffff);
		CAJUMP(parse_inter_residual);
	}
	ctx->transform_8x8_mode_flag = ctx->t.pps.transform_8x8_mode_flag;
	
	// initializations and jumps for mb_type
	#if !CABAC
		log_mb(ctx, "%smb_type: %u\n", ctx->log_indent, mb_type);
		if (mb_type > 22)
			CAJUMP(parse_I_mb, mb_type - 23);
		mb->refIdx_l = -1;
		mb->mvs_v[0] = mb->mvs_v[1] = mb->mvs_v[2] = mb->mvs_v[3] = mb->mvs_v[4] = mb->mvs_v[5] = mb->mvs_v[6] = mb->mvs_v[7] = (i16x8){};
		if (mb_type == 22)
			CAJUMP(parse_B_sub_mb);
		static const uint8_t mb_type2flags[22] = {0, 0x01, 0x10, 0x11, 0x05, 0x03,
			0x50, 0x30, 0x41, 0x21, 0x14, 0x12, 0x45, 0x23, 0x54, 0x32, 0x15, 0x13,
			0x51, 0x31, 0x55, 0x33};
		int flags8x8 = mb_type2flags[mb_type];
	#else
		int str = 4;
		if (!get_ae(ctx, 30) || (str = get_ae(ctx, 31),
			str += str + get_ae(ctx, 32),
			str += str + get_ae(ctx, 32),
			str += str + get_ae(ctx, 32), str - 8 < 5u))
		{
			str += str + get_ae(ctx, 32);
		}
		if (str == 13)
			CAJUMP(parse_I_mb, 32);
		static const uint8_t str2mb_type[26] = {3, 4, 5, 6, 7, 8, 9, 10, 1, 2, 0, 0,
			0, 0, 11, 22, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
		log_mb(ctx, "%smb_type: %u\n", ctx->log_indent, str2mb_type[str]);
		mb->refIdx_l = -1;
		mb->mvs_v[0] = mb->mvs_v[1] = mb->mvs_v[2] = mb->mvs_v[3] = mb->mvs_v[4] = mb->mvs_v[5] = mb->mvs_v[6] = mb->mvs_v[7] = (i16x8){};
		if (str == 15)
			CAJUMP(parse_B_sub_mb);
		static const uint8_t str2flags[26] = {0x11, 0x05, 0x03, 0x50, 0x30, 0x41,
			0x21, 0x14, 0x01, 0x10, 0, 0, 0, 0, 0x12, 0, 0x45, 0x23, 0x54, 0x32,
			0x15, 0x13, 0x51, 0x31, 0x55, 0x33};
		int flags8x8 = str2flags[str];
	#endif
	
	// decoding large blocks
	CACALL(parse_ref_idx, flags8x8);
	log_mb(ctx, "%smvds: [", ctx->log_indent);
	if (!(flags8x8 & 0xee)) { // 16x16
		mb->f.inter_eqs_s = little_endian32(0x1b5fbbff);
		if (flags8x8 & 0x01) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd, 0);
			decode_inter_16x16(ctx, mvd, 0);
		}
		if (flags8x8 & 0x10) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd + 32, 0);
			decode_inter_16x16(ctx, mvd, 1);
		}
	} else if (!(flags8x8 & 0xcc)) { // 8x16
		mb->f.inter_eqs_s = little_endian32(0x1b1bbbbb);
		if (flags8x8 & 0x01) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd, 0);
			decode_inter_8x16_left(ctx, mvd, 0);
		}
		if (flags8x8 & 0x02) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd, 4);
			decode_inter_8x16_right(ctx, mvd, 0);
		}
		if (flags8x8 & 0x10) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd + 32, 0);
			decode_inter_8x16_left(ctx, mvd, 1);
		}
		if (flags8x8 & 0x20) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd + 32, 4);
			decode_inter_8x16_right(ctx, mvd, 1);
		}
	} else { // 16x8
		mb->f.inter_eqs_s = little_endian32(0x1b5f1b5f);
		if (flags8x8 & 0x01) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd, 0);
			decode_inter_16x8_top(ctx, mvd, 0);
		}
		if (flags8x8 & 0x04) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd, 8);
			decode_inter_16x8_bottom(ctx, mvd, 0);
		}
		if (flags8x8 & 0x10) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd + 32, 0);
			decode_inter_16x8_top(ctx, mvd, 1);
		}
		if (flags8x8 & 0x40) {
			i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd + 32, 8);
			decode_inter_16x8_bottom(ctx, mvd, 1);
		}
	}
	log_mb(ctx, "]\n");
	CAJUMP(parse_inter_residual);
}



/**
 * Parse sub_mb_type in a P macroblock.
 * 
 * This function is distinct from parse_P_mb to allow different inlinings.
 * For each 8x8 block we fill a bitmask for the indices at which mvs will be
 * parsed, then we loop on these bits and broadcast the values accordingly.
 */
static void CAFUNC(parse_P_sub_mb, unsigned ref_idx_flags)
{
	// initializations for sub_mb_type
	log_mb(ctx, "%ssub_mb_types: [", ctx->log_indent);
	mb->mvs_v[0] = mb->mvs_v[1] = mb->mvs_v[2] = (i16x8){}; // values pointed to when A/B/C/D are unavailable
	mb->f.inter_eqs_s = 0;
	unsigned mvd_flags = 0;
	for (int i8x8 = 0; i8x8 < 4; i8x8++) {
		int i4x4 = i8x8 * 4;
		int flags = 1;
		int eqs = 0x1b;
		#if !CABAC
			int sub_mb_type = get_ue16(&ctx->t.gb, 3);
		#endif
		if (CACOND(sub_mb_type == 0, get_ae(ctx, 21)) || // 8x8
			(ctx->transform_8x8_mode_flag = 0, flags = 5, eqs = 0x11, CACOND(sub_mb_type == 1, !get_ae(ctx, 22)))) { // 8x4
			ctx->unavail4x4[i4x4] = (ctx->unavail4x4[i4x4] & 11) | (ctx->unavail4x4[i4x4 + 1] & 4);
			ctx->unavail4x4[i4x4 + 2] |= 4;
			ctx->refIdx4x4_C[i4x4] = 0x0d63 >> i4x4 & 15;
			ctx->mvs_C[i4x4] = ctx->mvs_C[i4x4 + 1];
		} else { // 4xN
			ctx->refIdx4x4_C[i4x4] = 0xdc32 >> i4x4 & 15;
			ctx->mvs_C[i4x4] = ctx->mvs_B[i4x4 + 1];
			if (CACOND(sub_mb_type == 2, get_ae(ctx, 23)))
				flags = 3, eqs = 0x0a;
			else
				flags = 15, eqs = 0;
		}
		mvd_flags |= flags << i4x4;
		mb->f.inter_eqs[i8x8] = eqs;
		log_mb(ctx, (i8x8 < 3) ? "%u," : "%u]\n", 0xc0000480 >> flags * 2 & 3);
	}
	CACALL(parse_ref_idx, ref_idx_flags);
	
	// load neighbouring refIdx values and shuffle them into A/B/C/D
	i8x16 BC = (i64x2){(int64_t)mbB->refIdx_l, (int64_t)mbC->refIdx_l};
	i8x16 Ar = (i64x2){(int64_t)mbA->refIdx_l, (int64_t)mb->refIdx_l};
	i8x16 BCAr0 = unziplo32(BC, Ar);
	i8x16 r0 = shuffle(BCAr0, ((i8x16){12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15}));
	i8x16 A0 = shuffle(BCAr0, ((i8x16){9, 12, 9, 12, 12, 13, 12, 13, 11, 14, 11, 14, 14, 15, 14, 15}));
	i8x16 B0 = shuffle(BCAr0, ((i8x16){2, 2, 12, 12, 3, 3, 13, 13, 12, 12, 14, 14, 13, 13, 15, 15}));
	i8x16 C0 = shuffle(BCAr0, ctx->refIdx4x4_C_v);
	i8x16 D0 = shuffle(BCAr0, ((i8x16){-1, 2, 9, 12, 2, 3, 12, 13, 9, 12, 11, 14, 12, 13, 14, 15}));
	D0[0] = mbD->refIdx[3];
	if (__builtin_expect(ctx->t.MbaffFrameFlag, 0)) { // the D locations of the left column (see mbaff_neighbours)
		const Edge264MvcMacroblock *AD = &ctx->mbaff_nb[5];
		D0[2] = AD->refIdx[0], D0[8] = AD->refIdx[1], D0[10] = AD->refIdx[2];
		A0[2] = ctx->mbaff_refIdxA[0], A0[10] = ctx->mbaff_refIdxA[1];
	}
	
	// combine them into a vector of 4-bit equality masks
	union { int8_t q[16]; i8x16 v; } refIdx4x4_eq;
	i8x16 u = ctx->unavail4x4_v[0];
	i8x16 uC = u & 4;
	refIdx4x4_eq.v = (uC - ifelse_mask(uC==4, r0==D0, r0==C0) * 2 - (r0==B0)) * 2 - (r0==A0 | u==14);
	
	// loop on mvs
	log_mb(ctx, "%smvds: [", ctx->log_indent);
	do {
		int i = __builtin_ctz(mvd_flags);
		i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd, i);
		
		// branch on equality mask
		i16x8 mvp;
		int eq = refIdx4x4_eq.q[i];
		int mvs_DC = eq & 8 ? ctx->mvs_D[i] : ctx->mvs_C[i];
		if (__builtin_expect(0xe9e9 >> eq & 1, 1)) {
			// neighbours are addressed relative to mvs_s across the macroblock array
			const int32_t *mvs = (const int32_t *)((const char *)mb + offsetof(Edge264MvcMacroblock, mvs_s));
			i16x8 mvA = (i32x4){mvs[ctx->mvs_A[i]]};
			i16x8 mvB = (i32x4){mvs[ctx->mvs_B[i]]};
			i16x8 mvDC = (i32x4){mvs[mvs_DC]};
			mvp = median16(mvA, mvB, mvDC);
		} else {
			int mvs_AB = eq & 1 ? ctx->mvs_A[i] : ctx->mvs_B[i];
			const int32_t *mvs = (const int32_t *)((const char *)mb + offsetof(Edge264MvcMacroblock, mvs_s));
			mvp = (i32x4){mvs[eq & 4 ? mvs_DC : mvs_AB]};
		}
		
		// broadcast absMvd and mvs to memory then call decoding
		static const int8_t masks[16] = {0, 15, 10, 5, 12, 3, 0, 0, 8, 0, 0, 0, 4, 0, 2, 1};
		static const int8_t widths[16] = {0, 8, 4, 4, 8, 8, 0, 0, 4, 0, 0, 0, 4, 0, 4, 4};
		static const int8_t heights[16] = {0, 8, 8, 8, 4, 4, 0, 0, 4, 0, 0, 0, 4, 0, 4, 4};
		int type = mvd_flags >> (i & -4) & 15;
		int m = masks[type];
		int i8x8 = i >> 2;
		i16x8 bits = {1, 2, 4, 8};
		i16x8 absMvd_mask = ((i16x8){m, m, m, m, m, m, m, m} & bits) == bits;
		i16x8 absMvd_old = (i64x2){mbc->absMvd_l[i8x8]};
		i16x8 mvs_mask = ziplo16(absMvd_mask, absMvd_mask);
		i32x4 mv = mvp + mvd;
		i16x8 mvs = broadcast32(mv, 0);
		mbc->absMvd_l[i8x8] = ((i64x2)ifelse_mask(absMvd_mask, pack_absMvd(mvd), absMvd_old))[0];
		mb->mvs_v[i8x8] = ifelse_mask(mvs_mask, mvs, mb->mvs_v[i8x8]);
		decode_inter(ctx, i, widths[type], heights[type]);
	} while (mvd_flags &= mvd_flags - 1);
	log_mb(ctx, "]\n");
	CAJUMP(parse_inter_residual);
}



/**
 * Parse mb_skip_flag and mb_type in a P macroblock.
 * 
 * Motion vector prediction is one of the hardest parts to decode (8.4.1.3),
 * here is a summary of the rules:
 * _ A/B/C/D are 4x4 blocks at relative pixel positions (-1,0)/(0,-1)/(W,-1)/(-1,-1)
 * _ if C is unavailable, take its values from D instead
 * _ any further unavailable block counts as refIdx=-1 and mv=(0,0)
 * _ parse refIdx for the current block
 * _ for 8x16 or 16x8, compare it with A(left)/C(right) or B(top)/A(bottom),
 *   if it matches take the mv from the same neighbour
 * _ otherwise if B and C are unavailable, take their values from A instead
 * _ then if only one of A/B/C equals refIdx, take mv from this neighbour
 * _ otherwise predict mv as median(mvA, mvB, mvC)
 * 
 * In general we implement these rules in two steps:
 * _ compare refIdx with A/B/C and produce a 3 bit equality mask (plus a bit
 *   for C->D replacement), which can be computed in parallel for all 4x4
 *   blocks since all refIdx values are parsed before all mvs
 * _ then for each block in sequence, fetch the proper neighbouring mv(s) and
 *   combine them depending on the mask
 */
static void CAFUNC(parse_P_mb)
{
	// Inter initializations, with constrained_intra_pred_flag sending the intra
	// modes of the neighbours to DC (dcPredModePredictedFlag, 8.3.1.1)
	mb->mbIsInterFlag = 1;
	mbc->Intra4x4PredMode_v = set8(ctx->t.pps.constrained_intra_pred_flag ? -2 : 2);
	mb->refIdx_l = (int64_t)(i8x8){0, 0, 0, 0, -1, -1, -1, -1};
	#if CABAC
		mbc->absMvd_v[0] = mbc->absMvd_v[1] = (i8x16){};
	#endif
	
	// parse mb_skip_run/flag
	#if !CABAC
		if (ctx->mb_skip_run < 0) {
			ctx->mb_skip_run = get_ue32(&ctx->t.gb, 139264);
			log_mb(ctx, "%smb_skip_run: %u\n", ctx->log_indent, ctx->mb_skip_run);
		}
		int mb_skip_flag = ctx->mb_skip_run-- > 0;
	#else
		// (an MBAFF frame may have parsed it ahead, see parse_slice_data_mbaff)
		int mb_skip_flag = ctx->mbaff_skip >= 0 ? ctx->mbaff_skip : get_ae(ctx, 13 - ctx->inc.mb_skip_flag);
		log_mb(ctx, "%smb_skip_flag: %u\n", ctx->log_indent, mb_skip_flag);
	#endif
	
	// earliest handling for P_Skip
	if (mb_skip_flag) {
		#if CABAC
			mb->f.mb_skip_flag = 1;
			ctx->mb_qp_delta_nz = 0;
		#endif
		decode_P_skip(ctx);
		return;
	}
	ctx->transform_8x8_mode_flag = ctx->t.pps.transform_8x8_mode_flag;
	
	// initializations and jumps for mb_type
	#if !CABAC
		int mb_type = get_ue16(&ctx->t.gb, 30);
		log_mb(ctx, "%smb_type: %u\n", ctx->log_indent, mb_type);
		if (mb_type > 4)
			CAJUMP(parse_I_mb, mb_type - 5);
		if (mb_type > 2)
			CAJUMP(parse_P_sub_mb, (mb_type + 12) & 15); // 3->15, 4->0
		CACALL(parse_ref_idx, 0x351 >> (mb_type << 2) & 15); // 0->1, 1->5, 2->3
	#else
		if (get_ae(ctx, 14)) { // Intra
			CAJUMP(parse_I_mb, 17);
		}
		int mb_type = get_ae(ctx, 15); // actually 1 and 3 are swapped
		mb_type += mb_type + get_ae(ctx, 16 + mb_type);
		log_mb(ctx, "%smb_type: %u\n", ctx->log_indent, (4 - mb_type) & 3);
		if (mb_type == 1)
			CAJUMP(parse_P_sub_mb, 15);
		CACALL(parse_ref_idx, (mb_type + 1) | 1); // 0->1, 2->3, 3->5
	#endif
	
	// decoding large blocks
	log_mb(ctx, "%smvds: [", ctx->log_indent);
	if (mb_type == 0) { // 16x16
		mb->f.inter_eqs_s = little_endian32(0x1b5fbbff);
		i16x8 mvd = CACALL(parse_mvd_pair, mbc->absMvd, 0);
		decode_inter_16x16(ctx, mvd, 0);
	} else if (mb_type == 2) { // 8x16
		mb->f.inter_eqs_s = little_endian32(0x1b1bbbbb);
		i16x8 mvd0 = CACALL(parse_mvd_pair, mbc->absMvd, 0);
		decode_inter_8x16_left(ctx, mvd0, 0);
		i16x8 mvd1 = CACALL(parse_mvd_pair, mbc->absMvd, 4);
		decode_inter_8x16_right(ctx, mvd1, 0);
	} else { // 16x8
		mb->f.inter_eqs_s = little_endian32(0x1b5f1b5f);
		i16x8 mvd0 = CACALL(parse_mvd_pair, mbc->absMvd, 0);
		decode_inter_16x8_top(ctx, mvd0, 0);
		i16x8 mvd1 = CACALL(parse_mvd_pair, mbc->absMvd, 8);
		decode_inter_16x8_bottom(ctx, mvd1, 0);
	}
	log_mb(ctx, "]\n");
	CAJUMP(parse_inter_residual);
}



#if !CABAC
/**
 * With slice groups (FMO), the macroblocks of a slice are not consecutive, so
 * a neighbour is available if it belongs to the slice group of the slice and
 * comes at or after the slice's first macroblock (6.4.1). The neighbouring
 * offsets are set anew for each macroblock (rather than once, when each
 * neighbour becomes available), from their values of initialize_context.
 * Returns the unavailability of A, B, C and D (bits 0 to 3).
 */
static noinline int fmo_neighbours(Edge264MvcContext *ctx) {
	int a = ctx->CurrMbAddr, w = ctx->t.pic_width_in_mbs, x = ctx->mbx, y = ctx->mby;
	const uint8_t *map = ctx->t.fmo_map;
	int g = ctx->t.fmo_group, first = ctx->t.first_mb_in_slice;
	#define IN_SLICE(n) (map[n] == g && (n) >= first)
	int availA = x > 0 && IN_SLICE(a - 1);
	int availB = y > 0 && IN_SLICE(a - w);
	int availC = y > 0 && x < w - 1 && IN_SLICE(a - w + 1);
	int availD = x > 0 && y > 0 && IN_SLICE(a - w - 1);
	#undef IN_SLICE
	ctx->A4x4_int8_v = (i16x16){0, 0, 2, 2, 1, 4, 3, 6, 8, 8, 10, 10, 9, 12, 11, 14};
	ctx->B4x4_int8_v = (i32x16){0, 1, 0, 1, 4, 5, 4, 5, 2, 3, 8, 9, 6, 7, 12, 13};
	if (ctx->t.ChromaArrayType == 1) {
		ctx->ACbCr_int8_v[0] = (i16x8){0, 0, 2, 2, 4, 4, 6, 6};
		ctx->BCbCr_int8_v[0] = (i32x8){0, 1, 0, 1, 4, 5, 4, 5};
	} else if (ctx->t.ChromaArrayType == 2) {
		ctx->ACbCr_int8_v[0] = (i16x8){0, 0, 2, 2, 4, 4, 6, 6};
		ctx->ACbCr_int8_v[1] = (i16x8){8, 8, 10, 10, 12, 12, 14, 14};
		ctx->BCbCr_int8_v[0] = (i32x8){0, 1, 0, 1, 2, 3, 4, 5};
		ctx->BCbCr_int8_v[1] = (i32x8){8, 9, 8, 9, 10, 11, 12, 13};
	}
	ctx->absMvd_A_v = (i16x16){0, 0, 4, 4, 2, 8, 6, 12, 16, 16, 20, 20, 18, 24, 22, 28};
	ctx->absMvd_B_v = (i32x16){0, 2, 0, 2, 8, 10, 8, 10, 4, 6, 16, 18, 12, 14, 24, 26};
	ctx->mvs_A_v = (i16x16){0, 0, 2, 2, 1, 4, 3, 6, 8, 8, 10, 10, 9, 12, 11, 14};
	ctx->mvs_B_v = (i32x16){0, 1, 0, 1, 4, 5, 4, 5, 2, 3, 8, 9, 6, 7, 12, 13};
	ctx->mvs_C_v = (i32x16){0, 1, 1, -1, 4, 5, 5, -1, 3, 6, 9, -1, 7, -1, 13, -1};
	ctx->mvs_D_v = (i32x16){0, 1, 2, 0, 4, 5, 1, 4, 8, 2, 10, 8, 3, 6, 9, 12};
	if (availA) { // as when A becomes available in parse_slice_data
		ctx->A4x4_int8[0] = 5 - (int)sizeof(*mbc);
		ctx->A4x4_int8[2] = 7 - (int)sizeof(*mbc);
		ctx->A4x4_int8[8] = 13 - (int)sizeof(*mbc);
		ctx->A4x4_int8[10] = 15 - (int)sizeof(*mbc);
		ctx->absMvd_A[0] = 10 - (int)sizeof(*mbc);
		ctx->absMvd_A[2] = 14 - (int)sizeof(*mbc);
		ctx->absMvd_A[8] = 26 - (int)sizeof(*mbc);
		ctx->absMvd_A[10] = 30 - (int)sizeof(*mbc);
		ctx->mvs_A[0] = 5 - (int)(sizeof(*mb) >> 2);
		ctx->mvs_A[2] = 7 - (int)(sizeof(*mb) >> 2);
		ctx->mvs_A[8] = 13 - (int)(sizeof(*mb) >> 2);
		ctx->mvs_A[10] = 15 - (int)(sizeof(*mb) >> 2);
		ctx->mvs_D[2] = 5 - (int)(sizeof(*mb) >> 2);
		ctx->mvs_D[8] = 7 - (int)(sizeof(*mb) >> 2);
		ctx->mvs_D[10] = 13 - (int)(sizeof(*mb) >> 2);
		if (ctx->t.ChromaArrayType == 1) {
			ctx->ACbCr_int8[0] = 1 - (int)sizeof(*mbc);
			ctx->ACbCr_int8[2] = 3 - (int)sizeof(*mbc);
			ctx->ACbCr_int8[4] = 5 - (int)sizeof(*mbc);
			ctx->ACbCr_int8[6] = 7 - (int)sizeof(*mbc);
		} else if (ctx->t.ChromaArrayType == 2) {
			for (int i = 0; i < 16; i += 2)
				ctx->ACbCr_int8[i] = i + 1 - (int)sizeof(*mbc);
		}
	}
	if (availB) { // as when B becomes available
		int offB_int8 = -(int)sizeof(*mbc);
		int offB_int32 = ((w + 1) * (int)sizeof(*mb)) >> 2;
		ctx->B4x4_int8[0] = 10 - offB_int8;
		ctx->B4x4_int8[1] = 11 - offB_int8;
		ctx->B4x4_int8[4] = 14 - offB_int8;
		ctx->B4x4_int8[5] = 15 - offB_int8;
		ctx->absMvd_B[0] = 20 - offB_int8;
		ctx->absMvd_B[1] = 22 - offB_int8;
		ctx->absMvd_B[4] = 28 - offB_int8;
		ctx->absMvd_B[5] = 30 - offB_int8;
		ctx->mvs_B[0] = 10 - offB_int32;
		ctx->mvs_B[1] = 11 - offB_int32;
		ctx->mvs_B[4] = 14 - offB_int32;
		ctx->mvs_B[5] = 15 - offB_int32;
		ctx->mvs_C[0] = 11 - offB_int32;
		ctx->mvs_C[1] = 14 - offB_int32;
		ctx->mvs_C[4] = 15 - offB_int32;
		ctx->mvs_D[1] = 10 - offB_int32;
		ctx->mvs_D[4] = 11 - offB_int32;
		ctx->mvs_D[5] = 14 - offB_int32;
		if (ctx->t.ChromaArrayType == 1) {
			ctx->BCbCr_int8[0] = 2 - offB_int8;
			ctx->BCbCr_int8[1] = 3 - offB_int8;
			ctx->BCbCr_int8[4] = 6 - offB_int8;
			ctx->BCbCr_int8[5] = 7 - offB_int8;
		} else if (ctx->t.ChromaArrayType == 2) {
			ctx->BCbCr_int8[0] = 6 - offB_int8;
			ctx->BCbCr_int8[1] = 7 - offB_int8;
			ctx->BCbCr_int8[8] = 14 - offB_int8;
			ctx->BCbCr_int8[9] = 15 - offB_int8;
		}
	}
	if (availC)
		ctx->mvs_C[5] = 10 - ((w * (int)sizeof(*mb)) >> 2);
	if (availD)
		ctx->mvs_D[0] = 15 - (((w + 2) * (int)sizeof(*mb)) >> 2);
	return !availA | !availB << 1 | !availC << 2 | !availD << 3;
}

/**
 * Moves to the next macroblock of the slice group (8.2.2 nextMbAddress), or
 * returns 0 at the end of the group. The pointers that parse_slice_data moves
 * by one macroblock are set from the address, keeping the copies at both
 * ends of the ring of neighbouring values (see advance_mbc).
 */
static noinline int fmo_next_mb(Edge264MvcContext *ctx) {
	int w = ctx->t.pic_width_in_mbs, size = w * ctx->t.pic_height_in_mbs;
	int n = ctx->CurrMbAddr + 1;
	while (n < size && ctx->t.fmo_map[n] != ctx->t.fmo_group)
		n++;
	if (n >= size)
		return 0;
	Edge264MvcMbCache *e = ctx->_mbc;
	if (e == ctx->mbc_ring)
		e[ctx->mbc_ring_size] = *e;
	if (e == ctx->mbc_ring + ctx->mbc_ring_size - 1)
		ctx->mbc_ring[-1] = *e;
	int x = n % w, y = n / w, off = x + y * (w + 1);
	ctx->CurrMbAddr = n;
	ctx->mbx = x;
	ctx->mby = y;
	ctx->_mb = ctx->t.mb_buffer + off;
	ctx->mbCol = ctx->t.slice_type == 1 ? ctx->t.mbCol_buffer + off : ctx->_mb;
	ctx->_mbc = ctx->mbc_ring + off % ctx->mbc_ring_size;
	ctx->samples_mb[0] = ctx->t.samples_buffers[ctx->currPic] + x * ctx->t.mbw_Y + y * ctx->t.stride[0] * 16;
	ctx->samples_mb[1] = ctx->t.samples_buffers[ctx->currPic] + x * ctx->t.mbw_C + y * ctx->t.stride[1] * ctx->t.mb_height_C + PLANE_C(&ctx->t, ctx->currPic);
	ctx->samples_mb[2] = ctx->samples_mb[1] + ctx->t.stride_Cr;
	return 1;
}
#endif



/**
 * This function loops through the macroblocks of a slice, initialising their
 * data and calling parse_{I/P/B}_mb for each one.
 */
static noinline void CAFUNC(parse_slice_data)
{
	
	int end_of_slice_flag = 0;
	do {
		#ifdef LOGS
			if (ctx->thread_id < 0) {
				log_mb(ctx, "  - mbAddr: %u\n", ctx->CurrMbAddr);
			} else {
				log_mb(ctx, "\n- thread_id: %d\n"
					"  FrameId: %u\n"
					"  mbAddr: %u\n",
					ctx->thread_id, ctx->t.FrameId, ctx->CurrMbAddr);
			}
		#endif
		
		// end the slice where the next one starts (mb_bound, which a slice started
		// before the next NAL learns as it goes), then claim the mb by updating
		// flip_bit atomically, unless an older slice already decoded it
		if (__builtin_expect(ctx->t.mb_bound == BOUND_UNKNOWN, 0)) {
			int32_t bound = __atomic_load_n(&ctx->d->task_bounds[ctx->task_id], __ATOMIC_ACQUIRE);
			if (bound != BOUND_UNKNOWN)
				known_mb_bound(ctx, bound, 0);
		}
		if (ctx->CurrMbAddr >= ctx->t.mb_bound)
			return;
		// reason: the claim is the first write to the macroblock, whose cache lines
		// another thread usually wrote last, so fetch those of the next one into
		// L2 ahead of it (into L1 they displaced data of CABAC-heavy macroblocks)
		for (int l = 0; l < (int)sizeof(*mb); l += 64)
			__builtin_prefetch((char *)(mb + 1) + l, 0, 2);
		// reason: the samples a picture writes were last used by an older picture
		// in its slot and have mostly left the caches, and every first write to a
		// cache line waited for it to be read, so fetch the rows of the next 64
		// bytes of samples into L2 whenever a macroblock starts 64 bytes of them
		// (every 4 macroblocks for 8-bit luma, 8 for chroma)
		if (!(ctx->mbx * ctx->t.mbw_Y & 63)) {
			const char *p = (const char *)ctx->samples_mb[0] + 64;
			for (int y = 0; y < 16; y++)
				__builtin_prefetch(p + y * ctx->t.stride[0], 1, 2);
		}
		if (!(ctx->mbx * ctx->t.mbw_C & 63)) {
			const char *p = (const char *)ctx->samples_mb[1] + 64;
			for (int y = 0; y < ctx->t.mb_height_C; y++) {
				__builtin_prefetch(p + y * ctx->t.stride[1], 1, 2);
				__builtin_prefetch(p + y * ctx->t.stride[1] + ctx->t.stride_Cr, 1, 2);
			}
		}
		int prev_recovery_bits = __atomic_exchange_n(&mb->recovery_bits, ctx->t.frame_flip_bit, __ATOMIC_ACQ_REL);
		if (prev_recovery_bits == ctx->t.frame_flip_bit && !claim_after_older_slices(ctx))
			return;

		// set and reset neighbouring pointers depending on their availability
		int unavail16x16 = (ctx->mbx == 0 ? 9 : 0) | (ctx->mbx == ctx->t.pic_width_in_mbs - 1) << 2 | (ctx->mby == 0 ? 14 : 0);
		int filter_edges = 4 | 3 & ~unavail16x16;
		mbA = mb - 1;
		mbB = mbA - ctx->t.pic_width_in_mbs;
		mbC = mbB + 1;
		mbD = mbB - 1;
		int decoded = ctx->CurrMbAddr - ctx->t.first_mb_in_slice;
		if (__builtin_expect(ctx->t.fmo_map != NULL, 0)) {
			int unavail = fmo_neighbours(ctx);
			if (unavail & ~unavail16x16 & 1)
				filter_edges &= ~(ctx->t.disable_deblocking_filter_idc >> 1); // A of another slice
			if (unavail & ~unavail16x16 & 2)
				filter_edges &= ~ctx->t.disable_deblocking_filter_idc; // B of another slice
			unavail16x16 |= unavail;
			if (unavail & 1)
				mbA = &unavail_mb;
			if (unavail & 2)
				mbB = &unavail_mb;
			if (unavail & 4)
				mbC = &unavail_mb;
			if (unavail & 8)
				mbD = &unavail_mb;
			ctx->fmo_decoded++;
		} else if (decoded <= ctx->t.pic_width_in_mbs + 1) {
			if (decoded == 1) { // A becomes available
				ctx->A4x4_int8[0] = 5 - (int)sizeof(*mbc);
				ctx->A4x4_int8[2] = 7 - (int)sizeof(*mbc);
				ctx->A4x4_int8[8] = 13 - (int)sizeof(*mbc);
				ctx->A4x4_int8[10] = 15 - (int)sizeof(*mbc);
				ctx->absMvd_A[0] = 10 - (int)sizeof(*mbc);
				ctx->absMvd_A[2] = 14 - (int)sizeof(*mbc);
				ctx->absMvd_A[8] = 26 - (int)sizeof(*mbc);
				ctx->absMvd_A[10] = 30 - (int)sizeof(*mbc);
				ctx->mvs_A[0] = 5 - (int)(sizeof(*mb) >> 2);
				ctx->mvs_A[2] = 7 - (int)(sizeof(*mb) >> 2);
				ctx->mvs_A[8] = 13 - (int)(sizeof(*mb) >> 2);
				ctx->mvs_A[10] = 15 - (int)(sizeof(*mb) >> 2);
				ctx->mvs_D[2] = 5 - (int)(sizeof(*mb) >> 2);
				ctx->mvs_D[8] = 7 - (int)(sizeof(*mb) >> 2);
				ctx->mvs_D[10] = 13 - (int)(sizeof(*mb) >> 2);
				if (ctx->t.ChromaArrayType == 1) {
					ctx->ACbCr_int8[0] = 1 - (int)sizeof(*mbc);
					ctx->ACbCr_int8[2] = 3 - (int)sizeof(*mbc);
					ctx->ACbCr_int8[4] = 5 - (int)sizeof(*mbc);
					ctx->ACbCr_int8[6] = 7 - (int)sizeof(*mbc);
				} else if (ctx->t.ChromaArrayType == 2) {
					for (int i = 0; i < 16; i += 2)
						ctx->ACbCr_int8[i] = i + 1 - (int)sizeof(*mbc);
				}
			} else if (decoded == 0) { // A is unavailable
				mbA = &unavail_mb;
				unavail16x16 |= 1;
				filter_edges &= ~(ctx->t.disable_deblocking_filter_idc >> 1); // impacts only bit 0
			}
			if (decoded == ctx->t.pic_width_in_mbs + 1) { // D becomes available
				int offD_int32 = ((ctx->t.pic_width_in_mbs + 2) * (int)sizeof(*mb)) >> 2;
				ctx->mvs_D[0] = 15 - offD_int32;
			} else { // D is unavailable
				mbD = &unavail_mb;
				unavail16x16 |= 8;
				if (decoded == ctx->t.pic_width_in_mbs) { // B becomes available
					int offB_int8 = -(int)sizeof(*mbc); // B is the next entry in the ring
					int offB_int32 = ((ctx->t.pic_width_in_mbs + 1) * (int)sizeof(*mb)) >> 2;
					ctx->B4x4_int8[0] = 10 - offB_int8;
					ctx->B4x4_int8[1] = 11 - offB_int8;
					ctx->B4x4_int8[4] = 14 - offB_int8;
					ctx->B4x4_int8[5] = 15 - offB_int8;
					ctx->absMvd_B[0] = 20 - offB_int8;
					ctx->absMvd_B[1] = 22 - offB_int8;
					ctx->absMvd_B[4] = 28 - offB_int8;
					ctx->absMvd_B[5] = 30 - offB_int8;
					ctx->mvs_B[0] = 10 - offB_int32;
					ctx->mvs_B[1] = 11 - offB_int32;
					ctx->mvs_B[4] = 14 - offB_int32;
					ctx->mvs_B[5] = 15 - offB_int32;
					ctx->mvs_C[0] = 11 - offB_int32;
					ctx->mvs_C[1] = 14 - offB_int32;
					ctx->mvs_C[4] = 15 - offB_int32;
					ctx->mvs_D[1] = 10 - offB_int32;
					ctx->mvs_D[4] = 11 - offB_int32;
					ctx->mvs_D[5] = 14 - offB_int32;
					if (ctx->t.ChromaArrayType == 1) {
						ctx->BCbCr_int8[0] = 2 - offB_int8;
						ctx->BCbCr_int8[1] = 3 - offB_int8;
						ctx->BCbCr_int8[4] = 6 - offB_int8;
						ctx->BCbCr_int8[5] = 7 - offB_int8;
					} else if (ctx->t.ChromaArrayType == 2) {
						ctx->BCbCr_int8[0] = 6 - offB_int8;
						ctx->BCbCr_int8[1] = 7 - offB_int8;
						ctx->BCbCr_int8[8] = 14 - offB_int8;
						ctx->BCbCr_int8[9] = 15 - offB_int8;
					}
				} else { // B is unavailable
					mbB = &unavail_mb;
					unavail16x16 |= 2;
					filter_edges &= ~ctx->t.disable_deblocking_filter_idc; // impacts only bit 1
					if (decoded == ctx->t.pic_width_in_mbs - 1) { // C becomes available
						int offC_int32 = (ctx->t.pic_width_in_mbs * (int)sizeof(*mb)) >> 2;
						ctx->mvs_C[5] = 10 - offC_int32;
					} else { // C is unavailable
						mbC = &unavail_mb;
						unavail16x16 |= 4;
					}
				}
			}
		}
		
		// initialize common macroblock values
		ctx->unavail4x4_v[0] = block_unavailability[unavail16x16];
		ctx->unavail16x16 = unavail16x16;
		ctx->inc.v = mbA->f.v + mbB->f.v + (mbB->f.v & flags_twice.v);
		mb->f.v = (i8x16){};
		mb->filter_edges = (ctx->t.disable_deblocking_filter_idc != 1) ? filter_edges : 0;
		mb->QP_s = ctx->t.QP_s;
		// the chroma 4x4 blocks take the availability of the luma blocks at their position
		if (ctx->t.ChromaArrayType == 1)
			ctx->unavail4x4_v[1] = shuffle(ctx->unavail4x4_v[0], (i8x16){0, 4, 8, 12, 0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1});
		else if (ctx->t.ChromaArrayType == 2)
			ctx->unavail4x4_v[1] = shuffle(ctx->unavail4x4_v[0], (i8x16){0, 4, 2, 6, 8, 12, 10, 14, 0, 4, 2, 6, 8, 12, 10, 14});
		// the coded flags of the neighbours, whose luma part a monochrome picture
		// reads as well (coded_block_pattern and coded_block_flag in CABAC)
		if (ctx->t.ChromaArrayType < 3)
			mb->bits_l = (mbA->bits_l >> 3 & 0x11111100111111) | (mbB->bits_l >> 1 & 0x42424200424242);
		mbc->nC_v[0] = mbc->nC_v[1] = mbc->nC_v[2] = (i8x16){};
		
		// Would it actually help to push this test outside the loop?
		if (ctx->t.slice_type == 0) {
			CACALL(parse_P_mb);
			// the P macroblocks of SP slices decode with the SP process, then
			// deblock as intra ones (8.7.2.1)
			if (ctx->t.sp_slice & mb->mbIsInterFlag) {
				decode_sp_mb(ctx);
				mb->mbIsInterFlag = 0;
			}
		} else if (ctx->t.slice_type == 1) {
			await_reference(ctx, ctx->t.RefPicList[1][0], ctx->mby);
			if (__builtin_expect(ctx->t.col_structure, 0))
				colocated_cross(ctx);
			CACALL(parse_B_mb);
		} else {
			int mb_type_or_ctxIdx = CACOND(get_ue16(&ctx->t.gb, 25), 5 - ctx->inc.mb_type_I_NxN);
			#if !CABAC
				log_mb(ctx, "%smb_type: %u\n", ctx->log_indent, mb_type_or_ctxIdx);
			#endif
			CACALL(parse_I_mb, mb_type_or_ctxIdx);
		}
		#if CABAC
			end_of_slice_flag = cabac_terminate(ctx);
			log_mb(ctx, "%send_of_slice_flag: %u\n", ctx->log_indent, end_of_slice_flag);
		#endif
		print_mb(ctx);
		mb->nC_Y_v = mbc->nC_v[0];
		#if !CABAC
			// CAVLC codes an 8x8 block as four interleaved 4x4 ones, whereas
			// deblocking (8.7.2.1) asks whether the 8x8 block has coefficients, also
			// for the neighbours of a later macroblock in a slice not deblocked
			if (mb->f.transform_size_8x8_flag)
				mb->nC_Y_v = (i8x16)((i32x4)mb->nC_Y_v == 0) - -1;
		#endif
		// with slice groups, jump to the next macroblock of the group, the slice
		// ending with the group (the picture is deblocked once complete)
		if (__builtin_expect(ctx->t.fmo_map != NULL, 0)) {
			if (!fmo_next_mb(ctx))
				return;
			continue;
		}
		advance_mbc(ctx);
		
		// deblock mbB while in cache, then point to the next macroblock
		if (ctx->CurrMbAddr - ctx->t.pic_width_in_mbs == ctx->t.next_deblock_addr) {
			ctx->t.next_deblock_addr += 1;
			mb -= ctx->t.pic_width_in_mbs + 1;
			ctx->samples_mb[0] -= ctx->t.stride[0] * 16;
			ctx->samples_mb[1] -= ctx->t.stride[1] * ctx->t.mb_height_C;
			ctx->samples_mb[2] -= ctx->t.stride[1] * ctx->t.mb_height_C;
			deblock_mb(ctx);
			mb += ctx->t.pic_width_in_mbs + 2;
			ctx->samples_mb[0] += ctx->t.stride[0] * 16 + ctx->t.mbw_Y;
			ctx->samples_mb[1] += ctx->t.stride[1] * ctx->t.mb_height_C + ctx->t.mbw_C;
			ctx->samples_mb[2] += ctx->t.stride[1] * ctx->t.mb_height_C + ctx->t.mbw_C;
		} else {
			mb++;
			ctx->samples_mb[0] += ctx->t.mbw_Y;
			ctx->samples_mb[1] += ctx->t.mbw_C;
			ctx->samples_mb[2] += ctx->t.mbw_C;
		}
		ctx->mbx++;
		ctx->CurrMbAddr++;
		ctx->mbCol++;
		if (ctx->mbx >= ctx->t.pic_width_in_mbs) {
			mb++; // skip the empty macroblock at the edge
			*mbc = unavail_mbc;
			advance_mbc(ctx);
			ctx->mbCol++;
			ctx->mby++;
			ctx->mbx = 0;
			ctx->samples_mb[0] += ctx->t.stride[0] * 16 - ctx->t.pic_width_in_mbs * ctx->t.mbw_Y;
			ctx->samples_mb[1] += ctx->t.stride[1] * ctx->t.mb_height_C - ctx->t.pic_width_in_mbs * ctx->t.mbw_C;
			ctx->samples_mb[2] += ctx->t.stride[1] * ctx->t.mb_height_C - ctx->t.pic_width_in_mbs * ctx->t.mbw_C;
			if (ctx->t.next_deblock_idc >= 0) {
				publish_frame_progress(ctx->d, ctx->t.next_deblock_idc,
					(ctx->t.disable_deblocking_filter_idc != 1) ? ctx->t.next_deblock_addr : ctx->CurrMbAddr);
			}
			if (ctx->mby >= ctx->t.pic_height_in_mbs)
				return;
		}
	} while (CACOND(ctx->mb_skip_run > 0 || !rbsp_end(&ctx->t.gb, 1), !end_of_slice_flag));
}

/**
 * MBAFF frames (MbaffFrameFlag 1) code macroblocks in pairs, each pair being
 * two frame macroblocks (rows 0-15 and 16-31) or two field macroblocks (the
 * even and odd rows) as told by mb_field_decoding_flag (7.3.4). The
 * macroblocks keep their place in the macroblock array of the frame (the top
 * one in the row 2 * the pair row, the bottom one below it), and a field
 * macroblock decodes as one of a field picture, with the strides doubled and
 * the references of field macroblocks (see mbaff_state).
 *
 * The neighbours of a macroblock (6.4.12.2, Table 6-4) are no longer at fixed
 * offsets in the array, and their motion vectors and reference indices are
 * scaled between frame and field units (8-217 to 8-220, 9.3.3.1.1.6 and
 * 9.3.3.1.1.7). So each macroblock is decoded in ctx->mbaff_nb[4], among
 * copies of its neighbours remapped and scaled as the current macroblock sees
 * them, at the offsets a picture of width 2 has (D, B, C on the row above, A
 * left of it), and copied back once decoded. The neighbouring samples need no
 * remapping, Table 6-4 giving the samples next to the macroblock in its own
 * frame or field rows.
 */
#if !CABAC

static always_inline Edge264MvcMacroblock *mbaff_pair_mb(Edge264MvcContext *ctx, int px, int py, int bottom) {
	return ctx->t.mb_buffer + px + (py * 2 + bottom) * (ctx->t.pic_width_in_mbs + 1);
}
static always_inline Edge264MvcMbCache *mbaff_pair_mbc(Edge264MvcContext *ctx, int px, int py, int bottom) {
	return ctx->mbaff_cache + ((py & 1) * ctx->t.pic_width_in_mbs + px) * 2 + bottom;
}
static always_inline int mbaff_pair_field(Edge264MvcContext *ctx, int pair) {
	static const int8_t dx[5] = {-1, 0, 1, -1, 0}, dy[5] = {0, -1, -1, -1, 0};
	return mbaff_pair_mb(ctx, ctx->mbaff_px + dx[pair], ctx->mbaff_py + dy[pair], 0)->f.mb_field_decoding_flag;
}

/**
 * Table 6-4: finds the macroblock containing the neighbouring location of the
 * current macroblock at row yN (of maxH rows), left of it (A, D) or above it
 * (B, C, D). Returns its pair (MBAFF_A..MBAFF_CUR) or -1 if not available, and
 * sets bottom to its position in the pair and yM to the row in it.
 */
static int mbaff_locate(Edge264MvcContext *ctx, int nb, int yN, int maxH, int *bottom, int *yM) {
	int frame = !ctx->mb_field, top = !(ctx->CurrMbAddr & 1);
	int pair = nb, bot = 1, y = yN;
	switch (nb) {
	case MBAFF_A:
		if (!(ctx->mbaff_avail & 1))
			return -1;
		if (frame) {
			if (!mbaff_pair_field(ctx, MBAFF_A))
				bot = !top;
			else
				bot = yN & 1, y = top ? yN >> 1 : (yN + maxH) >> 1;
		} else if (!mbaff_pair_field(ctx, MBAFF_A)) {
			bot = yN >= maxH / 2;
			y = (yN << 1) + !top - bot * maxH;
		} else {
			bot = !top;
		}
		break;
	case MBAFF_D:
		if (frame && !top) { // the D location of a bottom frame macroblock is in pair A
			if (!(ctx->mbaff_avail & 1))
				return -1;
			pair = MBAFF_A;
			if (!mbaff_pair_field(ctx, MBAFF_A))
				bot = 0;
			else
				y = (yN + maxH) >> 1;
			break;
		} // fallthrough
	case MBAFF_B:
	case MBAFF_C:
		if (frame && !top) {
			if (nb == MBAFF_C)
				return -1;
			pair = MBAFF_CUR, bot = 0;
			break;
		}
		if (!(ctx->mbaff_avail & 1 << nb))
			return -1;
		if (!frame && top) {
			if (!mbaff_pair_field(ctx, nb))
				y = yN * 2;
			else
				bot = 0;
		}
		break;
	}
	*bottom = bot;
	*yM = (y + maxH) % maxH;
	return pair;
}


static always_inline const Edge264MvcMacroblock *mbaff_located_mb(Edge264MvcContext *ctx, int pair, int bottom) {
	static const int8_t dx[5] = {-1, 0, 1, -1, 0}, dy[5] = {0, -1, -1, -1, 0};
	return mbaff_pair_mb(ctx, ctx->mbaff_px + dx[pair], ctx->mbaff_py + dy[pair], bottom);
}
static always_inline const Edge264MvcMbCache *mbaff_located_mbc(Edge264MvcContext *ctx, int pair, int bottom) {
	static const int8_t dx[5] = {-1, 0, 1, -1, 0}, dy[5] = {0, -1, -1, -1, 0};
	return mbaff_pair_mbc(ctx, ctx->mbaff_px + dx[pair], ctx->mbaff_py + dy[pair], bottom);
}

/**
 * Copies a neighbouring macroblock with its motion vectors and reference
 * indices in the units of the current macroblock (8-217 to 8-220), and the
 * flags of ref_idx greater than 0 as a frame macroblock sees those of a field
 * macroblock, refIdx > 1 (9.3.3.1.1.6).
 */
static void mbaff_copy_mb(Edge264MvcMacroblock *d, const Edge264MvcMacroblock *s, int field) {
	*d = *s;
	if (s->f.mb_field_decoding_flag == field)
		return;
	for (int i = 1; i < 64; i += 2)
		d->mvs[i] = field ? s->mvs[i] / 2 : s->mvs[i] * 2;
	for (int i = 0; i < 8; i++) {
		if (s->refIdx[i] >= 0)
			d->refIdx[i] = field ? s->refIdx[i] * 2 : s->refIdx[i] >> 1;
		if (!field && d->refIdx[i] <= 0)
			d->bits[0] &= ~(1u << bit8x8[4 + i]);
	}
}
static void mbaff_copy_mbc(Edge264MvcMbCache *d, const Edge264MvcMbCache *s, int scale) {
	*d = *s;
	if (scale > 0) { // field neighbour of a frame macroblock
		for (int i = 1; i < 64; i += 2)
			d->absMvd[i] = min(s->absMvd[i] * 2, 255);
	} else if (scale < 0) {
		for (int i = 1; i < 64; i += 2)
			d->absMvd[i] = s->absMvd[i] >> 1;
	}
}

/**
 * Fills the neighbours of the current macroblock in ctx->mbaff_nb and
 * ctx->mbaff_nbc (see above), as unavailable macroblocks where they are not
 * available. The values of A are gathered per row of 4x4 blocks (and of 8x8
 * blocks), which may come from both macroblocks of the left pair, and the
 * D locations of the blocks of the left column (read for motion vector
 * prediction) are in mbaff_nb[5]: those of blocks 2, 8 and 10 in blocks 5, 7
 * and 13, and their refIdx in refIdx[lx * 4 + 0..2].
 */
static noinline void mbaff_neighbours(Edge264MvcContext *ctx) {
	static const int8_t right4x4[4] = {5, 7, 13, 15};
	Edge264MvcMacroblock *n = ctx->mbaff_nb;
	Edge264MvcMbCache *nc = ctx->mbaff_nbc;
	int field = ctx->mb_field, bot, yM, pair;
	
	// B and C, whole macroblocks
	if ((pair = mbaff_locate(ctx, MBAFF_B, -1, 16, &bot, &yM)) >= 0) {
		const Edge264MvcMacroblock *s = mbaff_located_mb(ctx, pair, bot);
		mbaff_copy_mb(&n[1], s, field);
		mbaff_copy_mbc(&nc[2], mbaff_located_mbc(ctx, pair, bot), s->f.mb_field_decoding_flag - field);
	} else {
		n[1] = unavail_mb;
		nc[2] = unavail_mbc;
	}
	if ((pair = mbaff_locate(ctx, MBAFF_C, -1, 16, &bot, &yM)) >= 0)
		mbaff_copy_mb(&n[2], mbaff_located_mb(ctx, pair, bot), field);
	else
		n[2] = unavail_mb;
	
	// D, whose bottom-right 4x4 block takes the one at row yM
	if ((pair = mbaff_locate(ctx, MBAFF_D, -1, 16, &bot, &yM)) >= 0) {
		mbaff_copy_mb(&n[0], mbaff_located_mb(ctx, pair, bot), field);
		for (int lx = 0; lx < 2; lx++) {
			n[0].mvs_s[lx * 16 + 15] = n[0].mvs_s[lx * 16 + right4x4[yM >> 2]];
			n[0].refIdx[lx * 4 + 3] = n[0].refIdx[lx * 4 + 1 + (yM >> 3) * 2];
		}
	} else {
		n[0] = unavail_mb;
	}
	
	// A, gathered from both macroblocks of the left pair
	if (!(ctx->mbaff_avail & 1)) {
		n[3] = n[5] = unavail_mb;
		nc[0] = unavail_mbc;
		ctx->mbaff_refIdxA[0] = ctx->mbaff_refIdxA[1] = ctx->mbaff_refIdxA[2] = ctx->mbaff_refIdxA[3] = -1;
		return;
	}
	Edge264MvcMacroblock src[2] __attribute__((aligned(64)));
	Edge264MvcMbCache srcc[2] __attribute__((aligned(64)));
	int fA = mbaff_pair_field(ctx, MBAFF_A);
	for (int b = 0; b < 2; b++) {
		mbaff_copy_mb(&src[b], mbaff_located_mb(ctx, MBAFF_A, b), field);
		mbaff_copy_mbc(&srcc[b], mbaff_located_mbc(ctx, MBAFF_A, b), fA - field);
	}
	mbaff_locate(ctx, MBAFF_A, 0, 16, &bot, &yM);
	n[3] = src[bot]; // the macroblock A of 6.4.11.1, at (-1, 0)
	nc[0] = srcc[bot];
	for (int r = 0; r < 4; r++) {
		mbaff_locate(ctx, MBAFF_A, r * 4, 16, &bot, &yM);
		int d = right4x4[r], s = right4x4[yM >> 2];
		for (int lx = 0; lx < 2; lx++) {
			n[3].mvs_s[lx * 16 + d] = src[bot].mvs_s[lx * 16 + s];
			nc[0].absMvd[lx * 32 + d * 2] = srcc[bot].absMvd[lx * 32 + s * 2];
			nc[0].absMvd[lx * 32 + d * 2 + 1] = srcc[bot].absMvd[lx * 32 + s * 2 + 1];
		}
		nc[0].nC[d] = srcc[bot].nC[s];
		nc[0].Intra4x4PredMode[d] = srcc[bot].Intra4x4PredMode[s];
		n[3].nC_Y[d] = src[bot].nC_Y[s];
		for (int lx = 0; lx < 2 && (r & 1); lx++)
			ctx->mbaff_refIdxA[lx * 2 + (r >> 1)] = src[bot].refIdx[lx * 4 + 1 + (yM >> 3) * 2];
		if (!(r & 1)) { // 8x8 blocks
			int d8 = 1 + r, s8 = 1 + (yM >> 3) * 2;
			for (int lx = 0; lx < 2; lx++) {
				n[3].refIdx[lx * 4 + d8] = src[bot].refIdx[lx * 4 + s8];
				n[3].refPic[lx * 4 + d8] = src[bot].refPic[lx * 4 + s8];
			}
			int db = bit8x8[d8], sb = bit8x8[s8];
			for (int k = 0; k < 2; k++) {
				uint32_t m = 0x01010101u << db;
				n[3].bits[k] = (n[3].bits[k] & ~m) | ((src[bot].bits[k] >> sb & 0x01010101u) << db);
			}
		}
	}
	// chroma 4x4 blocks, in rows of 4 chroma samples
	if (ctx->t.ChromaArrayType == 1 || ctx->t.ChromaArrayType == 2) {
		int hC = ctx->t.mb_height_C, nCb = hC >> 1; // 4x4 blocks per component
		for (int r = 0; r < hC >> 2; r++) {
			mbaff_locate(ctx, MBAFF_A, r * 4, hC, &bot, &yM);
			int d = r * 2 + 1, s = (yM >> 2) * 2 + 1;
			nc[0].nC[16 + d] = srcc[bot].nC[16 + s];
			nc[0].nC[16 + nCb + d] = srcc[bot].nC[16 + nCb + s];
		}
	}
	// the D locations of the left column
	n[5] = unavail_mb;
	for (int r = 1; r < 4; r++) {
		mbaff_locate(ctx, MBAFF_A, r * 4 - 1, 16, &bot, &yM);
		for (int lx = 0; lx < 2; lx++) {
			n[5].mvs_s[lx * 16 + right4x4[r - 1]] = src[bot].mvs_s[lx * 16 + right4x4[yM >> 2]];
			n[5].refIdx[lx * 4 + r - 1] = src[bot].refIdx[lx * 4 + 1 + (yM >> 3) * 2];
		}
	}
}

/**
 * The ctxIdxInc of mb_skip_flag for a pair whose mb_field_decoding_flag is not
 * decoded yet, from the field decoding flag inferred for it (9.3.3.1.1.1): the
 * number of neighbours A and B that are skipped or not available. The top
 * macroblock of the pair is known to be skipped when the bottom one is parsed.
 */
static int mbaff_skip_inc(Edge264MvcContext *ctx, int bottom) {
	int saved = ctx->CurrMbAddr, bot, yM, pair, inc = 0;
	ctx->CurrMbAddr = (saved & ~1) | bottom;
	pair = mbaff_locate(ctx, MBAFF_A, 0, 16, &bot, &yM);
	inc += pair < 0 || mbaff_located_mb(ctx, pair, bot)->f.mb_skip_flag;
	pair = mbaff_locate(ctx, MBAFF_B, -1, 16, &bot, &yM);
	inc += pair < 0 || pair == MBAFF_CUR || mbaff_located_mb(ctx, pair, bot)->f.mb_skip_flag;
	ctx->CurrMbAddr = saved;
	return inc;
}

/**
 * Sets the context to decode the current macroblock as a frame or field
 * macroblock, the latter as in a field picture of its parity: strides,
 * position, height and reference lists (8.4.2.1: refIdx >> 1 of the frame
 * list, of the same parity if refIdx is even).
 */
static void mbaff_state(Edge264MvcContext *ctx, int field, int bottom) {
	int px = ctx->mbaff_px, py = ctx->mbaff_py;
	ctx->t.field_pic_flag = field;
	ctx->t.bottom_field_flag = field & bottom;
	ctx->t.stride[0] = ctx->mbaff_stride[0] << field;
	ctx->t.stride[1] = ctx->t.stride[2] = ctx->mbaff_stride[1] << field;
	ctx->t.pic_height_in_mbs = ctx->mbaff_height >> field;
	ctx->mbx = px;
	ctx->mby = field ? py : py * 2 + bottom;
	int rowY = py * 32 + (field ? bottom : bottom * 16);
	int rowC = py * 2 * ctx->t.mb_height_C + (field ? bottom : bottom * ctx->t.mb_height_C);
	uint8_t *base = ctx->t.samples_buffers[ctx->currPic];
	ctx->samples_mb[0] = base + px * ctx->t.mbw_Y + rowY * ctx->mbaff_stride[0];
	ctx->samples_mb[1] = base + PLANE_C(&ctx->t, ctx->currPic) + px * ctx->t.mbw_C + rowC * ctx->mbaff_stride[1];
	ctx->samples_mb[2] = ctx->samples_mb[1] + ctx->t.stride_Cr;
	int l = field ? 1 + bottom : 0;
	for (int i = 0; i < 4; i++)
		ctx->t.RefPicList_v[i] = ctx->mbaff_lists_v[l][i];
	if (l != ctx->mbaff_tables) {
		if (ctx->t.slice_type == 1 && (ctx->t.pps.weighted_bipred_idc == 2 || !ctx->t.direct_spatial_mv_pred_flag)) {
			memcpy(ctx->DistScaleFactor, ctx->mbaff_DSF[l], sizeof(ctx->DistScaleFactor));
			memcpy(ctx->MapPicToList0, ctx->mbaff_map[l], sizeof(ctx->MapPicToList0));
			if (ctx->t.pps.weighted_bipred_idc == 2)
				memcpy(ctx->implicit_weights, ctx->mbaff_iw[l], sizeof(ctx->implicit_weights));
		}
		if (ctx->t.pps.weighted_bipred_idc == 1 && (l == 0) != (ctx->mbaff_tables == 0)) {
			memcpy(ctx->t.explicit_weights, ctx->mbaff_ew[field], sizeof(ctx->t.explicit_weights));
			memcpy(ctx->t.explicit_offsets, ctx->mbaff_eo[field], sizeof(ctx->t.explicit_offsets));
		}
		ctx->mbaff_tables = l;
	}
	if (ctx->t.slice_type < 2) {
		int n0 = ctx->t.pps.num_ref_idx_active[0] << field;
		int n1 = ctx->t.pps.num_ref_idx_active[1] << field;
		ctx->num_ref_idx_mask = (n0 > 1) * 0x0f + (n1 > 1) * 0xf0;
		int max0 = n0 - 1;
		int max1 = ctx->t.slice_type == 0 ? -1 : n1 - 1;
		ctx->clip_ref_idx_v = (i8x8){max0, max0, max0, max0, max1, max1, max1, max1};
	}
}

/**
 * Initializes the context of an MBAFF slice after initialize_context: its
 * first pair, the frame values that mbaff_state changes, and the reference
 * lists of field macroblocks.
 */
static void initialize_mbaff(Edge264MvcContext *ctx) {
	int w = ctx->t.pic_width_in_mbs, pair = ctx->t.first_mb_in_slice >> 1;
	ctx->mbaff_px = pair % w;
	ctx->mbaff_py = pair / w;
	ctx->mbaff_stride[0] = ctx->t.stride[0];
	ctx->mbaff_stride[1] = ctx->t.stride[1];
	ctx->mbaff_height = ctx->t.pic_height_in_mbs;
	ctx->mbaff_cache = ctx->mbc_ring;
	for (int i = 0; i < 4; i++)
		ctx->mbaff_lists_v[0][i] = ctx->t.RefPicList_v[i];
	
	// the neighbouring offsets of available neighbours (as parse_slice_data sets
	// them) in ctx->mbaff_nb, laid out as a picture of width 2, and in
	// ctx->mbaff_nbc, with the D locations of the left column in mbaff_nb[5]
	int mb4 = (int)(sizeof(Edge264MvcMacroblock) >> 2), mbc1 = (int)sizeof(Edge264MvcMbCache);
	int offB_int8 = -mbc1, offB_int32 = 3 * mb4;
	ctx->A4x4_int8[0] = 5 - mbc1;
	ctx->A4x4_int8[2] = 7 - mbc1;
	ctx->A4x4_int8[8] = 13 - mbc1;
	ctx->A4x4_int8[10] = 15 - mbc1;
	ctx->B4x4_int8[0] = 10 - offB_int8;
	ctx->B4x4_int8[1] = 11 - offB_int8;
	ctx->B4x4_int8[4] = 14 - offB_int8;
	ctx->B4x4_int8[5] = 15 - offB_int8;
	if (ctx->t.ChromaArrayType == 1) {
		ctx->ACbCr_int8[0] = 1 - mbc1;
		ctx->ACbCr_int8[2] = 3 - mbc1;
		ctx->ACbCr_int8[4] = 5 - mbc1;
		ctx->ACbCr_int8[6] = 7 - mbc1;
		ctx->BCbCr_int8[0] = 2 - offB_int8;
		ctx->BCbCr_int8[1] = 3 - offB_int8;
		ctx->BCbCr_int8[4] = 6 - offB_int8;
		ctx->BCbCr_int8[5] = 7 - offB_int8;
	} else if (ctx->t.ChromaArrayType == 2) {
		for (int i = 0; i < 16; i += 2)
			ctx->ACbCr_int8[i] = i + 1 - mbc1;
		ctx->BCbCr_int8[0] = 6 - offB_int8;
		ctx->BCbCr_int8[1] = 7 - offB_int8;
		ctx->BCbCr_int8[8] = 14 - offB_int8;
		ctx->BCbCr_int8[9] = 15 - offB_int8;
	}
	if (ctx->t.slice_type < 2) {
		ctx->absMvd_A[0] = 10 - mbc1;
		ctx->absMvd_A[2] = 14 - mbc1;
		ctx->absMvd_A[8] = 26 - mbc1;
		ctx->absMvd_A[10] = 30 - mbc1;
		ctx->absMvd_B[0] = 20 - offB_int8;
		ctx->absMvd_B[1] = 22 - offB_int8;
		ctx->absMvd_B[4] = 28 - offB_int8;
		ctx->absMvd_B[5] = 30 - offB_int8;
		ctx->mvs_A[0] = 5 - mb4;
		ctx->mvs_A[2] = 7 - mb4;
		ctx->mvs_A[8] = 13 - mb4;
		ctx->mvs_A[10] = 15 - mb4;
		ctx->mvs_B[0] = 10 - offB_int32;
		ctx->mvs_B[1] = 11 - offB_int32;
		ctx->mvs_B[4] = 14 - offB_int32;
		ctx->mvs_B[5] = 15 - offB_int32;
		ctx->mvs_C[0] = 11 - offB_int32;
		ctx->mvs_C[1] = 14 - offB_int32;
		ctx->mvs_C[4] = 15 - offB_int32;
		ctx->mvs_C[5] = 10 - 2 * mb4;
		ctx->mvs_D[0] = 15 - 4 * mb4;
		ctx->mvs_D[1] = 10 - offB_int32;
		ctx->mvs_D[4] = 11 - offB_int32;
		ctx->mvs_D[5] = 14 - offB_int32;
		ctx->mvs_D[2] = 5 + mb4;
		ctx->mvs_D[8] = 7 + mb4;
		ctx->mvs_D[10] = 13 + mb4;
	}
	for (int p = 0; p < 2; p++) {
		for (int lx = 0; lx < 2; lx++) {
			for (int i = 0; i < 32; i++) {
				int f = ctx->t.RefPicList[lx][i >> 1];
				ctx->mbaff_lists[1 + p][lx][i] = f < 0 ? f : f + ((p ^ (i & 1)) ? FIELD_ID : 0);
			}
		}
	}
	
	// the weights of field macroblocks (8.4.2.3), explicit ones with refIdx >> 1
	// and implicit ones from the fields of the same parity as the macroblock
	// (8.4.2.3.1), likewise for temporal direct prediction (8.4.1.2.3)
	ctx->mbaff_tables = 0;
	if (ctx->t.pps.weighted_bipred_idc == 1) {
		memcpy(ctx->mbaff_ew[0], ctx->t.explicit_weights, sizeof(ctx->t.explicit_weights));
		memcpy(ctx->mbaff_eo[0], ctx->t.explicit_offsets, sizeof(ctx->t.explicit_offsets));
		for (int c = 0; c < 3; c++) {
			for (int i = 0; i < 64; i++) {
				ctx->mbaff_ew[1][c][i] = ctx->t.explicit_weights[c][(i & 32) + (i & 31) / 2];
				ctx->mbaff_eo[1][c][i] = ctx->t.explicit_offsets[c][(i & 32) + (i & 31) / 2];
			}
		}
	}
	if (ctx->t.slice_type == 1 && (ctx->t.pps.weighted_bipred_idc == 2 || !ctx->t.direct_spatial_mv_pred_flag)) {
		memcpy(ctx->mbaff_DSF[0], ctx->DistScaleFactor, sizeof(ctx->DistScaleFactor));
		memcpy(ctx->mbaff_map[0], ctx->MapPicToList0, sizeof(ctx->MapPicToList0));
		memcpy(ctx->mbaff_iw[0], ctx->implicit_weights, sizeof(ctx->implicit_weights));
		int n0 = ctx->t.pps.num_ref_idx_active[0] * 2;
		int n1 = ctx->t.pps.weighted_bipred_idc == 2 ? ctx->t.pps.num_ref_idx_active[1] * 2 : 1;
		for (int p = 0; p < 2; p++) {
			const int32_t *diff = ctx->t.diff_poc_fld[p];
			memset(ctx->mbaff_map[1 + p], 0, sizeof(ctx->mbaff_map[1 + p]));
			for (int refIdxL0 = n0; refIdxL0-- > 0; ) {
				int pic0 = ctx->mbaff_lists[1 + p][0][refIdxL0];
				if (pic0 < 0)
					continue;
				ctx->mbaff_map[1 + p][pic0] = refIdxL0;
				int long0 = ctx->t.prev_long_term_frames >> (pic0 & (FIELD_ID - 1)) & 1;
				int tb = clip3(-128, 127, diff[pic0]), DistScaleFactor = 256;
				for (int refIdxL1 = n1; refIdxL1-- > 0; ) {
					int pic1 = ctx->mbaff_lists[1 + p][1][refIdxL1];
					int td = pic1 < 0 ? 0 : clip3(-128, 127, (int32_t)((unsigned)diff[pic0] - (unsigned)diff[pic1]));
					int implicit_weight = 32;
					if (td != 0 && !long0) {
						int tx = (16384 + abs(td / 2)) / td;
						DistScaleFactor = min(max((tb * tx + 32) >> 6, -1024), 1023);
						if (!(ctx->t.prev_long_term_frames >> (pic1 & (FIELD_ID - 1)) & 1) && DistScaleFactor >= -256 && DistScaleFactor <= 515)
							implicit_weight = DistScaleFactor >> 2;
					} else {
						DistScaleFactor = 256;
					}
					ctx->mbaff_iw[1 + p][refIdxL0][refIdxL1] = implicit_weight + 64;
				}
				ctx->mbaff_DSF[1 + p][refIdxL0] = DistScaleFactor;
			}
		}
	}
}

/**
 * Restores the frame values of the context when the slice ends.
 */
static void finish_mbaff(Edge264MvcContext *ctx) {
	ctx->t.field_pic_flag = 0;
	ctx->t.bottom_field_flag = 0;
	ctx->t.stride[0] = ctx->mbaff_stride[0];
	ctx->t.stride[1] = ctx->t.stride[2] = ctx->mbaff_stride[1];
	ctx->t.pic_height_in_mbs = ctx->mbaff_height;
	for (int i = 0; i < 4; i++)
		ctx->t.RefPicList_v[i] = ctx->mbaff_lists_v[0][i];
	if (ctx->mbaff_tables != 0) {
		if (ctx->t.slice_type == 1 && (ctx->t.pps.weighted_bipred_idc == 2 || !ctx->t.direct_spatial_mv_pred_flag)) {
			memcpy(ctx->DistScaleFactor, ctx->mbaff_DSF[0], sizeof(ctx->DistScaleFactor));
			memcpy(ctx->MapPicToList0, ctx->mbaff_map[0], sizeof(ctx->MapPicToList0));
			memcpy(ctx->implicit_weights, ctx->mbaff_iw[0], sizeof(ctx->implicit_weights));
		}
		if (ctx->t.pps.weighted_bipred_idc == 1) {
			memcpy(ctx->t.explicit_weights, ctx->mbaff_ew[0], sizeof(ctx->t.explicit_weights));
			memcpy(ctx->t.explicit_offsets, ctx->mbaff_eo[0], sizeof(ctx->t.explicit_offsets));
		}
		ctx->mbaff_tables = 0;
	}
}

/**
 * Gathers the values that direct prediction reads of the colocated macroblock
 * in an MBAFF frame (8.4.1.2.1, Tables 8-6 and 8-8 for AFRM, with
 * direct_8x8_inference_flag 1, as in colocated_cross): the corner 4x4 block
 * of each 8x8 partition takes the block at (xCol, yM) of mbAddrCol, in the
 * field of RefPicList1[0] (of the parity of a field macroblock, else the
 * closer one) if it was decoded as fields, else in the frame, of which a
 * field macroblock takes the frame macroblock at yCol / 8, and a frame
 * macroblock takes the field macroblock of the closer field. The references
 * are those MapColToList0 looks for, the field of the parity of a field
 * macroblock or the frame of a frame macroblock, and temporal direct
 * prediction gets the vertical motion vectors scaled (8-193, 8-194).
 */
static noinline void colocated_mbaff(Edge264MvcContext *ctx) {
	static const int8_t xCol[4] = {0, 12, 0, 12}, yCol[4] = {0, 0, 12, 12};
	Edge264MvcMacroblock *c = &ctx->mbCol_cross;
	int w = ctx->t.pic_width_in_mbs, px = ctx->mbaff_px, py = ctx->mbaff_py;
	int bot = ctx->CurrMbAddr & 1, field = ctx->mb_field;
	int slot = ctx->mbaff_lists[0][1][0];
	int temporal = !ctx->t.direct_spatial_mv_pred_flag;
	const Edge264MvcMacroblock *frame = ctx->t.mbCol_buffer;
	int field_col = (ctx->t.field_frames >> slot & 1);
	*c = unavail_mb;
	c->f.inter_eqs_s = 0;
	for (int k = 0; k < 4; k++) {
		const Edge264MvcMacroblock *s;
		int yM, scale = 0, src_field; // scale: 1 = Frm_To_Fld, -1 = Fld_To_Frm
		if (field_col) { // colPic is a field (FLD), mbAddrCol5
			int p = field ? bot : ctx->t.col_bottom;
			s = frame + (p * (ctx->mbaff_height >> 1) + py) * (w + 1) + px;
			yM = field ? yCol[k] : 8 * bot + 4 * (yCol[k] >> 3);
			scale = field ? 0 : -1;
			src_field = 1;
		} else { // colPic is the frame (AFRM)
			const Edge264MvcMacroblock *X = frame + (py * 2 + bot) * (w + 1) + px;
			src_field = X->f.mb_field_decoding_flag;
			if (field == src_field) {
				s = X, yM = yCol[k];
			} else if (!field) { // mbAddrCol6
				s = frame + (py * 2 + ctx->t.col_bottom) * (w + 1) + px;
				yM = 8 * bot + 4 * (yCol[k] >> 3);
				scale = -1;
			} else { // mbAddrCol7
				s = frame + (py * 2 + (yCol[k] >> 3)) * (w + 1) + px;
				yM = (2 * yCol[k]) & 15;
				scale = 1;
			}
		}
		int b = (yM >> 3) * 8 + (xCol[k] >> 3) * 4 + ((yM & 7) >> 2) * 2 + ((xCol[k] & 7) >> 2);
		c->mbIsInterFlag |= s->mbIsInterFlag;
		for (int lx = 0; lx < 2; lx++) {
			int refIdx = s->refIdx[lx * 4 + (b >> 2)];
			int refPic = s->refPic[lx * 4 + (b >> 2)];
			c->refIdx[lx * 4 + k] = refIdx;
			if (refPic >= 0)
				refPic = field ? (src_field ? refPic : refPic + bot * FIELD_ID) : refPic & (FIELD_ID - 1);
			c->refPic[lx * 4 + k] = refPic;
			int mvx = refIdx < 0 ? 0 : s->mvs[lx * 32 + b * 2];
			int mvy = refIdx < 0 ? 0 : s->mvs[lx * 32 + b * 2 + 1];
			if (temporal && scale)
				mvy = scale > 0 ? mvy / 2 : mvy * 2;
			c->mvs[lx * 32 + k * 8 + k * 2] = mvx;
			c->mvs[lx * 32 + k * 8 + k * 2 + 1] = mvy;
		}
	}
	ctx->mbCol = c;
}
#endif



/**
 * The macroblock loop of MBAFF slices (7.3.4), see above.
 */
static noinline void CAFUNC(parse_slice_data_mbaff_loop)
{
	int end_of_slice_flag = 0;
	int w = ctx->t.pic_width_in_mbs;
	do {
		#ifdef LOGS
			if (ctx->thread_id < 0) {
				log_mb(ctx, "  - mbAddr: %u\n", ctx->CurrMbAddr);
			} else {
				log_mb(ctx, "\n- thread_id: %d\n"
					"  FrameId: %u\n"
					"  mbAddr: %u\n",
					ctx->thread_id, ctx->t.FrameId, ctx->CurrMbAddr);
			}
		#endif
		if (__builtin_expect(ctx->t.mb_bound == BOUND_UNKNOWN, 0)) {
			int32_t bound = __atomic_load_n(&ctx->d->task_bounds[ctx->task_id], __ATOMIC_ACQUIRE);
			if (bound != BOUND_UNKNOWN)
				known_mb_bound(ctx, bound, 0);
		}
		if (ctx->CurrMbAddr >= ctx->t.mb_bound)
			return;
		int bottom = ctx->CurrMbAddr & 1;
		int px = ctx->mbaff_px, py = ctx->mbaff_py;
		Edge264MvcMacroblock *real = mbaff_pair_mb(ctx, px, py, bottom);
		ctx->_mb = ctx->mbaff_mb = real;
		int prev_recovery_bits = __atomic_exchange_n(&real->recovery_bits, ctx->t.frame_flip_bit, __ATOMIC_ACQ_REL);
		if (prev_recovery_bits == ctx->t.frame_flip_bit && !claim_after_older_slices(ctx))
			return;
		
		// availability of the neighbouring pairs, and mb_field_decoding_flag with
		// the skip flags it may follow (7.3.4, 7.4.4)
		if (!bottom) {
			int pair = ctx->CurrMbAddr >> 1, first = ctx->t.first_mb_in_slice >> 1;
			int availA = px > 0 && pair - 1 >= first;
			int availB = py > 0 && pair - w >= first;
			int availC = py > 0 && px < w - 1 && pair - w + 1 >= first;
			int availD = px > 0 && py > 0 && pair - w - 1 >= first;
			ctx->mbaff_avail = availA | availB << 1 | availC << 2 | availD << 3;
			int inferred = availA ? mbaff_pair_field(ctx, MBAFF_A) : availB ? mbaff_pair_field(ctx, MBAFF_B) : 0;
			ctx->mb_field = inferred;
			int read_flag = 1;
			ctx->mbaff_skip = -1;
			if (ctx->t.slice_type != 2) {
				#if !CABAC
					if (ctx->mb_skip_run < 0) {
						ctx->mb_skip_run = get_ue32(&ctx->t.gb, 139264);
						log_mb(ctx, "%smb_skip_run: %u\n", ctx->log_indent, ctx->mb_skip_run);
					}
					// a skipped top macroblock followed by a skipped bottom one
					read_flag = ctx->mb_skip_run < 2 && !(ctx->mb_skip_run == 1 && rbsp_end(&ctx->t.gb, 1));
				#else
					int base = ctx->t.slice_type == 0 ? 13 : 26;
					ctx->mbaff_skip = get_ae(ctx, base - mbaff_skip_inc(ctx, 0)); // logged by parse_P/B_mb
					if (ctx->mbaff_skip) {
						ctx->mbaff_bot_skip = get_ae(ctx, base - mbaff_skip_inc(ctx, 1));
						log_mb(ctx, "%smb_skip_flag_of_bottom: %u\n", ctx->log_indent, ctx->mbaff_bot_skip);
						read_flag = !ctx->mbaff_bot_skip;
					}
				#endif
			}
			if (read_flag) {
				#if !CABAC
					ctx->mb_field = get_u1(&ctx->t.gb);
				#else
					int inc = (availA && mbaff_pair_field(ctx, MBAFF_A)) + (availB && mbaff_pair_field(ctx, MBAFF_B));
					ctx->mb_field = get_ae(ctx, 70 + inc);
				#endif
				log_mb(ctx, "%smb_field_decoding_flag: %u\n", ctx->log_indent, ctx->mb_field);
			}
		} else {
			#if CABAC
				// the bottom skip flag was parsed ahead if the top macroblock was skipped
				ctx->mbaff_skip = ctx->mbaff_skip > 0 ? ctx->mbaff_bot_skip : -1;
			#endif
		}
		
		// decode the macroblock amid its remapped neighbours
		mbaff_state(ctx, ctx->mb_field, bottom);
		mbaff_neighbours(ctx);
		ctx->mbaff_nb[4] = *real;
		ctx->_mb = &ctx->mbaff_nb[4];
		ctx->_mbc = &ctx->mbaff_nbc[1];
		mbA = &ctx->mbaff_nb[3];
		mbB = &ctx->mbaff_nb[1];
		mbC = &ctx->mbaff_nb[2];
		mbD = &ctx->mbaff_nb[0];
		ctx->mbCol = real;
		
		// availability of the neighbouring macroblocks A/B/C/D (6.4.12.2), and of
		// the edges to deblock, whose top edge in a frame pair is always there for
		// the bottom macroblock
		int a = ctx->mbaff_avail, frame_bot = !ctx->mb_field && bottom;
		int unavail16x16 = (~a & 1) | (frame_bot ? 4 : (~a & 2) | (~a & 4)) | (frame_bot ? (~a & 1) << 3 : ~a & 8);
		int filter_edges = 4 | (px > 0 && (ctx->t.disable_deblocking_filter_idc != 2 || (a & 1))) |
			(frame_bot || (py > 0 && (ctx->t.disable_deblocking_filter_idc != 2 || (a & 2)))) << 1;
		
		// initialize common macroblock values (as parse_slice_data)
		ctx->unavail4x4_v[0] = block_unavailability[unavail16x16];
		ctx->unavail16x16 = unavail16x16;
		ctx->inc.v = mbA->f.v + mbB->f.v + (mbB->f.v & flags_twice.v);
		mb->f.v = (i8x16){};
		mb->f.mb_field_decoding_flag = ctx->mb_field;
		mb->filter_edges = (ctx->t.disable_deblocking_filter_idc != 1) ? filter_edges : 0;
		mb->QP_s = ctx->t.QP_s;
		if (ctx->t.ChromaArrayType == 1)
			ctx->unavail4x4_v[1] = shuffle(ctx->unavail4x4_v[0], (i8x16){0, 4, 8, 12, 0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1});
		else if (ctx->t.ChromaArrayType == 2)
			ctx->unavail4x4_v[1] = shuffle(ctx->unavail4x4_v[0], (i8x16){0, 4, 2, 6, 8, 12, 10, 14, 0, 4, 2, 6, 8, 12, 10, 14});
		if (ctx->t.ChromaArrayType < 3)
			mb->bits_l = (mbA->bits_l >> 3 & 0x11111100111111) | (mbB->bits_l >> 1 & 0x42424200424242);
		mbc->nC_v[0] = mbc->nC_v[1] = mbc->nC_v[2] = (i8x16){};
		#if CABAC
			for (int i = 1; i < 4; i++) {
				ctx->sig_inc_v[i] = sig_inc_8x8[ctx->mb_field][i];
				ctx->scan_v[i] = scan_8x8_cabac[ctx->mb_field][i];
			}
		#endif
		
		if (ctx->t.slice_type == 0) {
			CACALL(parse_P_mb);
		} else if (ctx->t.slice_type == 1) {
			await_reference(ctx, ctx->t.RefPicList[1][0], ctx->mby);
			colocated_mbaff(ctx);
			CACALL(parse_B_mb);
		} else {
			int mb_type_or_ctxIdx = CACOND(get_ue16(&ctx->t.gb, 25), 5 - ctx->inc.mb_type_I_NxN);
			#if !CABAC
				log_mb(ctx, "%smb_type: %u\n", ctx->log_indent, mb_type_or_ctxIdx);
			#endif
			CACALL(parse_I_mb, mb_type_or_ctxIdx);
		}
		#if CABAC
			if (bottom) {
				end_of_slice_flag = cabac_terminate(ctx);
				log_mb(ctx, "%send_of_slice_flag: %u\n", ctx->log_indent, end_of_slice_flag);
			}
		#endif
		print_mb(ctx);
		mb->nC_Y_v = mbc->nC_v[0];
		#if !CABAC
			if (mb->f.transform_size_8x8_flag)
				mb->nC_Y_v = (i8x16)((i32x4)mb->nC_Y_v == 0) - -1;
		#endif
		mb->f.mb_field_decoding_flag = ctx->mb_field;
		
		// copy the macroblock back, except recovery_bits that it claimed above
		memcpy(real, mb, 1);
		memcpy((char *)real + 2, (char *)mb + 2, sizeof(*mb) - 2);
		*mbaff_pair_mbc(ctx, px, py, bottom) = *mbc;
		ctx->_mb = real;
		
		ctx->CurrMbAddr++;
		if (bottom) {
			ctx->mbaff_px = ++px;
			if (px >= w) {
				ctx->mbaff_px = 0;
				ctx->mbaff_py = ++py;
				if (py * 2 >= ctx->mbaff_height)
					return;
			}
		}
	} while (CACOND(ctx->mb_skip_run > 0 || !rbsp_end(&ctx->t.gb, 1), !end_of_slice_flag));
}

static noinline void CAFUNC(parse_slice_data_mbaff)
{
	initialize_mbaff(ctx);
	CACALL(parse_slice_data_mbaff_loop);
	finish_mbaff(ctx);
}



#undef CABAC
