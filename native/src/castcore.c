/*
 * castcore: AirPlay screen-mirroring + audio receiver core of xbox-cast.
 * Copyright (C) 2026 xbox-cast contributors. GPL-3.0-or-later (see LICENSE).
 *
 * Wraps the vendored UxPlay protocol library (native/third_party/uxplay/lib) into the xc_* API of
 * include/castcore.h. The UxPlay callback glue follows UxPlay's own uxplay.cpp (GPL-3.0) closely:
 * same features bits, same TXT records, same reset/feedback handling. See docs/core.md.
 */
#include "castcore.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"   /* UxPlay: winsock/pthread portability (threads.h) */
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"

#include "dmap.h"
#include "h264_sps.h"

#ifdef _MSC_VER
/* System import libraries castcore.lib needs (UxPlay sockets/adapters, OpenSSL's RNG and cert store). Recorded in
 * the .lib, so the app links only castcore.lib (build-uwp.ps1 merges OpenSSL + libplist into it). */
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#endif

#define XC_VERSION_STRING "castcore 0.1 (uxplay master-116e390)"
#define XC_DEFAULT_PORT 7000
#define XC_MISSED_FEEDBACK_LIMIT 15   /* seconds without a client heartbeat before the session is dropped (UxPlay default) */
#define XC_KEYFILE_NAME "castcore-ed25519.pem"

/* AirPlay audio compression types (UxPlay "ct") */
#define CT_PCM 1
#define CT_ALAC 2
#define CT_AAC_LC 4
#define CT_AAC_ELD 8

struct xc_receiver {
    xc_config     cfg;
    char         *name;
    char         *data_dir;
    xc_callbacks  cb;

    mutex_handle_t lock;         /* guards everything below that callbacks touch */
    int            log_level;    /* xc_log_level */

    raop_t        *raop;
    dnssd_t       *dnssd;
    unsigned short port;
    int            running;
    int            published_os; /* mdns_publish was used */

    /* supervisor thread: 1 Hz stats + heartbeat watchdog + connection resets */
    thread_handle_t sup_thread;
    int             sup_run;
    mutex_handle_t  sup_lock;
    cond_handle_t   sup_cond;
    int             reset_request;   /* conn_reset / watchdog: restart httpd (drops all connections) */

    /* session state */
    int      open_connections;
    unsigned missed_feedback;
    char     client_name[128];
    char     client_model[64];
    char     client_addr[64];
    int      client_reported;
    int      pin_shown;

    /* video */
    xc_video_codec v_codec;
    int      v_width, v_height;     /* last reported to on_video_format */
    int      v_fmt_pending;         /* codec packet seen, report format with the next frame */
    unsigned short src_w, src_h;    /* from the codec packet */
    int      low_delay_sps;
    int      saw_b_slice;
    uint8_t *sps_in;  size_t sps_in_len;    /* cache of the last SPS seen and its rewrite */
    uint8_t *sps_out; size_t sps_out_len;
    int      sps_logged;

    /* audio */
    int      a_ct;
    int      a_stream;              /* an audio RTP session is set up (cleared at audio TEARDOWN / last connection) */
    float    volume;

    /* stats (since last tick / since start) */
    uint64_t st_vframes, st_vbytes, st_aframes, st_abytes, st_vinvalid;
    uint64_t tot_vframes, tot_aframes, tot_vinvalid, tot_format_changes;
    int64_t  last_video_latency_ns;
    int64_t  last_ntp_offset_ns;    /* remote - local clock, from the last packet */
    int64_t  last_tick_ns;
};

/* ---------------------------------------------------------------- clocks */

int64_t xc_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t) (now.QuadPart / freq.QuadPart) * 1000000000LL +
           (int64_t) ((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/* UxPlay's ntp_time_local is on raop_ntp_get_local_time() = CLOCK_REALTIME in ns. Shift it onto the
   monotonic clock by the current (realtime - monotonic) difference, sampled now. */
static int64_t local_ntp_to_mono(uint64_t ntp_local)
{
    if (!ntp_local) return xc_now_ns();
    int64_t mono = xc_now_ns();
    int64_t real = (int64_t) get_local_time();
    return (int64_t) ntp_local - real + mono;
}

/* ---------------------------------------------------------------- logging */

static void xc_logv(xc_receiver *r, xc_log_level lvl, const char *fmt, va_list ap)
{
    char buf[1024];
    if (!r->cb.on_log || (int) lvl < r->log_level) return;
    vsnprintf(buf, sizeof(buf), fmt, ap);
    r->cb.on_log(r->cb.ctx, lvl, buf);
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void xc_log(xc_receiver *r, xc_log_level lvl, const char *fmt, ...);
static void xc_log(xc_receiver *r, xc_log_level lvl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xc_logv(r, lvl, fmt, ap);
    va_end(ap);
}

static void raop_log_cb(void *cls, int level, const char *msg)
{
    xc_receiver *r = (xc_receiver *) cls;
    xc_log_level lvl;
    size_t n;
    char buf[1024];
    if (level <= LOGGER_ERR) lvl = XC_LOG_ERROR;
    else if (level == LOGGER_WARNING) lvl = XC_LOG_WARN;
    else if (level <= LOGGER_INFO) lvl = XC_LOG_INFO;
    else lvl = XC_LOG_DEBUG;
    /* UxPlay messages often end in (or start with) newlines */
    while (*msg == '\n') msg++;
    snprintf(buf, sizeof(buf), "uxplay: %s", msg);
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
    if (n) xc_log(r, lvl, "%s", buf);
}

/* ---------------------------------------------------------------- video */

static int nal_is_keyframe(xc_video_codec codec, const uint8_t *nal)
{
    if (codec == XC_VIDEO_H265) {
        int t = (nal[0] >> 1) & 0x3f;
        return t >= 16 && t <= 21;   /* BLA/IDR/CRA */
    }
    return (nal[0] & 0x1f) == 5;
}

/* Finds the next Annex-B start code at or after pos. Returns the offset of the first NAL byte and its
   start-code length, or len if none. */
static size_t next_nal(const uint8_t *d, size_t len, size_t pos, size_t *sc_len)
{
    for (size_t i = pos; i + 3 <= len; i++) {
        if (d[i] == 0 && d[i + 1] == 0) {
            if (d[i + 2] == 1) { *sc_len = 3; return i + 3; }
            if (i + 4 <= len && d[i + 2] == 0 && d[i + 3] == 1) { *sc_len = 4; return i + 4; }
        }
    }
    *sc_len = 0;
    return len;
}

/* Parses (and when enabled rewrites) the H.264 SPS in an access unit. Returns a malloc'ed replacement
   buffer, or NULL to deliver the original. Called with r->lock held. */
static uint8_t *process_h264(xc_receiver *r, const uint8_t *d, size_t len, size_t *out_len,
                             int *is_key, int *sps_w, int *sps_h)
{
    size_t sc, start = next_nal(d, len, 0, &sc);
    const uint8_t *sps = NULL;
    size_t sps_len = 0, sps_off = 0, sps_sc = 0;
    *is_key = 0;
    while (start < len) {
        size_t sc2, nxt = next_nal(d, len, start, &sc2);
        size_t nal_end = (nxt < len) ? nxt - sc2 : len;
        int type = d[start] & 0x1f;
        if (type == 5) *is_key = 1;
        if (type == 7 && !sps) { sps = d + start; sps_len = nal_end - start; sps_off = start; sps_sc = sc; }
        if ((type == 1 || type == 5) && !r->saw_b_slice && xc_h264_slice_type(d + start, nal_end - start) == 1) {
            r->saw_b_slice = 1;
            xc_log(r, XC_LOG_WARN, "video: B slice seen: the sender reorders frames, SPS low-delay rewrite disabled");
        }
        start = nxt;
        sc = sc2;
    }
    if (!sps) return NULL;

    if (!r->sps_in || r->sps_in_len != sps_len || memcmp(r->sps_in, sps, sps_len)) {
        xc_h264_sps info;
        free(r->sps_in); free(r->sps_out);
        r->sps_in = NULL; r->sps_out = NULL; r->sps_in_len = r->sps_out_len = 0;
        r->sps_in = (uint8_t *) malloc(sps_len);
        if (r->sps_in) { memcpy(r->sps_in, sps, sps_len); r->sps_in_len = sps_len; }
        xc_h264_parse_sps(sps, sps_len, &info);
        xc_log(r, XC_LOG_INFO, "video: SPS %s", info.text);
        if (info.ok) {
            *sps_w = info.width;
            *sps_h = info.height;
            r->sps_out = xc_h264_rewrite_sps(sps, sps_len, &info, 0, &r->sps_out_len);
            if (r->sps_out) {
                xc_log(r, XC_LOG_INFO, "video: SPS rewritten to max_num_reorder_frames=0 (%u -> %u bytes)%s",
                       (unsigned) sps_len, (unsigned) r->sps_out_len, r->low_delay_sps ? "" : " (not applied: disabled)");
            }
        }
    } else if (r->sps_in) {
        xc_h264_sps info;
        if (xc_h264_parse_sps(sps, sps_len, &info) == 0) { *sps_w = info.width; *sps_h = info.height; }
    }
    (void) sps_sc;
    if (!r->low_delay_sps || r->saw_b_slice || !r->sps_out) return NULL;

    {
        size_t nlen = len - sps_len + r->sps_out_len;
        uint8_t *out = (uint8_t *) malloc(nlen);
        if (!out) return NULL;
        memcpy(out, d, sps_off);
        memcpy(out + sps_off, r->sps_out, r->sps_out_len);
        memcpy(out + sps_off + r->sps_out_len, d + sps_off + sps_len, len - sps_off - sps_len);
        *out_len = nlen;
        return out;
    }
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data)
{
    xc_receiver *r = (xc_receiver *) cls;
    xc_video_frame f;
    uint8_t *rewritten = NULL;
    size_t out_len = 0;
    int is_key = 0, w = 0, h = 0, report_fmt = 0;
    (void) ntp;
    if (data->data_len <= 4) return;

    MUTEX_LOCK(r->lock);
    /* raop_rtp_mirror marks undecryptable data by setting the first byte to 1 */
    if (data->data[0] == 1) {
        r->st_vinvalid++;
        r->tot_vinvalid++;
        MUTEX_UNLOCK(r->lock);
        return;
    }
    f.codec = data->is_h265 ? XC_VIDEO_H265 : XC_VIDEO_H264;
    if (f.codec == XC_VIDEO_H264) {
        rewritten = process_h264(r, data->data, (size_t) data->data_len, &out_len, &is_key, &w, &h);
    } else {
        size_t sc, pos = next_nal(data->data, (size_t) data->data_len, 0, &sc);
        while (pos < (size_t) data->data_len) {
            if (nal_is_keyframe(f.codec, data->data + pos)) { is_key = 1; break; }
            pos = next_nal(data->data, (size_t) data->data_len, pos, &sc);
        }
    }
    if (!w || !h) { w = r->src_w; h = r->src_h; }
    if (f.codec != r->v_codec || r->v_fmt_pending || (is_key && (w != r->v_width || h != r->v_height))) {
        if (f.codec != r->v_codec || w != r->v_width || h != r->v_height) {
            report_fmt = 1;
            r->tot_format_changes++;
        }
        r->v_codec = f.codec;
        r->v_width = w;
        r->v_height = h;
        r->v_fmt_pending = 0;
    }
    if (r->pin_shown) {
        r->pin_shown = 0;
        if (r->cb.on_pin) r->cb.on_pin(r->cb.ctx, "");
    }
    r->st_vframes++;
    r->tot_vframes++;
    r->st_vbytes += (uint64_t) data->data_len;
    if (data->ntp_time_local) {
        r->last_video_latency_ns = (int64_t) get_local_time() - (int64_t) data->ntp_time_local;
        r->last_ntp_offset_ns = (int64_t) data->ntp_time_remote - (int64_t) data->ntp_time_local;
    }
    MUTEX_UNLOCK(r->lock);

    if (report_fmt) {
        xc_log(r, XC_LOG_INFO, "video: format %s %dx%d", f.codec == XC_VIDEO_H265 ? "H.265" : "H.264", w, h);
        if (r->cb.on_video_format) r->cb.on_video_format(r->cb.ctx, f.codec, w, h);
    }
    f.data = rewritten ? rewritten : data->data;
    f.size = rewritten ? out_len : (size_t) data->data_len;
    f.pts_ns = local_ntp_to_mono(data->ntp_time_local);
    f.is_keyframe = is_key;
    f.width = w;
    f.height = h;
    if (r->cb.on_video) r->cb.on_video(r->cb.ctx, &f);
    free(rewritten);
}

static int cb_video_set_codec_and_size(void *cls, video_codec_t codec, unsigned short width_source,
                                       unsigned short height_source, unsigned short x_offset,
                                       unsigned short y_offset, unsigned short width, unsigned short height)
{
    xc_receiver *r = (xc_receiver *) cls;
    if (codec == VIDEO_CODEC_H265 && !r->cfg.allow_h265) {
        xc_log(r, XC_LOG_ERROR, "video: sender chose H.265 but allow_h265 is off");
        return -1;
    }
    MUTEX_LOCK(r->lock);
    if (width_source != r->src_w || height_source != r->src_h) {
        r->v_fmt_pending = 1;
    }
    r->src_w = width_source;
    r->src_h = height_source;
    MUTEX_UNLOCK(r->lock);
    xc_log(r, XC_LOG_DEBUG, "video: codec packet %s source %ux%u, offset %u,%u, display %ux%u",
           codec == VIDEO_CODEC_H265 ? "H.265" : "H.264", width_source, height_source, x_offset, y_offset, width, height);
    return 0;
}

static void video_flush_all(xc_receiver *r, const char *why)
{
    MUTEX_LOCK(r->lock);
    r->v_codec = (xc_video_codec) 0;
    r->v_width = r->v_height = 0;
    r->src_w = r->src_h = 0;
    r->saw_b_slice = 0;
    MUTEX_UNLOCK(r->lock);
    xc_log(r, XC_LOG_DEBUG, "video: flush (%s)", why);
    if (r->cb.on_video_flush) r->cb.on_video_flush(r->cb.ctx);
}

static void cb_video_flush(void *cls) { video_flush_all((xc_receiver *) cls, "client"); }

static void cb_video_reset(void *cls, reset_type_t type)
{
    xc_receiver *r = (xc_receiver *) cls;
    switch (type) {
    case RESET_TYPE_NOHOLD:        video_flush_all(r, "replaced by a new client"); break;
    case RESET_TYPE_RTP_SHUTDOWN:  video_flush_all(r, "mirroring stopped"); break;
    case RESET_TYPE_RTP_TO_HLS_TEARDOWN: video_flush_all(r, "mirroring teardown"); break;
    default:                       video_flush_all(r, "reset"); break;
    }
}

static void cb_video_pause(void *cls)  { xc_log((xc_receiver *) cls, XC_LOG_INFO, "video: sender paused the stream"); }
static void cb_video_resume(void *cls) { xc_log((xc_receiver *) cls, XC_LOG_INFO, "video: sender resumed the stream"); }

/* ---------------------------------------------------------------- audio */

/* ALAC magic cookie as FFmpeg's alac decoder wants it (36-byte 'alac' atom): 44100 Hz, 2 ch, 16 bit, 352 spf.
   AAC AudioSpecificConfigs: LC 44100/2 = 12 10, ELD 44100/2 480 spf = f8 e8 50 00 (same as UxPlay's caps). */
static const uint8_t alac_cookie[36] = {
    0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44 };
static const uint8_t asc_aac_lc[2] = { 0x12, 0x10 };
static const uint8_t asc_aac_eld[4] = { 0xf8, 0xe8, 0x50, 0x00 };

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *using_screen,
                                bool *is_media, uint64_t *audio_format)
{
    xc_receiver *r = (xc_receiver *) cls;
    xc_audio_codec codec;
    const uint8_t *cfg = NULL;
    size_t cfg_len = 0;
    xc_log(r, XC_LOG_INFO, "audio: ct=%d spf=%d usingScreen=%d isMedia=%d audioFormat=0x%llx", *ct, *spf,
           (int) *using_screen, (int) *is_media, (unsigned long long) *audio_format);
    switch (*ct) {
    case CT_ALAC:    codec = XC_AUDIO_ALAC;    cfg = alac_cookie; cfg_len = sizeof(alac_cookie); break;
    case CT_AAC_LC:  codec = XC_AUDIO_AAC_LC;  cfg = asc_aac_lc;  cfg_len = sizeof(asc_aac_lc);  break;
    case CT_AAC_ELD: codec = XC_AUDIO_AAC_ELD; cfg = asc_aac_eld; cfg_len = sizeof(asc_aac_eld); break;
    case CT_PCM:     codec = XC_AUDIO_PCM; break;
    default:
        xc_log(r, XC_LOG_ERROR, "audio: unknown compression type %d", *ct);
        return;
    }
    MUTEX_LOCK(r->lock);
    r->a_ct = *ct;
    r->a_stream = 1;
    MUTEX_UNLOCK(r->lock);
    if (r->cb.on_audio_format) r->cb.on_audio_format(r->cb.ctx, codec, 44100, 2, cfg, cfg_len);
    /* Screen Mirroring sends usingScreen=1 with AAC-ELD next to the video stream; AirPlay from an app's AirPlay button
       sends usingScreen=0 with ALAC (or AAC-LC), no video. Require both so a mirroring session never flashes the
       audio-only screen. */
    if (r->cb.on_audio_stream) r->cb.on_audio_stream(r->cb.ctx, 1, (!*using_screen && *ct != CT_AAC_ELD) ? 1 : 0);
}

/* audio TEARDOWN (UxPlay calls this "stop coverart rendering": the audio RTP session of this connection ended) */
static void cb_audio_stop(void *cls)
{
    xc_receiver *r = (xc_receiver *) cls;
    int was;
    MUTEX_LOCK(r->lock);
    was = r->a_stream;
    r->a_stream = 0;
    MUTEX_UNLOCK(r->lock);
    xc_log(r, XC_LOG_INFO, "audio: stream torn down");
    if (was && r->cb.on_audio_stream) r->cb.on_audio_stream(r->cb.ctx, 0, 0);
}

static void cb_audio_set_metadata(void *cls, const void *buffer, int buflen)
{
    xc_receiver *r = (xc_receiver *) cls;
    xc_dmap_track t;
    int n = buflen > 0 ? xc_dmap_parse_track((const uint8_t *) buffer, (size_t) buflen, &t) : -1;
    if (n < 0) {
        xc_log(r, XC_LOG_WARN, "audio: unparseable track metadata (%d bytes)", buflen);
        return;
    }
    xc_log(r, XC_LOG_INFO, "audio: track metadata %d bytes, %d fields (title %zu B, artist %zu B, album %zu B)",
           buflen, n, strlen(t.title), strlen(t.artist), strlen(t.album));
    if (r->cb.on_track) r->cb.on_track(r->cb.ctx, t.title, t.artist, t.album);
}

static void cb_audio_set_coverart(void *cls, const void *buffer, int buflen)
{
    xc_receiver *r = (xc_receiver *) cls;
    xc_log(r, XC_LOG_INFO, "audio: cover art %d bytes", buflen);
    if (r->cb.on_cover_art) r->cb.on_cover_art(r->cb.ctx, (const uint8_t *) buffer, buflen > 0 ? (size_t) buflen : 0);
}

static void cb_audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end)
{
    xc_receiver *r = (xc_receiver *) cls;
    /* RTP timestamps at 44.1 kHz; unsigned differences survive a wrap of the 32-bit counter */
    double pos = (double) (uint32_t) (*curr - *start) / 44100.0;
    double dur = (double) (uint32_t) (*end - *start) / 44100.0;
    if (*end == *start || pos > dur) {
        xc_log(r, XC_LOG_DEBUG, "audio: progress ignored (%u/%u/%u)", *start, *curr, *end);
        return;
    }
    xc_log(r, XC_LOG_INFO, "audio: progress %.1f / %.1f s", pos, dur);
    if (r->cb.on_progress) r->cb.on_progress(r->cb.ctx, pos, dur);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data)
{
    xc_receiver *r = (xc_receiver *) cls;
    xc_audio_packet p;
    uint8_t *swapped = NULL;
    (void) ntp;
    if (data->data_len <= 0) return;
    MUTEX_LOCK(r->lock);
    r->st_aframes++;
    r->tot_aframes++;
    r->st_abytes += (uint64_t) data->data_len;
    if (data->ntp_time_local && !r->st_vframes) {
        r->last_ntp_offset_ns = (int64_t) data->ntp_time_remote - (int64_t) data->ntp_time_local;
    }
    MUTEX_UNLOCK(r->lock);
    p.data = data->data;
    p.size = (size_t) data->data_len;
    if (data->ct == CT_PCM) {
        /* AirPlay L16 is big-endian; the contract promises s16le */
        swapped = (uint8_t *) malloc(p.size);
        if (swapped) {
            for (size_t i = 0; i + 1 < p.size; i += 2) { swapped[i] = data->data[i + 1]; swapped[i + 1] = data->data[i]; }
            p.data = swapped;
        }
    }
    p.pts_ns = local_ntp_to_mono(data->ntp_time_local);
    if (r->cb.on_audio) r->cb.on_audio(r->cb.ctx, &p);
    free(swapped);
}

static void cb_audio_flush(void *cls)
{
    xc_receiver *r = (xc_receiver *) cls;
    if (r->cb.on_audio_flush) r->cb.on_audio_flush(r->cb.ctx);
}

static void cb_audio_set_volume(void *cls, float volume)
{
    xc_receiver *r = (xc_receiver *) cls;
    MUTEX_LOCK(r->lock);
    r->volume = volume;
    MUTEX_UNLOCK(r->lock);
    xc_log(r, XC_LOG_DEBUG, "audio: volume %.1f dB", (double) volume);
    if (r->cb.on_volume) r->cb.on_volume(r->cb.ctx, volume);
}

static double cb_audio_set_client_volume(void *cls)
{
    xc_receiver *r = (xc_receiver *) cls;
    double v;
    MUTEX_LOCK(r->lock);
    v = (double) r->volume;
    MUTEX_UNLOCK(r->lock);
    return v;
}

/* ---------------------------------------------------------------- connections */

static void cb_report_client_address(void *cls, const char *ipaddr)
{
    xc_receiver *r = (xc_receiver *) cls;
    MUTEX_LOCK(r->lock);
    snprintf(r->client_addr, sizeof(r->client_addr), "%s", ipaddr ? ipaddr : "");
    MUTEX_UNLOCK(r->lock);
}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit)
{
    xc_receiver *r = (xc_receiver *) cls;
    char n[128], m[64], a[64];
    *admit = true;
    MUTEX_LOCK(r->lock);
    snprintf(r->client_name, sizeof(r->client_name), "%s", name ? name : "");
    snprintf(r->client_model, sizeof(r->client_model), "%s", model ? model : "");
    r->client_reported = 1;
    memcpy(n, r->client_name, sizeof(n));
    memcpy(m, r->client_model, sizeof(m));
    memcpy(a, r->client_addr, sizeof(a));
    MUTEX_UNLOCK(r->lock);
    xc_log(r, XC_LOG_INFO, "client connected: \"%s\" (%s) from %s, deviceID %s", n, m, a[0] ? a : "?",
           deviceid ? deviceid : "?");
    if (r->cb.on_client_connected) r->cb.on_client_connected(r->cb.ctx, n, m, a);
}

static void cb_conn_init(void *cls)
{
    xc_receiver *r = (xc_receiver *) cls;
    MUTEX_LOCK(r->lock);
    r->open_connections++;
    MUTEX_UNLOCK(r->lock);
}

static void cb_conn_destroy(void *cls)
{
    xc_receiver *r = (xc_receiver *) cls;
    int last, reported, pin;
    MUTEX_LOCK(r->lock);
    if (r->open_connections > 0) r->open_connections--;
    last = (r->open_connections == 0);
    reported = r->client_reported;
    pin = r->pin_shown;
    if (last) {
        r->client_reported = 0;
        r->missed_feedback = 0;
        r->pin_shown = 0;
        r->a_ct = 0;
        r->a_stream = 0;
    }
    MUTEX_UNLOCK(r->lock);
    if (last) {
        if (pin && r->cb.on_pin) r->cb.on_pin(r->cb.ctx, "");
        if (reported) {
            xc_log(r, XC_LOG_INFO, "client disconnected");
            if (r->cb.on_client_disconnected) r->cb.on_client_disconnected(r->cb.ctx, "closed");
        }
    }
}

static void request_reset(xc_receiver *r)
{
    MUTEX_LOCK(r->sup_lock);
    r->reset_request = 1;
    COND_SIGNAL(r->sup_cond);
    MUTEX_UNLOCK(r->sup_lock);
}

static void cb_conn_reset(void *cls, int reason)
{
    xc_receiver *r = (xc_receiver *) cls;
    xc_log(r, XC_LOG_WARN, "connection reset requested by the protocol (reason %d%s)", reason,
           reason == 1 ? ": lost connection with client" : "");
    request_reset(r);
}

static void cb_conn_feedback(void *cls)
{
    xc_receiver *r = (xc_receiver *) cls;
    MUTEX_LOCK(r->lock);
    r->missed_feedback = 0;
    MUTEX_UNLOCK(r->lock);
}

static void cb_display_pin(void *cls, char *pin)
{
    xc_receiver *r = (xc_receiver *) cls;
    MUTEX_LOCK(r->lock);
    r->pin_shown = 1;
    MUTEX_UNLOCK(r->lock);
    xc_log(r, XC_LOG_INFO, "PIN requested: showing it on screen");
    if (r->cb.on_pin) r->cb.on_pin(r->cb.ctx, pin);
}

static const char *cb_passwd(void *cls, int *len)
{
    xc_receiver *r = (xc_receiver *) cls;
    /* pin_mode 1 = UxPlay pin_pw 3: a random 4-digit password shown on screen at every connection */
    *len = r->cfg.pin_mode == 1 ? -1 : 0;
    return NULL;
}

static bool cb_check_register(void *cls, const char *pk) { (void) cls; (void) pk; return true; }
static void cb_register_client(void *cls, const char *id, const char *pk, const char *name)
{
    (void) cls; (void) id; (void) pk; (void) name;
}

/* ---------------------------------------------------------------- supervisor thread */

static void restart_httpd(xc_receiver *r, const char *why)
{
    unsigned short port = r->port;
    xc_log(r, XC_LOG_INFO, "dropping all connections (%s)", why);
    raop_stop_httpd(r->raop);
    if (raop_start_httpd(r->raop, &port) < 0) {
        xc_log(r, XC_LOG_ERROR, "could not restart the RTSP server on port %u", (unsigned) r->port);
    }
}

static void emit_stats(xc_receiver *r, int64_t now)
{
    char json[512];
    double secs;
    MUTEX_LOCK(r->lock);
    secs = r->last_tick_ns ? (double) (now - r->last_tick_ns) / 1e9 : 1.0;
    if (secs <= 0.0) secs = 1.0;
    snprintf(json, sizeof(json),
             "{\"connected\":%s,\"video_codec\":\"%s\",\"width\":%d,\"height\":%d,"
             "\"video_fps\":%.1f,\"video_kbps\":%.0f,\"video_frames\":%llu,\"video_invalid\":%llu,"
             "\"format_changes\":%llu,\"audio_codec\":%d,\"audio_pps\":%.1f,\"audio_kbps\":%.0f,"
             "\"audio_packets\":%llu,\"volume_db\":%.1f,\"net_latency_ms\":%.1f,\"ntp_offset_ms\":%.3f,"
             "\"missed_feedback_s\":%u}",
             r->client_reported ? "true" : "false",
             r->v_codec == XC_VIDEO_H265 ? "h265" : (r->v_codec == XC_VIDEO_H264 ? "h264" : ""),
             r->v_width, r->v_height,
             (double) r->st_vframes / secs, (double) r->st_vbytes * 8.0 / 1000.0 / secs,
             (unsigned long long) r->tot_vframes, (unsigned long long) r->tot_vinvalid,
             (unsigned long long) r->tot_format_changes, r->a_ct,
             (double) r->st_aframes / secs, (double) r->st_abytes * 8.0 / 1000.0 / secs,
             (unsigned long long) r->tot_aframes, (double) r->volume,
             (double) r->last_video_latency_ns / 1e6, (double) r->last_ntp_offset_ns / 1e6,
             r->missed_feedback);
    r->st_vframes = r->st_vbytes = r->st_aframes = r->st_abytes = r->st_vinvalid = 0;
    r->last_tick_ns = now;
    MUTEX_UNLOCK(r->lock);
    if (r->cb.on_stats) r->cb.on_stats(r->cb.ctx, json);
}

static THREAD_RETVAL supervisor_thread(void *arg)
{
    xc_receiver *r = (xc_receiver *) arg;
    int64_t next = xc_now_ns() + 1000000000LL;
    for (;;) {
        int reset = 0, stop;
        MUTEX_LOCK(r->sup_lock);
        while (r->sup_run && !r->reset_request && xc_now_ns() < next) {
            struct timespec ts;
            /* pthread (and our MSVC shim) wait on CLOCK_REALTIME; wake at most every 100 ms */
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&r->sup_cond, &r->sup_lock, &ts);
        }
        stop = !r->sup_run;
        reset = r->reset_request;
        r->reset_request = 0;
        MUTEX_UNLOCK(r->sup_lock);
        if (stop) break;
        if (reset) {
            restart_httpd(r, "protocol reset");
            continue;
        }
        {
            int64_t now = xc_now_ns();
            int lost = 0;
            unsigned missed;
            next = now + 1000000000LL;
            MUTEX_LOCK(r->lock);
            if (r->open_connections) {
                missed = ++r->missed_feedback;
                if (missed > XC_MISSED_FEEDBACK_LIMIT) {
                    lost = 1;
                    r->missed_feedback = 0;
                }
            } else {
                missed = r->missed_feedback = 0;
            }
            MUTEX_UNLOCK(r->lock);
            if (missed > 3 && !lost) {
                xc_log(r, XC_LOG_WARN, "%u s since the last client heartbeat (expected every 2 s)", missed);
            }
            if (lost) {
                restart_httpd(r, "client heartbeat lost for 15 s");
            }
            emit_stats(r, now);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- mdns_publish path */

/* Splits a DNS TXT rdata blob (length-prefixed "key=value" strings) into entries. */
static size_t parse_txt(const unsigned char *txt, int len, xc_txt_entry *out, char *store, size_t store_len,
                        size_t max_entries)
{
    size_t n = 0, used = 0;
    int pos = 0;
    while (pos < len && n < max_entries) {
        int l = txt[pos++];
        const char *s = (const char *) txt + pos;
        const char *eq;
        if (pos + l > len || used + (size_t) l + 2 > store_len) break;
        memcpy(store + used, s, (size_t) l);
        store[used + l] = '\0';
        eq = strchr(store + used, '=');
        out[n].key = store + used;
        if (eq) {
            store[used + (eq - (store + used))] = '\0';
            out[n].value = eq + 1;
        } else {
            out[n].value = "";
        }
        used += (size_t) l + 1;
        n++;
        pos += l;
    }
    return n;
}

static int publish_os(xc_receiver *r)
{
    xc_txt_entry entries[32];
    char store[1200];
    const char *txt;
    char raop_name[300];
    char hw[13];
    int len, rc;
    size_t n;
    if (dnssd_prepare_txt(r->dnssd)) {
        xc_log(r, XC_LOG_ERROR, "could not build the TXT records");
        return -1;
    }
    snprintf(hw, sizeof(hw), "%02X%02X%02X%02X%02X%02X", r->cfg.device_id[0], r->cfg.device_id[1],
             r->cfg.device_id[2], r->cfg.device_id[3], r->cfg.device_id[4], r->cfg.device_id[5]);
    snprintf(raop_name, sizeof(raop_name), "%s@%s", hw, r->name);

    txt = dnssd_get_airplay_txt(r->dnssd, &len);
    n = parse_txt((const unsigned char *) txt, len, entries, store, sizeof(store), 32);
    rc = r->cb.mdns_publish(r->cb.ctx, "_airplay._tcp", r->name, r->port, entries, n);
    if (rc) {
        xc_log(r, XC_LOG_ERROR, "mdns_publish(_airplay._tcp) failed: %d", rc);
        return -1;
    }
    txt = dnssd_get_raop_txt(r->dnssd, &len);
    n = parse_txt((const unsigned char *) txt, len, entries, store, sizeof(store), 32);
    rc = r->cb.mdns_publish(r->cb.ctx, "_raop._tcp", raop_name, r->port, entries, n);
    if (rc) {
        xc_log(r, XC_LOG_ERROR, "mdns_publish(_raop._tcp) failed: %d", rc);
        return -1;
    }
    r->published_os = 1;
    return 0;
}

/* ---------------------------------------------------------------- dnssd features (as uxplay.cpp start_dnssd) */

static void set_features(xc_receiver *r)
{
    static const signed char bits[32] = {
        0, 1, 1, 0,  0, 1, 1, 1,  0, 1, 1, 1,  1, 1, 1, 1,
        1, 1, 1, 1,  1, 1, 1, 0,  0, 1, 0, 1,  1, 0, 1, 0 };
    for (int i = 0; i < 32; i++) dnssd_set_airplay_features(r->dnssd, i, bits[i]);
    /* no HLS (bits 0 and 4 stay 0) */
    dnssd_set_airplay_features(r->dnssd, 42, r->cfg.allow_h265 ? 1 : 0);   /* SupportsScreenMultiCodec */
    dnssd_set_airplay_features(r->dnssd, 27, 0);                           /* legacy pairing off (no -pin nnnn mode) */
}

/* ---------------------------------------------------------------- public API */

int xc_api_version(void) { return XC_API_VERSION; }
const char *xc_version_string(void) { return XC_VERSION_STRING; }

void xc_set_log_level(xc_receiver *r, xc_log_level level)
{
    if (!r) return;
    r->log_level = (int) level;
    if (r->raop) raop_set_log_level(r->raop, level == XC_LOG_DEBUG ? LOGGER_DEBUG : LOGGER_INFO);
}

void xc_set_low_delay_sps(xc_receiver *r, int on)
{
    if (!r) return;
    MUTEX_LOCK(r->lock);
    r->low_delay_sps = on ? 1 : 0;
    MUTEX_UNLOCK(r->lock);
}

static char *dupstr(const char *s)
{
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s);
    d = (char *) malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

xc_receiver *xc_create(const xc_config *cfg, const xc_callbacks *cb)
{
    xc_receiver *r;
    if (!cfg || !cb) return NULL;
    r = (xc_receiver *) calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->cfg = *cfg;
    r->cb = *cb;
    r->log_level = XC_LOG_INFO;
    r->low_delay_sps = 1;
    MUTEX_CREATE(r->lock);
    MUTEX_CREATE(r->sup_lock);
    COND_CREATE(r->sup_cond);
    if (!cfg->name || !cfg->name[0] || strlen(cfg->name) > 63) {
        xc_log(r, XC_LOG_ERROR, "xc_create: name must be 1..63 bytes");
        goto fail;
    }
    if (!cfg->builtin_mdns && !cb->mdns_publish) {
        xc_log(r, XC_LOG_ERROR, "xc_create: mdns_publish is required unless builtin_mdns = 1");
        goto fail;
    }
    if (cfg->pin_mode != 0 && cfg->pin_mode != 1) {
        xc_log(r, XC_LOG_ERROR, "xc_create: pin_mode must be 0 or 1");
        goto fail;
    }
    r->name = dupstr(cfg->name);
    r->data_dir = dupstr(cfg->data_dir ? cfg->data_dir : "");
    if (!r->name || !r->data_dir) goto fail;
    r->cfg.name = r->name;
    r->cfg.data_dir = r->data_dir;
    if (!r->cfg.display_width) r->cfg.display_width = 1920;
    if (!r->cfg.display_height) r->cfg.display_height = 1080;
    if (!r->cfg.display_refresh) r->cfg.display_refresh = 60;
    if (!r->cfg.max_fps) r->cfg.max_fps = 60;
    r->volume = 0.0f;
    ntp_global_init();
    xc_log(r, XC_LOG_INFO, "%s: \"%s\", display %dx%d@%d max %d fps, h265 %s, pin %s, mDNS %s", XC_VERSION_STRING,
           r->name, r->cfg.display_width, r->cfg.display_height, r->cfg.display_refresh, r->cfg.max_fps,
           r->cfg.allow_h265 ? "on" : "off", r->cfg.pin_mode ? "on-screen" : "off",
           r->cfg.builtin_mdns ? "builtin" : "app (mdns_publish)");
    return r;
fail:
    free(r->name);
    free(r->data_dir);
    MUTEX_DESTROY(r->lock);
    MUTEX_DESTROY(r->sup_lock);
    COND_DESTROY(r->sup_cond);
    free(r);
    return NULL;
}

int xc_start(xc_receiver *r)
{
    raop_callbacks_t cbs;
    char mac[18];
    char keyfile[1024] = { 0 };
    unsigned short tcp[2], udp[3] = { 0, 0, 0 };
    int err = 0;
    if (!r) return -1;
    if (r->running) return 0;

    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", r->cfg.device_id[0], r->cfg.device_id[1],
             r->cfg.device_id[2], r->cfg.device_id[3], r->cfg.device_id[4], r->cfg.device_id[5]);
    if (r->data_dir[0]) {
        size_t n = strlen(r->data_dir);
        int sep = (r->data_dir[n - 1] == '/' || r->data_dir[n - 1] == '\\');
        snprintf(keyfile, sizeof(keyfile), "%s%s%s", r->data_dir, sep ? "" : "/", XC_KEYFILE_NAME);
    }

    r->dnssd = dnssd_init(r->name, (int) strlen(r->name), (const char *) r->cfg.device_id, 6,
                          (unsigned char) (r->cfg.pin_mode == 1 ? 3 : 0), &err);
    if (err || !r->dnssd) {
        xc_log(r, XC_LOG_ERROR, "dnssd_init failed: error %d", err);
        r->dnssd = NULL;
        return -1;
    }
    set_features(r);

    memset(&cbs, 0, sizeof(cbs));
    cbs.cls = r;
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.audio_set_metadata = cb_audio_set_metadata;
    cbs.audio_set_coverart = cb_audio_set_coverart;
    cbs.audio_stop_coverart_rendering = cb_audio_stop;
    cbs.audio_set_progress = cb_audio_set_progress;
    cbs.report_client_request = cb_report_client_request;
    cbs.report_client_address = cb_report_client_address;
    cbs.display_pin = cb_display_pin;
    cbs.register_client = cb_register_client;
    cbs.check_register = cb_check_register;
    cbs.passwd = cb_passwd;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec_and_size = cb_video_set_codec_and_size;

    r->raop = raop_init(&cbs);
    if (!r->raop) {
        xc_log(r, XC_LOG_ERROR, "raop_init failed (network init?)");
        goto fail;
    }
    raop_set_log_callback(r->raop, raop_log_cb, r);
    raop_set_log_level(r->raop, r->log_level == XC_LOG_DEBUG ? LOGGER_DEBUG : LOGGER_INFO);
    if (raop_init2(r->raop, 1 /* nohold: a new client replaces the current one */, mac, keyfile)) {
        xc_log(r, XC_LOG_ERROR, "raop_init2 failed (identity key %s)", keyfile[0] ? keyfile : "(derived)");
        free(r->raop);   /* as uxplay.cpp: raop_destroy needs a fully initialised raop */
        r->raop = NULL;
        goto fail;
    }
    if (keyfile[0]) xc_log(r, XC_LOG_INFO, "identity key: %s", keyfile);
    raop_set_plist(r->raop, "width", r->cfg.display_width);
    raop_set_plist(r->raop, "height", r->cfg.display_height);
    raop_set_plist(r->raop, "refreshRate", r->cfg.display_refresh);
    raop_set_plist(r->raop, "maxFPS", r->cfg.max_fps);
    tcp[0] = 0;
    tcp[1] = r->cfg.port ? r->cfg.port : XC_DEFAULT_PORT;
    raop_set_tcp_ports(r->raop, tcp);
    raop_set_udp_ports(r->raop, udp);
    r->port = raop_get_port(r->raop);
    if (raop_start_httpd(r->raop, &r->port) < 0) {
        xc_log(r, XC_LOG_ERROR, "could not start the RTSP server on TCP port %u (in use?)", (unsigned) tcp[1]);
        goto fail;
    }
    raop_set_port(r->raop, r->port);
    raop_set_dnssd(r->raop, r->dnssd);
    xc_log(r, XC_LOG_INFO, "RTSP/AirPlay server listening on TCP port %u", (unsigned) r->port);

    if (r->cfg.builtin_mdns) {
        int e1 = dnssd_register_raop(r->dnssd, r->port);
        int e2 = e1 ? 0 : dnssd_register_airplay(r->dnssd, r->port);
        if (e1 || e2) {
            xc_log(r, XC_LOG_ERROR, "builtin mDNS responder failed to start (socket error %d): UDP 5353 / multicast",
                   e1 ? e1 : e2);
            goto fail;
        }
        xc_log(r, XC_LOG_INFO, "builtin mDNS responder: advertising \"%s\" as host %s", r->name,
               dnssd_get_host_name(r->dnssd));
    } else if (publish_os(r)) {
        goto fail;
    } else {
        xc_log(r, XC_LOG_INFO, "advertised \"%s\" through mdns_publish", r->name);
    }

    r->sup_run = 1;
    r->reset_request = 0;
    r->last_tick_ns = 0;
    THREAD_CREATE(r->sup_thread, supervisor_thread, r);
    r->running = 1;
    return 0;

fail:
    r->running = 1;   /* let xc_stop clean up the partial state */
    xc_stop(r);
    return -1;
}

void xc_disconnect(xc_receiver *r)
{
    if (!r || !r->running || !r->raop) return;
    restart_httpd(r, "xc_disconnect");
}

void xc_stop(xc_receiver *r)
{
    if (!r || !r->running) return;
    if (r->sup_run) {
        MUTEX_LOCK(r->sup_lock);
        r->sup_run = 0;
        COND_SIGNAL(r->sup_cond);
        MUTEX_UNLOCK(r->sup_lock);
        THREAD_JOIN(r->sup_thread);
    }
    if (r->dnssd) {
        if (r->cfg.builtin_mdns) {
            dnssd_unregister_raop(r->dnssd);
            dnssd_unregister_airplay(r->dnssd);
        }
    }
    if (r->published_os) {
        if (r->cb.mdns_unpublish_all) r->cb.mdns_unpublish_all(r->cb.ctx);
        r->published_os = 0;
    }
    if (r->raop) {
        raop_destroy(r->raop);   /* stops httpd: every connection and its threads end here */
        r->raop = NULL;
    }
    if (r->dnssd) {
        dnssd_destroy(r->dnssd);
        r->dnssd = NULL;
    }
    MUTEX_LOCK(r->lock);
    free(r->sps_in); free(r->sps_out);
    r->sps_in = r->sps_out = NULL;
    r->sps_in_len = r->sps_out_len = 0;
    r->open_connections = 0;
    r->client_reported = 0;
    r->v_codec = (xc_video_codec) 0;
    r->v_width = r->v_height = 0;
    MUTEX_UNLOCK(r->lock);
    r->running = 0;
    xc_log(r, XC_LOG_INFO, "stopped");
}

void xc_destroy(xc_receiver *r)
{
    if (!r) return;
    xc_stop(r);
    MUTEX_DESTROY(r->lock);
    MUTEX_DESTROY(r->sup_lock);
    COND_DESTROY(r->sup_cond);
    free(r->name);
    free(r->data_dir);
    free(r);
}
