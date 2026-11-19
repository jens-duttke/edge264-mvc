# Test roadmap

Implemented tests carry a file name, the rest are planned.

edge264-mvc's own tests - MVC conformance, real-world decode robustness, memory safety and multithreading - live in [`tests/conformance`](conformance), [`tests/liveness`](liveness) and [`tests/asan`](asan) (described in the [README](../README.md#tests)) and run under `make check`. The synthetic per-branch matrix below is the original edge264's roadmap; edge264-mvc has begun filling in the MVC rows it has fixtures for (see [`tests/conformance/mvc-synthetic`](conformance/mvc-synthetic)).

| General tests | Expected | Test files |
| --- | --- | --- |
| All supported types of NAL units with/without logging | All OK | supp-nals |
| All unsupported types of NAL units with/without logging | All unsupp | unsupp-nals |
| Maximal header log-wise | All OK | max-logs |
| All conditions for detecting the start of a new frame | All OK | finish-frame |
| nal_ref_idc=0 on NAL types 5, 6, 7, 8, 9, 10, 11, 12 and 15 | All OK | nal-ref-idc-0 |
| forbidden_zero_bit=1 on a slice and a PPS | 2 errors, the rest OK | forbidden-bit |
| Surrounding the CPB/frame buffers with protected memory | All OK | page-boundaries |
| SEI/slice referencing an uninitialized SPS/PPS | 1 OK, 4 errors | missing-ps |
| Two non-ref frames with decreasing POC | All OK, any order | non-ref-dec-poc |
| Horizontal/vertical cropping leaving zero space | All OK, 1x1 frames | zero-cropping |
| P/B slice with nal_unit_type=5 or max_num_ref_frames=0 | 4 OK, 2 errors | no-refs-P-B-slice |
| IDR slice with frame_num>0 | All OK, clamped to 0 | pos-frame-num-idr |
| A ref that must bump out higher POCs to enter DPB (C.4.5.2) | All OK, check output order | poc-out-of-order |
| Two ref frames with the same frame_num but differing POC, then a third frame referencing both |  |  |
| Gap in frame_num while gaps_in_frame_num_value_allowed_flag=0 |  |  |
| Stream starting with non-IDR I frame |  |  |
| Stream starting with P/B frame |  |  |
| Ref slice with delta_pic_order_cnt_bottom=-2**31, then a second frame referencing it |  |  |
| Two frames A/B with intersecting top/bottom POC intervals in all possible intersections |  |  |
| A 32-bit POC overflow between 2 frames |  |  |
| A B-frame referencing frames with more than 2**16 POC diff |  |  |
| num_ref_idx_active>15 in SPS then no override in slice for L0 and L1 |  |  |
| A slice with more ref_pic_list_modifications than num_ref_idx_active/16 for L0 and L1 |  |  |
| A slice with ref_pic_list_modifications duplicating a ref then referencing the second one |  |  |
| A slice with insufficient ref frames with and without override of num_ref_idx_active for L0 and L1 |  |  |
| A modification of RefPicList[0/1] to a non-existing short/long term frame, then referencing it in mb |  |  |
| 33 IDR with long_term_reference_flag=0/1 while max_num_ref_frames=0 (8.2.5.1) |  |  |
| A new reference while max_num_ref_frames are already all long-term |  |  |
| All combinations of mmco on all non-existing/short/long refs, with at least twice each mmco |  |  |
| Two fields of the same frame being assigned different long-term frame indices then referenced |  |  |
| While all max_num_ref_frames are long-term, a ref_pic_list_modification that references all of them |  |  |
| An IDR picture with POC>0 |  |  |
| A picture with mmco=5 decoded after a picture with greater POC (8.2.1) |  |  |
| A P/B frame with zero references before or received with a gap in frame_num equal to max_ref_frames |  |  |
| A P/B frame referencing a non-existing/erroneous ref |  |  |
| A B frame with colPic set to a non-existing frame |  |  |
| A current frame mmco'ed to long-term while all max_num_ref_frames are already long-term |  |  |
| A mmco marking a non-existing picture to long-term |  |  |
| All combinations of IntraNxNPredMode with A/B/C/D unavailability with asserts for out-of-bounds reads |  |  |
| A direct Inter reference from colPic that is not present in RefPicList0 |  |  |
| A residual block with all coeffs at maximum 32-bit values |  |  |
| Two slices of the same frame separated by a currPic reset (ex. AUD) |  |  |
| Two frames with the same POC yet differing TopFieldOrderCnt/BottomFieldOrderCnt |  |  |
| Differing mmcos on two slices of the same frame |  |  |
| Sending 2 IDR, then reaching the lowest possible POC, then getting all frames |  |  |
| Two slices with mmco=5 yet frame_num>0 (to make it look like a new frame) |  |  |
| POCs spaced by more than half max bits, such that relying on a stale prevPicOrderCnt yields wrong POC |  |  |
| Filling the DPB with 16 refs then setting max_num_ref_frames=1 and adding a new ref frame |  |  |
| Adding a frame cropping after decoding a frame | Crop should not apply retroactively |  |
| Making a Direct ref_pic be used after it has been unreferenced |  |  |
| poc_type=2 and non-ref frame followed by non-ref pic, and the opposite (7.4.2.1.1) |  |  |
| direct_8x8_inference_flag=1 with frame_mbs_only_flag=0 |  |  |
| checking that a gap in frame_num with poc_type==0 does not insert refs in B slices |  |  |
| A SPS changing frame format while currPic>=0 |  |  |
| A frame allocator putting pic/mb allocs at start/end of a page boundary |  |  |
| Two escape sequences in a single refill (ex. from a Picture timing SEI message) |  |  |
| All supported NAL types with wrong omission or insertion of trailing bit |  |  |

| Parameter sets tests | Expected | Test files |
| --- | --- | --- |
| Invalid profile_idc=0/255 |  |  |
| Highest level_idc=255 |  |  |
| All unsupported values of chroma_format_idc |  |  |
| All unsupported values of bit_depth_luma/chroma |  |  |
| qpprime_y_zero_transform_bypass_flag=1 |  |  |
| All scaling lists default/fallback rules and repeated values for all indices, with residual macroblock |  |  |
| log2_max_frame_num=4 and a frame referencing another with the same frame_num%4 |  |  |
| Every unsupported feature should be reported as `EDGE264MVC_UNSUPPORTED` and make a log containing a `# unsupported` line |  |  |

| CAVLC tests | Expected | Test files |
| --- | --- | --- |
| All valid total_zeros=0-8-prefix+3-bit-suffix for TotalCoeffs in [0;15] for 4x4 and 2x2 |  |  |
| Invalid total_zeros=31/63/127-prefix for TotalCoeffs in [0;15] for 4x4 and 2x2 |  |  |
| All valid coeff_token=0-14-prefix+4-bit-suffix for nC=0/2/4, and valid 6-bit-values for nC=8 |  |  |
| Invalid coeff_token=31/63/127-prefix for nC=0/2/4, and invalid 6-bit-values for nC=8 |  |  |
| All valid levelCode=25-prefix+suffixLength-bit-suffix for all values of suffixLength |  |  |
| All valid run_before for all values of zerosLeft<=7 |  |  |
| Invalid run_before=31/63/127 for zerosLeft=7 |  |  |
| Macroblock of maximal size for all values of mb_type |  |  |
| mb_qp_delta=-26/25 that overflows on both sides |  |  |
| All valid inferences of nC for all values of nA/nB=unavail/other-slice/0-16 |  |  |
| All coded_block_pattern=[0;47] for I and P/B slices |  |  |
| All combinations of intra_chroma_pred_mode and Intra4x4/8x8/16x16PredMode with A/B-unavailability |  |  |
| All values of mb_type+sub_mb_types for I/P/B with ref_idx/mvds different than values from B_Direct |  |  |
| mvd=[-32768/0/32767,-32768/0/32767] in a single 16x16 macroblock |  |  |
| TotalCoeff=16 for a Intra16x16 AC block |  |  |
| A residual block with run_length=14 making zerosLeft negative |  |  |

| CABAC tests | Expected | Test files |
| --- | --- | --- |
| Mixing CAVLC and CABAC in a same frame |  |  |
| Single slice with at least 8 cabac_zero_word |  |  |

| MVC tests | Expected | Test files |
| --- | --- | --- |
| All wrong combinations of non_idr_flag with nal_unit_type=1/5 and nal_ref_idc=0/1 |  |  |
| nal_unit_type=14 then filler unit then nal_unit_type=1/5 |  |  |
| An nal_unit_type=5 view paired with a non_idr_flag=0 P view, or a non_idr_flag=1 view |  |  |
| Missing a base or non-base view | No stall: lone base emitted, orphan dependent dropped | mvc_unpaired_base, mvc_orphan_dependent |
| Receiving a SSPS yet only base views then |  |  |
| 16 ref base views while non base are non-refs |  |  |
| A SSPS with different pic_width_in_mbs/pic_height_in_mbs/chroma_format_idc than its SPS |  |  |
| A SSPS with num_views=1 |  |  |
| A non-base view with weighted_bipred_idc=2 |  |  |
| A non-base view with its base in RefPicList1[0] and direct_spatial_mv_pred_flag=0 (H.7.4.3) |  |  |
| A slice with num_ref_idx_l0_active>8 |  |  |
| svc_extension_flag=1 on a MVC stream |  |  |
| SSPS with additional_extension2_flag=1 and more trailing data |  |  |
| Gap in frame_num of 16 frames on both views |  |  |
| Specifying extra_frames=1 |  |  |
| Receiving a non-base view before its base | Paired, 2 frames | mvc-synthetic/mvc_dep_before_base |
| A stream sending non-base views after a few frames have been output | 2D then stereo, 4 frames | mvc-synthetic/mvc_late_dependent |

| Error recovery tests | Expected | Test files |
| --- | --- | --- |
| Tests to implement |  |  |
| A complete frame received twice |  |  |
| A slice of a frame received twice |  |  |
| Frame with correct and erroneous slice |  |  |
| All combinations erroneous/correct and all interval intersections on 2 slices |  |  |
| All failures of malloc |  |  |
| All (dis-)allowed bit positions at the end without rbsp_trailing_bit |  |  |
