// SPDX-License-Identifier: GPL-3.0-or-later
// Portions derived from moonlight-xbox (GPL-3.0), Streaming/FFmpegDecoder.cpp.
#include "pch.h"
#include "FfVideoDecoder.h"

#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "MediaClock.h"
#include "MediaLog.h"

namespace XboxCast { namespace Media {

std::string AvErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

AVPixelFormat FfVideoDecoder::GetFormat(AVCodecContext* ctx, const AVPixelFormat* fmts)
{
    FfVideoDecoder* self = static_cast<FfVideoDecoder*>(ctx->opaque);
    self->m_getFormatCalls++;
    std::string offered;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const char* n = av_get_pix_fmt_name(*p);
        offered += n ? n : "?";
        offered += ' ';
    }
    if (self->m_opt.hwDevice && !self->m_hwFailed) {
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p != self->m_opt.hwPixFmt) continue;
            if (!self->m_opt.hwSetup || self->m_opt.hwSetup(ctx)) {
                MLOGI("video", "get_format #%d: %s %dx%d -> %s (offered: %s)", self->m_getFormatCalls,
                      CodecName(self->m_opt.codec), ctx->coded_width, ctx->coded_height, av_get_pix_fmt_name(*p),
                      offered.c_str());
                return *p;
            }
            MLOGE("video", "hw frame pool setup failed for %dx%d: FALLING BACK TO SOFTWARE DECODE", ctx->coded_width,
                  ctx->coded_height);
            self->m_hwFailed = true;
            break;
        }
        if (!self->m_hwFailed) {
            MLOGE("video", "decoder did not offer %s (offered: %s): FALLING BACK TO SOFTWARE DECODE",
                  av_get_pix_fmt_name(self->m_opt.hwPixFmt), offered.c_str());
            self->m_hwFailed = true;
        }
    }
    // first software format
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            MLOGI("video", "get_format #%d: %s %dx%d -> %s (software)", self->m_getFormatCalls,
                  CodecName(self->m_opt.codec), ctx->coded_width, ctx->coded_height, d->name);
            return *p;
        }
    }
    MLOGE("video", "get_format: no usable format (offered: %s)", offered.c_str());
    return AV_PIX_FMT_NONE;
}

bool FfVideoDecoder::Open(const VideoDecoderOptions& opt)
{
    Close();
    m_opt = opt;
    m_hwFailed = false;
    m_getFormatCalls = 0;
    m_errors = 0;
    m_reorder.Reset();
    m_inflight.clear();
    m_seq = 0;
    m_maxHeld = 0;
    const AVCodecID id = opt.codec == XC_VIDEO_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) {
        MLOGE("video", "avcodec_find_decoder(%s) failed: FFmpeg built without this decoder", CodecName(opt.codec));
        return false;
    }
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) return false;
    m_ctx->opaque = this;
    m_ctx->get_format = &FfVideoDecoder::GetFormat;
    if (opt.lowDelayFlag) m_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_ctx->pkt_timebase = AVRational{1, 1000000000};
    m_ctx->err_recognition = 0;
    m_ctx->strict_std_compliance = opt.strictCompliance;
    if (opt.width > 0 && opt.height > 0) {
        m_ctx->width = opt.width;
        m_ctx->height = opt.height;
    }
    if (opt.hwDevice) {
        m_ctx->hw_device_ctx = av_buffer_ref(opt.hwDevice);
        m_ctx->thread_count = 1; // hwaccel: threads only add latency
        m_ctx->extra_hw_frames = opt.hwExtraFrames;
    } else {
        m_ctx->thread_count = opt.swThreads > 0 ? opt.swThreads : 1;
        m_ctx->thread_type = FF_THREAD_SLICE; // never frame threads (1 frame delay per thread)
    }
    int err = avcodec_open2(m_ctx, codec, nullptr);
    if (err < 0) {
        MLOGE("video", "avcodec_open2(%s) failed: %s", CodecName(opt.codec), AvErr(err).c_str());
        Close();
        return false;
    }
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    MLOGI("video", "decoder opened %s %dx%d %s, flags LOW_DELAY|FAST, threads %d (%s), libavcodec %u.%u.%u",
          CodecName(opt.codec), opt.width, opt.height, opt.hwDevice ? "hw requested" : "software", m_ctx->thread_count,
          opt.hwDevice ? "hw" : "slice", AV_VERSION_MAJOR(avcodec_version()), AV_VERSION_MINOR(avcodec_version()),
          AV_VERSION_MICRO(avcodec_version()));
    return true;
}

void FfVideoDecoder::Close()
{
    if (m_ctx) avcodec_free_context(&m_ctx);
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_frame) av_frame_free(&m_frame);
    m_lastHw = false;
}

int FfVideoDecoder::Decode(const uint8_t* data, size_t size, int64_t ptsNs, bool keyframe,
                           const std::function<void(AVFrame*, const DecodeTiming&)>& onFrame)
{
    if (!m_ctx) return AVERROR(EINVAL);
    if (m_opt.reorderFix && m_opt.codec == XC_VIDEO_H264 && m_reorder.Process(data, size, m_rewritten)) {
        data = m_rewritten.data();
        size = m_rewritten.size();
    }
    // padded copy: FFmpeg's bitstream readers over-read by up to AV_INPUT_BUFFER_PADDING_SIZE
    int err = av_new_packet(m_pkt, (int)size);
    if (err < 0) return err;
    std::memcpy(m_pkt->data, data, size);
    m_pkt->pts = ptsNs;
    m_pkt->dts = ptsNs;
    if (keyframe) m_pkt->flags |= AV_PKT_FLAG_KEY;
    const int64_t inNs = NowNs();
    err = avcodec_send_packet(m_ctx, m_pkt);
    av_packet_unref(m_pkt);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        if ((m_errors++ % 60) == 0)
            MLOGW("video", "avcodec_send_packet: %s (errors so far %llu)", AvErr(err).c_str(),
                  (unsigned long long)m_errors);
        return err;
    }
    const uint64_t seq = ++m_seq;
    m_inflight.push_back(InFlight{ptsNs, inNs, seq});
    if (m_inflight.size() > 64) m_inflight.pop_front(); // packets that never produce a picture (SEI-only, errors)
    int produced = 0;
    for (;;) {
        err = avcodec_receive_frame(m_ctx, m_frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) {
            if ((m_errors++ % 60) == 0) MLOGW("video", "avcodec_receive_frame: %s", AvErr(err).c_str());
            return produced ? produced : err;
        }
        DecodeTiming t;
        t.outNs = NowNs();
        t.inNs = inNs;
        const int64_t fpts = m_frame->pts != AV_NOPTS_VALUE ? m_frame->pts : m_frame->best_effort_timestamp;
        // match the picture to the packet it came from by pts; drop older entries (they produced nothing)
        while (!m_inflight.empty()) {
            InFlight f = m_inflight.front();
            m_inflight.pop_front();
            if (f.pts == fpts) {
                t.inNs = f.inNs;
                t.packetsHeld = (int)(seq - f.seq);
                break;
            }
        }
        if (t.packetsHeld > m_maxHeld) {
            m_maxHeld = t.packetsHeld;
            MLOGW("video", "decoder held a picture for %d packet(s) (%.1f ms in->out): reorder buffering, check SPS",
                  t.packetsHeld, NsToMs(t.outNs - t.inNs));
        }
        m_lastHw = m_frame->hw_frames_ctx != nullptr;
        produced++;
        onFrame(m_frame, t);
        av_frame_unref(m_frame);
    }
    return produced;
}

void FfVideoDecoder::Flush()
{
    if (m_ctx) avcodec_flush_buffers(m_ctx);
    m_inflight.clear();
    m_reorder.Reset();
    m_maxHeld = 0;
}

}} // namespace XboxCast::Media
