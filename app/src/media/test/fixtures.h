// xbox-cast media harness: fixture helpers (libavformat demux; test-only, the app never demuxes).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "xtest.h"

namespace xt {

inline std::string Fixture(const char* name)
{
    std::string p = FixtureDir() + "/" + name;
    struct stat st;
    if (FixtureDir().empty() || stat(p.c_str(), &st) != 0) throw Skip{std::string("fixture missing: ") + name};
    return p;
}

struct Packet {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    bool key = false;
};

struct Stream {
    std::vector<Packet> packets;
    std::vector<uint8_t> extradata;
    int sampleRate = 0, channels = 0;
    int codecId = 0;
};

// Reads every packet of the first stream of `type`. Raw .h264/.hevc files come out as Annex-B access units
// (FFmpeg's parser splits them), which is exactly what castcore hands us.
inline Stream ReadStream(const std::string& path, AVMediaType type)
{
    Stream s;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) throw Skip{"cannot open " + path};
    avformat_find_stream_info(fmt, nullptr);
    int idx = av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
    if (idx < 0) {
        avformat_close_input(&fmt);
        throw Skip{"no stream in " + path};
    }
    AVStream* st = fmt->streams[idx];
    if (st->codecpar->extradata_size > 0)
        s.extradata.assign(st->codecpar->extradata, st->codecpar->extradata + st->codecpar->extradata_size);
    s.sampleRate = st->codecpar->sample_rate;
    s.channels = st->codecpar->ch_layout.nb_channels;
    s.codecId = st->codecpar->codec_id;
    AVPacket* pkt = av_packet_alloc();
    int n = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx) {
            Packet p;
            p.data.assign(pkt->data, pkt->data + pkt->size);
            int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            p.ptsUs = ts != AV_NOPTS_VALUE ? av_rescale_q(ts, st->time_base, AVRational{1, 1000000}) : (int64_t)n * 16667;
            p.key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            s.packets.push_back(std::move(p));
            n++;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return s;
}

} // namespace xt
