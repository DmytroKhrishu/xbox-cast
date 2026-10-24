/*
 * castcore: AirPlay screen-mirroring + audio receiver core of xbox-cast.
 * Copyright (C) 2026 xbox-cast contributors. GPL-3.0-or-later (see LICENSE).
 *
 * Public C API, frozen by docs/contract.md section 1. Pure C, no platform headers.
 * Additions go at the END of structs / as new functions and bump XC_API_VERSION.
 */
#ifndef CASTCORE_H
#define CASTCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XC_API_VERSION 1

typedef enum { XC_LOG_DEBUG, XC_LOG_INFO, XC_LOG_WARN, XC_LOG_ERROR } xc_log_level;
typedef enum { XC_VIDEO_H264 = 1, XC_VIDEO_H265 = 2 } xc_video_codec;
typedef enum { XC_AUDIO_AAC_ELD = 1, XC_AUDIO_ALAC = 2, XC_AUDIO_AAC_LC = 3, XC_AUDIO_PCM = 4 } xc_audio_codec;

typedef struct { const char *key; const char *value; } xc_txt_entry;

typedef struct {
    const char    *name;            /* advertised name, UTF-8 */
    unsigned char  device_id[6];    /* stable random id (app persists it); used as AirPlay deviceid / RAOP MAC prefix */
    int            display_width;   /* advertised display (what iOS encodes for), e.g. 1920 */
    int            display_height;  /* e.g. 1080 */
    int            display_refresh; /* 60 */
    int            max_fps;         /* 60 */
    int            allow_h265;      /* advertise HEVC (4K-capable senders) */
    int            pin_mode;        /* 0 = none, 1 = on-screen PIN every connection */
    unsigned short port;            /* 0 = default 7000 */
    const char    *data_dir;        /* writable dir for the persistent ed25519 identity / pairing data */
    int            builtin_mdns;    /* 1 = castcore advertises itself with its own mDNS responder (UDP 5353, multicast,
                                       SO_REUSEADDR) and never calls mdns_publish. Fallback for platforms where the OS
                                       DNS-SD API can't advertise. DEFAULT on Xbox (setting `mdnsMode`).
                                       Must include the Xbox fixes: accept adapters named "Durango Virtual
                                       Network ... Adapter", host name `<host>-xc.local` (never the console's
                                       own name), stop the thread before closing sockets (suspend crash). */
} xc_config;

typedef struct {
    const uint8_t *data;            /* Annex-B, start codes, SPS/PPS(/VPS) prepended on keyframes and format changes */
    size_t         size;
    int64_t        pts_ns;          /* presentation time on the xc_now_ns() clock */
    int            is_keyframe;
    xc_video_codec codec;
    int            width, height;   /* coded size from the stream (0 if unknown) */
} xc_video_frame;

typedef struct {
    const uint8_t *data;            /* one decrypted compressed access unit (raw AAC/ALAC, no ADTS) or PCM s16le */
    size_t         size;
    int64_t        pts_ns;          /* xc_now_ns() clock */
} xc_audio_packet;

typedef struct {
    void *ctx;
    /* platform services (required) */
    int  (*mdns_publish)(void *ctx, const char *service_type /* "_airplay._tcp" | "_raop._tcp" */,
                         const char *instance_name, unsigned short port,
                         const xc_txt_entry *txt, size_t txt_count);   /* return 0 on success */
    void (*mdns_unpublish_all)(void *ctx);
    /* events (all optional) */
    void (*on_log)(void *ctx, xc_log_level level, const char *msg);
    void (*on_client_connected)(void *ctx, const char *client_name, const char *client_model, const char *client_addr);
    void (*on_client_disconnected)(void *ctx, const char *reason);
    void (*on_pin)(void *ctx, const char *pin);              /* show on screen; NULL/"" = hide */
    void (*on_video_format)(void *ctx, xc_video_codec codec, int width, int height);
    void (*on_video)(void *ctx, const xc_video_frame *frame);
    void (*on_video_flush)(void *ctx);                       /* stream reset (rotation/teardown): drop queued frames */
    void (*on_audio_format)(void *ctx, xc_audio_codec codec, int sample_rate, int channels,
                            const uint8_t *codec_config, size_t codec_config_size); /* AudioSpecificConfig / ALAC magic cookie */
    void (*on_audio)(void *ctx, const xc_audio_packet *packet);
    void (*on_audio_flush)(void *ctx);
    void (*on_volume)(void *ctx, float volume_db);            /* AirPlay volume: -144 = mute, -30..0 */
    void (*on_stats)(void *ctx, const char *json);            /* ~1 Hz: bitrate, fps in, packet loss, ntp offset */
} xc_callbacks;

typedef struct xc_receiver xc_receiver;

int          xc_api_version(void);
int64_t      xc_now_ns(void);                                 /* monotonic clock used for every pts */
xc_receiver *xc_create(const xc_config *cfg, const xc_callbacks *cb);  /* NULL on failure (logged via on_log) */
int          xc_start(xc_receiver *r);                        /* binds sockets, publishes mDNS; 0 = ok */
void         xc_disconnect(xc_receiver *r);                   /* drop the current client, keep advertising */
void         xc_stop(xc_receiver *r);                         /* unpublish + close everything */
void         xc_destroy(xc_receiver *r);
const char  *xc_version_string(void);                         /* "castcore x.y (uxplay <tag>)" */

/* ---- additive (no struct change, XC_API_VERSION stays 1) ---- */

/* Minimum level passed to on_log (default XC_LOG_INFO). XC_LOG_DEBUG also enables UxPlay's protocol debug
   messages (one or more lines per RTSP request; not per packet). Callable any time. */
void         xc_set_log_level(xc_receiver *r, xc_log_level level);

/* H.264 low-delay SPS rewrite (default ON). iOS's SPS has no VUI bitstream restriction, so a conforming
   decoder may hold several frames for reordering (screen-mirror-xbox measured 130-700 ms on Xbox).
   iOS never sends B slices, so castcore rewrites the SPS in delivered frames to max_num_reorder_frames = 0
   (and stops doing so for the session if a B slice ever appears). Pass 0 to deliver the sender's SPS
   untouched. Callable any time. */
void         xc_set_low_delay_sps(xc_receiver *r, int on);

#ifdef __cplusplus
}
#endif

#endif /* CASTCORE_H */
