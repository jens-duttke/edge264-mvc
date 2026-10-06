#include "edge264mvc_internal.h"
static noinline void known_mb_bound(Edge264MvcContext *ctx, int32_t mb_bound, int claimed);
static noinline int claim_after_older_slices(Edge264MvcContext *ctx);
static void ack_mb_bound(Edge264MvcDecoder *dec, int task_id, int pic);

#include "edge264mvc_bitstream.c"
#include "edge264mvc_deblock.c"
#include "edge264mvc_inter.c"
#include "edge264mvc_intra.c"
#include "edge264mvc_mvpred.c"
#include "edge264mvc_residual.c"
#ifdef LOGS
	#include "edge264mvc_sei.c"
#endif
#define CABAC 0
#include "edge264mvc_slice.c"
#define CABAC 1
#include "edge264mvc_slice.c"



/**
 * Default scaling matrices (tables 7-3 and 7-4).
 */
static const i8x16 Default_4x4_Intra =
	{6, 13, 20, 28, 13, 20, 28, 32, 20, 28, 32, 37, 28, 32, 37, 42};
static const i8x16 Default_4x4_Inter =
	{10, 14, 20, 24, 14, 20, 24, 27, 20, 24, 27, 30, 24, 27, 30, 34};
static const i8x16 Default_8x8_Intra[4] = {
	{ 6, 10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29},
	{13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33},
	{18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38},
	{25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42},
};
static const i8x16 Default_8x8_Inter[4] = {
	{ 9, 13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25},
	{15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28},
	{19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32},
	{22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35},
};



/**
 * These functions ease management of the DPB and the decoder state.
 * 
 * unset_currPic is called when a new picture is detected as per 7.4.1.2.4:
 * _ frame_num differs (dec->FrameNum contains the previous unwrapped decoded
 *   value unaffected by mmco=5)
 * _ nal_ref_idc differs
 * _ pic_order_cnt_type=0 and pic_order_cnt_lsb differs
 *   (dec->TopFieldOrderCnt contains the previous value unaffected by mmco=5)
 * _ pic_order_cnt_type=1 and TopFieldOrderCnt differs (as long as frame_num
 *   and nal_ref_idc tests are done before, this is equivalent with a
 *   difference on delta_pic_order_cnt[0])
 * _ idr_pic_id differs (useful on streams of IDR frames to distinguish them),
 *   with -1 when IdrPicFlag=0
 * _ view_id differs (between base and non-base)
 * 
 * pic_parameter_set_id is ignored since not stored, and spec mandates that
 * POCs should differ anyway. BottomFieldOrderCnt is ignored too because the
 * test on TopFieldOrderCnt is sufficient.
 */
static void release_held_task(Edge264MvcDecoder *dec, int32_t mb_bound);
static void settle_mb_bounds(Edge264MvcDecoder *dec, int pic);

static void unset_currPic(Edge264MvcDecoder *dec) {
	assert(dec->currPic >= 0);
	release_held_task(dec, INT_MAX); // the picture ends, so does its last slice
	settle_mb_bounds(dec, dec->currPic);
	int non_base_view = dec->non_base_frames >> dec->currPic & 1;
	// a picture the reference marking ran for sets PrevRefFrameNum (7.4.3) and
	// commits the marking, even when it discarded the picture itself
	if (dec->currPic_marked) {
		FrameMask same_views = non_base_view ? dec->non_base_frames : ~dec->non_base_frames;
		dec->PrevRefFrameNum[non_base_view] = dec->FrameNums[dec->currPic];
		dec->prevPicOrderCnt[non_base_view] = dec->FieldOrderCnt[0][dec->currPic];
		dec->prev_short_term_frames = (dec->prev_short_term_frames & ~same_views) | dec->short_term_frames;
		dec->prev_long_term_frames = (dec->prev_long_term_frames & ~same_views) | dec->long_term_frames;
		memcpy(dec->prev_LongTermFrameIdx, dec->LongTermFrameIdx, sizeof(dec->LongTermFrameIdx));
	}
	if (!non_base_view)
		dec->basePic = dec->currPic;
	dec->currPic = -1;
}

/**
 * Removes entry i of an output queue and closes the gap, so that the entries
 * stay packed at the front. bump_frame shifts a new entry in at index 0 and
 * drops the last one, and the fullness gate in decode_nal counts the
 * entries before the first empty one, so a gap left in the middle let later
 * bumps push a queued picture out of the queue, where get_frame never sees it.
 */
static void dequeue_frame(Edge264MvcDecoder *dec, int view, int i) {
	for (; i < QUEUE_SIZE - 1; i++)
		dec->get_frame_queue[view][i] = dec->get_frame_queue[view][i + 1];
	dec->get_frame_queue[view][QUEUE_SIZE - 1] = -1;
}

static int bump_frame(Edge264MvcDecoder *dec, int non_base_view, FrameMask ignored) {
	int pic = -1;
	int lowest_poc = INT_MAX;
	FrameMask same_views = non_base_view ? dec->non_base_frames : ~dec->non_base_frames;
	for (FrameMask o = dec->to_get_frames & ~dec->output_frames & same_views & ~ignored; o; o &= o - 1) {
		int i = mask_ctz(o);
		// Keep MVC output base-driven: never queue a dependent while its base is
		// still held. get_frame delivers by scanning the base queue and pairing each
		// base with its dependent, so a dependent queued ahead of its base cannot be
		// delivered - under DPB pressure that stalls the decoder, and reverse-pairing
		// the base to unstick it emits the base in decode rather than display order,
		// reordering a stream whose decode and display order differ (a frame_num gap
		// - issue #2). The base view's own bump queues the pair together, in display
		// order. A truly base-less dependent (dropped/corrupt base NAL) is drained by
		// get_frame's orphan valve, not here.
		if (non_base_view) {
			int paired = 0;
			for (FrameMask b = dec->output_frames & ~dec->non_base_frames; b; b &= b - 1) {
				int bb = mask_ctz(b);
				if (dec->FrameNums[bb] == dec->FrameNums[i] &&
					dec->FieldOrderCnt[0][bb] == dec->FieldOrderCnt[0][i]) {
					paired = 1;
					break;
				}
			}
			if (!paired)
				continue;
		}
		if (dec->FieldOrderCnt[0][i] < lowest_poc)
			lowest_poc = dec->FieldOrderCnt[0][pic = i];
	}
	if (pic < 0)
		return 0;
	// Bumping happens on the parsing thread in strict display order (lowest POC
	// first, prior GOP fully bumped before an IDR's frames), so a counter
	// captured here gives a globally monotonic display rank. Multithreaded
	// output then orders by this rank instead of the raw POC, which is not
	// monotonic across a POC reset (IDR) and would otherwise let a new GOP's
	// low-POC frame overtake the previous GOP's frames still in the queue.
	dec->DispOrder[pic] = dec->next_dispnum++;
	dec->output_frames |= (FrameMask)1 << pic;
	queue_push(dec, non_base_view, pic);
	return 1;
}

static int conceal_frame(Edge264MvcDecoder *dec, int id);
static void progress_or_wait(Edge264MvcDecoder *dec);

static int bump_all_frames(Edge264MvcDecoder *dec) {
	if (dec->currPic >= 0)
		unset_currPic(dec);
	while (bump_frame(dec, 0, 0) | bump_frame(dec, 1, 0));
	while (tm_any(dec->busy_tasks))
		progress_or_wait(dec);
	// Forward progress on a flush drain: an errored picture that never finalized
	// (its slice returned EBADMSG, so next_deblock_addr != INT_MAX) was bumped into
	// the output queue but later shifted out by other bumps without being
	// delivered - the flushing valve in get_frame skips an unfinished picture
	// mid-stream. Left in to_get_frames yet absent from the queue it is unreachable,
	// so this used to return ENOBUFS forever and a draining caller stalled. Conceal
	// it (finalize) and slot it back into the queue so the drain terminates - ffmpeg
	// likewise emits a damaged picture from such a corrupt stream. Inert for
	// well-formed streams, where every pending picture is finalized and still queued.
	FrameMask queued = 0;
	for (int i = 0; i < QUEUE_SIZE; i++) {
		if (dec->get_frame_queue[0][i] >= 0)
			queued |= (FrameMask)1 << dec->get_frame_queue[0][i];
		if (dec->get_frame_queue[1][i] >= 0)
			queued |= (FrameMask)1 << dec->get_frame_queue[1][i];
	}
	// Any picture still incomplete has no writer left (busy_tasks is empty) and
	// no slice to come, so conceal it now rather than let get_frame emit its
	// undecoded part with whatever its slot held before - which depends on the
	// slot allocation, hence on the thread timing. Bases first, since a damaged
	// dependent view is concealed from its base.
	for (FrameMask o = dec->to_get_frames & ~dec->non_base_frames; o; o &= o - 1) {
		int i = mask_ctz(o);
		if (__atomic_load_n(&dec->next_deblock_addr[i], __ATOMIC_ACQUIRE) != INT_MAX)
			conceal_frame(dec, i);
	}
	for (FrameMask o = dec->to_get_frames & dec->non_base_frames; o; o &= o - 1) {
		int i = mask_ctz(o);
		if (__atomic_load_n(&dec->next_deblock_addr[i], __ATOMIC_ACQUIRE) != INT_MAX)
			conceal_frame(dec, i);
	}
	for (FrameMask o = dec->to_get_frames & ~queued; o; o &= o - 1) {
		int i = mask_ctz(o);
		int v = dec->non_base_frames >> i & 1;
		for (int j = 0; j < QUEUE_SIZE; j++) {
			if (dec->get_frame_queue[v][j] < 0) {
				dec->get_frame_queue[v][j] = i;
				// a queued picture is marked for output, as by every other path
				// that queues one (a dependent view decoded before its base view
				// may not be yet), so that it is neither queued again nor dropped
				// while the caller holds it
				dec->output_frames |= (FrameMask)1 << i;
				break;
			}
		}
	}
	return dec->to_get_frames | dec->output_frames ? ENOBUFS : 0;
}

/**
 * Deterministic parse-side counterpart of get_frame's unpaired-base pairing
 * valve. get_frame bumps a parsed-but-unqueued dependent view lazily when its
 * base reaches the front of the output queue, gated on the dependent being
 * fully decoded. Under multithreading that gate depends on worker timing, and
 * the bump's output_frames side effect feeds back into the parse-side DPB
 * fullness/reorder triggers (they count to_get_frames & ~output_frames), so
 * the whole bump sequence - and with it the output order - becomes run-to-run
 * non-deterministic on real MVC streams. Do the same catch-up eagerly here on
 * the parsing thread after every NAL: walk the queued bases in display-rank
 * order and queue each one's parsed dependent (decoded or not - readiness is
 * checked at emission), stopping at the first base whose dependent is not yet
 * parsed, exactly where a single-threaded draining caller's valve would stop.
 * This keeps the trigger inputs a pure function of the parse history, so
 * single-thread and multithreaded runs bump and emit in the same order, and
 * the consumer-side valve never fires on a well-formed stream.
 */
static void catch_up_dependent_bumps(Edge264MvcDecoder *dec) {
	if (dec->ssps.BitDepth_Y == 0)
		return;
	FrameMask done = 0;
	for (;;) {
		int front = -1;
		int lowest = INT_MAX;
		for (int i = 0; i < QUEUE_SIZE && dec->get_frame_queue[0][i] >= 0; i++) { // packed at the front
			int q = dec->get_frame_queue[0][i];
			if (!(done & (FrameMask)1 << q) && dec->DispOrder[q] < lowest)
				lowest = dec->DispOrder[front = q];
		}
		if (front < 0)
			return;
		done |= (FrameMask)1 << front;
		// the two views of one access unit share a FrameNum and a POC
		int dep = -1;
		for (FrameMask o = dec->to_get_frames & dec->non_base_frames; o; o &= o - 1) {
			int d = mask_ctz(o);
			if (dec->FrameNums[d] == dec->FrameNums[front] &&
				dec->FieldOrderCnt[0][d] == dec->FieldOrderCnt[0][front]) {
				dep = d;
				break;
			}
		}
		if (dep < 0)
			return; // not parsed yet - a single-threaded draining caller would hold here
		if (!(dec->output_frames & (FrameMask)1 << dep)) {
			dec->output_frames |= (FrameMask)1 << dep;
			queue_push(dec, 1, dep);
		}
	}
}

static void flush_frames(Edge264MvcDecoder *dec) {
	// FIXME interrupt all threads then wait until they are back to wait
	release_held_task(dec, INT_MAX);
	assert(!(dec->n_threads == 0 && tm_any(dec->busy_tasks)));
	while (tm_any(dec->busy_tasks))
		progress_or_wait(dec);
}

// Returns ENOMEM if the memory could not be allocated, which no number of
// frames received would change (AGAIN would promise that).
static int alloc_frame(Edge264MvcDecoder *dec, int id) {
	int mbs = (dec->sps.pic_width_in_mbs + 1) * dec->sps.pic_height_in_mbs - 1;
	// The neighbours of the top row (B, C, D, up to pic_width_in_mbs + 2
	// macroblocks back) are read before their availability masks them out, so
	// they need memory of their own: a row of unavailable macroblocks before
	// the picture's. Without it those reads fell on the end of the samples, which
	// the slices decoding the bottom of the picture write meanwhile.
	int guard = dec->sps.pic_width_in_mbs + 2;
	unsigned samples_size = (dec->plane_size_Y + dec->plane_size_C + 16 + 63) & -64; // plus margin for overreads, and cache line alignment of mbs
	unsigned mbs_size = sizeof(Edge264MvcMacroblock) * (guard + mbs);
	dec->alloc_cb((void **)&dec->samples_buffers[id], samples_size, (void **)&dec->mb_buffers[id], mbs_size, dec->alloc_arg);
	Edge264MvcMacroblock *m = dec->mb_buffers[id];
	if (dec->samples_buffers[id] && m) {
		for (int i = 0; i < guard; i++)
			m[i] = unavail_mb;
		m = dec->mb_buffers[id] = m + guard;
		for (int i = 0; i < mbs; i += dec->sps.pic_width_in_mbs + 1) {
			for (int j = i; j < i + dec->sps.pic_width_in_mbs; j++)
				m[j].recovery_bits = 0;
			if (i + dec->sps.pic_width_in_mbs < mbs)
				m[i + dec->sps.pic_width_in_mbs] = unavail_mb;
		}
		if (id >= dec->frame_slots)
			__atomic_store_n(&dec->frame_slots, id + 1, __ATOMIC_RELAXED);
		return 0;
	} else {
		dec->free_cb(dec->samples_buffers[id], m, dec->alloc_arg);
		dec->samples_buffers[id] = NULL;
		dec->mb_buffers[id] = NULL;
		return ENOMEM;
	}
}

static void clear_decoder(Edge264MvcDecoder *dec) {
	// frames received but not released yet keep their slots until released
	FrameMask held = dec->output_frames & ~dec->to_get_frames;
	memset((void *)dec + offsetof(Edge264MvcDecoder, nal_ref_idc), 0, offsetof(Edge264MvcDecoder, log_base_us) - offsetof(Edge264MvcDecoder, nal_ref_idc));
	dec->output_frames = held;
	dec->currPic = dec->basePic = -1;
	dec->held_task = -1;
	memset(dec->task_wait_pic, -1, sizeof(dec->task_wait_pic));
	dec->PrevRefFrameNum[0] = dec->PrevRefFrameNum[1] = -1;
	memset(dec->get_frame_queue, -1, sizeof(dec->get_frame_queue));
	memset(dec->taskPics, -1, sizeof(dec->taskPics));
}

int ADD_VARIANT(parse_end_of_sequence)(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg) {
	int ret = EBADMSG;
	if (rbsp_end(&dec->gb, 0)) {
		// end_of_seq empties the DPB (Annex C.4.5.3): every picture of the finished
		// sequence must be output now. bump_all_frames queues them, but get_frame
		// still holds back a queued base whose dependent view is not in the DPB - its
		// unpaired-base valve only emits such a base once dec->flushing is set or the
		// output queue is full. On a live stream the next access unit fills the queue
		// and resolves it, but at an end_of_seq there is no next access unit, so on a
		// real 3D Blu-ray whose trailing picture had no paired dependent the tail base
		// stayed held here and a multi-clip caller that ends a clip on this NAL
		// (rather than the buf>=end drain) spun ENOBUFS on it. Set flushing so the
		// unpaired-base valve fires and the tail is emitted; the next NAL clears the
		// flag again (decode_nal), so a following sequence is unaffected.
		// Only the frames the caller has yet to receive hold it back, not those
		// it received and holds: they keep their slots across clear_decoder.
		dec->flushing = 1;
		bump_all_frames(dec);
		if (dec->to_get_frames)
			return ENOBUFS;
		clear_decoder(dec);
		ret = 0;
	}
	return print_dec(dec, "  decode_NAL_result: %s\n", ret);
}

#ifdef LOGS
	int ignore_NAL_log(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg) {
		return print_dec(dec, "  decode_NAL_result: %s\n", 0);
	}
	int unsup_NAL_log(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg) {
		return print_dec(dec, "  decode_NAL_result: %s\n", ENOTSUP);
	}
	int corrupt_NAL_log(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg) {
		return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
	}
#endif



/**
 * This function sets the context pointers to the frame about to be decoded,
 * and fills the context caches with useful values.
 */
static void initialize_context(Edge264MvcContext *ctx, int currPic)
{
	static const int8_t QP_Y2C[88] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39,
		39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39};
	
	union { int8_t q[MAX_FRAMES]; i8x16 v[MAX_FRAMES / 16]; } tb, td;
	ctx->CurrMbAddr = ctx->t.first_mb_in_slice;
	ctx->mby = (unsigned)ctx->t.first_mb_in_slice / (unsigned)ctx->t.pic_width_in_mbs;
	ctx->mbx = (unsigned)ctx->t.first_mb_in_slice % (unsigned)ctx->t.pic_width_in_mbs;
	ctx->samples_mb[0] = ctx->t.samples_buffers[currPic] + (ctx->mbx + ctx->mby * ctx->t.stride[0]) * 16;
	ctx->samples_mb[1] = ctx->t.samples_buffers[currPic] + (ctx->mbx + ctx->mby * ctx->t.stride[1]) * 8 + ctx->t.plane_size_Y;
	ctx->samples_mb[2] = ctx->samples_mb[1] + (ctx->t.stride[1] >> 1);
	int mb_offset = ctx->mbx + ctx->mby * (ctx->t.pic_width_in_mbs + 1);
	ctx->mbCol = ctx->_mb = ctx->t.mb_buffer + mb_offset;
	ctx->mbc_ring_size = ctx->t.pic_width_in_mbs + 2;
	ctx->_mbc = ctx->mbc_ring + mb_offset % ctx->mbc_ring_size;
	ctx->A4x4_int8_v = (i16x16){0, 0, 2, 2, 1, 4, 3, 6, 8, 8, 10, 10, 9, 12, 11, 14};
	ctx->B4x4_int8_v = (i32x16){0, 1, 0, 1, 4, 5, 4, 5, 2, 3, 8, 9, 6, 7, 12, 13};
	if (ctx->t.ChromaArrayType == 1) {
		ctx->ACbCr_int8_v[0] = (i16x8){0, 0, 2, 2, 4, 4, 6, 6};
		ctx->BCbCr_int8_v[0] = (i32x8){0, 1, 0, 1, 4, 5, 4, 5};
	}
	
	ctx->QP_C_v[0] = loadu128(QP_Y2C + 12 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[1] = loadu128(QP_Y2C + 28 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[2] = loadu128(QP_Y2C + 44 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[3] = loadu128(QP_Y2C + 60 + ctx->t.pps.chroma_qp_index_offset);
	ctx->QP_C_v[4] = loadu128(QP_Y2C + 12 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->QP_C_v[5] = loadu128(QP_Y2C + 28 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->QP_C_v[6] = loadu128(QP_Y2C + 44 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->QP_C_v[7] = loadu128(QP_Y2C + 60 + ctx->t.pps.second_chroma_qp_index_offset);
	ctx->t.QP[1] = ctx->QP_C[0][ctx->t.QP[0]];
	ctx->t.QP[2] = ctx->QP_C[1][ctx->t.QP[0]];
	for (int i = 1; i < 4; i++) {
		ctx->sig_inc_v[i] = sig_inc_8x8[0][i];
		ctx->last_inc_v[i] = last_inc_8x8[i];
		ctx->scan_v[i] = scan_8x8_cabac[0][i];
	}
	// CAVLC reads only scan[0..15], except on a damaged AC block of 15
	// coefficients that places one past its scan (coeff_token and total_zeros
	// allow 16 coefficients and 16 - TotalCoeff zeros, 9.2.1 and 9.2.3). As with
	// FFmpeg's 17-entry scan, it then lands on the block's own DC position, which
	// add_idct4x4 replaces with the DC, instead of on a coefficient of a later 8x8
	// block of the slice.
	if (!ctx->t.pps.entropy_coding_mode_flag)
		ctx->scan[16] = 0;
	for (int i = 0; i < 16; i++)
		ctx->c_v[i] = (i8x16){};
	
	// P/B slices
	if (ctx->t.slice_type < 2) {
		ctx->refIdx4x4_C_v = (i8x16){2, 3, 12, -1, 3, 6, 13, -1, 12, 13, 14, -1, 13, -1, 15, -1};
		ctx->absMvd_A_v = (i16x16){0, 0, 4, 4, 2, 8, 6, 12, 16, 16, 20, 20, 18, 24, 22, 28};
		ctx->absMvd_B_v = (i32x16){0, 2, 0, 2, 8, 10, 8, 10, 4, 6, 16, 18, 12, 14, 24, 26};
		ctx->mvs_A_v = (i16x16){0, 0, 2, 2, 1, 4, 3, 6, 8, 8, 10, 10, 9, 12, 11, 14};
		ctx->mvs_B_v = (i32x16){0, 1, 0, 1, 4, 5, 4, 5, 2, 3, 8, 9, 6, 7, 12, 13};
		ctx->mvs_C_v = (i32x16){0, 1, 1, -1, 4, 5, 5, -1, 3, 6, 9, -1, 7, -1, 13, -1};
		ctx->mvs_D_v = (i32x16){0, 1, 2, 0, 4, 5, 1, 4, 8, 2, 10, 8, 3, 6, 9, 12};
		ctx->num_ref_idx_mask = (ctx->t.pps.num_ref_idx_active[0] > 1) * 0x0f + (ctx->t.pps.num_ref_idx_active[1] > 1) * 0xf0;
		ctx->transform_8x8_mode_flag = ctx->t.pps.transform_8x8_mode_flag; // for P slices this value is constant
		int max0 = ctx->t.pps.num_ref_idx_active[0] - 1;
		int max1 = ctx->t.slice_type == 0 ? -1 : ctx->t.pps.num_ref_idx_active[1] - 1;
		ctx->clip_ref_idx_v = (i8x8){max0, max0, max0, max0, max1, max1, max1, max1};
		
		// B slides
		if (ctx->t.slice_type == 1) {
			ctx->mbCol = ctx->t.mbCol_buffer + mb_offset;
			
			// initializations for temporal prediction and implicit weights
			int rangeL1 = ctx->t.pps.num_ref_idx_active[1];
			if (ctx->t.pps.weighted_bipred_idc == 2 || (rangeL1 = 1, !ctx->t.direct_spatial_mv_pred_flag)) {
				// tb and td are clipped to 8 bits only now (8.4.1.2.3), from distances
				// subtracted modulo 2^32, so that two large ones cannot wrap their difference
				const i32x4 *d = ctx->t.diff_poc_v;
				for (int i = 0; i < MAX_FRAMES / 16; i++)
					tb.v[i] = packs16(packs32(d[i * 4], d[i * 4 + 1]), packs32(d[i * 4 + 2], d[i * 4 + 3]));
				memset(ctx->MapPicToList0, 0, sizeof(ctx->MapPicToList0)); // FIXME pictures not found in RefPicList0 should point to self
				for (int refIdxL0 = ctx->t.pps.num_ref_idx_active[0], DistScaleFactor = 0; refIdxL0-- > 0; ) {
					int pic0 = ctx->t.RefPicList[0][refIdxL0];
					ctx->MapPicToList0[pic0] = refIdxL0;
					u32x4 diff0 = set32(ctx->t.diff_poc[pic0]);
					for (int i = 0; i < MAX_FRAMES / 16; i++)
						td.v[i] = packs16(packs32(diff0 - d[i * 4], diff0 - d[i * 4 + 1]), packs32(diff0 - d[i * 4 + 2], diff0 - d[i * 4 + 3]));
					for (int refIdxL1 = rangeL1, implicit_weight; refIdxL1-- > 0; ) {
						int pic1 = ctx->t.RefPicList[1][refIdxL1];
						if (td.q[pic1] != 0 && !(ctx->t.prev_long_term_frames & (FrameMask)1 << pic0)) {
							int tx = (16384 + abs(td.q[pic1] / 2)) / td.q[pic1];
							DistScaleFactor = min(max((tb.q[pic0] * tx + 32) >> 6, -1024), 1023);
							implicit_weight = (!(ctx->t.prev_long_term_frames & (FrameMask)1 << pic1) && DistScaleFactor >= -256 && DistScaleFactor <= 515) ? DistScaleFactor >> 2 : 32;
						} else {
							DistScaleFactor = 256;
							implicit_weight = 32;
						}
						ctx->implicit_weights[refIdxL0][refIdxL1] = implicit_weight + 64;
					}
					ctx->DistScaleFactor[refIdxL0] = DistScaleFactor;
				}
			}
		}
	}
}



/**
 * Helper function to raise a probability sampled to 0..65535 to a power k.
 */
static unsigned ppow(unsigned p65536, unsigned k) {
	unsigned r = 65536;
	while (k) {
		if (k & 1)
			r = (r * p65536) >> 16;
		p65536 = (p65536 * p65536) >> 16;
		k >>= 1;
	}
	return r;
}



/**
 * If the slice ends on error, invalidate all its mbs and recover them.
 * 
 * For CAVLC the error is equiprobable in all of the slice mbs.
 * For CABAC every erroneous mb had a random probability p=2/383 to exit
 * early at end_of_slice_flag, so for each mb we only count a proportion
 * that reached CurrMbAddr without early exit: (1-p)^d, d being the
 * distance to the last decoded mb. We sum these proportions to normalize
 * the probabilities: (1-(1-p)^n)/p. Then we compute each probability as
 * the normalized sum of its proportion and all proportions before it:
 * 1-(1-(1-p)^d)/(1-(1-p)^n). Note that p is sampled to 16-bits int to
 * avoid dependency on float and to fit all multiplications on 32 bits
 * with max precision.
 * 
 * FIXME remove ldleft macros eventually
*/
static void recover_slice(Edge264MvcContext *ctx, int currPic, int keep_mb) {
	__atomic_fetch_or(&ctx->d->frame_flags[currPic], EDGE264MVC_VIEW_CONCEALED, __ATOMIC_RELAXED);
	// mark all previous mbs as erroneous and assign them an error probability
	ctx->mby = (unsigned)ctx->t.first_mb_in_slice / (unsigned)ctx->t.pic_width_in_mbs;
	ctx->mbx = (unsigned)ctx->t.first_mb_in_slice % (unsigned)ctx->t.pic_width_in_mbs;
	ctx->samples_mb[0] = ctx->t.samples_buffers[currPic] + (ctx->mbx + ctx->mby * ctx->t.stride[0]) * 16;
	ctx->samples_mb[1] = ctx->t.samples_buffers[currPic] + (ctx->mbx + ctx->mby * ctx->t.stride[1]) * 8 + ctx->t.plane_size_Y;
	ctx->samples_mb[2] = ctx->samples_mb[1] + (ctx->t.stride[1] >> 1);
	int mb_offset = ctx->mbx + ctx->mby * (ctx->t.pic_width_in_mbs + 1);
	ctx->_mb = ctx->t.mb_buffer + mb_offset;
	ctx->mbCol = ctx->t.mbCol_buffer ? ctx->t.mbCol_buffer + mb_offset : ctx->_mb; // no colocated picture outside B slices
	unsigned num = ctx->CurrMbAddr - ctx->t.first_mb_in_slice;
	unsigned div = 65536 - ppow(65194, num);
	for (unsigned i = 0; i < num; i++) {
		unsigned p12800 = (!ctx->t.pps.entropy_coding_mode_flag) ?
			((i + 1) * 12800 + num - 1) / num : // division with upward rounding
			((div - (65536 - ppow(65194, num - 1 - i))) * 12800 + div - 1) / div;
		if (ctx->t.first_mb_in_slice + i < (unsigned)keep_mb)
			goto next_mb; // published and kept as decoded
		ctx->_mb->error_probability = p12800 >> 7;
		unsigned p128 = p12800 / 100;
		
		// recover the macroblock depending on slice_type
		// FIXME use Intra function instead
		// reason: an I macroblock without error weight keeps its samples, and the
		// blend could not express it on SSE, whose signed 8-bit weights stop at 127
		if (ctx->t.slice_type == 2 && p128 > 0) { // I slice -> blend with intra DC
			size_t stride_Y = ctx->t.stride[0];
			DECL_SSTRIDE(stride_Y);
			uint8_t * restrict y0 = ctx->samples_mb[0];
			uint8_t * restrict y7 = y0 + stride_Y * 7;
			uint8_t * restrict yE = y7 + stride_Y * 7;
			i8x16 l = set8(-128), t = l;
			if (i == 0 || ctx->mbx == 0) { // A not available
				if (i >= ctx->t.pic_width_in_mbs) // B available
					l = t = loada128(SADDR(y0, -1));
			} else { // A available
				l = t = ldleftC(y0, stride_Y, 0); // from intra.c
				if (i >= ctx->t.pic_width_in_mbs) // B available
					t = loada128(SADDR(y0, -1));
			}
			i8x16 dcY = broadcast8(shrru16(sum8(t) + sum8(l), 5), 0);
			#if SIMD == SSE
				i8x16 w0 = (p128 < 128) ? ziplo8(set8(128 - p128), set8(p128)) : (i8x16){0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
				i8x16 w1 = w0;
				i64x2 wd = {p128 < 128 ? 7 : 0};
			#elif SIMD == NEON
				i16x8 w0 = (p128 < 128) ? (i16x8){128 - p128, p128} : (i16x8){0, 1};
				i16x8 w1 = w0;
				i16x8 wd = set16(p128 < 128 ? -7 : 0); // vshlq_s16 shifts right by a negative count
			#elif SIMD == WASM
				i16x8 w0 = set16(p128 < 128 ? 128 - p128 : 0);
				i16x8 w1 = set16(p128 < 128 ? p128 : 1);
				int wd = p128 < 128 ? 7 : 0;
			#elif SIMD == CLANG
				i16x8 w0 = set16(p128 < 128 ? 128 - p128 : 0);
				i16x8 w1 = set16(p128 < 128 ? p128 : 1);
				i16x8 wd = set16(p128 < 128 ? 7 : 0);
			#endif
			i16x8 o = {};
			*(i8x16 *)SADDR(y0,  0) = maddshrL(*(i8x16 *)SADDR(y0,  0), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y0,  1) = maddshrL(*(i8x16 *)SADDR(y0,  1), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y0,  2) = maddshrL(*(i8x16 *)SADDR(y0,  2), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y7, -4) = maddshrL(*(i8x16 *)SADDR(y7, -4), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y0,  4) = maddshrL(*(i8x16 *)SADDR(y0,  4), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y7, -2) = maddshrL(*(i8x16 *)SADDR(y7, -2), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y7, -1) = maddshrL(*(i8x16 *)SADDR(y7, -1), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y7,  0) = maddshrL(*(i8x16 *)SADDR(y7,  0), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y7,  1) = maddshrL(*(i8x16 *)SADDR(y7,  1), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y7,  2) = maddshrL(*(i8x16 *)SADDR(y7,  2), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(yE, -4) = maddshrL(*(i8x16 *)SADDR(yE, -4), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(y7,  4) = maddshrL(*(i8x16 *)SADDR(y7,  4), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(yE, -2) = maddshrL(*(i8x16 *)SADDR(yE, -2), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(yE, -1) = maddshrL(*(i8x16 *)SADDR(yE, -1), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(yE,  0) = maddshrL(*(i8x16 *)SADDR(yE,  0), dcY, w0, w1, o, wd);
			*(i8x16 *)SADDR(yE,  1) = maddshrL(*(i8x16 *)SADDR(yE,  1), dcY, w0, w1, o, wd);
			
			size_t stride_C = ctx->t.stride[1] >> 1;
			DECL_DSTRIDE(stride_C);
			uint8_t * restrict c0 = ctx->samples_mb[1];
			uint8_t * restrict c7 = c0 + stride_C * 7;
			uint8_t * restrict cE = c7 + stride_C * 7;
			// chroma DC from the available chroma neighbours like the luma one above,
			// never from samples outside the slice or above the picture
			i8x16 lc = set8(-128), tc = lc;
			if (i > 0 && ctx->mbx > 0) // A available
				lc = tc = ldleftC(c0, stride_C, 0); // from intra.c, left Cb then left Cr
			if (i >= ctx->t.pic_width_in_mbs) { // B available
				tc = loada64x2(DADDR(c0, -2), DADDR(c0, -1)); // top Cb then top Cr
				if (i == 0 || ctx->mbx == 0)
					lc = tc;
			}
			i8x16 b = ziplo64(tc, lc);
			i8x16 r = ziphi64(tc, lc);
			i8x16 dcb = broadcast8(shrru16(sum8(b), 4), 0);
			i8x16 dcr = broadcast8(shrru16(sum8(r), 4), 0);
			i8x16 dcC = ziplo64(dcb, dcr);
			i64x2 v0 = maddshrL(loada64x2(DADDR(c0,  0), DADDR(c0,  1)), dcC, w0, w1, o, wd);
			i64x2 v1 = maddshrL(loada64x2(DADDR(c0,  2), DADDR(c7, -4)), dcC, w0, w1, o, wd);
			i64x2 v2 = maddshrL(loada64x2(DADDR(c0,  4), DADDR(c7, -2)), dcC, w0, w1, o, wd);
			i64x2 v3 = maddshrL(loada64x2(DADDR(c7, -1), DADDR(c7,  0)), dcC, w0, w1, o, wd);
			i64x2 v4 = maddshrL(loada64x2(DADDR(c7,  1), DADDR(c7,  2)), dcC, w0, w1, o, wd);
			i64x2 v5 = maddshrL(loada64x2(DADDR(cE, -4), DADDR(c7,  4)), dcC, w0, w1, o, wd);
			i64x2 v6 = maddshrL(loada64x2(DADDR(cE, -2), DADDR(cE, -1)), dcC, w0, w1, o, wd);
			i64x2 v7 = maddshrL(loada64x2(DADDR(cE,  0), DADDR(cE,  1)), dcC, w0, w1, o, wd);
			*(int64_t *)DADDR(c0,  0) = v0[0];
			*(int64_t *)DADDR(c0,  1) = v0[1];
			*(int64_t *)DADDR(c0,  2) = v1[0];
			*(int64_t *)DADDR(c7, -4) = v1[1];
			*(int64_t *)DADDR(c0,  4) = v2[0];
			*(int64_t *)DADDR(c7, -2) = v2[1];
			*(int64_t *)DADDR(c7, -1) = v3[0];
			*(int64_t *)DADDR(c7,  0) = v3[1];
			*(int64_t *)DADDR(c7,  1) = v4[0];
			*(int64_t *)DADDR(c7,  2) = v4[1];
			*(int64_t *)DADDR(cE, -4) = v5[0];
			*(int64_t *)DADDR(c7,  4) = v5[1];
			*(int64_t *)DADDR(cE, -2) = v6[0];
			*(int64_t *)DADDR(cE, -1) = v6[1];
			*(int64_t *)DADDR(cE,  0) = v7[0];
			*(int64_t *)DADDR(cE,  1) = v7[1];
		} else if (i > 0 && p128 >= 32) { // recover above 25% error (arbitrary)
			if (ctx->t.slice_type == 0) { // P slice -> P_Skip
				mb->nC_Y_v = (i8x16){};
				// reason: decode_P_skip relies on the refIdx the P macroblock parser
				// sets before it, without which explicit weighted prediction would
				// index its weights with the -1 of an intra or unparsed macroblock
				mb->refIdx_l = (int64_t)(i8x8){0, 0, 0, 0, -1, -1, -1, -1};
				decode_P_skip(ctx);
			} else { // B slice -> B_Skip
				mb->nC_Y_v = (i8x16){};
				await_frame_progress(ctx, ctx->t.RefPicList[1][0], ctx->mby * ctx->t.pic_width_in_mbs + ctx->mbx + 1);
				decode_direct_mv_pred(ctx, 0xffffffff);
			}
		}
		__atomic_store_n(&ctx->_mb->recovery_bits, ctx->t.frame_flip_bit + 2, __ATOMIC_RELEASE);
		
		// point to the next macroblock
	next_mb:
		ctx->_mb++;
		ctx->mbx++;
		ctx->mbCol++;
		ctx->samples_mb[0] += 16;
		ctx->samples_mb[1] += 8;
		ctx->samples_mb[2] += 8;
		if (ctx->mbx >= ctx->t.pic_width_in_mbs) {
			ctx->_mb++;
			ctx->mbx = 0;
			ctx->mby++;
			ctx->mbCol++;
			ctx->samples_mb[0] += ctx->t.stride[0] * 16 - ctx->t.pic_width_in_mbs * 16;
			ctx->samples_mb[1] += ctx->t.stride[1] * 8 - ctx->t.pic_width_in_mbs * 8;
			ctx->samples_mb[2] += ctx->t.stride[1] * 8 - ctx->t.pic_width_in_mbs * 8;
		}
	}
}



/**
 * This function is called when a frame ends with a positive remaining_mbs.
 * 
 * It sets up recover_slice to go through all mbs and recover them while
 * setting their error probability to 100%.
 */
/*static void recover_frame(Edge264MvcDecoder *dec) {
	
}*/



/**
 * Deblock the macroblocks [from, to) of frame currPic in raster order.
 */
static void deblock_range(Edge264MvcContext *c, int currPic, int from, int to) {
	if ((unsigned)from >= (unsigned)to)
		return;
	c->mby = (unsigned)from / (unsigned)c->t.pic_width_in_mbs;
	c->mbx = (unsigned)from % (unsigned)c->t.pic_width_in_mbs;
	c->samples_mb[0] = c->t.samples_buffers[currPic] + (c->mbx + c->mby * c->t.stride[0]) * 16;
	c->samples_mb[1] = c->t.samples_buffers[currPic] + (c->mbx + c->mby * c->t.stride[1]) * 8 + c->t.plane_size_Y;
	c->samples_mb[2] = c->samples_mb[1] + (c->t.stride[1] >> 1);
	c->_mb = (Edge264MvcMacroblock *)c->t.mb_buffer + c->mbx + c->mby * (c->t.pic_width_in_mbs + 1);
	for (int addr = from; addr < to; addr++) {
		deblock_mb(c);
		c->_mb++;
		c->mbx++;
		c->samples_mb[0] += 16;
		c->samples_mb[1] += 8;
		c->samples_mb[2] += 8;
		if (c->mbx >= c->t.pic_width_in_mbs) {
			c->_mb++;
			c->mbx = 0;
			c->samples_mb[0] += c->t.stride[0] * 16 - c->t.pic_width_in_mbs * 16;
			c->samples_mb[1] += c->t.stride[1] * 8 - c->t.pic_width_in_mbs * 8;
			c->samples_mb[2] += c->t.stride[1] * 8 - c->t.pic_width_in_mbs * 8;
		}
	}
}

/**
 * Decide how a slice deblocks and publishes its macroblocks once decoded,
 * after the preceding slices of its frame (so that each macroblock is
 * deblocked with the parameters of its own slice):
 * _ SLICE_TURN if the deblocking frontier reached it, so it does it now;
 * _ SLICE_DEFERRED if a preceding slice is still being decoded, in which case
 *   it is recorded in deblock_pending and the thread publishing the preceding
 *   slice will do it (process_pending_slices), freeing this worker meanwhile;
 * _ SLICE_ABANDONED if no preceding slice is being decoded, i.e. one is
 *   missing or damaged (or arrives later with arbitrary slice order), which
 *   stops the frontier before this slice whatever the thread timing.
 * A damaged slice waits for its turn instead of being deferred, since it
 * recovers its unpublished macroblocks only after deblocking the others.
 */
enum { SLICE_ABANDONED, SLICE_TURN, SLICE_DEFERRED };
static int slice_turn(Edge264MvcContext *c, int currPic, uint32_t seq, int keep_mb, int ret) {
	Edge264MvcDecoder *dec = c->d;
	int32_t first = c->t.first_mb_in_slice;
	int32_t cur = __atomic_load_n(&dec->next_deblock_addr[currPic], __ATOMIC_ACQUIRE);
	if (cur < first && c->thread_id >= 0) {
		pthread_mutex_lock(&dec->lock);
		for (;;) {
			cur = __atomic_load_n(&dec->next_deblock_addr[currPic], __ATOMIC_ACQUIRE);
			if (cur >= first)
				break;
			// Only the slices decoded before this one can precede it, as when decoding
			// single-threaded: one that arrives later with a lower first_mb_in_slice
			// (arbitrary slice order, or a damaged stream) must not be waited for,
			// which made the outcome depend on the timing and could hold every
			// worker in this wait while the slices they waited for found none.
			int preceding = 0, i;
			TM_FOREACH(i, dec->busy_tasks) {
				preceding |= dec->taskPics[i] == currPic && dec->tasks[i].first_mb_in_slice < first &&
					(int32_t)(dec->task_seq[i] - seq) < 0;
			}
			if (!preceding)
				break;
			if (ret == 0 && ~dec->deblock_pending_slices) {
				int i = __builtin_ctzll(~dec->deblock_pending_slices);
				dec->deblock_pending[i] = (Edge264MvcPendingSlice){
					.pic = currPic,
					.deblock = c->t.disable_deblocking_filter_idc == 0,
					.first_mb = first,
					.keep_mb = keep_mb,
				};
				dec->deblock_pending_slices |= (uint64_t)1 << i;
				pthread_mutex_unlock(&dec->lock);
				return SLICE_DEFERRED;
			}
			wait_frame_locked(dec, currPic, first);
		}
		pthread_mutex_unlock(&dec->lock);
	}
	return cur >= first && cur <= c->CurrMbAddr ? SLICE_TURN : SLICE_ABANDONED;
}

/**
 * After publishing a slice up to frontier, deblock and publish in order the
 * following slices of the frame that finished decoding meanwhile.
 */
static void process_pending_slices(Edge264MvcContext *c, int currPic, int32_t frontier) {
	Edge264MvcDecoder *dec = c->d;
	if (c->thread_id < 0)
		return;
	for (;;) {
		pthread_mutex_lock(&dec->lock);
		int slot = -1;
		for (uint64_t b = dec->deblock_pending_slices; b; b &= b - 1) {
			int i = mask_ctz(b);
			if (dec->deblock_pending[i].pic == currPic && dec->deblock_pending[i].first_mb == frontier)
				slot = i;
		}
		if (slot < 0) {
			pthread_mutex_unlock(&dec->lock);
			return;
		}
		Edge264MvcPendingSlice s = dec->deblock_pending[slot];
		dec->deblock_pending_slices &= ~((uint64_t)1 << slot);
		pthread_mutex_unlock(&dec->lock);
		if (s.deblock)
			deblock_range(c, currPic, s.first_mb, s.keep_mb);
		publish_frame_progress(dec, currPic, s.keep_mb);
		frontier = s.keep_mb;
	}
}



/**
 * Saves (or restores) the samples of the slices before this one that its
 * deblocking may change: the bottom rows of the macroblocks above its first
 * macroblock row (top edges), and the macroblocks of that row before its first
 * macroblock (left edge of the first one, top edges of those below). Only
 * older, finished slices write there, while a younger one may already decode
 * the rest of the row. Returns 0 if the buffer could not be allocated.
 */
static int spec_rows(Edge264MvcContext *c, int slot, int restore) {
	Edge264MvcDecoder *dec = c->d;
	int width = c->t.pic_width_in_mbs;
	int y = c->t.first_mb_in_slice / width, x = c->t.first_mb_in_slice % width;
	struct { size_t offset, stride; int lines, from, to; } parts[6] = {
		{0, c->t.stride[0], y > 0 ? 4 : 0, x * 16, width * 16}, // luma above
		{0, c->t.stride[0], 16, 0, x * 16}, // luma before
		{c->t.plane_size_Y, c->t.stride[1], y > 0 ? 2 : 0, x * 8, width * 8}, // Cb above
		{c->t.plane_size_Y, c->t.stride[1], 8, 0, x * 8}, // Cb before
		{c->t.plane_size_Y + (c->t.stride[1] >> 1), c->t.stride[1], y > 0 ? 2 : 0, x * 8, width * 8}, // Cr above
		{c->t.plane_size_Y + (c->t.stride[1] >> 1), c->t.stride[1], 8, 0, x * 8}, // Cr before
	};
	size_t size = 0;
	for (int i = 0; i < 6; i++)
		size += (size_t)parts[i].lines * (parts[i].to - parts[i].from);
	if (size == 0) // a slice starting the picture has no slice before it
		return 1;
	if (dec->spec_rows_sizes[slot] < size) {
		free(dec->spec_rows_allocs[slot]);
		dec->spec_rows_allocs[slot] = malloc(size);
		dec->spec_rows_sizes[slot] = dec->spec_rows_allocs[slot] ? size : 0;
		if (!dec->spec_rows_allocs[slot])
			return 0;
	}
	uint8_t *buf = dec->spec_rows_allocs[slot];
	for (int i = 0; i < 6; i++) {
		int first_line = (i & 1) ? y * (i < 2 ? 16 : 8) : y * (i < 2 ? 16 : 8) - parts[i].lines;
		size_t n = parts[i].to - parts[i].from;
		for (int l = 0; l < parts[i].lines; l++, buf += n) {
			uint8_t *p = c->t.samples_buffers[c->currPic] + parts[i].offset + (size_t)(first_line + l) * parts[i].stride + parts[i].from;
			if (restore)
				memcpy(p, buf, n);
			else
				memcpy(buf, p, n);
		}
	}
	return 1;
}



/**
 * This function is the entry point for worker threads, where they consume
 * tasks continuously until stopped by the parent process.
 */
void *ADD_VARIANT(worker_loop)(void *arg) {
	Edge264MvcContext c, r; // r replays the reconstruction that c records
	c.d = (void *)((uintptr_t)arg & -MAX_THREADS);
	c.thread_id = c.d->n_threads ? (uintptr_t)arg & (MAX_THREADS - 1) : -1;
	c.log_base_us = c.d->log_base_us;
	c.log_cb = c.d->log_cb;
	c.log_arg = c.d->log_arg;
	c.log_indent = c.d->n_threads ? "  " : "    ";
	c.log_pos = 0;
	if (c.thread_id >= 0)
		pthread_mutex_lock(&c.d->lock);
	int chain = -1; // slice of the same picture to decode next, see the end of the loop
	while (1) {
		// Wait until the oldest pending task is ready and reserve it. Taking tasks
		// strictly in decoding order guarantees that every frame a running task
		// waits on (in wait_frame_progress) has its writers already running, and
		// that the oldest running task only depends on complete frames, so it
		// never waits and at least one worker always progresses. A chained slice
		// is left out, since the worker decoding the slices before it continues
		// with it, thus is a writer already running.
		int task_id = chain;
		chain = -1;
		if (task_id < 0) {
			for (;;) {
				TaskMask pool = tm_andnot(c.d->pending_tasks, c.d->chained_tasks);
				if (c.thread_id < 0 || c.d->shutdown || (tm_any(pool) && tm_has(c.d->ready_tasks, task_id = oldest_task(c.d, pool))))
					break;
				pthread_cond_wait(&c.d->task_ready, &c.d->lock);
			}
		}
		if (c.thread_id >= 0 && c.d->shutdown && task_id < 0) { // free_decoder requested a clean exit
			pthread_mutex_unlock(&c.d->lock);
			return NULL;
		}
		if (c.thread_id < 0)
			task_id = oldest_task(c.d, c.d->ready_tasks);
		assert(tm_has(c.d->ready_tasks, task_id));
		int currPic = c.d->taskPics[task_id];
		tm_clear(&c.d->pending_tasks, task_id);
		tm_clear(&c.d->ready_tasks, task_id);
		tm_clear(&c.d->chained_tasks, task_id);
		// Ready tasks wake one worker at a time rather than all of them, which
		// with many threads and short pictures kept them contending for the lock:
		// each worker taking a task wakes the next one if another is ready.
		TaskMask pool = tm_andnot(c.d->pending_tasks, c.d->chained_tasks);
		if (c.thread_id >= 0 && tm_any(pool) && tm_has(c.d->ready_tasks, oldest_task(c.d, pool)))
			pthread_cond_signal(&c.d->task_ready);
		int32_t mb_bound = __atomic_load_n(&c.d->task_bounds[task_id], __ATOMIC_ACQUIRE);
		if (mb_bound != BOUND_UNKNOWN)
			acked_set(c.d, task_id);
		// an older slice of the picture that may still decode past its bound may
		// roll its deblocking frontier back, so this one cannot deblock in its turn
		int after_spec = 0, i;
		TM_FOREACH(i, tm_andnot(c.d->busy_tasks, acked_load(c.d))) {
			after_spec |= c.d->taskPics[i] == currPic && (int32_t)(c.d->task_seq[i] - c.d->task_seq[task_id]) < 0;
		}
		// A multithreaded P slice heavy enough in entropy decoding records its
		// reconstruction, replayed as its references allow, so that parsing never
		// waits for them (see Edge264MvcRecord). Take a buffer for it.
		uint8_t *rec_buf = NULL;
		const Edge264MvcTask *next = &c.d->tasks[task_id];
		if (c.thread_id >= 0 && next->slice_type < REC_SLICE_TYPES && !c.log_cb && c.d->rec_active < c.d->rec_max &&
			next->gb.end - next->gb.CPB >= (ptrdiff_t)REC_MIN_BYTES_PER_MB * next->pic_width_in_mbs * next->pic_height_in_mbs) {
			rec_buf = c.d->rec_pool_size > 0 ? c.d->rec_pool[--c.d->rec_pool_size] : malloc(REC_BUF_SIZE);
			c.d->rec_active += rec_buf != NULL;
		}
		if (c.thread_id >= 0)
			pthread_mutex_unlock(&c.d->lock);
		unsigned long long clock_start = get_relative_time_us() - c.log_base_us;
		c.t = c.d->tasks[task_id];
		c.t.mb_bound = mb_bound;
		c.task_id = task_id;
		c.currPic = currPic;
		c.overrun = 0;
		unsigned approx_byte_size = c.t.gb.end - c.t.gb.CPB;
		#ifdef LOGS
			// single-threaded, a slice is decoded once the next NAL has bounded it, so
			// its macroblocks open their own entry rather than follow its header
			if (c.thread_id < 0 && c.log_cb) {
				Edge264MvcContext *ctx = &c;
				log_mb(ctx, "\n- thread_id: -1\n"
					"  FrameId: %u\n"
					"  first_mb_in_slice: %u\n"
					"  macroblocks_%s:\n",
					c.t.FrameId, c.t.first_mb_in_slice, c.t.pps.entropy_coding_mode_flag ? "cabac" : "cavlc");
			}
		#endif
		// The slice deblocks its macroblocks while decoding them if all the
		// macroblocks before it are already deblocked (or if it is deblocked
		// independently), and then publishes the deblocking frontier per row for
		// the tasks reading this frame. Otherwise it deblocks them at the end.
		int32_t cur_deblock_addr = __atomic_load_n(&c.d->next_deblock_addr[currPic], __ATOMIC_ACQUIRE);
		
		// (re)allocate the ring of neighbouring values for this thread, with
		// room for the copies at both ends and for the alignment of entries
		int slot = c.thread_id + 1;
		size_t ret = 0;
		if (c.d->mbc_ring_sizes[slot] < c.t.pic_width_in_mbs + 2) {
			free(c.d->mbc_ring_allocs[slot]);
			c.d->mbc_ring_allocs[slot] = malloc((c.t.pic_width_in_mbs + 4) * sizeof(Edge264MvcMbCache) + 63);
			c.d->mbc_ring_sizes[slot] = c.d->mbc_ring_allocs[slot] ? c.t.pic_width_in_mbs + 2 : 0;
		}
		if (c.d->mbc_ring_allocs[slot])
			c.mbc_ring = (Edge264MvcMbCache *)(((uintptr_t)c.d->mbc_ring_allocs[slot] + 63) & -64) + 1;
	decode_slice:;
		int in_turn = cur_deblock_addr == c.t.first_mb_in_slice && !after_spec;
		c.t.next_deblock_idc = in_turn ? currPic : -1;
		c.t.next_deblock_addr = (in_turn || c.t.disable_deblocking_filter_idc == 2) ? c.t.first_mb_in_slice : INT_MIN;
		// A slice started before its bound is known decodes, deblocks and publishes
		// as usual: should it go past the bound, the slice following it is the next
		// NAL, so no task of a later picture reads this one yet, and its rows can be
		// taken back. The rows of the slices before it that its deblocking changes
		// are kept aside, so that decoding it again deblocks the same samples (and
		// if they cannot be, it deblocks at the end like a slice out of its turn).
		int saved_rows = 0;
		if (c.t.mb_bound == BOUND_UNKNOWN && in_turn && c.t.disable_deblocking_filter_idc == 0 &&
			!(saved_rows = spec_rows(&c, slot, 0))) {
			in_turn = 0;
			c.t.next_deblock_idc = -1;
			c.t.next_deblock_addr = INT_MIN;
		}
		initialize_context(&c, currPic);
		
		// record with the replay context starting at the same state
		c.rec_tail = NULL;
		c.rc = c.pc = NULL;
		if (rec_buf) {
			r = c;
			r.pc = &c;
			r.rec_pending = 0;
			c.rc = &r;
			c.rec_buf = c.rec_head = c.rec_tail = rec_buf;
			c.rec_end = c.rec_buf + REC_BUF_SIZE - REC_MB_MAX;
		}

		// call the function containing the macroblock decoding loop
		ret = 0;
		if (!c.d->mbc_ring_allocs[slot]) {
			ret = ENOMEM;
		} else if (!c.t.pps.entropy_coding_mode_flag) {
			c.mb_skip_run = -1;
			parse_slice_data_cavlc(&c);
			if (!rbsp_end(&c.t.gb, 1))
				ret = EBADMSG;
		} else {
			// cabac_alignment_one_bit gives a good probability to catch random errors.
			if (cabac_start(&c)) {
				ret = EBADMSG; // FIXME error_flag
			} else {
				cabac_init(&c);
				c.mb_qp_delta_nz = 0;
				parse_slice_data_cabac(&c);
				// rbsp_stop_one_bit was consumed in cabac_terminate, and the possibility of cabac_zero_word implies we cannot require reaching end.
				// The cached reader also looks ahead past the slice's last byte, so a
				// slice whose CABAC data fills its NAL leaves msb_cache holding bytes
				// from beyond the NAL end (e.g. the next start code) - benign once every
				// macroblock of the picture has been decoded. Only treat a non-clean
				// trailing state as an error when the slice stopped before completing the
				// frame (a genuinely truncated/corrupt slice), so a complete final slice
				// with tight packing is not dropped (which would otherwise leave
				// remaining_mbs > 0 and deadlock the DPB mid-stream).
				if ((c.t.gb.msb_cache || (c.t.gb.lsb_cache & (c.t.gb.lsb_cache - 1))) &&
				    c.CurrMbAddr < c.t.pic_width_in_mbs * c.t.pic_height_in_mbs)
					ret = EBADMSG; // FIXME error_flag
			}
		}

		// finish the recorded reconstruction, waiting for the references now
		if (c.rec_tail) {
			replay_records(&c, 1);
			c.t.next_deblock_addr = r.t.next_deblock_addr;
			c.rec_tail = NULL;
		}

		// A slice that decoded all its data before the next NAL bounded it waits for
		// that. If it went past its bound, it undoes its claims, acks, and is
		// decoded again with the bound (nothing it did is visible yet).
		if (c.t.mb_bound == BOUND_UNKNOWN && c.d->mbc_ring_allocs[slot]) {
			pthread_mutex_lock(&c.d->lock);
			while ((mb_bound = __atomic_load_n(&c.d->task_bounds[task_id], __ATOMIC_ACQUIRE)) == BOUND_UNKNOWN) {
				__atomic_store_n(&c.d->progress_wake_addr[currPic], INT_MIN, __ATOMIC_SEQ_CST);
				pthread_cond_wait(&c.d->frame_progress[currPic], &c.d->lock);
			}
			pthread_mutex_unlock(&c.d->lock);
			known_mb_bound(&c, mb_bound, 0);
		}
		if (__builtin_expect(c.overrun, 0)) {
			if (in_turn)
				publish_frame_progress(c.d, currPic, c.t.first_mb_in_slice);
			if (saved_rows)
				spec_rows(&c, slot, 1);
			int width = c.t.pic_width_in_mbs;
			for (int addr = c.t.first_mb_in_slice; addr < c.CurrMbAddr; addr++)
				__atomic_store_n(&c.t.mb_buffer[addr % width + addr / width * (width + 1)].recovery_bits, c.t.frame_flip_bit ^ 1, __ATOMIC_RELAXED);
			ack_mb_bound(c.d, task_id, currPic);
			mb_bound = c.t.mb_bound;
			c.t = c.d->tasks[task_id];
			c.t.mb_bound = mb_bound;
			c.overrun = 0;
			goto decode_slice;
		}
		if (c.t.unref_cb)
			c.t.unref_cb((int)ret, c.t.unref_arg);
		
		// Only the macroblocks a slice publishes are final, and a damaged frame
		// is concealed from its deblocking frontier onwards (conceal_frame), so
		// what other tasks read never changes afterwards. On error, keep the
		// macroblocks that may already be published, i.e. those deblocked while
		// decoding (one row behind) or decoded without deblocking (whole rows).
		int end_mb = c.CurrMbAddr;
		int width = c.t.pic_width_in_mbs;
		int keep_mb = ret == 0 ? end_mb : max((int)c.t.first_mb_in_slice,
			c.t.disable_deblocking_filter_idc == 1 ? end_mb - end_mb % width : end_mb - width);
		
		// deblock and publish the slice after the preceding ones of its frame,
		// so that each macroblock is deblocked with the parameters of its own
		// slice rather than those of the slice completing the frame
		int turn = slice_turn(&c, currPic, c.d->task_seq[task_id], keep_mb, ret);
		if (turn == SLICE_TURN && c.t.next_deblock_addr < 0 && c.t.disable_deblocking_filter_idc == 0)
			c.t.next_deblock_addr = c.t.first_mb_in_slice;
		if (c.t.next_deblock_addr >= 0 && (turn != SLICE_DEFERRED || c.t.disable_deblocking_filter_idc == 2))
			deblock_range(&c, currPic, max(c.t.next_deblock_addr, (int)c.t.first_mb_in_slice), keep_mb);
		
		// on error, recover the other mbs and signal them as erroneous
		if (__builtin_expect(ret != 0, 0))
			recover_slice(&c, currPic, keep_mb);
		
		// update c.d->next_deblock_addr (atomic: read concurrently by other threads)
		if (turn == SLICE_TURN) {
			publish_frame_progress(c.d, currPic, keep_mb);
			process_pending_slices(&c, currPic, keep_mb);
		}

		// deblock the rest of the frame if all mbs have been decoded correctly
		// (only left when slices arrived out of order)
		int remaining_mbs = ret ?: __atomic_sub_fetch(&c.d->remaining_mbs[currPic], end_mb - c.t.first_mb_in_slice, __ATOMIC_ACQ_REL);
		if (remaining_mbs == 0) {
			int total_mbs = c.t.pic_width_in_mbs * c.t.pic_height_in_mbs;
			deblock_range(&c, currPic, __atomic_load_n(&c.d->next_deblock_addr[currPic], __ATOMIC_ACQUIRE), total_mbs);
			publish_frame_progress(c.d, currPic, INT_MAX); // signals the frame is complete
		}
		
		// print benchmarking information
		if (c.log_cb) {
			unsigned long long clock_end = get_relative_time_us() - c.log_base_us;
			snprintf(c.log_buf, sizeof(c.log_buf),
				"\n- thread_id: %d\n"
				"  FrameId: %u\n"
				"  first_mb_in_slice: %u\n"
				"  approx_byte_size: %u\n"
				"  decoding_start_us: %llu\n"
				"  decoding_end_us: %llu\n"
				"  slice_result: %s\n",
				c.thread_id, c.t.FrameId, c.t.first_mb_in_slice, approx_byte_size, clock_start, clock_end, ret_to_str(ret));
			c.log_cb(c.log_buf, c.log_arg);
		}
		
		// if multi-threaded, check if we are the last task to touch this frame and ensure it is complete
		if (c.thread_id >= 0) {
			pthread_mutex_lock(&c.d->lock);
			if (rec_buf) {
				c.d->rec_pool[c.d->rec_pool_size++] = rec_buf;
				c.d->rec_active--;
			}
			pthread_cond_signal(&c.d->task_complete);
			// Wake the slices waiting for this one in wait_slice_turn, which also
			// wait on its leaving when it stops before them. A frame this task
			// leaves incomplete may now have no writer left, so wake all waiters
			// then, for them to conceal it if they need it.
			// With other slices of the frame still to decode it keeps a writer, so
			// only its own waiters are woken (waking every frame's at each slice
			// sent them all contending for the lock on streams with many slices).
			wake_frame_waiters(c.d, currPic);
			if (remaining_mbs != 0) {
				FrameMask writers = 0;
				int j;
				TM_FOREACH(j, tm_andnot(c.d->busy_tasks, tm_bit(task_id)))
					writers |= (FrameMask)1 << c.d->taskPics[j];
				if (!(writers >> currPic & 1)) {
					for (int i = 0; i < c.d->frame_slots; i++)
						wake_frame_waiters(c.d, i);
				}
			}
			if (remaining_mbs == 0) {
				c.d->ready_tasks = ready_tasks(c.d);
				if (tm_any(c.d->ready_tasks))
					pthread_cond_signal(&c.d->task_ready); // passed on by the worker taking a task
			}
		}
		tm_clear(&c.d->busy_tasks, task_id);
		c.d->task_dependencies[task_id] = 0;
		c.d->taskPics[task_id] = -1;
		// let the tasks start that waited for this one to share its macroblocks,
		// before its slot (and bit) can be reused by another task
		TaskMask freed = tm_none();
		int waiting = 0;
		TM_FOREACH(i, c.d->busy_tasks) { // idle tasks get theirs anew when created
			if (tm_has(c.d->task_after[i], task_id)) {
				waiting = 1;
				tm_clear(&c.d->task_after[i], task_id);
				if (!tm_any(c.d->task_after[i]))
					tm_set(&freed, i);
			}
		}
		if (c.thread_id >= 0 && waiting && tm_any(c.d->ready_tasks = ready_tasks(c.d))) {
			// Continue with the next slice of this picture rather than leave it to
			// the pool, so that a picture's slices decode one after another, each
			// deblocking its rows as it decodes them: decoded in parallel, the slices
			// after the first deblock only once it is done, so a picture predicting
			// from this one waits for most of it. A chained slice the last slice
			// before it freed but that is not ready goes back to the pool.
			TaskMask next = tm_and(freed, c.d->chained_tasks);
			if (tm_any(tm_and(next, c.d->ready_tasks)))
				chain = oldest_task(c.d, tm_and(next, c.d->ready_tasks));
			c.d->chained_tasks = tm_andnot(c.d->chained_tasks, tm_andnot(next, c.d->ready_tasks));
			TaskMask pool = tm_andnot(c.d->pending_tasks, c.d->chained_tasks);
			if (chain >= 0)
				tm_clear(&pool, chain);
			if (tm_any(pool) && tm_has(c.d->ready_tasks, oldest_task(c.d, pool)))
				pthread_cond_signal(&c.d->task_ready);
		} else if (c.thread_id >= 0) {
			c.d->chained_tasks = tm_andnot(c.d->chained_tasks, freed); // not ready yet, back to the pool
		}
		if (c.thread_id >= 0)
			release_terminal_task_dependencies(c.d);
		if (c.thread_id < 0)
			return (void *)ret;
	}
	return NULL;
}



/**
 * Returns FrameNum, the frame_num of the current picture counted on past its
 * wraparound, from the frame_num of the previous reference picture of the
 * same view. It never starts again, not even at an IDR picture (frame_num 0,
 * so the next multiple of MaxFrameNum): the picture order counts of types 1
 * and 2 derived from it keep growing across IDR pictures, as those of type 0
 * do, which the output order of the pictures before an IDR picture relies on.
 */
static int derive_FrameNum(const Edge264MvcDecoder *dec, int frame_num, int FrameNumMask, int non_base_view) {
	int PrevRefFrameNum = dec->PrevRefFrameNum[non_base_view];
	return PrevRefFrameNum + 1 + ((frame_num - PrevRefFrameNum - 1) & FrameNumMask);
}



/**
 * Updates the reference flags by adaptive memory control or sliding window
 * marking process (8.2.5).
 */
static void parse_dec_ref_pic_marking(Edge264MvcDecoder *dec, Edge264MvcSeqParameterSet *sps)
{
	dec->currPic_marked = 1;
	// no_output_of_prior_pics_flag is easier to support than to signal unsupported
	if (dec->IdrPicFlag) {
		int no_output_of_prior_pics_flag = get_u1(&dec->gb);
		int long_term_flag = get_u1(&dec->gb);
		dec->short_term_frames = (FrameMask)(long_term_flag ^ 1) << dec->currPic;
		dec->long_term_frames = (FrameMask)long_term_flag << dec->currPic;
		memset(dec->LongTermFrameIdx, 0, sizeof(dec->LongTermFrameIdx));
		log_dec(dec, "  no_output_of_prior_pics_flag: %d\n"
			"  long_term_reference_flag: %d\n",
			no_output_of_prior_pics_flag,
			long_term_flag);
		while (bump_frame(dec, dec->nal_unit_type == 20, (FrameMask)1 << dec->currPic));
		return;
	}
	
	// 8.2.5.4 - Adaptive memory control marking process.
	int long_term_frame = 0;
	if (get_u1(&dec->gb)) {
		log_dec(dec, "  memory_management_control_operations:\n");
		int memory_management_control_operation;
		int i = 32;
		while ((memory_management_control_operation = get_ue16(&dec->gb, 6)) != 0 && i-- > 0) {
			int target = dec->currPic, FrameNum = 0, long_term_frame_idx = 0;
			if (10 & 1 << memory_management_control_operation) { // 1 or 3
				// target and dereference a given short-term or non-existing frame
				FrameNum = dec->FrameNum - 1 - get_ue32(&dec->gb, 4294967294);
				for (FrameMask r = dec->short_term_frames; r; r &= r - 1) {
					int j = mask_ctz(r);
					if (dec->FrameNums[j] == FrameNum) {
						target = j;
						dec->short_term_frames ^= (FrameMask)1 << j;
						dec->long_term_frames &= ~((FrameMask)1 << j);
					}
				}
			}
			if (92 & 1 << memory_management_control_operation) { // 2 or 3 or 4 or 6
				long_term_frame_idx = get_ue16(&dec->gb, sps->max_num_ref_frames - (memory_management_control_operation != 4));
				int up = (memory_management_control_operation == 4) ? INT_MAX : long_term_frame_idx;
				// dereference one or many long-term frames
				for (FrameMask r = dec->long_term_frames & ~dec->short_term_frames; r; r &= r - 1) {
					int j = mask_ctz(r);
					if (dec->LongTermFrameIdx[j] >= long_term_frame_idx && dec->LongTermFrameIdx[j] <= up)
						dec->long_term_frames ^= (FrameMask)1 << j;
				}
				if (72 & 1 << memory_management_control_operation) { // 3 or 6
					dec->LongTermFrameIdx[target] = long_term_frame_idx;
					if (memory_management_control_operation == 6)
						long_term_frame = 1;
					else if (target != dec->currPic)
						dec->long_term_frames |= (FrameMask)1 << target;
				}
			}
			if (memory_management_control_operation == 5) { // dereference all frames
				dec->short_term_frames = dec->long_term_frames = 0;
				dec->FrameNums[dec->currPic] = 0;
				// Reset only the current view's long-term indices. The short/long-term
				// bitmaps above are view-masked working copies (merged per-view in
				// unset_currPic), but LongTermFrameIdx is seeded and written back
				// wholesale, so zeroing all slots would clobber the co-decoded other
				// view's live long-term indices while its long-term flags survive the
				// masked merge. 8.2.5 marks reference pictures per view component.
				FrameMask same_views = (dec->non_base_frames >> dec->currPic & 1) ? dec->non_base_frames : ~dec->non_base_frames;
				for (FrameMask r = same_views; r; r &= r - 1)
					dec->LongTermFrameIdx[mask_ctz(r)] = 0;
				int tempPicOrderCnt = minw(dec->TopFieldOrderCnt, dec->BottomFieldOrderCnt);
				dec->FieldOrderCnt[0][dec->currPic] = (int)((unsigned)dec->TopFieldOrderCnt - tempPicOrderCnt);
				dec->FieldOrderCnt[1][dec->currPic] = (int)((unsigned)dec->BottomFieldOrderCnt - tempPicOrderCnt);
				while (bump_frame(dec, dec->nal_unit_type == 20, (FrameMask)1 << dec->currPic));
			}
			// one format per operation, since a format may not skip an argument
			// with %2$ (glibc's fortified printf aborts on it)
			switch (memory_management_control_operation) {
			case 1: log_dec(dec, "  - {mmco: 1, sref: %u} # dereference\n", FrameNum); break;
			case 2: log_dec(dec, "  - {mmco: 2, lref: %u} # dereference\n", long_term_frame_idx); break;
			case 3: log_dec(dec, "  - {mmco: 3, sref: %u, lref: %u} # convert\n", FrameNum, long_term_frame_idx); break;
			case 4: log_dec(dec, "  - {mmco: 4, lref: %d} # dereference on and above\n", long_term_frame_idx); break;
			case 5: log_dec(dec, "  - {mmco: 5} # dereference all\n"); break;
			case 6: log_dec(dec, "  - {mmco: 6, lref: %u} # convert current\n", long_term_frame_idx); break;
			}
		}
	}
	
	// 8.2.5.3 - Sliding window marking process. Guard on a short-term ref
	// existing: 8.2.5.3 presumes numShortTerm > 0 at capacity, but a
	// non-conformant stream that fills every slot with long-term refs (MMCO 6/3)
	// leaves short_term_frames == 0, so the loop below would not run, `next`
	// would stay 0, and the toggle would spuriously mark slot 0 short-term (and
	// demote it out of long_term_frames). Inert whenever a short-term ref exists.
	if (dec->short_term_frames && mask_popcount(dec->short_term_frames | dec->long_term_frames) >= sps->max_num_ref_frames) {
		int best = INT_MAX;
		int next = 0;
		// iterate on short-term and non-existing frames
		for (FrameMask r = dec->short_term_frames; r != 0; r &= r - 1) {
			int i = mask_ctz(r);
			if (best > dec->FrameNums[i])
				best = dec->FrameNums[next = i];
		}
		dec->short_term_frames ^= (FrameMask)1 << next;
		dec->long_term_frames &= ~((FrameMask)1 << next);
	}
	*(long_term_frame ? &dec->long_term_frames : &dec->short_term_frames) |= (FrameMask)1 << dec->currPic;
	
	// A non-conformant stream may now hold more references than
	// max_num_ref_frames (long-term ones the sliding window does not retire, or
	// MMCOs that add some), so discard one as FFmpeg does: the oldest short-term
	// or non-existing frame other than the current picture, the current picture
	// if it is the only short-term one, or the long-term reference with the
	// lowest LongTermFrameIdx if there is no short-term one. JM rejects the
	// stream instead.
	if (mask_popcount(dec->short_term_frames | dec->long_term_frames) > sps->max_num_ref_frames) {
		FrameMask candidates = dec->short_term_frames & ~((FrameMask)1 << dec->currPic);
		int unref = dec->currPic, lowest = INT_MAX;
		if (!dec->short_term_frames) {
			for (FrameMask r = dec->long_term_frames; r; r &= r - 1) {
				int i = mask_ctz(r);
				if (dec->LongTermFrameIdx[i] < lowest)
					lowest = dec->LongTermFrameIdx[unref = i];
			}
		}
		for (FrameMask r = candidates; r; r &= r - 1) {
			int i = mask_ctz(r);
			if (dec->FrameNums[i] < lowest)
				lowest = dec->FrameNums[unref = i];
		}
		dec->short_term_frames &= ~((FrameMask)1 << unref);
		dec->long_term_frames &= ~((FrameMask)1 << unref);
	}
}



/**
 * Parses coefficients for weighted sample prediction (7.4.3.2 and 8.4.2.3).
 */
static void parse_pred_weight_table(Edge264MvcDecoder *dec, Edge264MvcSeqParameterSet *sps, Edge264MvcTask *t)
{
	// further tests will depend only on weighted_bipred_idc
	if (t->slice_type == 0)
		t->pps.weighted_bipred_idc = t->pps.weighted_pred_flag;
	
	// parse explicit weights/offsets
	if (t->pps.weighted_bipred_idc == 1) {
		t->luma_log2_weight_denom = get_ue16(&dec->gb, 7);
		if (sps->ChromaArrayType != 0)
			t->chroma_log2_weight_denom = get_ue16(&dec->gb, 7);
		for (int l = 0; l <= t->slice_type; l++) {
			log_dec(dec, "  explicit_weights_l%u:\n", l);
			for (int i = l * 32; i < l * 32 + t->pps.num_ref_idx_active[l]; i++) {
				if (get_u1(&dec->gb)) {
					t->explicit_weights[0][i] = get_se16(&dec->gb, -128, 127);
					t->explicit_offsets[0][i] = get_se16(&dec->gb, -128, 127);
				} else {
					t->explicit_weights[0][i] = 1 << t->luma_log2_weight_denom;
					t->explicit_offsets[0][i] = 0;
				}
				if (sps->ChromaArrayType != 0 && get_u1(&dec->gb)) {
					t->explicit_weights[1][i] = get_se16(&dec->gb, -128, 127);
					t->explicit_offsets[1][i] = get_se16(&dec->gb, -128, 127);
					t->explicit_weights[2][i] = get_se16(&dec->gb, -128, 127);
					t->explicit_offsets[2][i] = get_se16(&dec->gb, -128, 127);
				} else {
					t->explicit_weights[1][i] = 1 << t->chroma_log2_weight_denom;
					t->explicit_offsets[1][i] = 0;
					t->explicit_weights[2][i] = 1 << t->chroma_log2_weight_denom;
					t->explicit_offsets[2][i] = 0;
				}
				log_dec(dec, sps->ChromaArrayType ? "  - {Y: \"*%d>>%u+%d\", Cb: \"*%d>>%u+%d\", Cr: \"*%d>>%u+%d\"}\n" : "  - {Y: \"*%d>>%u+%d\"}\n",
					t->explicit_weights[0][i], t->luma_log2_weight_denom, t->explicit_offsets[0][i],
					t->explicit_weights[1][i], t->chroma_log2_weight_denom, t->explicit_offsets[1][i],
					t->explicit_weights[2][i], t->chroma_log2_weight_denom, t->explicit_offsets[2][i]);
			}
		}
	}
}



/**
 * Initialises and updates the reference picture lists (8.2.4).
 *
 * Both initialisation and parsing of ref_pic_list_modification are fit into a
 * single function to foster compactness and maintenance. Performance is not
 * crucial here.
 */
static int parse_ref_pic_list_modification(Edge264MvcDecoder *dec, Edge264MvcSeqParameterSet *sps, Edge264MvcTask *t)
{
	// initial sort on FrameNum for P, on PicOrderCnt for B
	int count[3] = {0, 0, 0}; // number of refs before/after/long
	int size = 0;
	if (!dec->IdrPicFlag) {
		const int32_t *values = (t->slice_type == 0) ? dec->FrameNums : dec->FieldOrderCnt[0];
		int pic_value = (t->slice_type == 0) ? dec->FrameNum : dec->TopFieldOrderCnt;
		FrameMask refs = (t->slice_type != 0 && sps->pic_order_cnt_type == 0) ?
			dec->short_term_frames ^ dec->long_term_frames :
			dec->short_term_frames | dec->long_term_frames;
		// sort key = class (0 before, 1 after, 2 long-term) above the distance, in
		// 64 bits since a damaged stream can put references up to 2^32 away
		for (unsigned next = 0; refs; refs ^= (FrameMask)1 << next) {
			int64_t best = INT64_MAX;
			for (FrameMask r = refs; r; r &= r - 1) {
				int i = mask_ctz(r);
				int64_t diff = (int64_t)values[i] - pic_value;
				int64_t ShortTermNum = (diff <= 0) ? -diff : (1ll << 32) + diff;
				int64_t LongTermNum = dec->prev_LongTermFrameIdx[i] + (2ll << 32);
				int64_t v = (dec->short_term_frames & (FrameMask)1 << i) ? ShortTermNum : LongTermNum;
				if (v < best)
					best = v, next = i;
			}
			t->RefPicList[0][size++] = next;
			count[best >> 32]++;
		}
	}
	// fill RefPicListL1 by swapping before/after references
	for (int src = 0; src < size; src++) {
		int dst = (src < count[0]) ? src + count[1] :
			(src < count[0] + count[1]) ? src - count[0] : src;
		t->RefPicList[1][dst] = t->RefPicList[0][src];
	}
	
	// When decoding a field, extract a list of fields from each list of frames.
	/*union { int8_t q[32]; i8x16 v[2]; } RefFrameList;
	for (int l = 0; t->field_pic_flag && l <= t->slice_type; l++) {
		i8x16 v = t->RefPicList_v[l * 2];
		RefFrameList.v[0] = v;
		RefFrameList.v[1] = v + (i8x16){16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
		size = 0;
		int i = t->bottom_field_flag << 4; // first parity to check
		int j = i ^ 16; // other parity to alternate
		int lim_i = i + count[0] + count[1]; // set a first limit to short term frames
		int lim_j = j + count[0] + count[1]; // don't init with XOR as there can be 16 refs!
		int tot = count[0] + count[1] + count[2]; // ... then long term
		
		// probably not the most readable portion, yet otherwise needs a lot of code
		for (int k;;) {
			if (i >= lim_i) {
				if (j < lim_j) { // i reached limit but not j, swap them
					k = i, i = j, j = k;
					k = lim_i, lim_i = lim_j, lim_j = k;
				} else if (min(lim_i, lim_j) < tot) { // end of short term refs, go for long
					int parity = t->bottom_field_flag << 4;
					i = (t->bottom_field_flag << 4) + count[0] + count[1];
					j = i ^ 16;
					lim_i = i + count[2];
					lim_j = j + count[2];
				} else break; // end of long term refs, break
			}
			int pic = RefFrameList.q[i++];
			if (dec->prev_short_term_frames & (FrameMask)1 << pic) {
				t->RefPicList[l][size++] = pic;
				if (j < lim_j) { // swap parity if we have not emptied other parity yet
					k = i, i = j, j = k;
					k = lim_i, lim_i = lim_j, lim_j = k;
				}
			}
		}
	}*/
	
	// Swap the two first slots of RefPicListL1 if it the same as RefPicListL0.
	if (t->RefPicList[0][1] >= 0 && t->RefPicList[0][0] == t->RefPicList[1][0]) {
		t->RefPicList[1][0] = t->RefPicList[0][1];
		t->RefPicList[1][1] = t->RefPicList[0][0];
	}

	// Append the inter-view reference for the dependent view AFTER the temporal
	// 8.2.4.2.3 "RefPicList1 identical to RefPicList0 -> switch first two" step:
	// per H.8.2.1 the inter-view reference components follow the temporal reference
	// list initialisation, so that switch must operate on the temporal-only list.
	// Appending basePic before it - on a single-temporal-reference dependent B slice
	// where RefPicList0 == RefPicList1 = [temporal] - would make the list length 2,
	// fire the switch, and wrongly promote basePic to RefPicList1[0].
	if (dec->nal_unit_type == 20) { // second view: add inter-view ref for MVC
		t->RefPicList[0][size] = dec->basePic;
		t->RefPicList[1][size] = dec->basePic;
		size++;
	}

	// parse the ref_pic_list_modification() header
	for (int l = 0; l <= t->slice_type; l++) {
		unsigned picNumLX = (t->field_pic_flag) ? dec->FrameNum * 2 + 1 : dec->FrameNum;
		if (get_u1(&dec->gb)) { // ref_pic_list_modification_flag
			log_dec(dec, "  ref_pic_list_modifications_l%u: [", l);
			for (int refIdx = 0, modification_of_pic_nums_idc; (modification_of_pic_nums_idc = get_ue16(&dec->gb, 5)) != 3 && refIdx < 32; refIdx++) {
				// unsigned, as a damaged slice may code values up to 2^32 - 2, far
				// beyond MaxPicNum - 1 (7.4.3.1), on which picNumLX wraps anyway
				unsigned num = get_ue32(&dec->gb, 4294967294);
				log_dec(dec, "[\"%s\",%+lld],",
					modification_of_pic_nums_idc < 2 ? "sref" : modification_of_pic_nums_idc == 2 ? "lref" : "view",
					modification_of_pic_nums_idc % 4 == 0 ? -(long long)num - 1 : (long long)num + (modification_of_pic_nums_idc != 2));
				int pic = dec->basePic; // for modification_of_pic_nums_idc == 4 and 5
				if (modification_of_pic_nums_idc < 2) {
					picNumLX = (modification_of_pic_nums_idc == 0) ? picNumLX - (num + 1) : picNumLX + (num + 1);
					unsigned MaskFrameNum = (1 << sps->log2_max_frame_num) - 1;
					// iterate on short-term and non-existing frames, leaving -1 when
					// the picture is missing (a damaged stream), which the fix-up
					// below replaces - rather than the last frame iterated, which
					// depended on the DPB slot allocation and thus on threading
					pic = -1;
					for (FrameMask r = dec->short_term_frames; r; r &= r - 1) {
						if (!((dec->FrameNums[mask_ctz(r)] ^ picNumLX) & MaskFrameNum)) {
							pic = mask_ctz(r);
							break;
						}
					}
				} else if (modification_of_pic_nums_idc == 2) {
					// iterate on long-term frames only
					pic = -1;
					for (FrameMask r = dec->long_term_frames & ~dec->short_term_frames; r; r &= r - 1) {
						if (dec->prev_LongTermFrameIdx[mask_ctz(r)] == num) {
							pic = mask_ctz(r);
							break;
						}
					}
				}
				int buf = pic;
				int cIdx = refIdx;
				do {
					int swap = t->RefPicList[l][cIdx];
					t->RefPicList[l][cIdx] = buf;
					buf = swap;
				} while (++cIdx < t->pps.num_ref_idx_active[l] && buf != pic);
			}
			log_dec(dec, "]\n");
		}
	}

	// A non-conformant stream can leave RefPicList entries that index no valid
	// picture - surplus slots when num_ref_idx_active exceeds the available
	// references, or the basePic == -1 sentinel a list modification falls back to
	// when it resolves to no existing picture. Either would later index the
	// picture-keyed 0..31 buffers (the implicit-weight init in initialize_context,
	// MapPicToList0, ...) out of bounds. Replace every out-of-range referenced
	// entry with an in-range slot. Inert for conformant streams (all referenced
	// entries are valid); matches ffmpeg, which does not crash on these.
	// The current picture is never a valid replacement (nor a valid entry): a
	// picture predicting from itself reads its own undecoded samples single-
	// threaded, and waits forever on its own progress multithreaded. Without
	// any other picture to refer to (e.g. a P slice in an IDR picture), reject
	// the slice as corrupt.
	for (int l = 0; l <= t->slice_type; l++) {
		int valid = -1;
		for (int i = 0; i < t->pps.num_ref_idx_active[l] && valid < 0; i++) {
			if ((unsigned)t->RefPicList[l][i] < MAX_FRAMES && t->RefPicList[l][i] != dec->currPic)
				valid = t->RefPicList[l][i];
		}
		if (valid < 0)
			return EBADMSG;
		for (int i = 0; i < t->pps.num_ref_idx_active[l]; i++) {
			if ((unsigned)t->RefPicList[l][i] >= MAX_FRAMES || t->RefPicList[l][i] == dec->currPic)
				t->RefPicList[l][i] = valid;
		}
	}

	#ifdef LOGS
		for (int lx = 0; lx <= t->slice_type; lx++) {
			log_dec(dec, lx == 0 ? "  RefPicLists: [[" : "], [");
			for (int i = 0; i < t->pps.num_ref_idx_active[lx]; i++) {
				int pic = t->RefPicList[lx][i];
				log_dec(dec, i == 0 ? "%d" : ",%d", dec->FrameIds[pic]);
			}
		}
		log_dec(dec, "]]\n");
	#endif
	return 0;
}



/**
 * This fonction copies the last set of fields to finish initializing the task.
 */
static void initialize_task(Edge264MvcDecoder *dec, Edge264MvcSeqParameterSet *sps, Edge264MvcTask *t)
{
	// copy most essential fields from dec
	memcpy(&t->gb, &dec->gb, sizeof(dec->gb)); // GCC-14 crashes on dec->out = format
	t->ChromaArrayType = sps->ChromaArrayType;
	t->direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
	t->pic_width_in_mbs = sps->pic_width_in_mbs;
	t->pic_height_in_mbs = sps->pic_height_in_mbs;
	if (t->pps.pic_scaling_matrix_present_flag) {
		// An absent PPS scaling list is left as zero by parse_scaling_lists; it
		// falls back per Table 7-2 to rule set B (inherit the SPS lists) when the
		// SPS carries a scaling matrix, else to rule set A (the Default matrices
		// of tables 7-3/7-4). Falling back to the SPS's Flat_4x4_16 in the rule-A
		// case silently mis-dequantises any stream that declares a PPS scaling
		// matrix over a scaling-matrix-less SPS.
		i8x16 fb0, fb3;
		const i8x16 *fb8;
		i8x16 def8[8];
		if (sps->seq_scaling_matrix_present_flag) {
			fb0 = sps->weightScale4x4_v[0];
			fb3 = sps->weightScale4x4_v[3];
			fb8 = sps->weightScale8x8_v; // indexed by i & 7 -> SPS lists 6 (intra) and 7 (inter)
		} else {
			fb0 = Default_4x4_Intra;
			fb3 = Default_4x4_Inter;
			for (int i = 0; i < 4; i++) {
				def8[i] = Default_8x8_Intra[i];
				def8[4 + i] = Default_8x8_Inter[i];
			}
			fb8 = def8;
		}
		t->pps.weightScale4x4_v[0] = ifelse_mask(t->pps.weightScale4x4_v[0] == 0, fb0, t->pps.weightScale4x4_v[0]);
		t->pps.weightScale4x4_v[1] = ifelse_mask(t->pps.weightScale4x4_v[1] == 0, fb0, t->pps.weightScale4x4_v[1]);
		t->pps.weightScale4x4_v[2] = ifelse_mask(t->pps.weightScale4x4_v[2] == 0, fb0, t->pps.weightScale4x4_v[2]);
		t->pps.weightScale4x4_v[3] = ifelse_mask(t->pps.weightScale4x4_v[3] == 0, fb3, t->pps.weightScale4x4_v[3]);
		t->pps.weightScale4x4_v[4] = ifelse_mask(t->pps.weightScale4x4_v[4] == 0, fb3, t->pps.weightScale4x4_v[4]);
		t->pps.weightScale4x4_v[5] = ifelse_mask(t->pps.weightScale4x4_v[5] == 0, fb3, t->pps.weightScale4x4_v[5]);
		for (unsigned i = 0; i < 24; i++)
			t->pps.weightScale8x8_v[i] = ifelse_mask(t->pps.weightScale8x8_v[i] == 0, fb8[i & 7], t->pps.weightScale8x8_v[i]);
	} else {
		memcpy(t->pps.weightScale4x4_v, sps->weightScale4x4_v, 96);
		memcpy(t->pps.weightScale8x8_v, sps->weightScale8x8_v, 384);
	}
	t->frame_flip_bit = dec->frame_flip_bits >> dec->currPic & 1;
	t->stride[0] = dec->out.stride_Y;
	t->stride[1] = t->stride[2] = dec->out.stride_C;
	t->FrameId = dec->FrameIds[dec->currPic];
	t->plane_size_Y = dec->plane_size_Y;
	t->plane_size_C = dec->plane_size_C;
	t->prev_long_term_frames = dec->prev_long_term_frames & ~dec->prev_short_term_frames; // mask of only long-term frames
	t->mb_buffer = (Edge264MvcMacroblock *)dec->mb_buffers[dec->currPic];
	memcpy(t->samples_buffers, dec->samples_buffers, sizeof(t->samples_buffers));
	t->samples_clip_v[0] = set16((1 << sps->BitDepth_Y) - 1);
	t->samples_clip_v[1] = t->samples_clip_v[2] = set16((1 << sps->BitDepth_C) - 1);
	if (t->slice_type == 1) { // B slices
		t->mbCol_buffer = (Edge264MvcMacroblock *)dec->mb_buffers[t->RefPicList[1][0]];
		// colZeroFlag (8.4.1.2.2) needs RefPicList1[0] to be a short-term reference,
		// and an inter-view reference counts as neither short- nor long-term (H.8.4)
		int col = t->RefPicList[1][0];
		t->col_short_term = !(t->prev_long_term_frames >> col & 1) && !(dec->nal_unit_type == 20 && col == dec->basePic);
		if (t->pps.weighted_bipred_idc == 2 || !t->direct_spatial_mv_pred_flag) {
			// distances from the current picture, unclipped (see initialize_context)
			u32x4 poc = set32(minw(dec->TopFieldOrderCnt, dec->BottomFieldOrderCnt));
			for (int i = 0; i < MAX_FRAMES / 4; i++)
				t->diff_poc_v[i] = poc - minw32(dec->FieldOrderCnt_v[0][i], dec->FieldOrderCnt_v[1][i]);
		}
	}
}



// Replace the unpublished part of an abandoned damaged picture with
// deterministic neutral samples and macroblock metadata, then publish it as
// complete. Macroblocks before the deblocking frontier are final (other tasks
// may already have read them) so they are kept, and since the frontier only
// stops on a missing or damaged slice regardless of thread timing (see
// wait_slice_turn) the result is the same multithreaded or not. The caller
// holds lock and only selects slots with no in-flight task, so no worker can
// race these writes. The release store publishes the concealed buffers to
// dependent tasks. Returns 0 if the picture must be concealed later, once the
// base view it is concealed from is complete.
static int conceal_frame(Edge264MvcDecoder *dec, int id) {
	assert(dec->samples_buffers[id] && dec->mb_buffers[id]);
	__atomic_fetch_or(&dec->frame_flags[id], EDGE264MVC_VIEW_CONCEALED, __ATOMIC_RELAXED);
	// A damaged MVC dependent view is best concealed by the base view of its
	// access unit: the two eyes differ only by disparity, so the viewer sees one
	// flat frame instead of a green flash, and the pictures predicted from it
	// inherit a plausible reference rather than neutral samples. Identify that
	// base on (FrameNum, POC), the same key the pairing in bump_frame uses - a
	// POC-only match collides across short IDR sequences, where pictures of
	// different sequences share a full POC while carrying different frame_num
	// (tests/gen_same_poc_stream.py), and would fill the damaged eye from another
	// access unit: a stale picture presented as the other eye, worse than the
	// neutral samples it replaces. Only a base decoded before the picture can
	// be its own: a later picture with the same key (a damaged stream) may not
	// be decoded yet, and waiting for it deadlocked the workers that wait for
	// this picture, since tasks start in decoding order. Take the base once
	// complete, so no worker still writes it: leave the picture for a later
	// call while the base is being decoded (waiting here could stall the very
	// tasks completing it), and conceal the base first if it is damaged too.
	// Fall back to neutral samples without a base, so the result never depends
	// on thread timing.
	int base = -1;
	if (dec->non_base_frames >> id & 1) {
		FrameMask live = (dec->short_term_frames | dec->long_term_frames | dec->to_get_frames | dec->output_frames) &
			~dec->non_base_frames;
		for (FrameMask b = live; b; b &= b - 1) {
			int i = mask_ctz(b);
			if (dec->samples_buffers[i] && dec->FrameNums[i] == dec->FrameNums[id] &&
				dec->FieldOrderCnt[0][i] == dec->FieldOrderCnt[0][id] &&
				dec->FrameIds[i] < dec->FrameIds[id] &&
				(base < 0 || dec->FrameIds[i] > dec->FrameIds[base]))
				base = i;
		}
		if (base >= 0 && __atomic_load_n(&dec->next_deblock_addr[base], __ATOMIC_ACQUIRE) != INT_MAX) {
			if (writing_frames(dec) >> base & 1)
				return 0;
			if (base == dec->currPic)
				base = -1;
			else
				conceal_frame(dec, base);
		}
	}
	int width = dec->sps.pic_width_in_mbs;
	int height = dec->sps.pic_height_in_mbs;
	int total_mbs = width * height;
	int mbs = (width + 1) * height - 1;
	int from = min(max(__atomic_load_n(&dec->next_deblock_addr[id], __ATOMIC_ACQUIRE), 0), total_mbs);
	size_t stride_Y = dec->out.stride_Y;
	size_t stride_C = dec->out.stride_C;
	uint8_t *dst = dec->samples_buffers[id];
	const uint8_t *src = base >= 0 ? dec->samples_buffers[base] : NULL;
	int8_t recovery_bits = ((dec->frame_flip_bits >> id) & 1) + 2;
	Edge264MvcMacroblock *m = dec->mb_buffers[id];
	for (int addr = from; addr < total_mbs; addr++) {
		int x = addr % width;
		int y = addr / width;
		// keep the macroblocks a slice decoded or recovered in this picture (their
		// recovery bit 0 matches the picture's), which do not depend on timing
		if ((m[y * (width + 1) + x].recovery_bits & 1) == (recovery_bits & 1))
			continue;
		size_t offY = (size_t)y * 16 * stride_Y + x * 16;
		size_t offC = dec->plane_size_Y + (size_t)y * 8 * stride_C + x * 8;
		for (int r = 0; r < 16; r++, offY += stride_Y) {
			if (src)
				memcpy(dst + offY, src + offY, 16);
			else
				memset(dst + offY, 0, 16);
		}
		for (int r = 0; r < 16; r++, offC += stride_C >> 1) { // Cb and Cr rows alternate
			if (src)
				memcpy(dst + offC, src + offC, 8);
			else
				memset(dst + offC, 0, 8);
		}
		int row = y * (width + 1);
		m[row + x] = unavail_mb;
		m[row + x].error_probability = 100;
		m[row + x].recovery_bits = recovery_bits;
		if (x == width - 1 && row + width < mbs)
			m[row + width] = unavail_mb;
	}
	__atomic_store_n(&dec->remaining_mbs[id], 0, __ATOMIC_RELEASE);
	__atomic_store_n(&dec->next_deblock_addr[id], INT_MAX, __ATOMIC_RELEASE);
	if (dec->n_threads)
		wake_frame_waiters(dec, id);
	return 1;
}

// Break a wait on a reference picture that can no longer complete. A damaged
// slice may leave remaining_mbs positive after its last task exits; later
// tasks then wait on that frame while every worker sleeps. An unresolved
// dependency with no task targeting its slot (and not being parsed) has no
// possible writer. Conceal only those terminal dependencies, recompute
// readiness, and wake the workers. Single-threaded decoding only does it when
// no task is ready, while multithreaded workers call it whenever they would
// otherwise wait on such a frame, since a running task may already wait on the
// progress of a pending task that depends on it. Conformant streams never enter
// this path: an incomplete dependency retains a writer until it reaches INT_MAX.
static int release_terminal_task_dependencies(Edge264MvcDecoder *dec) {
	if (!dec->n_threads && (tm_any(dec->ready_tasks) || !tm_eq(dec->pending_tasks, dec->busy_tasks)))
		return 0;
	// Pictures awaiting output are concealed as soon as they are terminal too, so
	// that get_frame never delivers later pictures past them while they wait (the
	// number of pictures overtaking them would depend on the thread timing).
	FrameMask terminal = (depended_frames(dec) | dec->to_get_frames) & ~ready_frames(dec) & ~writing_frames(dec);
	if (dec->currPic >= 0)
		terminal &= ~((FrameMask)1 << dec->currPic);
	FrameMask concealed = 0;
	for (FrameMask b = terminal; b; b &= b - 1)
		concealed |= (FrameMask)conceal_frame(dec, mask_ctz(b)) << mask_ctz(b);
	if (concealed) {
		dec->ready_tasks = ready_tasks(dec);
		if (tm_any(dec->ready_tasks) && dec->n_threads)
			pthread_cond_signal(&dec->task_ready);
	}
	return concealed != 0;
}

// All task_complete waits assume that some worker can eventually signal. Check
// for the quiescent abandoned-reference state first; if concealment made a task
// runnable, let the caller re-evaluate its wait predicate without sleeping.
static void progress_or_wait(Edge264MvcDecoder *dec) {
	if (release_terminal_task_dependencies(dec) && tm_any(dec->ready_tasks))
		return;
	pthread_cond_wait(&dec->task_complete, &dec->lock);
}

// Marks that a task knows its bound and decodes nothing past it, and wakes a
// younger slice waiting for that in claim_after_older_slices. The seq_cst
// store-then-load pairs with the waiter's, so either it sees the ack or this
// sees it waiting.
static void ack_mb_bound(Edge264MvcDecoder *dec, int task_id, int pic) {
	acked_set(dec, task_id);
	if (__atomic_load_n(&dec->progress_wake_addr[pic], __ATOMIC_SEQ_CST) != INT_MAX) {
		pthread_mutex_lock(&dec->lock);
		wake_frame_waiters(dec, pic);
		pthread_mutex_unlock(&dec->lock);
	}
}

/**
 * Gives the held slice task its bound, now that the next NAL tells where it
 * ends: mb_bound is the first macroblock of the slice following it in decoding
 * order if that one starts after it, else INT_MAX. Like FFmpeg (next_slice_idx),
 * a slice thus never decodes past the start of the next one, which makes slices
 * that overlap on a damaged stream resolve the same way whatever the thread
 * timing, rather than by whichever worker claims a macroblock first.
 * Single-threaded, the task was held and runs now. Multithreaded, it started
 * when it was parsed and learns its bound as it goes (see known_mb_bound).
 */
static void release_held_task(Edge264MvcDecoder *dec, int32_t mb_bound) {
	int i = dec->held_task;
	if (i < 0)
		return;
	dec->held_task = -1;
	__atomic_store_n(&dec->task_bounds[i], mb_bound, __ATOMIC_RELEASE);
	if (dec->n_threads) {
		wake_frame_waiters(dec, dec->taskPics[i]); // a worker done with the slice waits there
	} else {
		dec->ready_tasks = ready_tasks(dec);
		while (tm_any(dec->busy_tasks)) {
			if (!tm_any(dec->ready_tasks)) {
				// Keep damaged-stream concealment deterministic across threading
				// modes. A terminal incomplete dependency has no writer here either;
				// conceal it before falling back to the historical force-run valve.
				release_terminal_task_dependencies(dec);
				if (!tm_any(dec->ready_tasks)) {
					// ready_tasks can also be 0 when task_dependencies includes the
					// current frame's own slot in a transitional state.
					tm_set(&dec->ready_tasks, oldest_task(dec, dec->pending_tasks));
				}
			}
			dec->worker_loop(dec);
		}
	}
}

/**
 * Called when a picture ends, before any task of a later picture can read it.
 * A slice that started before its bound was known may go on publishing rows
 * past it until it learns it, so wait until every started slice of the picture
 * acked its bound, having taken back what it published past it (overrun). The
 * last slice has no bound, the others learned theirs long before as a rule.
 */
static void settle_mb_bounds(Edge264MvcDecoder *dec, int pic) {
	if (!dec->n_threads)
		return;
	for (;;) {
		TaskMask unsettled = tm_none();
		TaskMask acked = acked_load(dec);
		int i;
		TM_FOREACH(i, tm_andnot(tm_andnot(dec->busy_tasks, dec->pending_tasks), acked)) {
			if (dec->taskPics[i] == pic && __atomic_load_n(&dec->task_bounds[i], __ATOMIC_RELAXED) != INT_MAX) {
				tm_set(&unsettled, i);
				if (dec->task_wait_pic[i] >= 0) // waiting for a reference, it learns the bound there
					wake_frame_waiters(dec, dec->task_wait_pic[i]);
			}
		}
		if (!tm_any(unsettled))
			return;
		__atomic_store_n(&dec->progress_wake_addr[pic], INT_MIN, __ATOMIC_SEQ_CST);
		if (!tm_any(tm_and(acked_load(dec), unsettled)))
			pthread_cond_wait(&dec->frame_progress[pic], &dec->lock);
	}
}

/**
 * Called on a multithreaded slice that started before its bound was known, at
 * the first macroblock after the parser set it, or after its last macroblock.
 * If it already decoded past the bound (overlapping slices of a damaged
 * stream), it is decoded again as if it had known the bound from the start
 * (overrun, see worker_loop). Otherwise it acks the bound for a younger slice
 * of its picture that found one of its macroblocks claimed meanwhile.
 */
static noinline void known_mb_bound(Edge264MvcContext *ctx, int32_t mb_bound, int claimed) {
	ctx->t.mb_bound = mb_bound;
	if (ctx->CurrMbAddr + claimed > mb_bound)
		ctx->overrun = 1; // acked once its claims are undone
	else
		ack_mb_bound(ctx->d, ctx->task_id, ctx->currPic);
}

/**
 * Called when a slice finds a macroblock already claimed in its picture. On a
 * conformant stream that never happens. An older slice that started before its
 * bound was known may have decoded past it into this slice, and gives those
 * macroblocks back once it learns the bound, so wait for every such slice to
 * ack it and try again. Any claim left is one an older slice decoding up to its
 * bound made, which ends this slice as when decoding single-threaded.
 */
static noinline int claim_after_older_slices(Edge264MvcContext *ctx) {
	Edge264MvcDecoder *dec = ctx->d;
	if (ctx->thread_id < 0)
		return 0;
	pthread_mutex_lock(&dec->lock);
	for (;;) {
		TaskMask unacked = tm_none();
		TaskMask acked = acked_load(dec);
		int i;
		TM_FOREACH(i, tm_andnot(dec->busy_tasks, acked)) {
			if (dec->taskPics[i] == ctx->currPic && (int32_t)dec->tasks[i].first_mb_in_slice < ctx->CurrMbAddr &&
				(int32_t)(dec->task_seq[i] - dec->task_seq[ctx->task_id]) < 0)
				tm_set(&unacked, i);
		}
		if (!tm_any(unacked))
			break;
		__atomic_store_n(&dec->progress_wake_addr[ctx->currPic], INT_MIN, __ATOMIC_SEQ_CST);
		if (!tm_any(tm_and(acked_load(dec), unacked)))
			pthread_cond_wait(&dec->frame_progress[ctx->currPic], &dec->lock);
	}
	pthread_mutex_unlock(&dec->lock);
	return __atomic_exchange_n(&ctx->_mb->recovery_bits, ctx->t.frame_flip_bit, __ATOMIC_ACQ_REL) != ctx->t.frame_flip_bit;
}



/**
 * Tells whether a full DPB or output queue is a stall that waiting cannot
 * resolve. A caller holding frames makes room by releasing them, and under
 * multithreading the pictures still being decoded come out once their tasks
 * finish. While a task runs, let the caller receive as usual (ENOBUFS):
 * receive_frame then waits for the next picture only, and the other tasks keep
 * decoding. Waiting here for all of them instead drained the frame pipeline at
 * every full DPB, which a stream with many reference frames hits every few
 * pictures. Once no task is left and the caller holds no frame, a further
 * round without any frame coming out leaves nothing but a valve to make
 * progress. That round starts with every task finished, so the outcome does
 * not depend on the threads.
 */
static int output_stalled(Edge264MvcDecoder *dec) {
	if (dec->output_frames & ~dec->to_get_frames) {
		dec->undelivered = 0;
		return 0;
	}
	if (dec->undelivered)
		return 1;
	if (tm_any(tm_andnot(dec->busy_tasks, held_tasks(dec)))) // the held slice belongs to the open picture
		return 0;
	dec->undelivered = 1;
	return 0;
}



/**
 * Called before a new picture (currPic < 0) when every DPB slot is taken by a
 * reference or a picture waiting for output and output_stalled, which only a
 * damaged stream causes, since a conformant one stays within
 * max_dec_frame_buffering. Returning ENOBUFS would then wait for frames
 * get_frame never delivers: a base held for a dependent view that will never
 * complete, or no picture queued at all. Instead, finish the in-flight tasks,
 * conceal and queue every picture waiting for output as at the end of a
 * stream, and let get_frame emit them, or with only references left, drop the
 * oldest one as the sliding window would (8.2.5.3). Waiting for the tasks
 * first keeps the outcome independent of the thread timing. Returns 1 if the
 * caller can now receive frames.
 */
static int make_room(Edge264MvcDecoder *dec, int non_base_view) {
	if (bump_all_frames(dec)) {
		dec->flushing = 1; // cleared by the next NAL
		return 1;
	}
	FrameMask same_views = non_base_view ? dec->non_base_frames : ~dec->non_base_frames;
	FrameMask refs = dec->prev_short_term_frames & ~dec->prev_long_term_frames;
	refs = (refs & same_views) ? refs & same_views : refs;
	int unref = -1, lowest = INT_MAX;
	for (FrameMask r = refs; r; r &= r - 1) {
		int i = mask_ctz(r);
		if (dec->FrameIds[i] < lowest)
			lowest = dec->FrameIds[unref = i];
	}
	if (unref < 0) // only long-term references left
		for (FrameMask r = dec->prev_long_term_frames; r; r &= r - 1) {
			int i = mask_ctz(r);
			if (dec->FrameIds[i] < lowest)
				lowest = dec->FrameIds[unref = i];
		}
	if (unref >= 0) {
		dec->prev_short_term_frames &= ~((FrameMask)1 << unref);
		dec->prev_long_term_frames &= ~((FrameMask)1 << unref);
	}
	return 0;
}



/**
 * This function matches slice_header() in 7.3.3, which it parses while updating
 * the DPB and initialising slice data for further decoding.
 */
int ADD_VARIANT(parse_slice_layer_without_partitioning)(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg)
{
	static const char * const slice_type_names[5] = {"P", "B", "I", "SP", "SI"};
	static const char * const disable_deblocking_filter_idc_names[3] = {"enabled", "disabled", "sliced"};
	int ret;

	// find and reserve an empty task to fill, with no more pictures in flight
	// than set for the threads (a picture has a task per slice)
	TaskMask avail_tasks;
	while (mask_popcount(writing_frames(dec)) > pics_in_flight(dec) || !tm_any(avail_tasks = tm_andnot(tm_all(), dec->busy_tasks)))
		progress_or_wait(dec);
	Edge264MvcTask *t = dec->tasks + tm_ctz(avail_tasks);
	t->unref_cb = unref_cb;
	t->unref_arg = unref_arg;
	t->RefPicList_v[0] = t->RefPicList_v[1] = t->RefPicList_v[2] = t->RefPicList_v[3] =
		(i8x16){-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
	
	// check on view_id
	int non_base_view = 1;
	FrameMask same_views = dec->non_base_frames;
	Edge264MvcSeqParameterSet *sps = &dec->ssps;
	if (dec->nal_unit_type != 20) {
		non_base_view = 0;
		same_views = ~same_views;
		sps = &dec->sps;
		dec->IdrPicFlag = dec->nal_unit_type == 5;
	}
	
	// first important fields and checks before decoding the slice header
	t->first_mb_in_slice = get_ue32(&dec->gb, 139263);
	int slice_type = get_ue16(&dec->gb, 9);
	slice_type = (dec->nal_unit_type == 5 || sps->max_num_ref_frames == 0) ? 2 : slice_type; // enforce condition in 7.4.3
	t->slice_type = (slice_type < 5) ? slice_type : slice_type - 5;
	int pic_parameter_set_id = get_ue16(&dec->gb, 255);
	log_dec(dec, "  first_mb_in_slice: %u\n"
		"  slice_type: %u # %s%s\n"
		"  pic_parameter_set_id: %u%s\n",
		t->first_mb_in_slice,
		slice_type, slice_type_names[t->slice_type], unsup_if(t->slice_type > 2),
		pic_parameter_set_id, unsup_if(pic_parameter_set_id >= 4));
	if (t->slice_type > 2 || pic_parameter_set_id >= 4)
		return print_dec(dec, "  decode_NAL_result: %s\n", ENOTSUP); // exit now if the rest of the slice can't be correctly parsed
	t->pps = dec->PPS[pic_parameter_set_id];
	if (!sps->BitDepth_Y || !t->pps.num_ref_idx_active[0])
		return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG); // exit now if SPS or PPS wasn't initialized
	// Frame buffers are sized from the base SPS (alloc_frame), and every view of
	// MVC has the frame size of the base view. A subset SPS of another size made
	// a format change that cleared the base SPS, after which dependent slices
	// allocated frames from its zero size (-1 macroblocks, i.e. 4 GB).
	if (sps->pic_width_in_mbs != dec->sps.pic_width_in_mbs || sps->pic_height_in_mbs != dec->sps.pic_height_in_mbs)
		return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
	// first_mb_in_slice must address a macroblock inside the current picture
	// (7.4.3: 0..PicSizeInMbs-1). An out-of-range value sets CurrMbAddr (and the
	// derived mb/sample pointers in initialize_context) past the frame, so the
	// macroblock loop writes far out of bounds - a hard crash, not a decode
	// error. This is reachable when a caller feeds two interleaved elementary
	// streams of different resolutions into one decoder (e.g. a Blu-ray main +
	// secondary/PiP video): after the smaller stream's SPS becomes active, a
	// leftover slice from the larger picture carries a first_mb_in_slice beyond
	// the smaller geometry. Reject it as corrupt instead of dereferencing wild
	// pointers; matches ffmpeg, which drops such a slice. Inert for conformant
	// streams, whose first_mb_in_slice is always in range.
	if (t->first_mb_in_slice >= sps->pic_width_in_mbs * sps->pic_height_in_mbs)
		return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);

	// keep frame_num on stack until we can compute FrameNum after all unset_currPic
	int frame_num = get_uv(&dec->gb, sps->log2_max_frame_num);
	frame_num = dec->IdrPicFlag ? 0 : frame_num; // enforce condition in 7.4.3
	
	// As long as PAFF/MBAFF are unsupported, this code won't execute (but is still kept).
	t->field_pic_flag = 0;
	t->bottom_field_flag = 0;
	if (!sps->frame_mbs_only_flag) {
		t->field_pic_flag = get_u1(&dec->gb);
		log_dec(dec, "  field_pic_flag: %u\n", t->field_pic_flag);
		if (t->field_pic_flag) {
			t->bottom_field_flag = get_u1(&dec->gb);
			log_dec(dec, "  bottom_field_flag: %u\n",
				t->bottom_field_flag);
		}
	}
	t->MbaffFrameFlag = sps->mb_adaptive_frame_field_flag & ~t->field_pic_flag;
	
	// idr_pic_id is used to detect new frames in streams of IDRs
	int idr_pic_id = -1;
	if (dec->IdrPicFlag) {
		idr_pic_id = get_ue32(&dec->gb, 65535);
		log_dec(dec, "  idr_pic_id: %u\n", idr_pic_id);
	}
	
	// An access unit holds a single view component per view (H.7.4.1.2.4), so
	// while a dependent picture is open an inter-coded type-20 slice normally
	// continues it. dep_corrupt below is exactly that test: it holds when the
	// slice's frame_num is NOT one a new picture could legally carry here, i.e.
	// the slice claims to continue the open picture. If a new-picture trigger
	// then fires anyway - a differing POC or nal_ref_idc, or an IDR whose
	// frame_num 7.4.3 forces to 0 - the header is damaged rather than the start
	// of a picture. That is the shape a 3D Blu-ray rip has when one right-eye
	// slice is corrupted while its base view stays intact. Accepting such a slice
	// closes the open picture, inserts non-existing frames and seeds
	// PrevRefFrameNum and prevPicOrderCnt of the dependent view with garbage;
	// every later dependent POC then mismatches its base, the base-driven pairing
	// in bump_frame never queues them, and the DPB fills until decode_NAL returns
	// ENOBUFS forever. Reject it before it alters any state, so the remaining
	// slices of the open picture still decode. Inert for well-formed MVC, where a
	// base picture always closes the dependent one first, and for a stream that
	// merely lost its base view, whose dependent pictures follow each other with
	// frame_num advancing by one (tests/liveness/mvc_orphan_*). The exception is
	// an anchor (IDR) dependent picture reached with no base view of its own
	// access unit: its frame_num of 0 lands here and is rejected, which is the
	// wanted outcome - the inter-view reference it predicts from is gone, so the
	// alternative is decoding it against a stale base of an earlier access unit.
	int FrameNumMask = (1 << sps->log2_max_frame_num) - 1;
	int dep_corrupt = 0;
	if (dec->nal_unit_type == 20 && t->slice_type < 2 && dec->currPic >= 0 &&
		(dec->non_base_frames >> dec->currPic & 1)) {
		int prev = dec->currPic_marked ? dec->FrameNums[dec->currPic] : dec->PrevRefFrameNum[non_base_view];
		dep_corrupt = ((frame_num - prev - 1) & FrameNumMask) > 0;
	}

	// dep_corrupt keys on frame_num, so it only sees damage that frame_num itself
	// reveals, and only on an inter slice. The same jam is reachable through every
	// other new-picture trigger: an intra slice of an open dependent IDR picture
	// whose idr_pic_id, nal_ref_idc or pic_order_cnt alone is corrupt opens a
	// second dependent picture carrying the same (FrameNum, POC) as the first; the
	// base of the access unit pairs with one of them, the other is never queued by
	// bump_frame, and the DPB jams exactly as above. dep_continuation catches the
	// whole family at once, on the invariant that makes it impossible in a
	// conformant stream: arbitrary slice order is not allowed in either MVC
	// profile (H.10.1.1 and H.10.1.2 both state it), so by 7.4.3 the slices of a
	// picture arrive with non-decreasing first_mb_in_slice and a picture's first
	// slice always addresses macroblock 0. A type-20 slice that starts further in,
	// right where the open dependent picture continues, therefore cannot be the
	// start of a new picture - if a new-picture trigger fires on it, the header is
	// damaged. Reject it before it alters any state, so the remaining slices of
	// the open picture still decode. Inert for well-formed MVC at any slice count,
	// and for a stream that merely lost its base view, whose dependent pictures
	// each begin at macroblock 0.
	int dep_continuation = dec->nal_unit_type == 20 && dec->currPic >= 0 &&
		(dec->non_base_frames >> dec->currPic & 1) && t->first_mb_in_slice > 0;

	// detect the start of a new frame (7.4.1.2.4)
	int frame_num_changed = dec->currPic >= 0 && frame_num != (dec->FrameNum & FrameNumMask);
	int nal_ref_idc_changed = dec->currPic >= 0 && (dec->nal_ref_idc > 0) != dec->currPic_reference;
	if (dep_corrupt && (frame_num_changed || nal_ref_idc_changed))
		return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
	if (dep_continuation && (frame_num_changed || nal_ref_idc_changed ||
		idr_pic_id != dec->idr_pic_id))
		return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
	if (dec->currPic >= 0 && (frame_num_changed || nal_ref_idc_changed ||
		(dec->nal_unit_type == 20) != (dec->non_base_frames >> dec->currPic & 1) ||
		idr_pic_id != dec->idr_pic_id)) {
		unset_currPic(dec);
	}
	dec->idr_pic_id = idr_pic_id;

	// An inter-coded MVC dependent-view slice (type 20, P/B) predicts from the base
	// view of its access unit via the inter-view reference. A corrupt/incomplete
	// stream that carries such dependent slices but no decodable base view (e.g. no
	// base-view SPS, so no base picture is ever created and basePic stays -1) leaves
	// every dependent slice with a RefPicList that resolves only to its own
	// not-yet-decoded slot: the inter-view reference is basePic == -1, and the
	// out-of-range fix-up in parse_ref_pic_list_modification falls back to the
	// current picture's own slot. That is a task whose only dependency is its own
	// frame, which in the multithreaded path never clears - the worker never runs
	// it, the frame never completes, and once 16 such tasks pile up the parser
	// blocks forever waiting for a free task slot (a hard deadlock, never returning
	// from decode_nal). Reject the slice as corrupt before it reserves any
	// decoder state; matches ffmpeg, which reports the missing base view and
	// produces no frame. Restricted to P/B slices (slice_type < 2): an intra
	// dependent slice has no RefPicList and no inter-view reference, so it decodes
	// standalone and pairs once its base arrives - which is exactly the legal
	// dependent-before-base NAL reordering (see mvc_dep_before_base), where the
	// base view of the access unit follows the dependent view in decode order and
	// basePic is momentarily -1. basePic is only ever set by a base view (in
	// unset_currPic above, once the base picture of the access unit closes), so a
	// conformant inter-coded dependent view - whose base always precedes it - sees
	// basePic >= 0 here; this is inert for well-formed MVC.
	if (dec->nal_unit_type == 20 && dec->basePic < 0 && t->slice_type < 2)
		return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);

	// Compute Top/BottomFieldOrderCnt (8.2.1), and FrameNum after the last possible unset_currPic
	int TopFieldOrderCnt, BottomFieldOrderCnt;
	if (sps->pic_order_cnt_type == 0) {
		int pic_order_cnt_lsb = get_uv(&dec->gb, sps->log2_max_pic_order_cnt_lsb);
		int shift = WORD_BIT - sps->log2_max_pic_order_cnt_lsb;
		if (dec->currPic >= 0 && pic_order_cnt_lsb != ((unsigned)dec->TopFieldOrderCnt << shift >> shift)) {
			if (dep_corrupt || dep_continuation)
				return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
			unset_currPic(dec);
		}
		// unset_currPic must happen before prevPicOrderCnt to get an up-to-date value
		dec->FrameNum = derive_FrameNum(dec, frame_num, FrameNumMask, non_base_view);
		int prevPicOrderCnt = dec->prevPicOrderCnt[non_base_view];
		int inc = (int)(((unsigned)pic_order_cnt_lsb - (unsigned)prevPicOrderCnt) << shift) >> shift; // sign-extends the lsb difference
		// picture order counts are added modulo 2^32: on a damaged stream the
		// syntax elements (up to +-2^31) overflow an int, which is undefined
		BottomFieldOrderCnt = TopFieldOrderCnt = (int)((unsigned)prevPicOrderCnt + inc);
		log_dec(dec, "  pic_order_cnt: {type: 0, bits: %u, absolute: %d",
			sps->log2_max_pic_order_cnt_lsb, TopFieldOrderCnt);
		if (t->pps.bottom_field_pic_order_in_frame_present_flag && !t->field_pic_flag) {
			BottomFieldOrderCnt = (int)((unsigned)BottomFieldOrderCnt + get_se32(&dec->gb, (-1u << 31) + 1, (1u << 31) - 1));
			log_dec(dec, ", bottom: %d", BottomFieldOrderCnt);
		}
		log_dec(dec, "}\n");
	} else if (sps->pic_order_cnt_type == 1) {
		log_dec(dec, "  pic_order_cnt: {type: 1");
		int delta_pic_order_cnt0 = 0;
		int delta_pic_order_cnt1 = 0;
		if (!sps->delta_pic_order_always_zero_flag) {
			delta_pic_order_cnt0 = get_se32(&dec->gb, (-1u << 31) + 1, (1u << 31) - 1);
			log_dec(dec, ", delta0: %d", delta_pic_order_cnt0);
			if (t->pps.bottom_field_pic_order_in_frame_present_flag && !t->field_pic_flag) {
				delta_pic_order_cnt1 = get_se32(&dec->gb, (-1u << 31) + 1, (1u << 31) - 1);
				log_dec(dec, ", delta1: %d", delta_pic_order_cnt1);
			}
		}
		if (dec->currPic >= 0 && delta_pic_order_cnt0 != dec->delta_pic_order_cnt0) {
			if (dep_corrupt || dep_continuation)
				return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
			unset_currPic(dec);
		}
		dec->delta_pic_order_cnt0 = delta_pic_order_cnt0;
		// unset_currPic must happen before PrevRefFrameNum to get a definitive value
		dec->FrameNum = derive_FrameNum(dec, frame_num, FrameNumMask, non_base_view);
		int absFrameNum = (sps->num_ref_frames_in_pic_order_cnt_cycle > 0) ? dec->FrameNum : 0;
		absFrameNum -= (dec->nal_ref_idc == 0 && absFrameNum > 0);
		// added modulo 2^32 like the type 0 counts above
		unsigned top = (unsigned)delta_pic_order_cnt0 + (dec->nal_ref_idc ? 0 : sps->offset_for_non_ref_pic);
		if (absFrameNum > 0) {
			top += (unsigned)((absFrameNum - 1) / sps->num_ref_frames_in_pic_order_cnt_cycle) *
				sps->PicOrderCntDeltas[sps->num_ref_frames_in_pic_order_cnt_cycle - 1] +
				sps->PicOrderCntDeltas[(absFrameNum - 1) % sps->num_ref_frames_in_pic_order_cnt_cycle];
		}
		TopFieldOrderCnt = (int)top;
		BottomFieldOrderCnt = (int)(top + sps->offset_for_top_to_bottom_field + delta_pic_order_cnt1);
		log_dec(dec, (TopFieldOrderCnt == BottomFieldOrderCnt) ?
			", absolute: %d}\n" : ", absolute: %d, bottom: %d}\n",
			TopFieldOrderCnt, BottomFieldOrderCnt);
	} else {
		dec->FrameNum = derive_FrameNum(dec, frame_num, FrameNumMask, non_base_view);
		TopFieldOrderCnt = BottomFieldOrderCnt = (int)((unsigned)dec->FrameNum * 2 + (dec->nal_ref_idc != 0) - 1);
		log_dec(dec, "  pic_order_cnt: {type: 2, absolute: %d}\n", TopFieldOrderCnt);
	}
	
	// 7.3.3 puts redundant_pic_cnt right after the picture order count fields
	// when the active PPS enables it. Leaving these bits unread shifts every
	// later field of the header, which ends up placing cabac_alignment_one_bit
	// inside slice data and corrupting the entire slice, so such a PPS used to
	// make the stream unsupported. A redundant coded picture (7.4.3:
	// redundant_pic_cnt greater than 0) is a lower quality copy of content the
	// primary picture already carries, meant to be decoded only when the primary
	// is lost; decoding it here would overwrite the primary, so skip that slice
	// and keep the stream going.
	if (t->pps.redundant_pic_cnt_present_flag) {
		int redundant_pic_cnt = get_ue16(&dec->gb, 127);
		log_dec(dec, "  redundant_pic_cnt: %u%s\n", redundant_pic_cnt, unsup_if(redundant_pic_cnt > 0));
		if (redundant_pic_cnt > 0)
			return print_dec(dec, "  decode_NAL_result: %s\n", ENOTSUP);
	}
	dec->TopFieldOrderCnt = TopFieldOrderCnt;
	dec->BottomFieldOrderCnt = BottomFieldOrderCnt;
	log_dec(dec, "  frame_num: {bits: %u, absolute: %u}\n",
		sps->log2_max_frame_num, dec->FrameNum);
	
	// An SPS may lower max_num_ref_frames without an IDR picture (a damaged
	// stream, 7.4.1.2.1), which leaves more references than it allows: drop the
	// oldest ones, as the sliding window would (8.2.5.3), and long-term ones only
	// when no short-term one is left.
	while (mask_popcount((dec->prev_short_term_frames | dec->prev_long_term_frames) & same_views) > sps->max_num_ref_frames) {
		FrameMask shorts = dec->prev_short_term_frames & same_views;
		int unref = -1, lowest = INT_MAX;
		for (FrameMask r = shorts ? shorts : dec->prev_long_term_frames & same_views; r; r &= r - 1) {
			int i = mask_ctz(r);
			int key = shorts ? dec->FrameNums[i] : dec->prev_LongTermFrameIdx[i];
			if (key < lowest)
				lowest = key, unref = i;
		}
		dec->prev_short_term_frames &= ~((FrameMask)1 << unref);
		dec->prev_long_term_frames &= ~((FrameMask)1 << unref);
	}
	
	// check for gaps in frame_num (8.2.5.2), which only non-IDR pictures have:
	// an IDR picture continues at the next multiple of MaxFrameNum, and the
	// reference pictures before it are all dropped anyway
	int gap = dec->FrameNum - dec->PrevRefFrameNum[non_base_view];
	if (__builtin_expect(gap > 1, 0) && !dec->IdrPicFlag) {
		// The frames inferred for the gap get the content of the latest short-term
		// reference of this view, as in FFmpeg (zeros without one, as conceal_frame
		// uses). They have no content of their own, so a damaged stream predicting
		// from one otherwise read whatever the reused slot held, which depended on
		// thread timing. Find it before the sliding window below may dereference it.
		int prev = -1;
		for (FrameMask r = same_views & dec->prev_short_term_frames; r; r &= r - 1) {
			int i = mask_ctz(r);
			if (dec->samples_buffers[i] && (prev < 0 || dec->FrameIds[i] > dec->FrameIds[prev]))
				prev = i;
		}
		// make enough non-reference slots by dereferencing short-term and non-existing frames
		int sref_slots = sps->max_num_ref_frames - mask_popcount(same_views & dec->prev_long_term_frames & ~dec->prev_short_term_frames);
		// A frame_num gap needs a short-term slot to hold the inferred
		// non-existing frames. If every reference slot is already long-term there
		// is none to reclaim - a non-conformant stream. Reject it gracefully (no
		// frame has been allocated yet, currPic < 0) instead of aborting on the
		// old assert(sref_slots > 0) or proceeding into a stalled DPB state.
		if (sref_slots <= 0)
			return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
		int non_existing = min(gap - 1, sref_slots);
		for (int num_srefs = non_existing + mask_popcount(same_views & dec->prev_short_term_frames); num_srefs > sref_slots; num_srefs--) {
			int unref = 0, lowest = INT_MAX;
			for (FrameMask r = same_views & dec->prev_short_term_frames; r; r &= r - 1) {
				int i = mask_ctz(r);
				if (dec->FrameNums[i] < lowest)
					lowest = dec->FrameNums[unref = i];
			}
			dec->prev_short_term_frames &= ~((FrameMask)1 << unref);
			dec->prev_long_term_frames &= ~((FrameMask)1 << unref);
		}
		// bump frames until there are enough available slots in the DPB
		FrameMask reference_frames = dec->prev_short_term_frames | dec->prev_long_term_frames;
		assert(dec->currPic < 0);
		while (non_existing + mask_popcount(reference_frames | dec->to_get_frames & ~dec->output_frames) > sps->max_dec_frame_buffering && bump_frame(dec, non_base_view, 0));
		// Bound the assert by the physical DPB capacity (MAX_FRAMES slots), not the
		// signaled max_dec_frame_buffering: an MVC stream whose two views together
		// need more held frames than the base-derived MFB (a deep-B pyramid, or a
		// frame_num gap that inserts non-existing references into an already-full
		// view set) legitimately overshoots MFB while staying within the physical
		// slots. The bump loop above relieves what it can; the real overflow guard
		// is the ENOBUFS backpressure below. Keying this to MFB aborted such streams
		// (issue #2). Conformant streams stay <= MFB, so this is inert for them.
		assert(non_existing + mask_popcount(reference_frames | dec->to_get_frames & ~dec->output_frames) <= MAX_FRAMES);
		while (non_existing + mask_popcount(reference_frames | dec->to_get_frames | dec->output_frames) > MAX_FRAMES) {
			if (!output_stalled(dec) || make_room(dec, non_base_view))
				return ENOBUFS; // exit here if we must wait for get_frame to consume and return enough frames
			reference_frames = dec->prev_short_term_frames | dec->prev_long_term_frames;
		}
		// wait until enough empty slots are undepended and not written by in-flight tasks
		FrameMask unavail;
		while (non_existing + mask_popcount(unavail = reference_frames | dec->to_get_frames | dec->output_frames | depended_frames(dec) | inflight_frames(dec)) > MAX_FRAMES)
			progress_or_wait(dec);
		// finally insert the last non-existing frames one by one
		for (unsigned FrameNum = dec->FrameNum - non_existing; FrameNum < dec->FrameNum; FrameNum++) {
			int i = mask_ctz(~unavail);
			if (dec->samples_buffers[i] == NULL &&
				(ret = alloc_frame(dec, i)))
				return ret;
			unavail |= (FrameMask)1 << i;
			dec->prev_short_term_frames |= (FrameMask)1 << i;
			dec->prev_long_term_frames |= (FrameMask)1 << i;
			dec->non_base_frames = dec->non_base_frames & ~((FrameMask)1 << i) | (FrameMask)non_base_view << i;
			dec->FrameNums[i] = dec->PrevRefFrameNum[non_base_view] = FrameNum;
			dec->FrameIds[i] = ++dec->prevFrameId;
			int PicOrderCnt = 0;
			if (sps->pic_order_cnt_type == 2) {
				PicOrderCnt = (int)(FrameNum * 2);
			} else if (sps->num_ref_frames_in_pic_order_cnt_cycle > 0 && FrameNum > 0) {
				// Mirror the correct main-path derivation (8.2.1.2): absFrameNum
				// equals FrameNum for these inferred-reference gap frames, so the
				// cycle multiplier is PicOrderCntDeltas[cycle-1] (the full-cycle
				// sum, the only element written) and both the quotient and the
				// remainder use FrameNum-1. Indexing [cycle] read one past the
				// int16_t[255] array (cycle up to 255) into the next SPS field.
				PicOrderCnt = (int)((FrameNum - 1) / sps->num_ref_frames_in_pic_order_cnt_cycle *
					sps->PicOrderCntDeltas[sps->num_ref_frames_in_pic_order_cnt_cycle - 1] +
					sps->PicOrderCntDeltas[(FrameNum - 1) % sps->num_ref_frames_in_pic_order_cnt_cycle]);
			}
			dec->FieldOrderCnt[0][i] = dec->FieldOrderCnt[1][i] = PicOrderCnt;
			// copy the samples once no task writes them (concealing an abandoned
			// picture first), and make every macroblock intra for colocated reads
			if (prev >= 0) {
				while (__atomic_load_n(&dec->next_deblock_addr[prev], __ATOMIC_ACQUIRE) != INT_MAX &&
					((writing_frames(dec) >> prev & 1) || !conceal_frame(dec, prev)))
					progress_or_wait(dec);
				if (prev != i)
					memcpy(dec->samples_buffers[i], dec->samples_buffers[prev], dec->plane_size_Y + dec->plane_size_C);
			} else {
				memset(dec->samples_buffers[i], 0, dec->plane_size_Y + dec->plane_size_C);
			}
			Edge264MvcMacroblock *m = dec->mb_buffers[i];
			int width = dec->sps.pic_width_in_mbs;
			for (int row = 0; row < dec->sps.pic_height_in_mbs * (width + 1); row += width + 1) {
				for (int x = row; x < row + width; x++) {
					int8_t recovery_bits = m[x].recovery_bits;
					m[x] = unavail_mb;
					m[x].recovery_bits = recovery_bits;
				}
			}
			prev = i;
			dec->remaining_mbs[i] = 0;
			__atomic_store_n(&dec->next_deblock_addr[i], INT_MAX, __ATOMIC_RELEASE);
		}
	}
	
	// conceal the previous picture now if it was left incomplete and nothing writes it
	if (dec->currPic < 0)
		release_terminal_task_dependencies(dec);
	
	// find and possibly allocate a memory slot for the upcoming frame
	if (dec->currPic < 0) {
		// MVC: prevent dependent view from aliasing the base view's DPB slot,
		// since the dependent view references the base view's pixels for
		// inter-view prediction and would corrupt them by overwriting. Count it
		// as taken from the start, since a slot found free only before excluding
		// it left no slot at all (and a slot index past the DPB).
		FrameMask reference_frames = dec->prev_short_term_frames | dec->prev_long_term_frames |
			(non_base_view && dec->basePic >= 0 ? (FrameMask)1 << dec->basePic : 0);
		while (mask_popcount(reference_frames | dec->to_get_frames | dec->output_frames) == MAX_FRAMES) {
			if (!output_stalled(dec) || make_room(dec, non_base_view))
				return ENOBUFS; // exit here if we must wait for get_frame to consume and return a frame slot
			reference_frames = dec->prev_short_term_frames | dec->prev_long_term_frames |
				(non_base_view && dec->basePic >= 0 ? (FrameMask)1 << dec->basePic : 0);
		}
		// wait until at least one empty slot is undepended and not written by an
		// in-flight task (or returned in the meantime). inflight_frames matters
		// when a frame leaves to_get_frames/output_frames while its decode tasks
		// still run (e.g. get_frame's orphan-dependent valve on a base-less
		// dependent tail): without it the slot is reallocated mid-decode and the
		// stale tasks corrupt the new picture's remaining_mbs (see inflight_frames).
		FrameMask unavail;
		while (mask_popcount(unavail = reference_frames | dec->to_get_frames | dec->output_frames | depended_frames(dec) | inflight_frames(dec)) >= MAX_FRAMES)
			progress_or_wait(dec);
		int currPic = mask_ctz(~unavail);
		if (dec->samples_buffers[currPic] == NULL &&
			(ret = alloc_frame(dec, currPic)))
			return ret;
		dec->currPic = currPic;
		dec->currPic_reference = dec->nal_ref_idc != 0;
		dec->frame_flags[currPic] = dec->IdrPicFlag ? EDGE264MVC_VIEW_IDR : 0;
		dec->frame_pts[currPic] = dec->in_pts;
		dec->frame_user_data[currPic] = dec->in_user_data;
		dec->non_base_frames = dec->non_base_frames & ~((FrameMask)1 << currPic) | (FrameMask)non_base_view << currPic;
		dec->frame_flip_bits ^= (FrameMask)1 << currPic;
		dec->FrameIds[currPic] = ++dec->prevFrameId;
		dec->FrameNums[currPic] = dec->FrameNum;
		dec->FieldOrderCnt[0][currPic] = dec->TopFieldOrderCnt;
		dec->FieldOrderCnt[1][currPic] = dec->BottomFieldOrderCnt;
		dec->remaining_mbs[currPic] = sps->pic_width_in_mbs * sps->pic_height_in_mbs;
		__atomic_store_n(&dec->next_deblock_addr[currPic], 0, __ATOMIC_RELEASE);
		// forget the slices a previous picture in this slot left to deblock
		// after a missing slice, which nothing will process anymore
		for (uint64_t b = dec->deblock_pending_slices; b; b &= b - 1) {
			if (dec->deblock_pending[__builtin_ctzll(b)].pic == currPic)
				dec->deblock_pending_slices &= ~(b & -b);
		}
		log_dec(dec, "  FrameId: %u\n", dec->FrameIds[currPic]);
	}
	
	// each slice has the initial references state of the previous frame
	dec->currPic_marked = 0;
	dec->short_term_frames = dec->prev_short_term_frames & same_views;
	dec->long_term_frames = dec->prev_long_term_frames & same_views;
	memcpy(dec->LongTermFrameIdx, dec->prev_LongTermFrameIdx, sizeof(dec->LongTermFrameIdx));
	
	// P/B slices
	if (t->slice_type < 2) {
		if (t->slice_type == 1) {
			t->direct_spatial_mv_pred_flag = get_u1(&dec->gb);
			log_dec(dec, "  direct_spatial_mv_pred_flag: %u\n",
				t->direct_spatial_mv_pred_flag);
		}
		int lim = 16 << t->field_pic_flag; // MVC limit is not enforced since MVC detection is too cumbersome
		if (get_u1(&dec->gb)) { // num_ref_idx_active_override_flag
			t->pps.num_ref_idx_active[0] = get_ue16(&dec->gb, lim - 1) + 1;
			if (t->slice_type)
				t->pps.num_ref_idx_active[1] = get_ue16(&dec->gb, lim - 1) + 1;
			log_dec(dec, "  num_ref_idx_active: {override_flag: 1");
		} else {
			t->pps.num_ref_idx_active[0] = min(t->pps.num_ref_idx_active[0], lim);
			t->pps.num_ref_idx_active[1] = min(t->pps.num_ref_idx_active[1], lim);
			log_dec(dec, "  num_ref_idx_active: {override_flag: 0");
		}
		log_dec(dec, t->slice_type ? ", l0: %u, l1: %u}\n" : ", l0: %u}\n",
			t->pps.num_ref_idx_active[0], t->pps.num_ref_idx_active[1]);
		
		if (parse_ref_pic_list_modification(dec, sps, t))
			return print_dec(dec, "  decode_NAL_result: %s\n", EBADMSG);
		parse_pred_weight_table(dec, sps, t);
	}
	
	if (dec->nal_ref_idc)
		parse_dec_ref_pic_marking(dec, sps);
	
	t->cabac_init_idc = 0;
	if (t->pps.entropy_coding_mode_flag && t->slice_type != 2) {
		t->cabac_init_idc = 1 + get_ue16(&dec->gb, 2);
		log_dec(dec, "  cabac_init_idc: %u\n", t->cabac_init_idc - 1);
	}
	t->QP[0] = t->pps.QPprime_Y + get_se16(&dec->gb, -t->pps.QPprime_Y, 51 - t->pps.QPprime_Y); // FIXME QpBdOffset
	log_dec(dec, "  slice_qp_delta: %d\n", t->QP[0] - t->pps.QPprime_Y);
	
	if (t->pps.deblocking_filter_control_present_flag) {
		t->disable_deblocking_filter_idc = get_ue16(&dec->gb, 2);
		log_dec(dec, "  disable_deblocking_filter_idc: %u # %s\n",
			t->disable_deblocking_filter_idc, disable_deblocking_filter_idc_names[t->disable_deblocking_filter_idc]);
		if (t->disable_deblocking_filter_idc != 1) {
			t->FilterOffsetA = get_se16(&dec->gb, -6, 6) * 2;
			t->FilterOffsetB = get_se16(&dec->gb, -6, 6) * 2;
			log_dec(dec, "  slice_alpha_c0_offset: %d\n"
				"  slice_beta_offset: %d\n", t->FilterOffsetA, t->FilterOffsetB);
		}
	} else {
		t->disable_deblocking_filter_idc = 0;
		t->FilterOffsetA = 0;
		t->FilterOffsetB = 0;
	}
	// every macroblock keeps the offsets of its slice next to its QP, for the
	// deblocking that may run when another slice completes the picture (8.7.2.2)
	t->QP[3] = (t->FilterOffsetA >> 1 & 15) | (t->FilterOffsetB >> 1) * 16;
	
	// add the new frame into the DPB if not done already (C.4.5)
	if (!(dec->to_get_frames & (FrameMask)1 << dec->currPic)) {
		FrameMask short_term_frames = dec->prev_short_term_frames & ~same_views | dec->short_term_frames;
		FrameMask long_term_frames = dec->prev_long_term_frames & ~same_views | dec->long_term_frames;
		FrameMask reference_frames = short_term_frames | long_term_frames;
		assert(mask_popcount(reference_frames & same_views) <= sps->max_num_ref_frames);
		// See the frame_num-gap assert above: the held set (both views' references
		// plus the other view's undrained pictures) can exceed the signaled
		// max_dec_frame_buffering on a legal MVC stream whose combined DPB need
		// outgrows the base-view-derived MFB, so bound only by the physical
		// slots. Inert for conformant streams (which stay <= MFB).
		assert(mask_popcount(reference_frames | dec->to_get_frames & ~dec->output_frames & ~same_views) <= MAX_FRAMES);
		int max_bump = sps->max_num_ref_frames;
		if (!dec->nal_ref_idc) {
			max_bump = 0;
			for (FrameMask o = dec->to_get_frames & ~dec->output_frames & same_views; o; o &= o - 1)
				max_bump += dec->FieldOrderCnt[0][mask_ctz(o)] < dec->TopFieldOrderCnt;
		}
		while (mask_popcount(reference_frames | dec->to_get_frames & ~dec->output_frames) > sps->max_dec_frame_buffering && max_bump--)
			bump_frame(dec, non_base_view, 0);
		dec->to_get_frames |= (FrameMask)1 << dec->currPic;
		if (max_bump < 0) {
			// This immediate-output path bypasses bump_frame, so it must consume a
			// display rank the same way: DispOrder[currPic] otherwise keeps the
			// rank of the slot's previous occupant, which under multithreading can
			// undercut the ranks of frames still queued and reorder the output
			// (single-thread drains the queue promptly, hiding the stale rank).
			// This picture precedes every queued frame still waiting for a rank
			// (max_bump < 0 means all lower-POC waiting frames were just bumped),
			// so the next counter value is its correct rank.
			dec->DispOrder[dec->currPic] = dec->next_dispnum++;
			dec->output_frames |= (FrameMask)1 << dec->currPic;
			queue_push(dec, non_base_view, dec->currPic);
		} else if (mask_popcount(dec->to_get_frames & ~dec->output_frames) > sps->max_num_reorder_frames) {
			bump_frame(dec, non_base_view, 0);
		}
		#ifdef LOGS
			log_dec(dec, "  DecodedPictureBuffer:\n");
			FrameMask reordered_frames = dec->to_get_frames & ~dec->output_frames;
			for (int i = 0; i < mask_bits(short_term_frames | long_term_frames | reordered_frames); i++) {
				log_dec(dec, "  - {id: %u", dec->FrameIds[i]);
				if ((short_term_frames | long_term_frames) & (FrameMask)1 << i)
					log_dec(dec, ~long_term_frames & (FrameMask)1 << i ? ", sref: %u" : ~short_term_frames & (FrameMask)1 << i ? ", lref: %u" : ", nref: %u", short_term_frames & (FrameMask)1 << i ? dec->FrameNums[i] : dec->LongTermFrameIdx[i]);
				if (reordered_frames & (FrameMask)1 << i)
					log_dec(dec, ", poc: %d", minw(dec->FieldOrderCnt[0][i], dec->FieldOrderCnt[1][i]));
				if (dec->ssps.BitDepth_Y)
					log_dec(dec, ", view: %u", (unsigned)(dec->non_base_frames >> i & 1));
				log_dec(dec, "}\n");
			}
		#endif
	}
	
	// prepare the task, whose end the next NAL tells (held until then when
	// single-threaded), and tell the previous slice of this picture where it ends
	initialize_task(dec, sps, t);
	int task_id = t - dec->tasks;
	int prev = dec->held_task;
	if (prev >= 0) {
		uint32_t prev_first = dec->tasks[prev].first_mb_in_slice;
		release_held_task(dec, dec->taskPics[prev] == dec->currPic && t->first_mb_in_slice > prev_first ?
			(int32_t)t->first_mb_in_slice : INT_MAX);
	}
	// A slice whose macroblocks may overlap those of an older one still busy (a
	// lower slice arriving later, or one before it that had no bound) starts after
	// it, as it does single-threaded, where every older slice is done.
	// Multithreaded, every slice starts after the older busy slices of its
	// picture, and the worker decoding them continues with it (chained_tasks).
	TaskMask after = tm_none();
	int j;
	TM_FOREACH(j, dec->busy_tasks) {
		if (dec->taskPics[j] == dec->currPic &&
			(dec->n_threads || __atomic_load_n(&dec->task_bounds[j], __ATOMIC_RELAXED) > (int32_t)t->first_mb_in_slice))
			tm_set(&after, j);
	}
	dec->task_after[task_id] = after;
	tm_clear(&dec->chained_tasks, task_id);
	if (dec->n_threads && tm_any(after))
		tm_set(&dec->chained_tasks, task_id);
	acked_clear(dec, task_id);
	t->mb_bound = BOUND_UNKNOWN;
	__atomic_store_n(&dec->task_bounds[task_id], BOUND_UNKNOWN, __ATOMIC_RELAXED);
	tm_set(&dec->busy_tasks, task_id);
	tm_set(&dec->pending_tasks, task_id);
	dec->task_dependencies[task_id] = refs_to_mask(t);
	// FIXME check against dependencies on non-reference slots
	dec->taskPics[task_id] = dec->currPic;
	dec->task_seq[task_id] = dec->next_task_seq++;
	dec->held_task = task_id;
	if (dec->n_threads) {
		dec->ready_tasks = ready_tasks(dec);
		// A reference that has no writer left can only be completed by
		// concealing it, which no other event may trigger before the workers,
		// taking the oldest pending task first, all wait for this one.
		if (!tm_has(dec->ready_tasks, task_id))
			release_terminal_task_dependencies(dec);
		// a chained slice is taken by the worker of the slice before it
		if (!tm_has(dec->chained_tasks, task_id))
			pthread_cond_signal(&dec->task_ready);
	}
	return print_dec(dec, "  decode_NAL_result: %s\n", 0);
}



/**
 * AUDs are ignored outside logging, so that the behavior is equal whether they
 * are included or not, hence a present AUD cannot cover an otherwise bug.
 */
#ifdef LOGS
	int parse_access_unit_delimiter_log(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg) {
		const char *primary_pic_type_names[8] =
			{"I", "I,P", "I,P,B", "SI", "SI,SP", "I,SI", "I,SI,P,SP", "I,SI,P,SP,B"};
		int primary_pic_type = get_uv(&dec->gb, 3);
		log_dec(dec, "  primary_pic_type: %u # %s\n",
			primary_pic_type, primary_pic_type_names[primary_pic_type]);
		return print_dec(dec, "  decode_NAL_result: %s\n", rbsp_end(&dec->gb, 1) ? 0 : EBADMSG);
	}
#endif



int ADD_VARIANT(parse_nal_unit_header_extension)(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg) {
	unsigned u = get_uv(&dec->gb, 24);
	log_dec(dec, "  svc_extension_flag: %u%s\n", u >> 23, unsup_if(u >> 23));
	int ret = ENOTSUP;
	if (!(u >> 23)) {
		dec->IdrPicFlag = u >> 22 & 1 ^ 1;
		log_dec(dec, "  non_idr_flag: %u\n"
			"  priority_id: %d\n"
			"  view_id: %d\n"
			"  temporal_id: %d\n"
			"  anchor_pic_flag: %u\n"
			"  inter_view_flag: %u\n",
			u >> 22 & 1,
			u >> 16 & 0x3f,
			u >> 6 & 0x3ff,
			u >> 3 & 7,
			u >> 2 & 1,
			u >> 1 & 1);
		if (dec->nal_unit_type == 20)
			return ADD_VARIANT(parse_slice_layer_without_partitioning)(dec, unref_cb, unref_arg);
		ret = 0;
	}
	return print_dec(dec, "  decode_NAL_result: %s\n", rbsp_end(&dec->gb, 0) ? ret : EBADMSG);
}



/**
 * Parses the scaling lists into w4x4 and w8x8 (7.3.2.1 and Table 7-2).
 *
 * Fall-back rules for indices 0, 3, 6 and 7 are applied by keeping the
 * existing list, so they must be initialised with Default scaling lists at
 * the very first call.
 */
static void parse_scaling_lists(Edge264MvcDecoder *dec, i8x16 *w4x4, i8x16 *w8x8, int transform_8x8_mode_flag, int chroma_format_idc)
{
	i8x16 fb4x4 = *w4x4; // fall-back
	i8x16 d4x4 = Default_4x4_Intra; // for useDefaultScalingMatrixFlag
	for (int i = 0; i < 6; i++, w4x4++) {
		log_dec(dec, "  - [");
		if (i == 3) {
			fb4x4 = *w4x4;
			d4x4 = Default_4x4_Inter;
		}
		if (!get_u1(&dec->gb)) { // scaling_list_present_flag
			*w4x4 = fb4x4;
		} else {
			unsigned nextScale = (8 + get_se16(&dec->gb, -128, 127)) & 255;
			log_dec(dec, "%u", nextScale);
			if (nextScale == 0) {
				*w4x4 = fb4x4 = d4x4;
			} else {
				for (unsigned j = 0, lastScale;;) {
					((uint8_t *)w4x4)[((int8_t *)scan_4x4)[j]] = nextScale ?: lastScale;
					if (++j >= 16)
						break;
					if (nextScale != 0) {
						lastScale = nextScale;
						nextScale = (nextScale + get_se16(&dec->gb, -128, 127)) & 255;
						log_dec(dec, ",%u", nextScale);
					}
				}
				fb4x4 = *w4x4;
			}
		}
		log_dec(dec, "]\n");
	}
	
	// For 8x8 scaling lists, we really have no better choice than pointers.
	if (!transform_8x8_mode_flag)
		return;
	for (int i = 0; i < (chroma_format_idc == 3 ? 6 : 2); i++, w8x8 += 4) {
		log_dec(dec, "  - [");
		if (!get_u1(&dec->gb)) {
			if (i >= 2) {
				w8x8[0] = w8x8[-8];
				w8x8[1] = w8x8[-7];
				w8x8[2] = w8x8[-6];
				w8x8[3] = w8x8[-5];
			}
		} else {
			unsigned nextScale = (8 + get_se16(&dec->gb, -128, 127)) & 255;
			log_dec(dec, "%u", nextScale);
			if (nextScale == 0) {
				const i8x16 *d8x8 = (i % 2 == 0) ? Default_8x8_Intra : Default_8x8_Inter;
				w8x8[0] = d8x8[0];
				w8x8[1] = d8x8[1];
				w8x8[2] = d8x8[2];
				w8x8[3] = d8x8[3];
			} else {
				for (unsigned j = 0, lastScale = 0;;) {
					((uint8_t *)w8x8)[((int8_t *)scan_8x8_cabac)[j]] = nextScale ?: lastScale;
					if (++j >= 64)
						break;
					if (nextScale != 0) {
						lastScale = nextScale;
						nextScale = (nextScale + get_se16(&dec->gb, -128, 127)) & 255;
						log_dec(dec, ",%u", nextScale);
					}
				}
			}
		}
		log_dec(dec, "]\n");
	}
}



/**
 * Parses the PPS into a copy of the current SPS, then saves it into one of four
 * PPS slots if a rbsp_trailing_bits pattern follows.
 */
int ADD_VARIANT(parse_pic_parameter_set)(Edge264MvcDecoder *dec,  Edge264MvcUnrefCb unref_cb, void *unref_arg)
{
	static const char * const weighted_pred_names[3] = {"average", "explicit", "implicit"};
	
	// temp storage, committed if entire NAL is correct
	Edge264MvcPicParameterSet pps = {
		.transform_8x8_mode_flag = 0,
		.weightScale4x4_v = {},
		.weightScale8x8_v = {},
	};
	int ret = 0;
	
	// Actual streams never use more than 4 PPSs (I, P, B, b).
	int pic_parameter_set_id = get_ue16(&dec->gb, 255);
	if (pic_parameter_set_id >= 4)
		ret = ENOTSUP;
	get_ue16(&dec->gb, 31); // seq_parameter_set_id
	pps.entropy_coding_mode_flag = get_u1(&dec->gb);
	pps.bottom_field_pic_order_in_frame_present_flag = get_u1(&dec->gb);
	int num_slice_groups = get_ue16(&dec->gb, 7) + 1;
	if (num_slice_groups > 1)
		ret = ENOTSUP;
	log_dec(dec, "  pic_parameter_set_id: %u%s\n"
		"  entropy_coding_mode_flag: %u # %s\n"
		"  bottom_field_pic_order_in_frame_present_flag: %u\n"
		"  num_slice_groups: %u%s\n",
		pic_parameter_set_id, unsup_if(pic_parameter_set_id >= 4),
		pps.entropy_coding_mode_flag, pps.entropy_coding_mode_flag ? "CABAC" : "CAVLC",
		pps.bottom_field_pic_order_in_frame_present_flag,
		num_slice_groups, unsup_if(num_slice_groups > 1));
	// FMO (multiple slice groups) is unsupported, and the slice_group_map syntax
	// that follows here is deliberately not parsed. Bail out now: leaving the bit
	// position mid-map would make the rbsp_end check at the end of this function
	// misfire EBADMSG (a hard decode error, reported as a FAIL) instead of the
	// intended ENOTSUP. Returning unsupported cleanly lets a caller skip the
	// stream rather than treat it as corrupt (matching the SPS's early ENOTSUP
	// exits for other unsupported feature sets).
	if (num_slice_groups > 1)
		return print_dec(dec, "  decode_NAL_result: %s\n", ENOTSUP);

	// (num_ref_idx_active[0] != 0) is used as indicator that the PPS is initialised.
	pps.num_ref_idx_active[0] = get_ue16(&dec->gb, 31) + 1;
	pps.num_ref_idx_active[1] = get_ue16(&dec->gb, 31) + 1;
	pps.weighted_pred_flag = get_u1(&dec->gb);
	pps.weighted_bipred_idc = get_uv(&dec->gb, 2);
	pps.QPprime_Y = get_se16(&dec->gb, -26, 25) + 26; // FIXME QpBdOffset
	get_se16(&dec->gb, -26, 25); // pic_init_qs
	pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset = get_se16(&dec->gb, -12, 12);
	pps.deblocking_filter_control_present_flag = get_u1(&dec->gb);
	pps.constrained_intra_pred_flag = get_u1(&dec->gb);
	pps.redundant_pic_cnt_present_flag = get_u1(&dec->gb);
	if (pps.constrained_intra_pred_flag)
		ret = ENOTSUP;
	log_dec(dec, "  num_ref_idx_default_active: {l0: %u, l1: %u}\n"
		"  weighted_pred_flag: %u # %s\n"
		"  weighted_bipred_idc: %u # %s\n"
		"  pic_init_qp: %u\n"
		"  chroma_qp_index_offset: %d\n"
		"  deblocking_filter_control_present_flag: %u\n"
		"  constrained_intra_pred_flag: %u%s\n"
		"  redundant_pic_cnt_present_flag: %u\n",
		pps.num_ref_idx_active[0], pps.num_ref_idx_active[1],
		pps.weighted_pred_flag, weighted_pred_names[pps.weighted_pred_flag],
		pps.weighted_bipred_idc, weighted_pred_names[pps.weighted_bipred_idc],
		pps.QPprime_Y,
		pps.chroma_qp_index_offset,
		pps.deblocking_filter_control_present_flag,
		pps.constrained_intra_pred_flag, unsup_if(pps.constrained_intra_pred_flag),
		pps.redundant_pic_cnt_present_flag);
	
	if (!rbsp_end(&dec->gb, 1)) {
		pps.transform_8x8_mode_flag = get_u1(&dec->gb);
		log_dec(dec, "  transform_8x8_mode_flag: %u\n",
			pps.transform_8x8_mode_flag);
		pps.pic_scaling_matrix_present_flag = get_u1(&dec->gb);
		if (pps.pic_scaling_matrix_present_flag) {
			log_dec(dec, "  pic_scaling_matrix:\n");
			parse_scaling_lists(dec, pps.weightScale4x4_v, pps.weightScale8x8_v, pps.transform_8x8_mode_flag, dec->sps.chroma_format_idc);
		}
		pps.second_chroma_qp_index_offset = get_se16(&dec->gb, -12, 12);
		log_dec(dec, "  second_chroma_qp_index_offset: %d\n",
			pps.second_chroma_qp_index_offset);
	}
	
	// decoding errors have more precedence over unsupported features (in case errors enabled them)
	if (!rbsp_end(&dec->gb, 1))
		ret = EBADMSG;
	if (ret == 0)
		dec->PPS[pic_parameter_set_id] = pps;
	return print_dec(dec, "  decode_NAL_result: %s\n", ret);
}



/**
 * For the sake of implementation simplicity, the responsibility for timing
 * management is left to demuxing libraries, hence any HRD data is ignored.
 */
static void parse_hrd_parameters(Edge264MvcDecoder *dec, Edge264MvcSeqParameterSet *sps, int8_t *cpb_cnt, const char *indent) {
	*cpb_cnt = get_ue16(&dec->gb, 31) + 1;
	int bit_rate_scale = get_uv(&dec->gb, 4);
	int cpb_size_scale = get_uv(&dec->gb, 4);
	log_dec(dec, "%scpbs:\n", indent);
	for (int i = 0; i < *cpb_cnt; i++) {
		unsigned long long bit_rate_value = get_ue32(&dec->gb, 4294967294) + 1;
		unsigned long long cpb_size_value = get_ue32(&dec->gb, 4294967294) + 1;
		int cbr_flag = get_u1(&dec->gb);
		log_dec(dec, "%s- {bit_rate: %llu, size: %llu, cbr_flag: %u}\n",
			indent, bit_rate_value << (6 + bit_rate_scale), cpb_size_value << (4 + cpb_size_scale), cbr_flag);
	}
	unsigned delays = get_uv(&dec->gb, 20);
	sps->initial_cpb_removal_delay_length = (delays >> 15) + 1;
	sps->cpb_removal_delay_length = ((delays >> 10) & 0x1f) + 1;
	sps->dpb_output_delay_length = ((delays >> 5) & 0x1f) + 1;
	sps->time_offset_length = delays & 0x1f;
	log_dec(dec, "%sinitial_cpb_removal_delay_length: %u\n"
		"%scpb_removal_delay_length: %u\n"
		"%sdpb_output_delay_length: %u\n"
		"%stime_offset_length: %u\n",
		indent, sps->initial_cpb_removal_delay_length,
		indent, sps->cpb_removal_delay_length,
		indent, sps->dpb_output_delay_length,
		indent, sps->time_offset_length);
}



/**
 * To avoid cluttering the memory layout with unused data, VUI parameters are
 * mostly ignored until explicitly asked in the future.
 */
static void parse_vui_parameters(Edge264MvcDecoder *dec, Edge264MvcSeqParameterSet *sps)
{
	static const unsigned ratio2sar[32] = {0, 0x00010001, 0x000c000b,
		0x000a000b, 0x0010000b, 0x00280021, 0x0018000b, 0x0014000b, 0x0020000b,
		0x00500021, 0x0012000b, 0x000f000b, 0x00400021, 0x00a00063, 0x00040003,
		0x00030002, 0x00020001};
	static const char * const video_format_names[8] = {"Component", "PAL",
		"NTSC", "SECAM", "MAC", [5 ... 7] = "Unknown"};
	static const char * const colour_primaries_names[] = {
		[0 ... 23] = "Unknown",
		[1] = "ITU-R BT.709-5",
		[4] = "ITU-R BT.470-6 System M",
		[5] = "ITU-R BT.470-6 System B, G",
		[6 ... 7] = "ITU-R BT.601-6 525",
		[8] = "Generic film",
		[9] = "ITU-R BT.2020-2",
		[10] = "CIE 1931 XYZ",
		[11] = "Society of Motion Picture and Television Engineers RP 431-2",
		[12] = "Society of Motion Picture and Television Engineers EG 432-1",
		[22] = "EBU Tech. 3213-E",
	};
	static const char * const transfer_characteristics_names[] = {
		[0 ... 18] = "Unknown",
		[1] = "ITU-R BT.709-5",
		[4] = "ITU-R BT.470-6 System M",
		[5] = "ITU-R BT.470-6 System B, G",
		[6] = "ITU-R BT.601-6 525 or 625",
		[7] = "Society of Motion Picture and Television Engineers 240M",
		[8] = "Linear transfer characteristics",
		[9] = "Logarithmic transfer characteristic (100:1 range)",
		[10] = "Logarithmic transfer characteristic (100 * Sqrt( 10 ) : 1 range)",
		[11] = "IEC 61966-2-4",
		[12] = "ITU-R BT.1361-0",
		[13] = "IEC 61966-2-1 sRGB or sYCC",
		[14] = "ITU-R BT.2020-2 (10 bit system)",
		[15] = "ITU-R BT.2020-2 (12 bit system)",
		[16] = "Society of Motion Picture and Television Engineers ST 2084",
		[17] = "Society of Motion Picture and Television Engineers ST 428-1",
	};
	static const char * const matrix_coefficients_names[] = {
		[0 ... 12] = "Unknown",
		[1] = "Kr = 0.2126; Kb = 0.0722",
		[4] = "Kr = 0.30; Kb = 0.11",
		[5 ... 6] = "Kr = 0.299; Kb = 0.114",
		[7] = "Kr = 0.212; Kb = 0.087",
		[8] = "YCgCo",
		[9] = "Kr = 0.2627; Kb = 0.0593 (non-constant luminance)",
		[10] = "Kr = 0.2627; Kb = 0.0593 (constant luminance)",
		[11] = "Y'D'zD'x",
	};
	
	if (get_u1(&dec->gb)) {
		int aspect_ratio_idc = get_uv(&dec->gb, 8);
		unsigned sar = (aspect_ratio_idc == 255) ? get_uv(&dec->gb, 32) : ratio2sar[aspect_ratio_idc & 31];
		int sar_width = sar >> 16;
		int sar_height = sar & 0xffff;
		log_dec(dec, "    aspect_ratio: {idc: %u, width: %u, height: %u}\n",
			aspect_ratio_idc, sar_width, sar_height);
	}
	int overscan_appropriate_flag = get_u1(&dec->gb) ? get_u1(&dec->gb) : -1;
	log_dec(dec, "    overscan_appropriate_flag: %d\n",
		overscan_appropriate_flag);
	if (get_u1(&dec->gb)) {
		int video_format = get_uv(&dec->gb, 3);
		int video_full_range_flag = get_u1(&dec->gb);
		log_dec(dec, "    video_format: %u # %s\n"
			"    video_full_range_flag: %u\n",
			video_format, video_format_names[video_format],
			video_full_range_flag);
		if (get_u1(&dec->gb)) {
			unsigned desc = get_uv(&dec->gb, 24);
			int colour_primaries = desc >> 16;
			int transfer_characteristics = (desc >> 8) & 0xff;
			int matrix_coefficients = desc & 0xff;
			log_dec(dec, "    colour_primaries: %u # %s\n"
				"    transfer_characteristics: %u # %s\n"
				"    matrix_coefficients: %u # %s\n",
				colour_primaries, colour_primaries_names[min(colour_primaries, 23)],
				transfer_characteristics, transfer_characteristics_names[min(transfer_characteristics, 18)],
				matrix_coefficients, matrix_coefficients_names[min(matrix_coefficients, 12)]);
		}
	}
	if (get_u1(&dec->gb)) {
		int chroma_sample_loc_type_top_field = get_ue16(&dec->gb, 5);
		int chroma_sample_loc_type_bottom_field = get_ue16(&dec->gb, 5);
		log_dec(dec, "    chroma_sample_loc: {top: %u, bottom: %u}\n",
			chroma_sample_loc_type_top_field, chroma_sample_loc_type_bottom_field);
	}
	if (get_u1(&dec->gb)) {
		sps->num_units_in_tick = maxu(get_uv(&dec->gb, 32), 1);
		sps->time_scale = maxu(get_uv(&dec->gb, 32), 1);
		int fixed_frame_rate_flag = get_u1(&dec->gb);
		log_dec(dec, "    num_units_in_tick: %u\n"
			"    time_scale: %u\n"
			"    fixed_frame_rate_flag: %u\n",
			sps->num_units_in_tick,
			sps->time_scale,
			fixed_frame_rate_flag);
	}
	int nal_hrd_parameters_present_flag = get_u1(&dec->gb);
	if (nal_hrd_parameters_present_flag) {
		log_dec(dec, "    nal_hrd_parameters:\n");
		parse_hrd_parameters(dec, sps, &sps->nal_hrd_cpb_cnt, "      ");
	}
	int vcl_hrd_parameters_present_flag = get_u1(&dec->gb);
	if (vcl_hrd_parameters_present_flag) {
		log_dec(dec, "    vcl_hrd_parameters:\n");
		parse_hrd_parameters(dec, sps, &sps->vcl_hrd_cpb_cnt, "      ");
	}
	if (nal_hrd_parameters_present_flag | vcl_hrd_parameters_present_flag) {
		int low_delay_hrd_flag = get_u1(&dec->gb);
		log_dec(dec, "    low_delay_hrd_flag: %u\n",
			low_delay_hrd_flag);
	}
	sps->pic_struct_present_flag = get_u1(&dec->gb);
	log_dec(dec, "    pic_struct_present_flag: %u\n",
		sps->pic_struct_present_flag);
	if (get_u1(&dec->gb)) {
		int motion_vectors_over_pic_boundaries_flag = get_u1(&dec->gb);
		log_dec(dec, "    motion_vectors_over_pic_boundaries_flag: %u\n",
			motion_vectors_over_pic_boundaries_flag);
		int RawMbBits = 256 * sps->BitDepth_Y + (64 << sps->chroma_format_idc & ~64) * sps->BitDepth_C;
		int max_bytes_per_pic_denom = get_ue16(&dec->gb, 16);
		if (max_bytes_per_pic_denom)
			log_dec(dec, "    max_bytes_per_pic: %u\n", (sps->pic_width_in_mbs * sps->pic_height_in_mbs * RawMbBits) / (8 * max_bytes_per_pic_denom));
		int max_bits_per_mb_denom = get_ue16(&dec->gb, 16);
		if (max_bits_per_mb_denom)
			log_dec(dec, "    max_bits_per_mb: %u\n", (128 + RawMbBits) / max_bits_per_mb_denom);
		int log2_max_mv_length_horizontal = get_ue16(&dec->gb, 15);
		int log2_max_mv_length_vertical = get_ue16(&dec->gb, 15);
		// we don't enforce MaxDpbFrames here since violating the level is harmless
		int max_num_reorder_frames = get_ue16(&dec->gb, 16);
		sps->max_dec_frame_buffering = max(get_ue16(&dec->gb, 16), sps->max_num_ref_frames);
		sps->max_num_reorder_frames = min(max_num_reorder_frames, sps->max_dec_frame_buffering);
		log_dec(dec, "    log2_max_mv_length_horizontal: %u\n"
			"    log2_max_mv_length_vertical: %u\n"
			"    max_num_reorder_frames: %u\n"
			"    max_dec_frame_buffering: %u\n", // in units of view components (Annex C p.311)
			log2_max_mv_length_horizontal,
			log2_max_mv_length_vertical,
			sps->max_num_reorder_frames,
			sps->max_dec_frame_buffering);
	} else {
		log_dec(dec, "    max_num_reorder_frames: %u # inferred\n"
			"    max_dec_frame_buffering: %u # inferred\n",
			sps->max_num_reorder_frames,
			sps->max_dec_frame_buffering);
	}
}



/**
 * Parses the MVC VUI parameters extension, only advancing the stream pointer
 * for error detection, and ignoring it until requested in the future.
 */
static void parse_mvc_vui_parameters_extension(Edge264MvcDecoder *dec, Edge264MvcSeqParameterSet *sps)
{
	log_dec(dec, "  vui_mvc_operation_points:\n");
	// stop at the end of a damaged NAL, past which each count reads as its largest value
	for (int i = get_ue16(&dec->gb, 1023); i-- >= 0 && bits_left(&dec->gb) >= 0;) {
		int temporal_id = get_uv(&dec->gb, 3);
		log_dec(dec, "  - temporal_id: %u\n"
			"    target_views: [", temporal_id);
		for (int j = get_ue16(&dec->gb, 1023); j >= 0 && bits_left(&dec->gb) >= 0; j--) {
			int view_id = get_ue16(&dec->gb, 1023);
			log_dec(dec, j ? "%u," : "%u]\n", view_id);
		}
		if (get_u1(&dec->gb)) {
			unsigned num_units_in_tick = get_uv(&dec->gb, 32);
			unsigned time_scale = get_uv(&dec->gb, 32);
			int fixed_frame_rate_flag = get_u1(&dec->gb);
			log_dec(dec, "    num_units_in_tick: %u\n"
				"    time_scale: %u\n"
				"    fixed_frame_rate_flag: %u\n",
				num_units_in_tick, time_scale, fixed_frame_rate_flag);
		}
		int vui_mvc_nal_hrd_parameters_present_flag = get_u1(&dec->gb);
		if (vui_mvc_nal_hrd_parameters_present_flag) {
			log_dec(dec, "    vui_mvc_nal_hrd_parameters:\n");
			parse_hrd_parameters(dec, sps, &sps->nal_hrd_cpb_cnt, "      ");
		}
		int vui_mvc_vcl_hrd_parameters_present_flag = get_u1(&dec->gb);
		if (vui_mvc_vcl_hrd_parameters_present_flag) {
			log_dec(dec, "    vui_mvc_vcl_hrd_parameters:\n");
			parse_hrd_parameters(dec, sps, &sps->vcl_hrd_cpb_cnt, "      ");
		}
		if (vui_mvc_nal_hrd_parameters_present_flag | vui_mvc_vcl_hrd_parameters_present_flag) {
			int low_delay_hrd_flag = get_u1(&dec->gb);
			log_dec(dec, "    low_delay_hrd_flag: %u\n", low_delay_hrd_flag);
		}
		int pic_struct_present_flag = get_u1(&dec->gb);
		log_dec(dec, "    pic_struct_present_flag: %u\n", pic_struct_present_flag);
	}
}



/**
 * Parses (and mostly ignores) the SPS extension for MVC.
 */
static int parse_seq_parameter_set_mvc_extension(Edge264MvcDecoder *dec, int profile_idc)
{
	// returning unsupported asap is more efficient than keeping tedious code afterwards
	int num_views = get_ue16(&dec->gb, 1023) + 1;
	for (int i = 0; i < num_views; i++) {
		int view_id = get_ue16(&dec->gb, 1023);
		log_dec(dec, i ? ",%u" : "  view_ids: [%u", view_id);
	}
	log_dec(dec, "]%s\n", unsup_if(num_views != 2));
	if (num_views != 2)
		return ENOTSUP;
	
	// inter-view refs are ignored since we always add them anyway
	int num_anchor_refs_l0 = get_ue16(&dec->gb, 1);
	if (num_anchor_refs_l0)
		get_ue16(&dec->gb, 1023);
	int num_anchor_refs_l1 = get_ue16(&dec->gb, 1);
	if (num_anchor_refs_l1)
		get_ue16(&dec->gb, 1023);
	int num_non_anchor_refs_l0 = get_ue16(&dec->gb, 1);
	if (num_non_anchor_refs_l0)
		get_ue16(&dec->gb, 1023);
	int num_non_anchor_refs_l1 = get_ue16(&dec->gb, 1);
	if (num_non_anchor_refs_l1)
		get_ue16(&dec->gb, 1023);
	log_dec(dec, "  num_anchor_refs: {l0: %u, l1: %u}\n"
		"  num_non_anchor_refs: {l0: %u, l1: %u}\n"
		"  level_values_signalled:\n",
		num_anchor_refs_l0, num_anchor_refs_l1,
		num_non_anchor_refs_l0, num_non_anchor_refs_l1);
	
	// level values and operation points are similarly ignored; past the end of a
	// damaged NAL each count reads as its largest value, so stop there rather than
	// loop up to 64 x 1024 x 1024 times on clamped values
	for (int i = get_ue16(&dec->gb, 63); i >= 0 && bits_left(&dec->gb) >= 0; i--) {
		int level_idc = get_uv(&dec->gb, 8);
		log_dec(dec, "  - idc: %.1f\n"
			"    operation_points: [", (float)level_idc / 10);
		for (int j = get_ue16(&dec->gb, 1023); j >= 0 && bits_left(&dec->gb) >= 0; j--) {
			int applicable_op_temporal_id = get_uv(&dec->gb, 3);
			log_dec(dec, "{temporal_id: %u, target_views: [", applicable_op_temporal_id);
			for (int k = get_ue16(&dec->gb, 1023); k >= 0 && bits_left(&dec->gb) >= 0; k--) {
				int applicable_op_target_view_id = get_ue16(&dec->gb, 1023);
				log_dec(dec, k ? "%u," : "%u], num_views: ", applicable_op_target_view_id);
			}
			int applicable_op_num_views = get_ue16(&dec->gb, 1023) + 1;
			log_dec(dec, j ? "%u}," : "%u}]\n", applicable_op_num_views);
		}
	}
	return profile_idc == 134 ? ENOTSUP : 0; // MFC is unsupported until streams actually use it
}



/**
 * Parses the SPS into an Edge264MvcSeqParameterSet, then saves it if a
 * rbsp_trailing_bits pattern follows.
 */
int ADD_VARIANT(parse_seq_parameter_set)(Edge264MvcDecoder *dec, Edge264MvcUnrefCb unref_cb, void *unref_arg)
{
	static const char * const profile_idc_names[256] = {
		[0 ... 255] = "Unknown",
		[44] = "CAVLC 4:4:4 Intra",
		[66] = "Baseline",
		[77] = "Main",
		[83] = "Scalable Baseline",
		[86] = "Scalable High",
		[88] = "Extended",
		[100] = "High",
		[110] = "High 10",
		[118] = "Multiview High",
		[122] = "High 4:2:2",
		[128] = "Stereo High",
		[134] = "MFC High",
		[135] = "MFC Depth High",
		[138] = "Multiview Depth High",
		[139] = "Enhanced Multiview Depth High",
		[244] = "High 4:4:4 Predictive",
	};
	static const char * const chroma_format_idc_names[4] = {"4:0:0", "4:2:0", "4:2:2", "4:4:4"};
	static const uint32_t MaxDpbMbs[64] = {
		396, 396, 396, 396, 396, 396, 396, 396, 396, 396, 396, // level 1
		900, // levels 1b and 1.1
		2376, 2376, 2376, 2376, 2376, 2376, 2376, 2376, 2376, // levels 1.2, 1.3 and 2
		4752, // level 2.1
		8100, 8100, 8100, 8100, 8100, 8100, 8100, 8100, 8100, // levels 2.2 and 3
		18000, // level 3.1
		20480, // level 3.2
		32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768, // levels 4 and 4.1
		34816, // level 4.2
		110400, 110400, 110400, 110400, 110400, 110400, 110400, 110400, // level 5
		184320, 184320, // levels 5.1 and 5.2
		696320, 696320, 696320, 696320, 696320, 696320, 696320, 696320, 696320, 696320, // levels 6, 6.1 and 6.2
		UINT_MAX // no limit beyond
	};
	
	// temp storage, committed if entire NAL is correct
	Edge264MvcSeqParameterSet sps = {
		.chroma_format_idc = 1,
		.ChromaArrayType = 1,
		.BitDepth_Y = 8,
		.BitDepth_C = 8,
		.log2_max_pic_order_cnt_lsb = 16,
		.initial_cpb_removal_delay_length = 24,
		.cpb_removal_delay_length = 24,
		.dpb_output_delay_length = 24,
		.time_offset_length = 24,
		.weightScale4x4_v = {[0 ... 5] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16}},
		.weightScale8x8_v = {[0 ... 23] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16}},
	};
	int ret = 0;
	
	// Profiles are only useful to initialize max_num_reorder_frames/max_dec_frame_buffering.
	int profile_idc = get_uv(&dec->gb, 8);
	unsigned constraint_set_flags = get_uv(&dec->gb, 8);
	int level_idc = get_uv(&dec->gb, 8);
	get_ue16(&dec->gb, 31); // seq_parameter_set_id is ignored until useful cases arise
	log_dec(dec, "  profile_idc: %u # %s%s\n"
		"  constraint_set_flags: [%u,%u,%u,%u,%u,%u]\n"
		"  level_idc: %.1f\n",
		profile_idc, profile_idc_names[profile_idc], unsup_if(dec->nal_unit_type == 15 && (profile_idc != 118 && profile_idc != 128)),
		constraint_set_flags >> 7, (constraint_set_flags >> 6) & 1, (constraint_set_flags >> 5) & 1, (constraint_set_flags >> 4) & 1, (constraint_set_flags >> 3) & 1, (constraint_set_flags >> 2) & 1,
		(float)level_idc / 10);
	
	if (profile_idc != 66 && profile_idc != 77 && profile_idc != 88) {
		sps.ChromaArrayType = sps.chroma_format_idc = get_ue16(&dec->gb, 3);
		log_dec(dec, "  chroma_format_idc: %u # %s%s\n",
			sps.chroma_format_idc, chroma_format_idc_names[sps.chroma_format_idc], unsup_if(sps.chroma_format_idc != 1));
		if (sps.chroma_format_idc != 1) {
			ret = ENOTSUP;
			if (sps.chroma_format_idc == 3) {
				int separate_colour_plane_flag = get_u1(&dec->gb);
				sps.ChromaArrayType = (separate_colour_plane_flag * 3) ^ 3;
				log_dec(dec, "  separate_colour_plane_flag: %u\n",
					separate_colour_plane_flag);
			}
		}
		sps.BitDepth_Y = 8 + get_ue16(&dec->gb, 6);
		if (sps.BitDepth_Y > 8)
			ret = ENOTSUP;
		sps.BitDepth_C = 8 + get_ue16(&dec->gb, 6);
		if (sps.BitDepth_C > 8)
			ret = ENOTSUP;
		sps.qpprime_y_zero_transform_bypass_flag = get_u1(&dec->gb);
		if (sps.qpprime_y_zero_transform_bypass_flag)
			ret = ENOTSUP;
		log_dec(dec, "  bit_depth: {luma: %u, chroma: %u}%s\n"
			"  qpprime_y_zero_transform_bypass_flag: %u%s\n",
			sps.BitDepth_Y, sps.BitDepth_C, unsup_if(sps.BitDepth_Y + sps.BitDepth_Y != 16),
			sps.qpprime_y_zero_transform_bypass_flag, unsup_if(sps.qpprime_y_zero_transform_bypass_flag));
		sps.seq_scaling_matrix_present_flag = get_u1(&dec->gb);
		if (sps.seq_scaling_matrix_present_flag) {
			sps.weightScale4x4_v[0] = Default_4x4_Intra;
			sps.weightScale4x4_v[3] = Default_4x4_Inter;
			for (int i = 0; i < 4; i++) {
				sps.weightScale8x8_v[i] = Default_8x8_Intra[i]; // scaling list 6
				sps.weightScale8x8_v[4 + i] = Default_8x8_Inter[i]; // scaling list 7
			}
			log_dec(dec, "  seq_scaling_matrix:\n");
			parse_scaling_lists(dec, sps.weightScale4x4_v, sps.weightScale8x8_v, 1, sps.chroma_format_idc);
		}
	} else {
		log_dec(dec, "  chroma_format_idc: 1 # 4:2:0 # inferred\n"
			"  bit_depth: {luma: 8, chroma: 8} # inferred\n");
	}
	
	sps.log2_max_frame_num = get_ue16(&dec->gb, 12) + 4;
	sps.pic_order_cnt_type = get_ue16(&dec->gb, 2);
	log_dec(dec, "  log2_max_frame_num: %u\n"
		"  pic_order_cnt_type: %u\n",
		sps.log2_max_frame_num,
		sps.pic_order_cnt_type);
	
	if (sps.pic_order_cnt_type == 0) {
		sps.log2_max_pic_order_cnt_lsb = get_ue16(&dec->gb, 12) + 4;
		log_dec(dec, "  log2_max_pic_order_cnt_lsb: %u\n",
			sps.log2_max_pic_order_cnt_lsb);
	
	// clearly one of the spec's useless bits (and a waste of time to implement)
	} else if (sps.pic_order_cnt_type == 1) {
		sps.delta_pic_order_always_zero_flag = get_u1(&dec->gb);
		sps.offset_for_non_ref_pic = get_se32(&dec->gb, -32768, 32767); // tighter than spec thanks to condition on DiffPicOrderCnt
		sps.offset_for_top_to_bottom_field = get_se32(&dec->gb, -32768, 32767);
		sps.num_ref_frames_in_pic_order_cnt_cycle = get_ue16(&dec->gb, 255);
		log_dec(dec, "  delta_pic_order_always_zero_flag: %u\n"
			"  offset_for_non_ref_pic: %d\n"
			"  offset_for_top_to_bottom_field: %d\n"
			"  offsets_for_ref_frames: [",
			sps.delta_pic_order_always_zero_flag,
			sps.offset_for_non_ref_pic,
			sps.offset_for_top_to_bottom_field);
		for (int i = 0, delta = 0; i < sps.num_ref_frames_in_pic_order_cnt_cycle; i++) {
			int offset_for_ref_frame = get_se32(&dec->gb, -65535, 65535);
			log_dec(dec, (i < sps.num_ref_frames_in_pic_order_cnt_cycle - 1) ? "%d," : "%d", offset_for_ref_frame);
			sps.PicOrderCntDeltas[i] = delta += offset_for_ref_frame;
		}
		log_dec(dec, "]\n");
	}
	
	// Max width is imposed by some int16 storage, wait for actual needs to push it.
	int max_num_ref_frames = get_ue16(&dec->gb, 16);
	int gaps_in_frame_num_value_allowed_flag = get_u1(&dec->gb);
	sps.pic_width_in_mbs = get_ue16(&dec->gb, 1022) + 1;
	// frame_mbs_only_flag is not parsed until the next line, so the historical
	// bound "527 << sps.frame_mbs_only_flag" always evaluated with the zero-
	// initialized flag (== 527), wrongly clamping tall *progressive* streams
	// (flag == 1, where pic_height_in_map_units maps 1:1 to MB rows) at 528 rows
	// / 8448px and then mis-sizing the frame buffers. Use the looser progressive
	// bound 527 << 1; interlaced (flag == 0) is rejected as ENOTSUP just below.
	int pic_height_in_map_units = get_ue16(&dec->gb, 527 << 1) + 1;
	sps.frame_mbs_only_flag = get_u1(&dec->gb);
	if (!sps.frame_mbs_only_flag)
		ret = ENOTSUP;
	sps.pic_height_in_mbs = pic_height_in_map_units << 1 >> sps.frame_mbs_only_flag;
	int mvc = (dec->nal_unit_type == 15);
	// contrary to H.10.2.1-f we force MaxDpbFrames a multiple of 2 for MVC
	int MaxDpbFrames = min((MaxDpbMbs[min(level_idc, 63)] / (unsigned)(sps.pic_width_in_mbs * sps.pic_height_in_mbs)) << mvc, 16);
	// A reference picture always occupies one DPB slot, so the reference set is
	// >= 1 whenever any picture is kept for reference. Some encoders (x264 for
	// single-frame / all-intra clips) signal max_num_ref_frames == 0 yet still
	// emit a nal_ref_idc>0 IDR, which 8.2.5.1 marks short-term for reference;
	// that lone self-reference would then exceed a zero limit and abort the
	// C.4.5 invariant asserts in parse_slice_layer_without_partitioning (and,
	// via the derived max_dec_frame_buffering below, the fullness assert too).
	// Floor at 1 so a reference picture fits, matching ffmpeg. Conformant >= 1
	// values are unchanged, and a stream that truly keeps no reference frame
	// (every slice nal_ref_idc == 0) never marks one, so the floor is inert.
	// Bound the reference set by the *physical* DPB capacity (16 slots, 8 per view
	// for MVC), NOT by the level-derived MaxDpbFrames. A stream whose frame size
	// exceeds its signaled level (non-conformant, but common in real-world encodes
	// and remuxes) yields a MaxDpbFrames smaller than its own signaled
	// max_num_ref_frames; clamping the reference count down to that made the
	// sliding-window marking (8.2.5.3) retire pictures the slices still reference,
	// silently corrupting inter prediction - and, under multithreading, racing on
	// the prematurely reused DPB slot (nondeterministic output). ffmpeg honours the
	// signaled value regardless of level, so we do too. Conformant streams are
	// unchanged: there MaxDpbFrames already covers max_num_ref_frames, so the old
	// min(., MaxDpbFrames >> mvc) and the new min(., 16 >> mvc) both return the
	// signaled value (which parsing already bounded to <= 16).
	sps.max_num_ref_frames = max(min(max_num_ref_frames, 16 >> mvc), 1);
	// A stream whose resolution exceeds its signaled level makes MaxDpbMbs/frame
	// == 0, so the inferred MaxDpbFrames (and the max_dec_frame_buffering derived
	// from it below) would be 0 even though the floored reference set needs >= 1
	// slot - the very first reference picture then trips the C.4.5 fullness assert
	// during slice-header parsing. Floor the derived DPB size at the reference
	// count so a kept picture always fits. Inert for conformant streams (there
	// MaxDpbFrames already covers the references) and matches ffmpeg, which
	// decodes such over-level clips.
	MaxDpbFrames = max(MaxDpbFrames, sps.max_num_ref_frames << mvc);
	if (movemask(set8(profile_idc) == ((u8x16){44, 86, 100, 110, 122, 244})) &&
		(constraint_set_flags & 1 << 4)) {
		sps.max_num_reorder_frames = 0;
		sps.max_dec_frame_buffering = sps.max_num_ref_frames << mvc;
	} else {
		sps.max_num_reorder_frames = sps.max_dec_frame_buffering = MaxDpbFrames;
	}
	log_dec(dec, "  max_num_ref_frames: %u\n"
		"  gaps_in_frame_num_value_allowed_flag: %u\n"
		"  pic_size_in_mbs: {width: %u, height: %u}\n"
		"  frame_mbs_only_flag: %u%s\n",
		sps.max_num_ref_frames,
		gaps_in_frame_num_value_allowed_flag,
		sps.pic_width_in_mbs,
		sps.pic_height_in_mbs,
		sps.frame_mbs_only_flag, unsup_if(!sps.frame_mbs_only_flag));
	if (sps.frame_mbs_only_flag == 0) {
		sps.mb_adaptive_frame_field_flag = get_u1(&dec->gb);
		log_dec(dec, "  mb_adaptive_frame_field_flag: %u\n",
			sps.mb_adaptive_frame_field_flag);
	}
	sps.direct_8x8_inference_flag = get_u1(&dec->gb);
	log_dec(dec, "  direct_8x8_inference_flag: %u\n",
		sps.direct_8x8_inference_flag);
	
	// frame_cropping_flag
	if (get_u1(&dec->gb)) {
		unsigned shiftX = ((sps.ChromaArrayType == 1) | (sps.ChromaArrayType == 2));
		unsigned shiftY = (sps.ChromaArrayType == 1) + 1 - sps.frame_mbs_only_flag;
		int limX = (sps.pic_width_in_mbs << 4 >> shiftX) - 1;
		int limY = (sps.pic_height_in_mbs << 4 >> shiftY) - 1;
		sps.frame_crop_offsets[3] = get_ue16(&dec->gb, limX) << shiftX;
		sps.frame_crop_offsets[1] = get_ue16(&dec->gb, limX - (sps.frame_crop_offsets[3] >> shiftX)) << shiftX;
		sps.frame_crop_offsets[0] = get_ue16(&dec->gb, limY) << shiftY;
		sps.frame_crop_offsets[2] = get_ue16(&dec->gb, limY - (sps.frame_crop_offsets[0] >> shiftY)) << shiftY;
		log_dec(dec, "  frame_crop_offsets: {left: %u, right: %u, top: %u, bottom: %u}\n",
			sps.frame_crop_offsets[3], sps.frame_crop_offsets[1], sps.frame_crop_offsets[0], sps.frame_crop_offsets[2]);
	}
	
	// Frames larger than max_frame_pixels are not decoded, which also bounds the
	// memory a crafted SPS takes. The limit applies to the frame after cropping,
	// as the caller receives it, and the coded frame may exceed it only by
	// rounding each dimension up to whole macroblocks. By default it is the
	// largest coded frame any level allows (Table A-1, MaxFS of level 6.2).
	int frame_mbs = sps.pic_width_in_mbs * sps.pic_height_in_mbs;
	if (dec->max_frame_pixels == 0) {
		if (frame_mbs > 139264)
			ret = ENOTSUP;
	} else if ((int64_t)((sps.pic_width_in_mbs << 4) - sps.frame_crop_offsets[3] - sps.frame_crop_offsets[1]) *
		((sps.pic_height_in_mbs << 4) - sps.frame_crop_offsets[0] - sps.frame_crop_offsets[2]) > dec->max_frame_pixels ||
		frame_mbs > dec->max_frame_pixels / 256 + sps.pic_width_in_mbs + sps.pic_height_in_mbs + 1) {
		ret = ENOTSUP;
	}
	
	int vui_present = get_u1(&dec->gb);
	int inferred_max_num_reorder_frames = sps.max_num_reorder_frames;
	int inferred_max_dec_frame_buffering = sps.max_dec_frame_buffering;
	if (vui_present) {
		log_dec(dec, "  vui_parameters:\n");
		parse_vui_parameters(dec, &sps);
	} else {
		log_dec(dec, "  max_num_reorder_frames: %u # inferred\n"
			"  max_dec_frame_buffering: %u # inferred\n",
			sps.max_num_reorder_frames,
			sps.max_dec_frame_buffering);
	}
	
	// additional stuff for subset_seq_parameter_set
	if (dec->nal_unit_type == 15) {
		// The base VUI's max_dec_frame_buffering describes the base view alone;
		// the MVC DPB holds both views, so floor it at the MVC-doubled level
		// limit (MaxDpbFrames was already doubled via << mvc). Some encoders
		// signal only the base-view value here, which undersizes the joint DPB
		// and trips the C.4.5 fullness checks during decode.
		sps.max_dec_frame_buffering = max(sps.max_dec_frame_buffering, MaxDpbFrames);
		if (profile_idc != 118 && profile_idc != 128 && profile_idc != 134 ||
			(get_u1(&dec->gb), parse_seq_parameter_set_mvc_extension(dec, profile_idc)))
			return print_dec(dec, "  decode_NAL_result: %s\n", ENOTSUP); // we shouldn't parse any further thus exit now
		if (get_u1(&dec->gb))
			parse_mvc_vui_parameters_extension(dec, &sps);
		get_u1(&dec->gb); // additional_extension2_flag
	}
	
	// check if the SPS can be committed
	if (!rbsp_end(&dec->gb, 1)) {
		// Remuxed 3D Blu-ray subset SPS NALs are occasionally non-conformant,
		// carrying a couple of stray bits before rbsp_trailing_bits. Every
		// syntax element above has already been parsed and bounds-checked, so
		// accept the subset SPS as long as the leftover slack is confined to
		// the NAL's trailing bytes (matching the leniency of reference
		// decoders); a larger gap means the bit position is genuinely off and
		// the SPS is dropped. The cast turns an over-read (negative) into a
		// large unsigned value, so it is rejected too.
		int64_t bits_to_end = bits_left(&dec->gb) - 1;
		if (dec->nal_unit_type == 7 && vui_present) {
			// A malformed VUI (a common encoder bug ffmpeg reports as "Overread
			// VUI by N bits" and tolerates) leaves the bit position off at the end
			// of an otherwise valid SPS. The VUI is the last, non-normative element
			// and every decoding-relevant field before it has already been parsed
			// and bounds-checked, so accept the SPS rather than dropping the whole
			// stream - but revert the VUI's two contributed values to the inferred
			// defaults so nothing from the over-read leaks into the DPB sizing.
			sps.max_num_reorder_frames = inferred_max_num_reorder_frames;
			sps.max_dec_frame_buffering = inferred_max_dec_frame_buffering;
		} else if (dec->nal_unit_type != 15 || (uint64_t)bits_to_end > 16) {
			ret = EBADMSG;
		}
	}
	if (ret == 0) {
		
		// compute the resulting frame format
		Edge264MvcOutput format = {};
		int width = sps.pic_width_in_mbs << 4;
		int height = sps.pic_height_in_mbs << 4;
		format.bit_depth_Y = sps.BitDepth_Y;
		format.width_Y = width - sps.frame_crop_offsets[3] - sps.frame_crop_offsets[1];
		format.height_Y = height - sps.frame_crop_offsets[0] - sps.frame_crop_offsets[2];
		format.stride_Y = (sps.BitDepth_Y == 8) ? width : width << 1;
		// reason: mb_errors is not yet exported (always NULL), so this stride is
		// currently unused. It truncates in int16_t at >=108 mbs wide (304 B/mb);
		// widen stride_mb (an ABI change) together with populating mb_errors.
		format.stride_mb = sps.pic_width_in_mbs * sizeof(Edge264MvcMacroblock);
		if (!(format.stride_Y & 2047)) // add an offset to stride if it is a multiple of 2048
			format.stride_Y += (sps.BitDepth_Y == 8) ? 16 : 32;
		memcpy(format.frame_crop_offsets, &sps.frame_crop_offsets_l, 8);
		if (sps.chroma_format_idc > 0) {
			format.bit_depth_C = sps.BitDepth_C;
			format.width_C = sps.chroma_format_idc == 3 ? format.width_Y : format.width_Y >> 1;
			format.height_C = sps.chroma_format_idc == 1 ? format.height_Y >> 1 : format.height_Y;
			format.stride_C = (sps.chroma_format_idc == 3 ? width << 1 : width) << (sps.BitDepth_C > 8);
			if (!(format.stride_C & 4095)) // add an offset to stride if it is a multiple of 4096
				format.stride_C += (sps.chroma_format_idc == 3 ? 16 : 8) << (sps.BitDepth_C > 8);
		}
		
		// bump all frames and clear the decoder if the frame format changes
		if (memcmp(&format, &dec->out, sizeof(Edge264MvcOutput))) {
			// The previous sequence ends here like at an end_of_seq: every picture
			// must be output, so let get_frame emit an MVC base whose dependent view
			// never comes (otherwise it holds the base, and this NAL returned ENOBUFS
			// until the caller drained the whole stream). The next NAL clears the flag.
			// Frames the caller received and holds do not hold it back either (see
			// parse_end_of_sequence), and keep their buffers until released.
			dec->flushing = 1;
			bump_all_frames(dec);
			if (dec->to_get_frames)
				return ENOBUFS; // SPS should be reparsed after clearing frames, so we don't print it yet
			clear_decoder(dec);
			memcpy(&dec->out, &format, sizeof(format)); // GCC-14 crashes on dec->out = format
			dec->plane_size_Y = format.stride_Y * height;
			dec->plane_size_C = format.stride_C * (sps.chroma_format_idc == 1 ? height >> 1 : height);
			dec->frame_flip_bits = 0;
			dec->stale_frames |= dec->output_frames; // freed by return_frame
			for (int i = 0; i < MAX_FRAMES; i++) {
				if (dec->samples_buffers[i] != NULL && !(dec->stale_frames & (FrameMask)1 << i)) {
					dec->free_cb(dec->samples_buffers[i], dec->mb_buffers[i], dec->alloc_arg);
					dec->samples_buffers[i] = NULL;
					dec->mb_buffers[i] = NULL;
				}
			}
		}
		*((dec->nal_unit_type == 7) ? &dec->sps : &dec->ssps) = sps;
		
		// fix frame limits when MVC is detected
		if (dec->ssps.BitDepth_Y > 0) {
			dec->sps.max_num_ref_frames = min(dec->sps.max_num_ref_frames, 8);
			dec->sps.max_dec_frame_buffering = dec->ssps.max_dec_frame_buffering =
				max(dec->ssps.max_dec_frame_buffering, dec->sps.max_num_ref_frames + dec->ssps.max_num_ref_frames);
			dec->sps.max_num_reorder_frames = dec->ssps.max_num_reorder_frames;
		}
	}
	return print_dec(dec, "  decode_NAL_result: %s\n", ret);
}
