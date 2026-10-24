// xbox-cast media: a decoded picture handed from the decode thread to the render thread.
// Holds its own AVFrame reference: for D3D11VA frames that keeps the decoder surface out of the pool until the
// renderer replaces it (the pool has hwExtraFrames spare surfaces for exactly this).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

namespace XboxCast { namespace Media {

struct VideoFrame {
    AVFrame* frame = nullptr;   // owned reference
    int64_t ptsNs = 0;          // xc_now_ns clock
    int64_t arrivalNs = 0;      // when castcore handed us the packet
    int64_t decodedNs = 0;      // when the picture came out of the decoder
    uint32_t deviceGen = 0;     // D3DDevice generation the surface belongs to (hw frames)
    bool hw = false;            // AV_PIX_FMT_D3D11 surface (else a software frame in system memory)
    bool streamStart = false;   // first picture after open/flush/format change: renderer snaps instead of animating

    VideoFrame() = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    ~VideoFrame()
    {
        if (frame) av_frame_free(&frame);
    }
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;

}} // namespace XboxCast::Media
