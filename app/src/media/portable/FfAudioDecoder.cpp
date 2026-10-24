// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "FfAudioDecoder.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include "FfVideoDecoder.h" // AvErr
#include "MediaLog.h"

namespace XboxCast { namespace Media {

const char* FfAudioDecoder::CodecName(xc_audio_codec c)
{
    switch (c) {
    case XC_AUDIO_AAC_ELD: return "aac-eld";
    case XC_AUDIO_ALAC: return "alac";
    case XC_AUDIO_AAC_LC: return "aac-lc";
    case XC_AUDIO_PCM: return "pcm";
    }
    return "?";
}

namespace {
struct BitWriter {
    std::vector<uint8_t> out;
    int bit = 0;
    void Put(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--) {
            if (bit == 0) out.push_back(0);
            if ((v >> i) & 1) out.back() |= (uint8_t)(0x80 >> bit);
            bit = (bit + 1) & 7;
        }
    }
};

int SampleRateIndex(int rate)
{
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; i++)
        if (kRates[i] == rate) return i;
    return -1;
}
} // namespace

std::vector<uint8_t> FfAudioDecoder::DefaultAsc(int objectType, int sampleRate, int channels, int frameLength)
{
    BitWriter w;
    if (objectType >= 31) {
        w.Put(31, 5);
        w.Put((uint32_t)(objectType - 32), 6);
    } else {
        w.Put((uint32_t)objectType, 5);
    }
    int idx = SampleRateIndex(sampleRate);
    if (idx < 0) {
        w.Put(15, 4);
        w.Put((uint32_t)sampleRate, 24);
    } else {
        w.Put((uint32_t)idx, 4);
    }
    w.Put((uint32_t)channels, 4);
    if (objectType == 39) { // ELDSpecificConfig
        w.Put(frameLength == 480 ? 1 : 0, 1);
        w.Put(0, 3);  // section/scalefactor/spectral data resilience
        w.Put(0, 1);  // ldSbrPresentFlag
        w.Put(0, 4);  // ELDEXT_TERM
    } else {      // GASpecificConfig
        w.Put(frameLength == 960 ? 1 : 0, 1);
        w.Put(0, 1);  // dependsOnCoreCoder
        w.Put(0, 1);  // extensionFlag
    }
    return w.out;
}

std::vector<uint8_t> FfAudioDecoder::AlacExtradata(const uint8_t* cookie, size_t n, int sampleRate, int channels)
{
    std::vector<uint8_t> e;
    if (cookie && n >= 36 && std::memcmp(cookie + 4, "alac", 4) == 0) {
        e.assign(cookie, cookie + 36);
        return e;
    }
    // FFmpeg wants the 36-byte 'alac' atom: size(4) 'alac'(4) version(4) + 24-byte ALACSpecificConfig
    static const uint8_t hdr[12] = {0, 0, 0, 36, 'a', 'l', 'a', 'c', 0, 0, 0, 0};
    e.assign(hdr, hdr + 12);
    if (cookie && n >= 24) {
        e.insert(e.end(), cookie, cookie + 24);
    } else {
        // AirPlay default: 352 frames/packet, 16-bit, pb 40, mb 10, kb 14, maxRun 255
        uint8_t c[24] = {0, 0, 0x01, 0x60, 0, 16, 40, 10, 14, (uint8_t)channels, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0};
        c[20] = (uint8_t)(sampleRate >> 24);
        c[21] = (uint8_t)(sampleRate >> 16);
        c[22] = (uint8_t)(sampleRate >> 8);
        c[23] = (uint8_t)sampleRate;
        e.insert(e.end(), c, c + 24);
    }
    return e;
}

bool FfAudioDecoder::Open(xc_audio_codec codec, int sampleRate, int channels, const uint8_t* cfg, size_t cfgSize,
                          int outRate, int outChannels)
{
    Close();
    m_codec = codec;
    m_inRate = sampleRate > 0 ? sampleRate : 44100;
    m_inCh = channels > 0 ? channels : 2;
    m_outRate = outRate > 0 ? outRate : 48000;
    m_outCh = outChannels > 0 ? outChannels : 2;
    m_errors = 0;

    if (codec == XC_AUDIO_PCM) {
        m_open = true;
        MLOGI("audio", "decoder opened pcm s16le %d Hz %d ch -> float %d Hz %d ch", m_inRate, m_inCh, m_outRate, m_outCh);
        return true;
    }

    std::vector<uint8_t> extradata;
    AVCodecID id = AV_CODEC_ID_AAC;
    if (codec == XC_AUDIO_ALAC) {
        id = AV_CODEC_ID_ALAC;
        extradata = AlacExtradata(cfg, cfgSize, m_inRate, m_inCh);
    } else if (cfg && cfgSize > 0) {
        extradata.assign(cfg, cfg + cfgSize);
    } else {
        // iOS mirroring: AAC-ELD 44.1 kHz stereo 480 samples; AAC-LC 1024 samples
        extradata = codec == XC_AUDIO_AAC_ELD ? DefaultAsc(39, m_inRate, m_inCh, 480) : DefaultAsc(2, m_inRate, m_inCh, 1024);
        MLOGW("audio", "no codec config from sender, using default %s ASC (%zu bytes)", CodecName(codec), extradata.size());
    }

    const AVCodec* dec = avcodec_find_decoder(id);
    if (!dec) {
        MLOGE("audio", "avcodec_find_decoder(%s) failed: FFmpeg built without it", CodecName(codec));
        return false;
    }
    m_ctx = avcodec_alloc_context3(dec);
    if (!m_ctx) return false;
    m_ctx->extradata = (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(m_ctx->extradata, extradata.data(), extradata.size());
    m_ctx->extradata_size = (int)extradata.size();
    m_ctx->sample_rate = m_inRate;
    av_channel_layout_default(&m_ctx->ch_layout, m_inCh);
    m_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_ctx->pkt_timebase = AVRational{1, 1000000000};
    int err = avcodec_open2(m_ctx, dec, nullptr);
    if (err < 0) {
        MLOGE("audio", "avcodec_open2(%s) failed: %s", CodecName(codec), AvErr(err).c_str());
        Close();
        return false;
    }
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    std::string hex;
    for (size_t i = 0; i < extradata.size() && i < 40; i++) {
        char b[4];
        snprintf(b, sizeof(b), "%02x", extradata[i]);
        hex += b;
    }
    MLOGI("audio", "decoder opened %s (%s) %d Hz %d ch, config %s, frame_size %d -> float %d Hz %d ch", CodecName(codec),
          dec->name, m_inRate, m_inCh, hex.c_str(), m_ctx->frame_size, m_outRate, m_outCh);
    m_open = true;
    return true;
}

void FfAudioDecoder::Close()
{
    if (m_ctx) avcodec_free_context(&m_ctx);
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_frame) av_frame_free(&m_frame);
    if (m_swr) swr_free(&m_swr);
    av_channel_layout_uninit(&m_swrLayout);
    m_swrFmt = AV_SAMPLE_FMT_NONE;
    m_swrRate = 0;
    m_open = false;
}

bool FfAudioDecoder::EnsureSwr(AVSampleFormat fmt, int rate, const AVChannelLayout* layout)
{
    if (m_swr && fmt == m_swrFmt && rate == m_swrRate && av_channel_layout_compare(layout, &m_swrLayout) == 0)
        return true;
    if (m_swr) swr_free(&m_swr);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, m_outCh);
    int err = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_outRate, layout, fmt, rate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (err >= 0) err = swr_init(m_swr);
    if (err < 0) {
        MLOGE("audio", "swresample init failed: %s", AvErr(err).c_str());
        if (m_swr) swr_free(&m_swr);
        return false;
    }
    m_swrFmt = fmt;
    m_swrRate = rate;
    av_channel_layout_uninit(&m_swrLayout);
    av_channel_layout_copy(&m_swrLayout, layout);
    MLOGI("audio", "resampler: %s %d Hz %d ch -> flt %d Hz %d ch%s", av_get_sample_fmt_name(fmt), rate,
          layout->nb_channels, m_outRate, m_outCh, rate == m_outRate ? " (format only)" : "");
    return true;
}

int FfAudioDecoder::Convert(const uint8_t** in, int inFrames, int64_t ptsNs,
                            const std::function<void(const float*, int, int64_t)>& onPcm)
{
    int maxOut = swr_get_out_samples(m_swr, inFrames);
    if (maxOut <= 0) return 0;
    m_out.resize((size_t)maxOut * m_outCh);
    uint8_t* outp = (uint8_t*)m_out.data();
    // samples still buffered inside swr (filter delay) come out first: shift pts back by that delay
    int64_t delayNs = swr_get_delay(m_swr, 1000000000LL);
    int n = swr_convert(m_swr, &outp, maxOut, in, inFrames);
    if (n < 0) return n;
    if (n > 0) onPcm(m_out.data(), n, ptsNs - delayNs);
    return n;
}

int FfAudioDecoder::Decode(const uint8_t* data, size_t size, int64_t ptsNs,
                           const std::function<void(const float*, int, int64_t)>& onPcm)
{
    if (!m_open) return AVERROR(EINVAL);
    if (m_codec == XC_AUDIO_PCM) {
        AVChannelLayout l;
        av_channel_layout_default(&l, m_inCh);
        bool ok = EnsureSwr(AV_SAMPLE_FMT_S16, m_inRate, &l);
        av_channel_layout_uninit(&l);
        if (!ok) return AVERROR(EINVAL);
        const uint8_t* in[1] = {data};
        return Convert(in, (int)(size / (2 * (size_t)m_inCh)), ptsNs, onPcm);
    }
    int err = av_new_packet(m_pkt, (int)size);
    if (err < 0) return err;
    std::memcpy(m_pkt->data, data, size);
    m_pkt->pts = ptsNs;
    err = avcodec_send_packet(m_ctx, m_pkt);
    av_packet_unref(m_pkt);
    if (err < 0) {
        if ((m_errors++ % 100) == 0) MLOGW("audio", "avcodec_send_packet: %s", AvErr(err).c_str());
        return err;
    }
    int total = 0;
    for (;;) {
        err = avcodec_receive_frame(m_ctx, m_frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) {
            if ((m_errors++ % 100) == 0) MLOGW("audio", "avcodec_receive_frame: %s", AvErr(err).c_str());
            return total ? total : err;
        }
        if (EnsureSwr((AVSampleFormat)m_frame->format, m_frame->sample_rate, &m_frame->ch_layout)) {
            int64_t pts = m_frame->pts != AV_NOPTS_VALUE ? m_frame->pts : ptsNs;
            int n = Convert((const uint8_t**)m_frame->extended_data, m_frame->nb_samples, pts, onPcm);
            if (n > 0) total += n;
        }
        av_frame_unref(m_frame);
    }
    return total;
}

void FfAudioDecoder::Flush()
{
    if (m_ctx) avcodec_flush_buffers(m_ctx);
    if (m_swr) {
        swr_close(m_swr);
        swr_init(m_swr);
    }
}

}} // namespace XboxCast::Media
