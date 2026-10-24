// xbox-cast media: FFmpeg audio decoder + resampler (portable). AAC-ELD / AAC-LC (AudioSpecificConfig), ALAC (magic
// cookie), PCM s16le -> interleaved float at the output rate/channels. Identical code on Linux (harness) and Xbox.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include "XcTypes.h"

namespace XboxCast { namespace Media {

class FfAudioDecoder {
public:
    FfAudioDecoder() = default;
    ~FfAudioDecoder() { Close(); }
    FfAudioDecoder(const FfAudioDecoder&) = delete;
    FfAudioDecoder& operator=(const FfAudioDecoder&) = delete;

    bool Open(xc_audio_codec codec, int sampleRate, int channels, const uint8_t* cfg, size_t cfgSize, int outRate,
              int outChannels);
    void Close();
    bool IsOpen() const { return m_open; }

    // pcm: interleaved float, outChannels; ptsNs of the first output sample. Returns frames produced or <0 on error.
    int Decode(const uint8_t* data, size_t size, int64_t ptsNs,
               const std::function<void(const float* pcm, int frames, int64_t ptsNs)>& onPcm);
    void Flush();
    // Soft resampling compensation: stretch (> 0) or squeeze (< 0) the output by ppm. Kept until changed.
    void SetCompensationPpm(int ppm);
    int OutRate() const { return m_outRate; }
    int OutChannels() const { return m_outCh; }
    int InRate() const { return m_inRate; }
    int InChannels() const { return m_inCh; }
    xc_audio_codec Codec() const { return m_codec; }
    static const char* CodecName(xc_audio_codec c);

    // Default configs AirPlay senders use when castcore passes none (exposed for tests).
    static std::vector<uint8_t> DefaultAsc(int objectType, int sampleRate, int channels, int frameLength);
    static std::vector<uint8_t> AlacExtradata(const uint8_t* cookie, size_t n, int sampleRate, int channels);

private:
    bool EnsureSwr(AVSampleFormat fmt, int rate, const AVChannelLayout* layout);
    int Convert(const uint8_t** in, int inFrames, int64_t ptsNs,
                const std::function<void(const float*, int, int64_t)>& onPcm);

    xc_audio_codec m_codec = XC_AUDIO_AAC_ELD;
    bool m_open = false;
    AVCodecContext* m_ctx = nullptr;
    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame = nullptr;
    SwrContext* m_swr = nullptr;
    AVSampleFormat m_swrFmt = AV_SAMPLE_FMT_NONE;
    int m_swrRate = 0;
    AVChannelLayout m_swrLayout{};
    int m_inRate = 0, m_inCh = 0, m_outRate = 48000, m_outCh = 2;
    std::vector<float> m_out;
    uint64_t m_errors = 0;
    int m_compPpm = 0;
};

}} // namespace XboxCast::Media
