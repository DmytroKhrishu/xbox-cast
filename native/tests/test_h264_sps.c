/*
 * Unit test for src/h264_sps.c (xbox-cast castcore). GPL-3.0-or-later.
 * Approach from test/h264sps/h264sps_test.cpp of github.com/vproHacks/screen-mirror-xbox (GPL-3.0).
 *
 * Takes a real x264 stream (tests/data), makes iOS-like SPS variants from it (no bitstream
 * restriction; no VUI at all), checks that xc_h264_rewrite_sps() turns them back into SPSs that
 * FFmpeg's H.264 decoder accepts with zero reordering delay: every frame decodes, and each frame
 * comes out as soon as its packet is sent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include "h264_sps.h"
#include "data/x264_96x64_h264.h"

static int failures;
#define CHECK(cond, what) do { int ok_ = (cond); printf("  %s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

typedef struct { const uint8_t *p; size_t n; } span;

/* splits the Annex-B test stream into NALs (without start codes) */
static size_t split(const uint8_t *d, size_t len, span *out, size_t max)
{
    size_t n = 0, i = 0, start = (size_t) -1;
    while (i + 3 <= len) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            size_t end = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
            if (start != (size_t) -1 && n < max) { out[n].p = d + start; out[n].n = end - start; n++; }
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (start != (size_t) -1 && n < max) { out[n].p = d + start; out[n].n = len - start; n++; }
    return n;
}

/* RBSP of the NAL cut after `bit` bits, then a 0 flag and the stop bit, re-escaped */
static uint8_t *cut_sps(const span *sps, size_t bit, size_t *out_len)
{
    uint8_t rbsp[256], tmp[256], *out;
    size_t rn = 0, pos = 0, n = 0;
    int zeros = 0;
    for (size_t i = 1; i < sps->n; i++) {
        if (i + 2 < sps->n && sps->p[i] == 0 && sps->p[i + 1] == 0 && sps->p[i + 2] == 3) { rbsp[rn++] = 0; rbsp[rn++] = 0; i += 2; continue; }
        rbsp[rn++] = sps->p[i];
    }
    memset(tmp, 0, sizeof(tmp));
#define PUT(v) do { if (v) tmp[pos / 8] |= (uint8_t) (0x80 >> (pos % 8)); pos++; } while (0)
    for (size_t i = 0; i < bit; i++) PUT((rbsp[i / 8] >> (7 - i % 8)) & 1);
    PUT(0);
    PUT(1);
    while (pos % 8) PUT(0);
    out = (uint8_t *) malloc(2 * (pos / 8) + 2);
    out[n++] = sps->p[0];
    for (size_t i = 0; i < pos / 8; i++) {
        if (zeros >= 2 && tmp[i] <= 3) { out[n++] = 3; zeros = 0; }
        out[n++] = tmp[i];
        zeros = tmp[i] == 0 ? zeros + 1 : 0;
    }
    *out_len = n;
    return out;
}

/* decodes the stream with the given SPS; returns decoded frames, worst packets-in-flight, has_b_frames */
static void decode(const span *nals, size_t count, const uint8_t *sps, size_t sps_len,
                   int *decoded, int *max_delay, int *reorder)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    uint8_t au[4096];
    int sent = 0, first = 1;
    *decoded = *max_delay = 0;
    avcodec_open2(ctx, codec, NULL);
    for (size_t i = 0; i < count; i++) {
        int type = nals[i].p[0] & 0x1f;
        size_t n = 0;
        if (type != 1 && type != 5) continue;
        if (first) {
            memcpy(au + n, sc, 4); n += 4; memcpy(au + n, sps, sps_len); n += sps_len;
            for (size_t j = 0; j < count; j++) {
                if ((nals[j].p[0] & 0x1f) == 8) { memcpy(au + n, sc, 4); n += 4; memcpy(au + n, nals[j].p, nals[j].n); n += nals[j].n; }
            }
            first = 0;
        }
        memcpy(au + n, sc, 4); n += 4; memcpy(au + n, nals[i].p, nals[i].n); n += nals[i].n;
        av_new_packet(pkt, (int) n);
        memcpy(pkt->data, au, n);
        if (avcodec_send_packet(ctx, pkt) == 0) sent++;
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            (*decoded)++;
            if (sent - *decoded > *max_delay) *max_delay = sent - *decoded;
            av_frame_unref(frame);
        }
    }
    *reorder = ctx->has_b_frames;
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) == 0) { (*decoded)++; av_frame_unref(frame); }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
}

static void run_case(const char *name, const span *nals, size_t count, const uint8_t *sps, size_t sps_len, int slices)
{
    xc_h264_sps info, info2;
    uint8_t *fixed;
    size_t fixed_len = 0;
    int dec, delay, reorder;
    printf("%s\n", name);
    CHECK(xc_h264_parse_sps(sps, sps_len, &info) == 0, "input SPS parses");
    printf("       %s\n", info.text);
    CHECK(info.width == 96 && info.height == 64, "coded size 96x64");
    fixed = xc_h264_rewrite_sps(sps, sps_len, &info, 0, &fixed_len);
    CHECK(fixed != NULL, "rewrite produced an SPS");
    if (!fixed) return;
    CHECK(xc_h264_parse_sps(fixed, fixed_len, &info2) == 0, "rewritten SPS parses");
    printf("       %s\n", info2.text);
    CHECK(info2.has_vui && info2.has_restriction && info2.reorder_frames == 0, "rewritten SPS has max_num_reorder_frames = 0");
    CHECK(info2.width == info.width && info2.height == info.height && info2.ref_frames == info.ref_frames,
          "size and ref frames unchanged");
    CHECK(xc_h264_rewrite_sps(fixed, fixed_len, &info2, 0, &fixed_len) == NULL, "already-restricted SPS is left alone");
    free(fixed);
    fixed = xc_h264_rewrite_sps(sps, sps_len, &info, 0, &fixed_len);   /* the check above reset fixed_len */
    decode(nals, count, fixed, fixed_len, &dec, &delay, &reorder);
    printf("       FFmpeg: %d/%d frames, max delay %d packets, has_b_frames %d\n", dec, slices, delay, reorder);
    CHECK(dec == slices, "FFmpeg decodes every frame");
    CHECK(reorder == 0 && delay == 0, "FFmpeg outputs each frame immediately (no reordering delay)");
    free(fixed);
}

int main(void)
{
    span nals[64];
    size_t count = split(x264_96x64_h264, sizeof(x264_96x64_h264), nals, 64);
    const span *sps = NULL;
    int slices = 0;
    xc_h264_sps info;
    for (size_t i = 0; i < count; i++) {
        int t = nals[i].p[0] & 0x1f;
        if (t == 7 && !sps) sps = &nals[i];
        if (t == 1 || t == 5) slices++;
    }
    printf("test stream: %zu NALs, %d slices\n", count, slices);
    if (!sps || slices != 10) { printf("FAIL test stream\n"); return 1; }

    CHECK(xc_h264_parse_sps(sps->p, sps->n, &info) == 0 && info.has_vui && info.has_restriction,
          "x264 SPS has a VUI with bitstream restriction");
    printf("       %s\n", info.text);
    CHECK(xc_h264_slice_type(nals[count - 1].p, nals[count - 1].n) == 0, "last slice is a P slice");
    {
        size_t len;
        uint8_t *no_restr = cut_sps(sps, info.restriction_flag_bit, &len);
        run_case("case: VUI without bitstream restriction (like iOS)", nals, count, no_restr, len, slices);
        free(no_restr);
        uint8_t *no_vui = cut_sps(sps, info.vui_flag_bit, &len);
        run_case("case: no VUI at all", nals, count, no_vui, len, slices);
        free(no_vui);
    }
    {
        /* reference: an iOS-like SPS without restriction makes FFmpeg assume reordering (worst case) */
        size_t len;
        int dec, delay, reorder;
        uint8_t *no_restr = cut_sps(sps, info.restriction_flag_bit, &len);
        decode(nals, count, no_restr, len, &dec, &delay, &reorder);
        printf("reference (unrewritten iOS-like SPS): %d frames, max delay %d packets, has_b_frames %d\n", dec, delay, reorder);
        free(no_restr);
    }
    printf(failures ? "\n%d FAILURES\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
