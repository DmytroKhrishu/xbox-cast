// xbox-cast media: video decode thread (FFmpeg H.264 / HEVC, D3D11VA on the shared device, software fallback).
//
// castcore thread -> OnFrame copies the access unit into a bounded FIFO and returns (~1 ms budget).
// decode thread   -> pops in order and decodes EVERY access unit (AirPlay mirroring is one long GOP: dropping a
//                    reference picture corrupts everything until the next IDR, and we cannot ask the sender for one).
//                    Each picture goes to the renderer's one-slot mailbox; while more access units are already queued,
//                    pictures are decoded but not handed over (counted in framesDroppedRender), so a backlog drains
//                    at decode speed and the screen shows the newest picture as soon as the backlog is gone.
// Overflow        -> only if the FIFO overflows (decoder stalled for seconds) everything queued is dropped and decoding
//                    resumes at the next access unit carrying parameter sets (keyframe). Logged loudly.
// Format change   -> OnFormat with a new codec/size (rotation = new stream header) reopens the decoder; OnFlush drops
//                    the queue, flushes the decoder and waits for the next keyframe.
// Device lost     -> the decoder notices a new device generation and reopens on it (waits for a keyframe).
//
// Latency: the H.264 SPS from iOS lacks bitstream_restriction, so a conforming decoder buffers up to the DPB size before
// output (130-700 ms on Xbox, see screen-mirror-xbox docs/DEVLOG.md). FfVideoDecoder rewrites the SPS to
// max_num_reorder_frames=0 and sets AV_CODEC_FLAG_LOW_DELAY; per-frame in->out time is measured and exported
// (decodeInOutMs*, decodeHeldMax). See portable/H264Sps.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "D3DDevice.h"
#include "MediaPlatform.h"
#include "VideoFrame.h"
#include "portable/FfVideoDecoder.h"
#include "portable/MediaStats.h"
#include "portable/Queues.h"

namespace XboxCast { namespace Media {

class VideoRenderer;

struct VideoDecodeStats {
    std::string codec, decoder, swFormat;
    int codedW = 0, codedH = 0;
    double inputFps = 0, decodeFps = 0;
    double decodeMsAvg = 0, decodeMsMax = 0;     // avcodec_send_packet + receive_frame wall time
    double inOutMsAvg = 0, inOutMsMax = 0;       // packet in -> its picture out
    int heldMax = 0;
    int spsRewritten = 0;
    int queue = 0;
    uint64_t framesIn = 0, framesDecoded = 0, droppedQueue = 0, skippedBacklog = 0, errors = 0;
    bool active = false;
};

class VideoDecoder {
public:
    VideoDecoder(std::function<std::shared_ptr<D3DDevice>()> getDevice, VideoRenderer* renderer);
    ~VideoDecoder();

    void Start();
    void Stop();          // joins the thread, closes the decoder

    // castcore threads (must return fast)
    void OnFormat(xc_video_codec codec, int w, int h);
    void OnFrame(const xc_video_frame& f);
    void OnFlush();

    VideoDecodeStats Stats();

private:
    struct Item {
        enum Kind { Frame, Format, Flush } kind = Frame;
        std::vector<uint8_t> data;
        int64_t ptsNs = 0, arrivalNs = 0;
        bool keyframe = false;
        xc_video_codec codec = XC_VIDEO_H264;
        int w = 0, h = 0;
    };

    void ThreadMain();
    bool OpenDecoder();
    void CloseDecoder();
    bool SetupHwFrames(AVCodecContext* ctx);
    void DecodeItem(Item& it);

    std::function<std::shared_ptr<D3DDevice>()> m_getDevice;
    VideoRenderer* m_renderer;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    BoundedQueue<Item> m_queue{240}; // ~4 s at 60 fps: only a stalled decoder ever fills it
    std::atomic<bool> m_overflowed{false};

    // decode thread
    FfVideoDecoder m_dec;
    std::shared_ptr<D3DDevice> m_dev;
    AVBufferRef* m_hwDevice = nullptr;
    xc_video_codec m_codec = XC_VIDEO_H264;
    int m_w = 0, m_h = 0;
    bool m_haveFormat = false;
    bool m_waitKey = true;          // drop until an access unit with parameter sets
    bool m_streamStart = true;      // next picture starts a new stream (renderer snaps its layout)
    bool m_hwDisabled = false;      // hw setup failed for this stream: stay in software until the next format
    int64_t m_lastLogNs = 0;

    // stats
    std::mutex m_statsMu;
    VideoDecodeStats m_stats;
    RateWindow m_inWin, m_decWin, m_ioWin;
};

}} // namespace XboxCast::Media
