// xbox-cast media: H.264 SPS helpers for the mirrored stream (portable).
//
// Why: the iPhone's SPS (High, POC type 0, 4 ref frames) has no VUI bitstream_restriction, so a conforming decoder
// must assume frames can be reordered and may hold up to a full DPB before output (screen-mirror-xbox measured
// 130-700 ms extra latency from this). iOS never sends B slices, so we rewrite the SPS in flight to declare
// max_num_reorder_frames = 0. A B slice switches the rewrite off for the rest of the stream (logged).
//
// Derived from screen-mirror-xbox (GPL-3.0, github.com/vproHacks/screen-mirror-xbox), src/app/H264Sps.{h,cpp}
// (ParseSps / RewriteSps / SliceType), adapted: namespace, Annex-B access-unit rewriting, StripRestriction for tests.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace XboxCast { namespace Media { namespace H264 {

struct SpsInfo {
    std::string text;
    bool ok = false;
    uint32_t refFrames = 0;
    bool hasVui = false;
    bool hasRestriction = false;
    uint32_t reorderFrames = 0;
    uint32_t dpbFrames = 0;
    size_t vuiFlagBit = 0;          // position of vui_parameters_present_flag in the RBSP
    size_t restrictionFlagBit = 0;  // position of bitstream_restriction_flag (when hasVui)
};

// nal: one SPS NAL unit including its header byte, without a start code
SpsInfo ParseSps(const uint8_t* nal, size_t len);

// The SPS NAL with a bitstream restriction declaring reorderFrames (0) reordering, or empty to keep it as it is
// (already has a restriction, or unparseable).
std::vector<uint8_t> RewriteSps(const uint8_t* nal, size_t len, const SpsInfo& info, uint32_t reorderFrames = 0);

// Test helper: the SPS without its bitstream_restriction (what iOS sends). Empty if it has none.
std::vector<uint8_t> StripRestriction(const uint8_t* nal, size_t len, const SpsInfo& info);

// slice_type of a slice NAL modulo 5 (0 P, 1 B, 2 I, 3 SP, 4 SI), or -1
int SliceType(const uint8_t* nal, size_t len);

// Per-stream Annex-B rewriter used by the video decoder.
class ReorderFix {
public:
    // Rewrites every SPS in the access unit (Annex-B) into out. Returns false (out untouched) when nothing changed.
    bool Process(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
    void Reset() { *this = ReorderFix(); }

    bool Disabled() const { return m_bSlices > 0; }
    int SpsRewritten() const { return m_rewritten; }
    const std::string& LastSpsText() const { return m_lastText; }

    // Splits Annex-B into NAL units (header byte included, start codes excluded).
    template <typename F>
    static void ForEachNal(const uint8_t* d, size_t n, F f)
    {
        size_t i = 0, start = (size_t)-1;
        while (i + 2 < n) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                if (start != (size_t)-1) {
                    size_t end = i;
                    while (end > start && d[end - 1] == 0) end--; // trailing zero of a 4-byte start code
                    f(d + start, end - start);
                }
                i += 3;
                start = i;
                continue;
            }
            i++;
        }
        if (start != (size_t)-1 && start < n) f(d + start, n - start);
    }

private:
    int m_bSlices = 0;
    int m_rewritten = 0;
    std::vector<uint8_t> m_lastIn, m_lastOut; // cache: iOS repeats the same SPS on every keyframe
    std::string m_lastText;
};

// True when an Annex-B access unit carries a parameter set (H.264 SPS, or HEVC VPS/SPS): a decoder can start here.
bool HasParameterSets(const uint8_t* data, size_t size, bool hevc);

}}} // namespace XboxCast::Media::H264
