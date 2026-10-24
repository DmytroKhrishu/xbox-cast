// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from screen-mirror-xbox (GPL-3.0, github.com/vproHacks/screen-mirror-xbox), src/app/H264Sps.cpp.
#include "pch.h"
#include "H264Sps.h"

#include <algorithm>
#include <cstdio>

#include "MediaLog.h"

namespace XboxCast { namespace Media { namespace H264 {

namespace {
// Exp-Golomb reader over an RBSP (emulation-prevention bytes removed)
struct BitReader {
    std::vector<uint8_t> d;
    size_t bit = 0;
    bool over = false;
    uint32_t U(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, bit++) {
            if (bit / 8 >= d.size()) {
                over = true;
                return v;
            }
            v = (v << 1) | ((d[bit / 8] >> (7 - bit % 8)) & 1);
        }
        return v;
    }
    uint32_t Ue()
    {
        int zeros = 0;
        while (U(1) == 0 && !over && zeros < 31) zeros++;
        return ((1u << zeros) - 1) + U(zeros);
    }
    int32_t Se()
    {
        uint32_t k = Ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }
};

struct BitWriter {
    std::vector<uint8_t> d;
    size_t bit = 0;
    void U(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--, bit++) {
            if (bit % 8 == 0) d.push_back(0);
            if ((v >> i) & 1) d.back() |= (uint8_t)(0x80 >> (bit % 8));
        }
    }
    void Ue(uint32_t v)
    {
        uint32_t x = v + 1;
        int len = 0;
        for (uint32_t t = x; t > 1; t >>= 1) len++;
        U(0, len);
        U(x, len + 1);
    }
};

// NAL unit -> RBSP: drops the header byte and the emulation-prevention bytes
std::vector<uint8_t> Unescape(const uint8_t* nal, size_t len)
{
    std::vector<uint8_t> out;
    out.reserve(len);
    for (size_t i = 1; i < len; i++) {
        if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            out.push_back(0);
            out.push_back(0);
            i += 2;
            continue;
        }
        out.push_back(nal[i]);
    }
    return out;
}

std::vector<uint8_t> Escape(uint8_t header, const std::vector<uint8_t>& rbsp)
{
    std::vector<uint8_t> result{header};
    int zeros = 0;
    for (uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            result.push_back(3); // emulation prevention
            zeros = 0;
        }
        result.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return result;
}

void SkipHrd(BitReader& b)
{
    uint32_t count = b.Ue() + 1;
    b.U(8); // bit_rate_scale, cpb_size_scale
    for (uint32_t i = 0; i < count && !b.over; i++) {
        b.Ue();
        b.Ue();
        b.U(1);
    }
    b.U(20);
}
} // namespace

SpsInfo ParseSps(const uint8_t* nal, size_t len)
{
    SpsInfo info;
    BitReader b;
    b.d = Unescape(nal, len);
    uint32_t profile = b.U(8), constraints = b.U(8), level = b.U(8);
    b.Ue(); // seq_parameter_set_id
    uint32_t chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
        chroma = b.Ue();
        if (chroma == 3) b.U(1);
        b.Ue();
        b.Ue();
        b.U(1);
        if (b.U(1)) // scaling matrices
            for (int i = 0; i < (chroma == 3 ? 12 : 8) && !b.over; i++)
                if (b.U(1)) {
                    int size = i < 6 ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < size && next != 0 && !b.over; j++) {
                        next = (last + b.Se() + 256) % 256;
                        if (next) last = next;
                    }
                }
    }
    b.Ue(); // log2_max_frame_num_minus4
    uint32_t pocType = b.Ue();
    if (pocType == 0) {
        b.Ue();
    } else if (pocType == 1) {
        b.U(1);
        b.Se();
        b.Se();
        uint32_t n = b.Ue();
        for (uint32_t i = 0; i < n && !b.over; i++) b.Se();
    }
    info.refFrames = b.Ue();
    b.U(1);
    uint32_t wMbs = b.Ue() + 1, hMaps = b.Ue() + 1;
    uint32_t frameMbsOnly = b.U(1);
    if (!frameMbsOnly) b.U(1);
    b.U(1);
    if (b.U(1))
        for (int i = 0; i < 4; i++) b.Ue(); // cropping
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
                          "profile %u (constraints 0x%02x) level %u.%u, %ux%u MBs%s, ref frames %u, POC type %u", profile,
                          constraints, level / 10, level % 10, wMbs, hMaps * (2 - frameMbsOnly),
                          frameMbsOnly ? "" : " (interlaced)", info.refFrames, pocType);
    if (n < 0 || n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    info.vuiFlagBit = b.bit;
    info.hasVui = b.U(1) != 0;
    if (b.over) {
        info.text = std::string(buf) + ", truncated";
        return info;
    }
    if (!info.hasVui) {
        std::snprintf(buf + n, sizeof(buf) - n, ", no VUI");
        info.text = buf;
        info.ok = true;
        return info;
    }
    if (b.U(1) && b.U(8) == 255) b.U(32); // aspect ratio
    if (b.U(1)) b.U(1);                   // overscan
    if (b.U(1)) {                         // video signal type
        b.U(4);
        if (b.U(1)) b.U(24);
    }
    if (b.U(1)) { // chroma location
        b.Ue();
        b.Ue();
    }
    bool timing = b.U(1) != 0;
    if (timing) {
        b.U(32);
        b.U(32);
        b.U(1);
    }
    bool nalHrd = b.U(1) != 0;
    if (nalHrd) SkipHrd(b);
    bool vclHrd = b.U(1) != 0;
    if (vclHrd) SkipHrd(b);
    if (nalHrd || vclHrd) b.U(1);
    b.U(1); // pic_struct_present
    info.restrictionFlagBit = b.bit;
    info.hasRestriction = b.U(1) != 0;
    if (info.hasRestriction) {
        b.U(1);
        for (int i = 0; i < 4; i++) b.Ue();
        info.reorderFrames = b.Ue();
        info.dpbFrames = b.Ue();
        std::snprintf(buf + n, sizeof(buf) - n, ", timing %s, reorder frames %u, DPB frames %u", timing ? "yes" : "no",
                      info.reorderFrames, info.dpbFrames);
    } else {
        std::snprintf(buf + n, sizeof(buf) - n, ", timing %s, no bitstream restriction", timing ? "yes" : "no");
    }
    info.ok = !b.over;
    info.text = buf;
    return info;
}

std::vector<uint8_t> RewriteSps(const uint8_t* nal, size_t len, const SpsInfo& info, uint32_t reorderFrames)
{
    if (!info.ok || (info.hasVui && info.hasRestriction)) return {};
    BitReader in;
    in.d = Unescape(nal, len);
    BitWriter out;
    size_t keep = info.hasVui ? info.restrictionFlagBit : info.vuiFlagBit;
    for (size_t i = 0; i < keep; i++) out.U(in.U(1), 1);
    if (!info.hasVui) {
        out.U(1, 1); // vui_parameters_present_flag
        out.U(0, 8); // no aspect ratio, overscan, signal type, chroma location, timing, NAL HRD, VCL HRD, pic_struct
    }
    out.U(1, 1);  // bitstream_restriction_flag
    out.U(1, 1);  // motion_vectors_over_pic_boundaries_flag
    out.Ue(2);    // max_bytes_per_pic_denom (default)
    out.Ue(1);    // max_bits_per_mb_denom (default)
    out.Ue(16);   // log2_max_mv_length_horizontal (default)
    out.Ue(16);   // log2_max_mv_length_vertical (default)
    out.Ue(reorderFrames);                       // max_num_reorder_frames
    out.Ue((std::max)(info.refFrames, 1u));      // max_dec_frame_buffering (at least the reference frames)
    out.U(1, 1);  // rbsp_stop_one_bit
    while (out.bit % 8) out.U(0, 1);
    return Escape(nal[0], out.d);
}

std::vector<uint8_t> StripRestriction(const uint8_t* nal, size_t len, const SpsInfo& info)
{
    if (!info.ok || !info.hasVui || !info.hasRestriction) return {};
    BitReader in;
    in.d = Unescape(nal, len);
    BitWriter out;
    for (size_t i = 0; i < info.restrictionFlagBit; i++) out.U(in.U(1), 1);
    out.U(0, 1); // bitstream_restriction_flag
    out.U(1, 1); // rbsp_stop_one_bit
    while (out.bit % 8) out.U(0, 1);
    return Escape(nal[0], out.d);
}

int SliceType(const uint8_t* nal, size_t len)
{
    BitReader b;
    b.d = Unescape(nal, (std::min<size_t>)(len, 16));
    b.Ue(); // first_mb_in_slice
    uint32_t t = b.Ue();
    return b.over ? -1 : (int)(t % 5);
}

bool ReorderFix::Process(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
    // Pass 1: look for B slices (turns the rewrite off) and SPS NALs that need rewriting.
    bool needs = false;
    ForEachNal(data, size, [&](const uint8_t* nal, size_t len) {
        if (len < 2) return;
        int type = nal[0] & 0x1f;
        if ((type == 1 || type == 5) && SliceType(nal, len) == 1) {
            if (m_bSlices++ == 0)
                MLOGW("video", "B slice in the stream: SPS no-reorder rewrite DISABLED for this stream");
        }
        if (type == 7) needs = true;
    });
    if (!needs || m_bSlices > 0) return false;

    // Pass 2: rebuild the access unit with rewritten SPS NALs.
    bool changed = false;
    std::vector<uint8_t> result;
    result.reserve(size + 16);
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    ForEachNal(data, size, [&](const uint8_t* nal, size_t len) {
        result.insert(result.end(), kStart, kStart + 4);
        if (len >= 2 && (nal[0] & 0x1f) == 7) {
            if (m_lastIn.size() == len && std::equal(nal, nal + len, m_lastIn.begin())) {
                if (!m_lastOut.empty()) {
                    result.insert(result.end(), m_lastOut.begin(), m_lastOut.end());
                    changed = true;
                    return;
                }
            } else {
                SpsInfo info = ParseSps(nal, len);
                m_lastIn.assign(nal, nal + len);
                m_lastOut = RewriteSps(nal, len, info, 0);
                m_lastText = info.text;
                if (!m_lastOut.empty()) {
                    m_rewritten++;
                    MLOGI("video", "SPS %s -> rewritten to max_num_reorder_frames 0 (decoder outputs each frame at once)",
                          info.text.c_str());
                    result.insert(result.end(), m_lastOut.begin(), m_lastOut.end());
                    changed = true;
                    return;
                }
                MLOGI("video", "SPS %s (kept as is)", info.text.c_str());
            }
        }
        result.insert(result.end(), nal, nal + len);
    });
    if (!changed) return false;
    out.swap(result);
    return true;
}

bool HasParameterSets(const uint8_t* data, size_t size, bool hevc)
{
    bool found = false;
    ReorderFix::ForEachNal(data, size, [&](const uint8_t* nal, size_t len) {
        if (len < 1) return;
        if (hevc) {
            int t = (nal[0] >> 1) & 0x3f;
            if (t == 32 || t == 33) found = true; // VPS, SPS
        } else if ((nal[0] & 0x1f) == 7) {
            found = true;
        }
    });
    return found;
}

}}} // namespace XboxCast::Media::H264
