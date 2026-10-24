// SPDX-License-Identifier: GPL-3.0-or-later
// D3D11VA frame pool with SHADER_RESOURCE binding follows moonlight-xbox (GPL-3.0,
// github.com/TheElixZammuto/moonlight-xbox), Streaming/FFmpegDecoder.cpp setupDirectSampleFramesContext.
#include "pch.h"
#include "VideoDecoder.h"

#include <cstring>

#include "VideoRenderer.h"
#include "portable/H264Sps.h"
#include "portable/MediaClock.h"

namespace XboxCast { namespace Media {

namespace {
const int64_t kLogIntervalNs = 10000000000LL;
}

VideoDecoder::VideoDecoder(std::function<std::shared_ptr<D3DDevice>()> getDevice, VideoRenderer* renderer)
    : m_getDevice(std::move(getDevice)), m_renderer(renderer)
{
}

VideoDecoder::~VideoDecoder() { Stop(); }

void VideoDecoder::Start()
{
    if (m_running.exchange(true)) return;
    m_queue.Reopen();
    m_thread = std::thread([this] { ThreadMain(); });
}

void VideoDecoder::Stop()
{
    if (!m_running.exchange(false)) return;
    m_queue.Close();
    if (m_thread.joinable()) m_thread.join();
    m_queue.Clear();
}

// ---------------------------------------------------------------- castcore threads

void VideoDecoder::OnFormat(xc_video_codec codec, int w, int h)
{
    Item it;
    it.kind = Item::Format;
    it.codec = codec;
    it.w = w;
    it.h = h;
    MLOGI("video", "format %s %dx%d", FfVideoDecoder::CodecName(codec), w, h);
    m_queue.PushEvict(std::move(it)); // a format change must never be lost
}

void VideoDecoder::OnFrame(const xc_video_frame& f)
{
    if (!f.data || f.size == 0) return;
    const int64_t now = NowNs();
    Item it;
    it.kind = Item::Frame;
    it.data.assign(f.data, f.data + f.size);
    it.ptsNs = f.pts_ns;
    it.arrivalNs = now;
    it.keyframe = f.is_keyframe != 0;
    it.codec = f.codec;
    it.w = f.width;
    it.h = f.height;
    {
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.framesIn++;
        m_inWin.Add(now, 0);
    }
    if (!m_queue.TryPush(std::move(it))) {
        // decoder stalled for seconds: drop the backlog, restart at the next keyframe
        size_t n = m_queue.RemoveIf([](const Item& q) { return q.kind == Item::Frame; });
        m_overflowed = true;
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.droppedQueue += n + 1;
        MLOGE("video", "decode queue overflow: dropped %zu access units, waiting for the next keyframe", n + 1);
    }
}

void VideoDecoder::OnFlush()
{
    size_t n = m_queue.RemoveIf([](const Item& q) { return q.kind == Item::Frame; });
    Item it;
    it.kind = Item::Flush;
    m_queue.PushEvict(std::move(it));
    MLOGI("video", "flush (dropped %zu queued access units)", n);
}

// ---------------------------------------------------------------- decode thread

bool VideoDecoder::SetupHwFrames(AVCodecContext* ctx)
{
    AVBufferRef* frames = nullptr;
    int err = avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, AV_PIX_FMT_D3D11, &frames);
    if (err < 0 || !frames) {
        MLOGE("video", "avcodec_get_hw_frames_parameters failed: %s", AvErr(err).c_str());
        return false;
    }
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    auto* d3d = reinterpret_cast<AVD3D11VAFramesContext*>(fc->hwctx);
    // default is BIND_DECODER only: add SHADER_RESOURCE so the renderer samples the decoder surfaces directly
    d3d->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    // pool size: FFmpeg adds extra_hw_frames (VideoDecoderOptions::hwExtraFrames) for the renderer's current + mailbox
    err = av_hwframe_ctx_init(frames);
    if (err < 0) {
        MLOGE("video", "av_hwframe_ctx_init(BIND_DECODER|BIND_SHADER_RESOURCE, %s %dx%d) failed: %s",
              av_get_pix_fmt_name(fc->sw_format), fc->width, fc->height, AvErr(err).c_str());
        av_buffer_unref(&frames);
        return false;
    }
    MLOGI("video", "d3d11va frame pool: %s %dx%d, %d surfaces, bind DECODER|SHADER_RESOURCE",
          av_get_pix_fmt_name(fc->sw_format), fc->width, fc->height, fc->initial_pool_size);
    if (ctx->hw_frames_ctx) av_buffer_unref(&ctx->hw_frames_ctx);
    ctx->hw_frames_ctx = frames;
    return true;
}

void VideoDecoder::CloseDecoder()
{
    m_dec.Close();
    if (m_hwDevice) av_buffer_unref(&m_hwDevice);
}

bool VideoDecoder::OpenDecoder()
{
    CloseDecoder();
    m_dev = m_getDevice ? m_getDevice() : nullptr;
    if (m_dev && !m_dev->Lost() && !m_hwDisabled) m_hwDevice = m_dev->NewHwDevice();
    VideoDecoderOptions o;
    o.codec = m_codec;
    o.width = m_w;
    o.height = m_h;
    if (m_hwDevice) {
        o.hwDevice = m_hwDevice;
        o.hwPixFmt = AV_PIX_FMT_D3D11;
        o.hwSetup = [this](AVCodecContext* c) { return SetupHwFrames(c); };
    } else {
        MLOGW("video", "%s: SOFTWARE decode (%s)", FfVideoDecoder::CodecName(m_codec),
              !m_dev ? "no D3D device" : m_hwDisabled ? "hw failed for this stream" : "no D3D11 video device");
    }
    bool ok = m_dec.Open(o);
    if (!ok && m_hwDevice) {
        MLOGE("video", "hw decoder open failed: retrying in software");
        CloseDecoder();
        o = VideoDecoderOptions();
        o.codec = m_codec;
        o.width = m_w;
        o.height = m_h;
        ok = m_dec.Open(o);
    }
    std::lock_guard<std::mutex> lk(m_statsMu);
    m_stats.codec = FfVideoDecoder::CodecName(m_codec);
    m_stats.decoder = m_dec.HwRequested() ? "d3d11va" : "software";
    m_stats.codedW = m_w;
    m_stats.codedH = m_h;
    m_stats.heldMax = 0;
    return ok;
}

void VideoDecoder::DecodeItem(Item& it)
{
    if (it.kind == Item::Format) {
        const bool changed = !m_haveFormat || it.codec != m_codec || it.w != m_w || it.h != m_h;
        m_codec = it.codec;
        m_w = it.w;
        m_h = it.h;
        m_haveFormat = true;
        m_hwDisabled = false;
        if (changed || !m_dec.IsOpen()) {
            OpenDecoder();
            m_waitKey = true;
            m_streamStart = true;
            if (m_renderer) m_renderer->ResetStream();
        }
        return;
    }
    if (it.kind == Item::Flush) {
        if (m_dec.IsOpen()) m_dec.Flush();
        m_waitKey = true;
        m_streamStart = true;
        if (m_renderer) m_renderer->ResetStream();
        return;
    }

    // frame
    if (m_overflowed.exchange(false)) m_waitKey = true;
    if (!m_haveFormat || it.codec != m_codec || (it.w > 0 && it.h > 0 && (it.w != m_w || it.h != m_h))) {
        // no (or a stale) OnFormat: take the geometry from the frame itself
        if (it.w > 0 && it.h > 0) {
            MLOGI("video", "frame carries new format %s %dx%d (was %s %dx%d): reopening",
                  FfVideoDecoder::CodecName(it.codec), it.w, it.h, FfVideoDecoder::CodecName(m_codec), m_w, m_h);
            Item f;
            f.kind = Item::Format;
            f.codec = it.codec;
            f.w = it.w;
            f.h = it.h;
            DecodeItem(f);
        } else if (!m_haveFormat) {
            m_codec = it.codec;
            m_haveFormat = true;
            OpenDecoder();
        }
    }
    // device changed under us (lost + recreated): reopen on the new one
    if (m_dec.IsOpen() && m_dev && m_dec.HwRequested()) {
        auto cur = m_getDevice ? m_getDevice() : nullptr;
        if (m_dev->Lost() || (cur && cur.get() != m_dev.get())) {
            MLOGW("video", "D3D device changed (gen %u -> %u): reopening decoder, waiting for keyframe",
                  m_dev->Generation(), cur ? cur->Generation() : 0);
            OpenDecoder();
            m_waitKey = true;
            m_streamStart = true;
        }
    }
    if (!m_dec.IsOpen() && !OpenDecoder()) return;

    const bool hevc = m_codec == XC_VIDEO_H265;
    if (m_waitKey) {
        if (!H264::HasParameterSets(it.data.data(), it.data.size(), hevc) && !it.keyframe) {
            std::lock_guard<std::mutex> lk(m_statsMu);
            m_stats.droppedQueue++;
            return;
        }
        m_waitKey = false;
        MLOGI("video", "decoding from keyframe pts %lld (%zu bytes)", (long long)it.ptsNs, it.data.size());
    }

    // a backlog is waiting: decode this picture but don't hand it to the renderer (the newer one will replace it)
    const bool backlog = m_queue.Size() > 0;
    const int64_t t0 = NowNs();
    int produced = m_dec.Decode(it.data.data(), it.data.size(), it.ptsNs, it.keyframe,
                                [&](AVFrame* fr, const DecodeTiming& t) {
        const int64_t now = NowNs();
        {
            std::lock_guard<std::mutex> lk(m_statsMu);
            m_stats.framesDecoded++;
            m_ioWin.Add(now, NsToMs(t.outNs - t.inNs));
            m_stats.swFormat = av_get_pix_fmt_name(fr->hw_frames_ctx
                                                       ? reinterpret_cast<AVHWFramesContext*>(fr->hw_frames_ctx->data)->sw_format
                                                       : (AVPixelFormat)fr->format);
            m_stats.codedW = fr->width;
            m_stats.codedH = fr->height;
            m_stats.decoder = fr->hw_frames_ctx ? "d3d11va" : "software";
        }
        if (backlog) {
            std::lock_guard<std::mutex> lk(m_statsMu);
            m_stats.skippedBacklog++;
            return;
        }
        auto vf = std::make_shared<VideoFrame>();
        vf->frame = av_frame_alloc();
        if (!vf->frame || av_frame_ref(vf->frame, fr) < 0) return;
        vf->ptsNs = it.ptsNs;
        vf->arrivalNs = it.arrivalNs;
        vf->decodedNs = now;
        vf->hw = fr->format == AV_PIX_FMT_D3D11;
        vf->deviceGen = m_dev ? m_dev->Generation() : 0;
        vf->streamStart = m_streamStart;
        m_streamStart = false;
        if (m_renderer) m_renderer->Submit(std::move(vf));
    });
    const int64_t t1 = NowNs();
    if (m_dec.HwFailed() && !m_hwDisabled) {
        // get_format already switched this context to software and keeps decoding (no reopen: that would wait for
        // the next keyframe, and the iPhone sends those rarely). Stays software until the next format change.
        m_hwDisabled = true;
        MLOGE("video", "hardware decode unavailable for this stream: continuing in SOFTWARE");
    }
    std::lock_guard<std::mutex> lk(m_statsMu);
    if (produced >= 0) m_decWin.Add(t1, NsToMs(t1 - t0));
    m_stats.errors = m_dec.Errors();
    m_stats.heldMax = m_dec.MaxPacketsHeld() > m_stats.heldMax ? m_dec.MaxPacketsHeld() : m_stats.heldMax;
    m_stats.spsRewritten = m_dec.Reorder().SpsRewritten();
}

void VideoDecoder::ThreadMain()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    MLOGI("video", "decode thread started");
    while (m_running.load()) {
        Item it;
        if (!m_queue.Pop(it, std::chrono::milliseconds(200))) continue;
        DecodeItem(it);
        const int64_t now = NowNs();
        if (now - m_lastLogNs >= kLogIntervalNs) {
            m_lastLogNs = now;
            VideoDecodeStats s = Stats();
            if (s.framesIn > 0)
                MLOGI("video",
                      "%s %s %dx%d %s: in %.1f fps, decoded %.1f fps, decode %.2f/%.2f ms, in->out %.2f/%.2f ms, "
                      "held max %d, queue %d, dropped %llu, backlog-skipped %llu, errors %llu",
                      s.codec.c_str(), s.decoder.c_str(), s.codedW, s.codedH, s.swFormat.c_str(), s.inputFps,
                      s.decodeFps, s.decodeMsAvg, s.decodeMsMax, s.inOutMsAvg, s.inOutMsMax, s.heldMax, s.queue,
                      (unsigned long long)s.droppedQueue, (unsigned long long)s.skippedBacklog,
                      (unsigned long long)s.errors);
        }
    }
    CloseDecoder();
    m_dev.reset();
    m_haveFormat = false;
    MLOGI("video", "decode thread stopped");
}

VideoDecodeStats VideoDecoder::Stats()
{
    std::lock_guard<std::mutex> lk(m_statsMu);
    const int64_t now = NowNs();
    m_inWin.Tick(now);
    m_decWin.Tick(now);
    m_ioWin.Tick(now);
    VideoDecodeStats s = m_stats;
    s.inputFps = m_inWin.Rate();
    s.decodeFps = m_ioWin.Rate();
    s.decodeMsAvg = m_decWin.Avg();
    s.decodeMsMax = m_decWin.Max();
    s.inOutMsAvg = m_ioWin.Avg();
    s.inOutMsMax = m_ioWin.Max();
    s.queue = (int)m_queue.Size();
    s.active = s.inputFps > 0;
    return s;
}

}} // namespace XboxCast::Media
