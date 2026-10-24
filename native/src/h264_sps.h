/*
 * castcore: H.264 SPS helpers (internal). GPL-3.0-or-later.
 * C port of src/app/H264Sps.h from github.com/vproHacks/screen-mirror-xbox (GPL-3.0).
 */
#ifndef XC_H264_SPS_H
#define XC_H264_SPS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     text[200];             /* human readable summary for the log */
    int      ok;
    int      width, height;         /* coded size after cropping */
    uint32_t ref_frames;
    int      has_vui;
    int      has_restriction;
    uint32_t reorder_frames;
    size_t   vui_flag_bit;          /* position of vui_parameters_present_flag in the RBSP */
    size_t   restriction_flag_bit;  /* position of bitstream_restriction_flag (when has_vui) */
} xc_h264_sps;

/* nal: one SPS NAL unit including its header byte, without start code. 0 = parsed. */
int xc_h264_parse_sps(const uint8_t *nal, size_t len, xc_h264_sps *info);

/* Returns a malloc'ed SPS NAL (header byte included) that declares max_num_reorder_frames =
   reorder_frames via a VUI bitstream restriction, or NULL to keep the SPS as it is (already
   restricted, unparseable). */
uint8_t *xc_h264_rewrite_sps(const uint8_t *nal, size_t len, const xc_h264_sps *info, uint32_t reorder_frames,
                             size_t *out_len);

/* slice_type of a slice NAL (header byte included) modulo 5 (0 P, 1 B, 2 I, 3 SP, 4 SI), or -1 */
int xc_h264_slice_type(const uint8_t *nal, size_t len);

#endif
