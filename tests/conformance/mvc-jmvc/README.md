# MVC fixtures encoded with the JMVC reference encoder

Small two-view streams encoded with JMVC 8.5, the MVC reference software published as ITU-T H.264.2, for MVC coding tools that neither the JVT MVC vectors in `../mvc/` nor the hand-authored streams in `../mvc-synthetic/` exercise. Run by `make check` through `tests/conformance_check.c`, single-threaded, paced and multithreaded, like the rest of `manifest.txt`.

Both views are anchored to the JMVC decoder: `conformance_check emit` self-verifies the base view against `<name>.yuv` and the dependent view against `<name>.1.yuv`, both written by the JMVC decoder from the committed `.264`, and the line is only accepted with `check=OK dep_check=OK`. For these streams the JMVC decoder output is also identical to the reconstruction the encoder wrote. The `.yuv` files are not committed (regenerable from the `.264`).

The source is two horizontally offset crops of FFmpeg's `testsrc2`, so the views differ by a disparity of 12 pixels, with a static background and moving elements.

## `interview_colocated.264`

Guards colZeroFlag for an inter-view reference at `RefPicList1[0]`. In a dependent-view B slice that uses spatial direct prediction, colZeroFlag (8.4.1.2.2) may only be set when `RefPicList1[0]` is a short-term reference picture, and an inter-view reference counts as neither short- nor long-term (H.8.4, whose note draws exactly this consequence). With `InterPredPicsFirst 0` JMVC places the base view first in both reference lists of every dependent-view B slice, so the colocated macroblock is the base view's, which is still wherever the background is static. A decoder that takes the inter-view reference for a short-term one zeroes the motion of those direct macroblocks, while the encoder predicted it from their neighbours, which carry the disparity: the dependent view goes wrong in every non-anchor picture and the error propagates through its references. Without the fix the dependent-view hash of this line fails; the base view is unaffected.

176x144, Stereo High, CABAC, 8x8 transform, GOP 4 with hierarchical B pictures, an anchor every 4 pictures, 9 frames. Reproduce with the JMVC encoder and assembler:

    ffmpeg -f lavfi -i testsrc2=size=188x144:rate=25 -frames:v 9 -vf crop=176:144:0:0 -pix_fmt yuv420p -f rawvideo src_0.yuv
    ffmpeg -f lavfi -i testsrc2=size=188x144:rate=25 -frames:v 9 -vf crop=176:144:12:0 -pix_fmt yuv420p -f rawvideo src_1.yuv
    H264AVCEncoderLibTestStatic -vf enc.cfg 0
    H264AVCEncoderLibTestStatic -vf enc.cfg 1
    MVCBitStreamAssemblerStatic -vf asm.cfg
    H264AVCDecoderLibTestStatic interview_colocated.264 dec.yuv 2   # dec_0.yuv, dec_1.yuv

`asm.cfg`:

    OutputFile              interview_colocated.264
    NumberOfViews           2
    InputFile0              out_0.264
    InputFile1              out_1.264

`enc.cfg`:

    InputFile               src
    OutputFile              out
    SourceWidth             176
    SourceHeight            144
    ReconFile               rec
    MotionFile              motion
    FrameRate               25
    MaxDelay                1200.0
    FramesToBeEncoded       9
    SymbolMode              1
    FRExt                   1
    BasisQP                 30.0
    GOPSize                 4
    IntraPeriod             4
    NumberReferenceFrames   2
    InterPredPicsFirst      0
    SearchMode              4
    SearchFuncFullPel       3
    SearchFuncSubPel        2
    SearchRange             16
    BiPredIter              4
    IterSearchRange         8
    LoopFilterDisable       0
    LoopFilterAlphaC0Offset 0
    LoopFilterBetaOffset    0
    ICMode                  0
    MotionSkipMode          0
    SingleLoopDecoding      0
    NumViewsMinusOne        1
    ViewOrder               0-1
    View_ID                 0
    Fwd_NumAnchorRefs       0
    Bwd_NumAnchorRefs       0
    Fwd_NumNonAnchorRefs    0
    Bwd_NumNonAnchorRefs    0
    View_ID                 1
    Fwd_NumAnchorRefs       1
    Bwd_NumAnchorRefs       0
    Fwd_NumNonAnchorRefs    1
    Bwd_NumNonAnchorRefs    1
    Fwd_AnchorRefs          0 0
    Fwd_NonAnchorRefs       0 0
    Bwd_NonAnchorRefs       0 0

Keep `IntraPeriod` equal to `GOPSize`: with non-anchor pictures between anchors, JMVC 8.5 writes one reference list modification more than the list holds in the first dependent-view B picture after a base-view P picture, and its own decoder rejects the stream.
