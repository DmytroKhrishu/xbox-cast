/* xbox-cast media: TEMPORARY copy of the castcore public types from docs/contract.md section 1.
 * Used only while native/include/castcore.h is not on main / not on the include path.
 * Linux harness only: in the app, core/ReceiverHost.h provides the same fallback (one definition per program).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef XBOXCAST_CASTCORE_TYPES_FALLBACK_H
#define XBOXCAST_CASTCORE_TYPES_FALLBACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XC_API_VERSION
#define XC_API_VERSION 1
#endif

typedef enum { XC_VIDEO_H264 = 1, XC_VIDEO_H265 = 2 } xc_video_codec;
typedef enum { XC_AUDIO_AAC_ELD = 1, XC_AUDIO_ALAC = 2, XC_AUDIO_AAC_LC = 3, XC_AUDIO_PCM = 4 } xc_audio_codec;

typedef struct {
    const uint8_t *data;
    size_t         size;
    int64_t        pts_ns;
    int            is_keyframe;
    xc_video_codec codec;
    int            width, height;
} xc_video_frame;

typedef struct {
    const uint8_t *data;
    size_t         size;
    int64_t        pts_ns;
} xc_audio_packet;

#ifdef __cplusplus
}
#endif

#endif
