// xbox-cast media: FFmpeg video decoder wrapper (portable: plain FFmpeg API, no platform headers).
// The app plugs D3D11VA in through HwSetup (VideoDecoder.cpp); the Linux harness runs the identical code in software.
//
// Low-latency settings (see docs/media.md):
//   AV_CODEC_FLAG_LOW_DELAY, AV_CODEC_FLAG2_FAST, hw: thread_count 1; sw: slice threads only (frame threads add
//   a frame of delay per thread). AirPlay mirroring has no B-frames; one access unit in -> one picture out in the
//   same Decode() call (verified by the Linux harness: "reorder delay").
//
// Hardware path / fallback: get_format() prefers the hw pixel format when a hw device is attached and HwSetup
// succeeds (it allocates the frame pool with SHADER_RESOURCE binding so the renderer samples decoder surfaces
// directly). If HwSetup fails, or the driver rejects the stream, get_format() returns the software format and FFmpeg
// decodes on the CPU; the switch is logged loudly (tag "video").
//
// Portions derived from moonlight-xbox (GPL-3.0, github.com/TheElixZammuto/moonlight-xbox),
// Streaming/FFmpegDecoder.cpp: ff_get_format / setupDirectSampleFramesContext approach.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include "H264Sps.h"
#include "XcTypes.h"

namespace XboxCast { namespace Media {

struct VideoDecoderOptions {
    xc_video_codec codec = XC_VIDEO_H264;
    int width = 0, height = 0;       // hint only (stream SPS wins)
    AVBufferRef* hwDevice = nullptr; // borrowed; a new ref is taken. nullptr = software
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE; // e.g. AV_PIX_FMT_D3D11
    // Called from get_format when the hw format is offered: set up avctx->hw_frames_ctx. Return false to fall back
    // to software decoding for this stream.
    std::function<bool(AVCodecContext*)> hwSetup;
    int hwExtraFrames = 4;           // surfaces beyond the DPB: renderer holds 1 current + 1 queued, plus slack
    int swThreads = 4;               // slice threads for the software path
    bool reorderFix = true;          // H.264: rewrite SPS to max_num_reorder_frames 0 (see H264Sps.h)
    bool lowDelayFlag = true;        // AV_CODEC_FLAG_LOW_DELAY (tests switch it off to measure the difference)
    int strictCompliance = 0;        // test knob: FF_COMPLIANCE_STRICT makes FFmpeg behave like a conforming decoder
                                     // (assumes max reordering when the SPS has no bitstream_restriction)
};

// Per-frame decode timing: in = packet handed to avcodec_send_packet, out = picture returned.
struct DecodeTiming {
    int64_t inNs = 0;      // when the packet carrying this picture went in
    int64_t outNs = 0;     // when the picture came out
    int packetsHeld = 0;   // packets sent after this picture's packet before it came out (0 = 1-in-1-out)
};

class FfVideoDecoder {
public:
    FfVideoDecoder() = default;
    ~FfVideoDecoder() { Close(); }
    FfVideoDecoder(const FfVideoDecoder&) = delete;
    FfVideoDecoder& operator=(const FfVideoDecoder&) = delete;

    bool Open(const VideoDecoderOptions& opt);
    void Close();
    bool IsOpen() const { return m_ctx != nullptr; }

    // Decode one access unit (Annex-B). onFrame gets every picture produced (the frame is reused after the callback
    // returns: av_frame_ref/move it to keep it). Returns number of frames produced, or a negative AVERROR.
    int Decode(const uint8_t* data, size_t size, int64_t ptsNs, bool keyframe,
               const std::function<void(AVFrame*, const DecodeTiming&)>& onFrame);

    // Drop decoder state (stream reset / rotation). The next frame should be a keyframe.
    void Flush();

    // True when the last produced frame came out of the hw path.
    bool LastFrameHw() const { return m_lastHw; }
    bool HwRequested() const { return m_opt.hwDevice != nullptr; }
    bool HwFailed() const { return m_hwFailed; }
    xc_video_codec Codec() const { return m_opt.codec; }
    AVCodecContext* Context() const { return m_ctx; }
    int GetFormatCalls() const { return m_getFormatCalls; }
    const H264::ReorderFix& Reorder() const { return m_reorder; }
    // Largest packetsHeld seen since Open/Flush (0 means the decoder never buffered a frame).
    int MaxPacketsHeld() const { return m_maxHeld; }
    uint64_t Errors() const { return m_errors; }

    static const char* CodecName(xc_video_codec c) { return c == XC_VIDEO_H265 ? "hevc" : "h264"; }

private:
    static AVPixelFormat GetFormat(AVCodecContext* ctx, const AVPixelFormat* fmts);

    VideoDecoderOptions m_opt;
    AVCodecContext* m_ctx = nullptr;
    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame = nullptr;
    bool m_lastHw = false;
    bool m_hwFailed = false;
    int m_getFormatCalls = 0;
    uint64_t m_errors = 0;
    H264::ReorderFix m_reorder;
    std::vector<uint8_t> m_rewritten;
    struct InFlight { int64_t pts; int64_t inNs; uint64_t seq; };
    std::deque<InFlight> m_inflight; // packets sent, picture not out yet
    uint64_t m_seq = 0;
    int m_maxHeld = 0;
};

std::string AvErr(int err);

}} // namespace XboxCast::Media
