/*
 * castcore: H.264 SPS parsing and the "no frame reordering" rewrite.
 * Copyright (C) 2026 xbox-cast contributors. GPL-3.0-or-later.
 *
 * C port of src/app/H264Sps.cpp from github.com/vproHacks/screen-mirror-xbox (GPL-3.0).
 *
 * Why: the iPhone's SPS (High profile, POC type 0, 4 reference frames) has no VUI bitstream
 * restriction, so a conforming decoder must assume frames may be reordered and can hold up to a
 * full DPB before outputting each one. screen-mirror-xbox measured 130-700 ms extra latency on
 * Xbox (Media Foundation) because of it. iOS's real-time encoder never sends B slices, so the SPS
 * can declare max_num_reorder_frames = 0 and every decoder outputs a frame as soon as it is decoded.
 */
#include "h264_sps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *d;
    size_t len;
    size_t bit;
    int over;
} bit_reader;

static uint32_t br_u(bit_reader *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->bit++) {
        if (b->bit / 8 >= b->len) {
            b->over = 1;
            return v;
        }
        v = (v << 1) | ((b->d[b->bit / 8] >> (7 - b->bit % 8)) & 1);
    }
    return v;
}

static uint32_t br_ue(bit_reader *b)
{
    int zeros = 0;
    while (br_u(b, 1) == 0 && !b->over && zeros < 31) {
        zeros++;
    }
    return ((1u << zeros) - 1) + br_u(b, zeros);
}

static int32_t br_se(bit_reader *b)
{
    uint32_t k = br_ue(b);
    return (k & 1) ? (int32_t) ((k + 1) / 2) : -(int32_t) (k / 2);
}

typedef struct {
    uint8_t *d;
    size_t cap;
    size_t bit;
    int oom;
} bit_writer;

static void bw_u(bit_writer *w, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, w->bit++) {
        size_t byte = w->bit / 8;
        if (byte >= w->cap) {
            w->oom = 1;
            return;
        }
        if (w->bit % 8 == 0) {
            w->d[byte] = 0;
        }
        if ((v >> i) & 1) {
            w->d[byte] |= (uint8_t) (0x80 >> (w->bit % 8));
        }
    }
}

static void bw_ue(bit_writer *w, uint32_t v)
{
    uint32_t x = v + 1;
    int len = 0;
    for (uint32_t t = x; t > 1; t >>= 1) {
        len++;
    }
    bw_u(w, 0, len);
    bw_u(w, x, len + 1);
}

/* NAL unit -> RBSP: drops the header byte and the emulation-prevention bytes. Caller frees. */
static uint8_t *unescape(const uint8_t *nal, size_t len, size_t *out_len)
{
    uint8_t *out = (uint8_t *) malloc(len ? len : 1);
    size_t n = 0;
    if (!out) {
        *out_len = 0;
        return NULL;
    }
    for (size_t i = 1; i < len; i++) {
        if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            out[n++] = 0;
            out[n++] = 0;
            i += 2;
            continue;
        }
        out[n++] = nal[i];
    }
    *out_len = n;
    return out;
}

static void skip_hrd(bit_reader *b)
{
    uint32_t count = br_ue(b) + 1;
    br_u(b, 8); /* bit_rate_scale, cpb_size_scale */
    for (uint32_t i = 0; i < count && !b->over; i++) {
        br_ue(b);
        br_ue(b);
        br_u(b, 1);
    }
    br_u(b, 20);
}

int xc_h264_parse_sps(const uint8_t *nal, size_t len, xc_h264_sps *info)
{
    bit_reader b;
    memset(info, 0, sizeof(*info));
    memset(&b, 0, sizeof(b));
    if (len < 4) {
        snprintf(info->text, sizeof(info->text), "SPS too short (%u bytes)", (unsigned) len);
        return -1;
    }
    b.d = unescape(nal, len, &b.len);
    if (!b.d) {
        return -1;
    }
    uint32_t profile = br_u(&b, 8), constraints = br_u(&b, 8), level = br_u(&b, 8);
    br_ue(&b); /* seq_parameter_set_id */
    uint32_t chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
        chroma = br_ue(&b);
        if (chroma == 3) br_u(&b, 1);
        br_ue(&b);
        br_ue(&b);
        br_u(&b, 1);
        if (br_u(&b, 1)) { /* scaling matrices */
            for (int i = 0; i < (chroma == 3 ? 12 : 8) && !b.over; i++) {
                if (br_u(&b, 1)) {
                    int size = i < 6 ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < size && next != 0 && !b.over; j++) {
                        next = (last + br_se(&b) + 256) % 256;
                        if (next) last = next;
                    }
                }
            }
        }
    }
    br_ue(&b); /* log2_max_frame_num_minus4 */
    uint32_t poc_type = br_ue(&b);
    if (poc_type == 0) {
        br_ue(&b);
    } else if (poc_type == 1) {
        br_u(&b, 1);
        br_se(&b);
        br_se(&b);
        uint32_t n = br_ue(&b);
        for (uint32_t i = 0; i < n && !b.over; i++) br_se(&b);
    }
    info->ref_frames = br_ue(&b);
    br_u(&b, 1);
    uint32_t w_mbs = br_ue(&b) + 1, h_maps = br_ue(&b) + 1;
    uint32_t frame_mbs_only = br_u(&b, 1);
    if (!frame_mbs_only) br_u(&b, 1);
    br_u(&b, 1);
    uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (br_u(&b, 1)) {
        crop_l = br_ue(&b);
        crop_r = br_ue(&b);
        crop_t = br_ue(&b);
        crop_b = br_ue(&b);
    }
    {
        /* coded size after cropping (4:2:0 / 4:2:2 / 4:4:4, progressive or interlaced) */
        uint32_t sub_w = (chroma == 1 || chroma == 2) ? 2 : 1;
        uint32_t sub_h = (chroma == 1) ? 2 : 1;
        uint32_t crop_unit_x = chroma ? sub_w : 1;
        uint32_t crop_unit_y = (chroma ? sub_h : 1) * (2 - frame_mbs_only);
        int64_t w = (int64_t) w_mbs * 16 - (int64_t) crop_unit_x * (crop_l + crop_r);
        int64_t h = (int64_t) h_maps * 16 * (2 - frame_mbs_only) - (int64_t) crop_unit_y * (crop_t + crop_b);
        info->width = (w > 0 && w < 65536) ? (int) w : 0;
        info->height = (h > 0 && h < 65536) ? (int) h : 0;
    }
    int n = snprintf(info->text, sizeof(info->text),
                     "profile %u (constraints 0x%02x) level %u.%u, %dx%d%s, ref frames %u, POC type %u",
                     profile, constraints, level / 10, level % 10, info->width, info->height,
                     frame_mbs_only ? "" : " (interlaced)", info->ref_frames, poc_type);
    if (n < 0 || n >= (int) sizeof(info->text)) n = (int) sizeof(info->text) - 1;
    info->vui_flag_bit = b.bit;
    info->has_vui = (int) br_u(&b, 1);
    if (b.over) {
        snprintf(info->text + n, sizeof(info->text) - n, ", truncated");
        free(b.d);
        return -1;
    }
    if (!info->has_vui) {
        snprintf(info->text + n, sizeof(info->text) - n, ", no VUI");
        info->ok = 1;
        free(b.d);
        return 0;
    }
    if (br_u(&b, 1) && br_u(&b, 8) == 255) br_u(&b, 32); /* aspect ratio */
    if (br_u(&b, 1)) br_u(&b, 1);                        /* overscan */
    if (br_u(&b, 1)) {                                   /* video signal type */
        br_u(&b, 4);
        if (br_u(&b, 1)) br_u(&b, 24);
    }
    if (br_u(&b, 1)) { /* chroma location */
        br_ue(&b);
        br_ue(&b);
    }
    int timing = (int) br_u(&b, 1);
    if (timing) {
        br_u(&b, 32);
        br_u(&b, 32);
        br_u(&b, 1);
    }
    int nal_hrd = (int) br_u(&b, 1);
    if (nal_hrd) skip_hrd(&b);
    int vcl_hrd = (int) br_u(&b, 1);
    if (vcl_hrd) skip_hrd(&b);
    if (nal_hrd || vcl_hrd) br_u(&b, 1);
    br_u(&b, 1); /* pic_struct_present */
    info->restriction_flag_bit = b.bit;
    info->has_restriction = (int) br_u(&b, 1);
    if (info->has_restriction) {
        br_u(&b, 1);
        for (int i = 0; i < 4; i++) br_ue(&b);
        info->reorder_frames = br_ue(&b);
        uint32_t dpb = br_ue(&b);
        snprintf(info->text + n, sizeof(info->text) - n, ", timing %s, reorder frames %u, DPB frames %u",
                 timing ? "yes" : "no", info->reorder_frames, dpb);
    } else {
        snprintf(info->text + n, sizeof(info->text) - n, ", timing %s, no bitstream restriction",
                 timing ? "yes" : "no");
    }
    info->ok = !b.over;
    free(b.d);
    return info->ok ? 0 : -1;
}

uint8_t *xc_h264_rewrite_sps(const uint8_t *nal, size_t len, const xc_h264_sps *info, uint32_t reorder_frames,
                             size_t *out_len)
{
    bit_reader in;
    bit_writer out;
    uint8_t *rbsp_out = NULL, *result = NULL;
    size_t keep, n = 0;
    *out_len = 0;
    if (!info->ok || (info->has_vui && info->has_restriction)) {
        return NULL;
    }
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.d = unescape(nal, len, &in.len);
    if (!in.d) {
        return NULL;
    }
    out.cap = in.len + 32;
    out.d = rbsp_out = (uint8_t *) calloc(1, out.cap);
    if (!out.d) {
        free(in.d);
        return NULL;
    }
    keep = info->has_vui ? info->restriction_flag_bit : info->vui_flag_bit;
    for (size_t i = 0; i < keep; i++) bw_u(&out, br_u(&in, 1), 1);
    if (!info->has_vui) {
        bw_u(&out, 1, 1); /* vui_parameters_present_flag */
        bw_u(&out, 0, 8); /* no aspect ratio, overscan, signal type, chroma loc, timing, NAL HRD, VCL HRD, pic_struct */
    }
    bw_u(&out, 1, 1);  /* bitstream_restriction_flag */
    bw_u(&out, 1, 1);  /* motion_vectors_over_pic_boundaries_flag */
    bw_ue(&out, 2);    /* max_bytes_per_pic_denom (default) */
    bw_ue(&out, 1);    /* max_bits_per_mb_denom (default) */
    bw_ue(&out, 16);   /* log2_max_mv_length_horizontal (default) */
    bw_ue(&out, 16);   /* log2_max_mv_length_vertical (default) */
    bw_ue(&out, reorder_frames);                                /* max_num_reorder_frames */
    bw_ue(&out, info->ref_frames > 1 ? info->ref_frames : 1);   /* max_dec_frame_buffering */
    bw_u(&out, 1, 1);  /* rbsp_stop_one_bit */
    while (out.bit % 8) bw_u(&out, 0, 1);
    free(in.d);
    if (out.oom || in.over) {
        free(rbsp_out);
        return NULL;
    }
    /* header byte + RBSP with emulation prevention (worst case +50%) */
    size_t rbsp_len = out.bit / 8;
    result = (uint8_t *) malloc(1 + rbsp_len + rbsp_len / 2 + 2);
    if (!result) {
        free(rbsp_out);
        return NULL;
    }
    result[n++] = nal[0];
    int zeros = 0;
    for (size_t i = 0; i < rbsp_len; i++) {
        uint8_t byte = rbsp_out[i];
        if (zeros >= 2 && byte <= 3) {
            result[n++] = 3;
            zeros = 0;
        }
        result[n++] = byte;
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    free(rbsp_out);
    *out_len = n;
    return result;
}

int xc_h264_slice_type(const uint8_t *nal, size_t len)
{
    bit_reader b;
    memset(&b, 0, sizeof(b));
    b.d = unescape(nal, len < 16 ? len : 16, &b.len);
    if (!b.d) return -1;
    br_ue(&b); /* first_mb_in_slice */
    uint32_t t = br_ue(&b);
    int over = b.over;
    free(b.d);
    return over ? -1 : (int) (t % 5);
}
