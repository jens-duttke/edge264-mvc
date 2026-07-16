static int parse_buffering_period(Edge264Decoder *dec) {
	get_ue16(&dec->gb, 31);
	if (!dec->sps.BitDepth_Y) // if SPS wasn't initialized
		return EBADMSG;
	log_dec(dec, "    delay_bits: %u\n", dec->sps.initial_cpb_removal_delay_length);
	if (dec->sps.nal_hrd_cpb_cnt)
		log_dec(dec, "    nal_hrd_cpbs:\n");
	for (int i = dec->sps.nal_hrd_cpb_cnt; i--; ) {
		int initial_cpb_removal_delay = get_uv(&dec->gb, dec->sps.initial_cpb_removal_delay_length);
		int initial_cpb_removal_delay_offset = get_uv(&dec->gb, dec->sps.initial_cpb_removal_delay_length);
		log_dec(dec, "    - initial_cpb_removal_delay: %u\n"
			"      initial_cpb_removal_delay_offset: %u\n",
			initial_cpb_removal_delay, initial_cpb_removal_delay_offset);
	}
	if (dec->sps.vcl_hrd_cpb_cnt)
		log_dec(dec, "    vcl_hrd_cpbs:\n");
	for (int i = dec->sps.vcl_hrd_cpb_cnt; i--; ) {
		int initial_cpb_removal_delay = get_uv(&dec->gb, dec->sps.initial_cpb_removal_delay_length);
		int initial_cpb_removal_delay_offset = get_uv(&dec->gb, dec->sps.initial_cpb_removal_delay_length);
		log_dec(dec, "    - initial_cpb_removal_delay: %u\n"
			"      initial_cpb_removal_delay_offset: %u\n",
			initial_cpb_removal_delay, initial_cpb_removal_delay_offset);
	}
	return 0;
}



static int parse_pic_timing(Edge264Decoder *dec) {
	static const char * const pic_struct_names[16] = {
		"progressive frame", "top field", "bottom field", "top then bottom",
		"bottom then top", "top then bottom then top",
		"bottom then top then bottom", "frame doubling", "frame tripling",
		[9 ... 15] = "Unknown"};
	static const char * const ct_type_names[4] = {
		"progressive", "interlaced", [2 ... 3] = "unknown"};
	
	if (dec->sps.nal_hrd_cpb_cnt | dec->sps.vcl_hrd_cpb_cnt) {
		int cpb_removal_delay = get_uv(&dec->gb, dec->sps.cpb_removal_delay_length);
		int dpb_output_delay = get_uv(&dec->gb, dec->sps.dpb_output_delay_length);
		log_dec(dec, "    cpb_removal_delay: %u\n"
			"    dpb_output_delay: %u\n",
			cpb_removal_delay, dpb_output_delay);
	}
	if (dec->sps.pic_struct_present_flag) {
		int pic_struct = get_uv(&dec->gb, 4);
		int NumClockTS = 0x3be95 >> (pic_struct * 2) & 3;
		log_dec(dec, "    pic_struct: %s (%u)\n",
			pic_struct_names[pic_struct], pic_struct);
		log_dec(dec, "    clock_timestamps:\n");
		while (NumClockTS--) {
			if (!get_u1(&dec->gb)) // clock_timestamp_flag
				continue;
			unsigned u = get_uv(&dec->gb, 19);
			if (u & 1 << 10) {
				unsigned v = get_uv(&dec->gb, 17);
				dec->sS = v >> 11;
				dec->mM = v >> 5 & 0x3f;
				dec->hH = v & 0x1f;
			} else if (get_u1(&dec->gb)) { // seconds_flag
				unsigned w = get_uv(&dec->gb, 7);
				dec->sS = w >> 1;
				if (w & 1) {
					unsigned x = get_uv(&dec->gb, 7);
					dec->mM = x >> 1;
					if (x & 1)
						dec->hH = get_uv(&dec->gb, 5);
				}
			}
			int tOffset = 0;
			if (dec->sps.time_offset_length)
				tOffset = get_uv(&dec->gb, dec->sps.time_offset_length);
			log_dec(dec, "      - scan_type: %s (%u)\n"
				"        discontinuity_flag: %u\n"
				"        clockTimestamp: \"%02u:%02u:%02u+%u/%u\"\n",
				ct_type_names[u >> 17], u >> 17,
				u >> 9 & 1,
				dec->hH, dec->mM, dec->sS, (u & 0xff) * (dec->sps.num_units_in_tick * (1 + (u >> 16 & 1))) + tOffset, dec->sps.time_scale);
		}
	}
	return 0;
}



static int parse_pan_scan_rect(Edge264Decoder *dec) {
	unsigned pan_scan_rect_id = get_ue32(&dec->gb, 4294967294);
	int pan_scan_rect_cancel_flag = get_u1(&dec->gb);
	log_dec(dec, "    pan_scan_rect_id: %u\n"
		"    pan_scan_rect_cancel_flag: %u\n",
		pan_scan_rect_id, pan_scan_rect_cancel_flag);
	if (!pan_scan_rect_cancel_flag) {
		log_dec(dec, "    fields:\n");
		for (int i = get_ue16(&dec->gb, 2) + 1; i--; ) {
			int pan_scan_rect_left_offset = get_se32(&dec->gb, -2147483647, 2147483647);
			int pan_scan_rect_right_offset = get_se32(&dec->gb, -2147483647, 2147483647);
			int pan_scan_rect_top_offset = get_se32(&dec->gb, -2147483647, 2147483647);
			int pan_scan_rect_bottom_offset = get_se32(&dec->gb, -2147483647, 2147483647);
			log_dec(dec, "      - {left: %d, right: %d, top: %d, bottom: %d}\n",
				pan_scan_rect_left_offset, pan_scan_rect_right_offset, pan_scan_rect_top_offset, pan_scan_rect_bottom_offset);
		}
		int pan_scan_rect_repetition_period = get_ue16(&dec->gb, 16384);
		log_dec(dec, "    pan_scan_rect_repetition_period: %u\n",
			pan_scan_rect_repetition_period);
	}
	return 0;
}



typedef int (*SEI_Parser)(Edge264Decoder *dec);
int ADD_VARIANT(parse_sei)(Edge264Decoder *dec, Edge264UnrefCb unref_cb, void *unref_arg)
{
	// FIXME reduce array size to minimum!
	static const char * const payloadType_names[206] = {
		[0 ... 205] = "Unknown",
		[0] = "Buffering period",
		[1] = "Picture timing",
		[2] = "Pan-scan rectangle",
	};
	static const SEI_Parser parse_sei_message[206] = {
		[0] = parse_buffering_period,
		[1] = parse_pic_timing,
		[2] = parse_pan_scan_rect,
	};
	
	if (print_dec(dec, "  sei_messages:\n", 0))
		return ENOTSUP;
	int nal_ret = 0;
	// not a loop on rbsp_end since must also stop when we cross end
	while (dec->gb.msb_cache << 1 ||
		(dec->gb.lsb_cache & (dec->gb.lsb_cache - 1)) ||
		dec->gb.CPB < dec->gb.end) // if CPB >= end then all future refills will yield 0
	{
		// unsigned: payloadType/payloadSize are ff_byte sums (H.264 7.3.2.3.1)
		// with no upper bound; signed overflow on a crafted run is UB and could
		// make payloadType negative, defeating the `<= 205` dispatch guard below.
		unsigned byte, payloadType = 0, payloadSize = 0;
		do {
			byte = get_uv(&dec->gb, 8);
			payloadType += byte;
		} while (byte == 255);
		do {
			byte = get_uv(&dec->gb, 8);
			payloadSize += byte;
		} while (byte == 255);
		log_dec(dec, "  - payloadType: %s (%u)\n",
			payloadType <= 205 ? payloadType_names[payloadType] : "Reserved", payloadType);
		Edge264GetBits start = dec->gb;
		int sei_ret = ENOTSUP;
		if (payloadType <= 205 && parse_sei_message[payloadType])
			sei_ret = parse_sei_message[payloadType](dec);
		sei_ret = print_dec(dec, "    parse_SEI_result: %s\n", sei_ret);
		if (sei_ret == EBADMSG)
			nal_ret = EBADMSG;
		// Advance to the end of this payload (start + payloadSize) regardless of
		// the handler outcome. sei_payload is exactly payloadSize bytes (7.3.2.3);
		// a successful handler may still short-read a forward-compatible payload
		// that carries trailing reserved/extension data. The old success path only
		// byte-ALIGNED, leaving that tail in the stream to be re-parsed as bogus
		// follow-on messages - the reader ended mid-syntax, rbsp_end failed and the
		// whole (valid) SEI NAL misreturned EBADMSG. Restore start and skip
		// payloadSize like the unsupported-message path, bounding the skip by the
		// bits actually left in this NAL (cache-aware), NOT by CPB. CPB is the
		// bitstream reader's refill pointer, not its read position: a NAL small
		// enough to fit entirely in the bit cache has CPB == end already, so a
		// `CPB < end` guard skips nothing. This bound also keeps a DoS guard:
		// payloadType/payloadSize are attacker-controlled and uncapped (H.264
		// 7.3.2.3.1), and bits_left never exceeds the real NAL size, so a crafted
		// ~INT_MAX payloadSize cannot spin.
		dec->gb = start;
		int bits_left = (int)(dec->gb.end - dec->gb.CPB) * 8 + SIZE_BIT * 2 - 1 - ctz(dec->gb.lsb_cache);
		for (unsigned n = payloadSize; n-- > 0 && bits_left >= 8; bits_left -= 8)
			get_uv(&dec->gb, 8);
	}
	return rbsp_end(&dec->gb, 1) ? nal_ret : EBADMSG;
}
