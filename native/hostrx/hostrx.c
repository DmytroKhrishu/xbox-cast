/*
 * hostrx: Linux test receiver for castcore (xbox-cast). GPL-3.0-or-later.
 *
 * Runs castcore on this machine, advertises it with avahi (or castcore's builtin mDNS responder),
 * dumps what an iPhone sends and decodes the audio with libavcodec to prove the whole path works:
 *   <out>/video.h264 | video.h265   Annex-B elementary stream as delivered by on_video
 *   <out>/audio.<codec>              compressed audio packets, each prefixed by a 4-byte big-endian length
 *   <out>/audio.wav                  the audio decoded by libavcodec (s16le, 44.1 kHz stereo)
 *   <out>/stats.jsonl                castcore's 1 Hz on_stats JSON lines
 * See native/hostrx/README.md.
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>

#include "castcore.h"

/* ---------------------------------------------------------------- options / state */

typedef struct {
    const char *name;
    const char *out;
    int seconds;
    int builtin_mdns;
    int h265;
    int pin;
    int debug;
    int low_delay;
    int width, height, fps;
    unsigned short port;
    int quiet_stats;
} options;

typedef struct {
    pthread_mutex_t lock;
    options o;
    int64_t t0;

    /* avahi */
    AvahiThreadedPoll *poll;
    AvahiClient *client;
    AvahiEntryGroup *group;
    int published;

    /* video */
    FILE *vfile;
    xc_video_codec vcodec;
    uint64_t vframes, vkeyframes, vbytes, vformat_changes, vflushes;
    int vw, vh;
    int64_t first_video_ns, last_video_ns, last_pts_ns;
    int64_t max_pts_lead_ns, min_pts_lead_ns;   /* pts - arrival time */
    int got_video;

    /* audio */
    FILE *afile;
    char afile_name[64];
    xc_audio_codec acodec;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frm;
    FILE *wav;
    uint64_t wav_bytes;
    uint64_t apackets, abytes, adecoded_frames, adecode_errors, asamples;
    int asample_rate, achannels;
    float volume_db;

    /* session */
    int connected;
    char client_name[128], client_model[64], client_addr[64];
    uint64_t sessions;
    char last_stats[512];
} hostrx_state;

static hostrx_state g;
static volatile sig_atomic_t g_stop;

static double since_start(void) { return (double) (xc_now_ns() - g.t0) / 1e9; }

static void logf_(const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[2048];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%8.3f] %-5s %s\n", since_start(), tag, buf);
    fflush(stderr);
}

static char *path_join(const char *dir, const char *file)
{
    static char buf[4][4096];
    static int i;
    i = (i + 1) & 3;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", dir, file);
    return buf[i];
}

/* ---------------------------------------------------------------- avahi publisher (mdns_publish) */

static void group_cb(AvahiEntryGroup *grp, AvahiEntryGroupState state, void *userdata)
{
    (void) userdata;
    switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        logf_("mdns", "avahi: services established (visible on the network)");
        break;
    case AVAHI_ENTRY_GROUP_COLLISION:
        logf_("mdns", "avahi: NAME COLLISION: another device already uses \"%s\"; restart hostrx with another --name",
              g.o.name);
        break;
    case AVAHI_ENTRY_GROUP_FAILURE:
        logf_("mdns", "avahi: entry group failure: %s",
              avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(grp))));
        break;
    default:
        break;
    }
}

static void client_cb(AvahiClient *c, AvahiClientState state, void *userdata)
{
    (void) userdata;
    if (state == AVAHI_CLIENT_FAILURE) {
        logf_("mdns", "avahi client failure: %s", avahi_strerror(avahi_client_errno(c)));
    }
}

/* called by castcore from xc_start (main thread), once per service */
static int cb_mdns_publish(void *ctx, const char *service_type, const char *instance_name, unsigned short port,
                           const xc_txt_entry *txt, size_t txt_count)
{
    AvahiStringList *sl = NULL;
    int ret;
    (void) ctx;
    if (!g.client) return -1;
    for (size_t i = txt_count; i-- > 0;) {   /* avahi prepends: add backwards to keep castcore's order */
        sl = avahi_string_list_add_pair(sl, txt[i].key, txt[i].value);
    }
    avahi_threaded_poll_lock(g.poll);
    if (!g.group) g.group = avahi_entry_group_new(g.client, group_cb, NULL);
    if (!g.group) {
        avahi_threaded_poll_unlock(g.poll);
        avahi_string_list_free(sl);
        logf_("mdns", "avahi_entry_group_new failed: %s", avahi_strerror(avahi_client_errno(g.client)));
        return -1;
    }
    ret = avahi_entry_group_add_service_strlst(g.group, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, 0, instance_name,
                                               service_type, NULL, NULL, port, sl);
    if (ret == 0 && !strcmp(service_type, "_raop._tcp")) {
        ret = avahi_entry_group_commit(g.group);   /* castcore publishes _airplay first, _raop second */
        if (ret == 0) g.published = 1;
    }
    avahi_threaded_poll_unlock(g.poll);
    avahi_string_list_free(sl);
    if (ret < 0) {
        logf_("mdns", "avahi: publishing %s \"%s\" failed: %s", service_type, instance_name, avahi_strerror(ret));
        return -1;
    }
    logf_("mdns", "avahi: %s \"%s\" port %u, %zu TXT entries", service_type, instance_name, port, txt_count);
    return 0;
}

static void cb_mdns_unpublish_all(void *ctx)
{
    (void) ctx;
    if (!g.poll) return;
    avahi_threaded_poll_lock(g.poll);
    if (g.group) {
        avahi_entry_group_reset(g.group);
        avahi_entry_group_free(g.group);
        g.group = NULL;
    }
    g.published = 0;
    avahi_threaded_poll_unlock(g.poll);
    logf_("mdns", "avahi: services withdrawn");
}

/* ---------------------------------------------------------------- wav */

static void wav_header(FILE *f, int rate, int ch, uint32_t data_bytes)
{
    uint8_t h[44];
    uint32_t byte_rate = (uint32_t) rate * (uint32_t) ch * 2;
#define LE32(p, v) do { (p)[0] = (uint8_t) (v); (p)[1] = (uint8_t) ((v) >> 8); (p)[2] = (uint8_t) ((v) >> 16); (p)[3] = (uint8_t) ((v) >> 24); } while (0)
#define LE16(p, v) do { (p)[0] = (uint8_t) (v); (p)[1] = (uint8_t) ((v) >> 8); } while (0)
    memcpy(h, "RIFF", 4); LE32(h + 4, 36 + data_bytes); memcpy(h + 8, "WAVEfmt ", 8);
    LE32(h + 16, 16); LE16(h + 20, 1); LE16(h + 22, ch); LE32(h + 24, (uint32_t) rate); LE32(h + 28, byte_rate);
    LE16(h + 32, ch * 2); LE16(h + 34, 16); memcpy(h + 36, "data", 4); LE32(h + 40, data_bytes);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
}

static int16_t clip16(float v)
{
    int x = (int) (v * 32767.0f);
    return (int16_t) (x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

/* writes one decoded frame as interleaved s16le */
static void wav_write_frame(const AVFrame *f)
{
    int ch = f->ch_layout.nb_channels, n = f->nb_samples;
    int16_t buf[8192 * 2];
    if (!g.wav || ch < 1 || ch > 2 || n > 8192) return;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < ch; c++) {
            int16_t s = 0;
            switch (f->format) {
            case AV_SAMPLE_FMT_FLTP: s = clip16(((const float *) f->extended_data[c])[i]); break;
            case AV_SAMPLE_FMT_FLT:  s = clip16(((const float *) f->extended_data[0])[i * ch + c]); break;
            case AV_SAMPLE_FMT_S16P: s = ((const int16_t *) f->extended_data[c])[i]; break;
            case AV_SAMPLE_FMT_S16:  s = ((const int16_t *) f->extended_data[0])[i * ch + c]; break;
            case AV_SAMPLE_FMT_S32P: s = (int16_t) (((const int32_t *) f->extended_data[c])[i] >> 16); break;
            case AV_SAMPLE_FMT_S32:  s = (int16_t) (((const int32_t *) f->extended_data[0])[i * ch + c] >> 16); break;
            default: return;
            }
            buf[i * ch + c] = s;
        }
    }
    fwrite(buf, 2, (size_t) n * (size_t) ch, g.wav);
    g.wav_bytes += (uint64_t) n * (uint64_t) ch * 2;
    g.asamples += (uint64_t) n;
}

/* ---------------------------------------------------------------- audio */

static const char *acodec_name(xc_audio_codec c)
{
    switch (c) {
    case XC_AUDIO_AAC_ELD: return "aac-eld";
    case XC_AUDIO_ALAC: return "alac";
    case XC_AUDIO_AAC_LC: return "aac-lc";
    case XC_AUDIO_PCM: return "pcm";
    default: return "unknown";
    }
}

static void close_decoder(void)
{
    if (g.dec) avcodec_free_context(&g.dec);
    if (g.pkt) av_packet_free(&g.pkt);
    if (g.frm) av_frame_free(&g.frm);
}

static void cb_audio_format(void *ctx, xc_audio_codec codec, int rate, int channels, const uint8_t *cfg, size_t n)
{
    char hex[64] = { 0 };
    enum AVCodecID id = AV_CODEC_ID_NONE;
    (void) ctx;
    for (size_t i = 0; i < n && i < 30; i++) sprintf(hex + 2 * i, "%02x", cfg[i]);
    pthread_mutex_lock(&g.lock);
    logf_("audio", "format %s %d Hz %d ch, codec config %s", acodec_name(codec), rate, channels, n ? hex : "(none)");
    close_decoder();
    if (g.afile && codec != g.acodec) { fclose(g.afile); g.afile = NULL; }
    g.acodec = codec;
    g.asample_rate = rate;
    g.achannels = channels;
    if (!g.afile) {
        snprintf(g.afile_name, sizeof(g.afile_name), "audio.%s", acodec_name(codec));
        g.afile = fopen(path_join(g.o.out, g.afile_name), "ab");
    }
    if (!g.wav) {
        g.wav = fopen(path_join(g.o.out, "audio.wav"), "wb+");
        if (g.wav) wav_header(g.wav, rate, channels, 0);
    }
    switch (codec) {
    case XC_AUDIO_AAC_ELD:
    case XC_AUDIO_AAC_LC: id = AV_CODEC_ID_AAC; break;
    case XC_AUDIO_ALAC: id = AV_CODEC_ID_ALAC; break;
    default: break;
    }
    if (id != AV_CODEC_ID_NONE) {
        const AVCodec *dc = avcodec_find_decoder(id);
        g.dec = dc ? avcodec_alloc_context3(dc) : NULL;
        if (g.dec) {
            g.dec->extradata = (uint8_t *) av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(g.dec->extradata, cfg, n);
            g.dec->extradata_size = (int) n;
            g.dec->sample_rate = rate;
            av_channel_layout_default(&g.dec->ch_layout, channels);
            if (avcodec_open2(g.dec, dc, NULL) < 0) {
                logf_("audio", "libavcodec: cannot open the %s decoder", dc->name);
                avcodec_free_context(&g.dec);
            } else {
                g.pkt = av_packet_alloc();
                g.frm = av_frame_alloc();
                logf_("audio", "libavcodec %s decoder open: %d Hz, %d ch, frame size %d", dc->name, g.dec->sample_rate,
                      g.dec->ch_layout.nb_channels, g.dec->frame_size);
            }
        }
    }
    pthread_mutex_unlock(&g.lock);
}

static void cb_audio(void *ctx, const xc_audio_packet *p)
{
    uint8_t len[4];
    (void) ctx;
    pthread_mutex_lock(&g.lock);
    g.apackets++;
    g.abytes += p->size;
    if (g.afile) {
        len[0] = (uint8_t) (p->size >> 24); len[1] = (uint8_t) (p->size >> 16);
        len[2] = (uint8_t) (p->size >> 8);  len[3] = (uint8_t) p->size;
        fwrite(len, 1, 4, g.afile);
        fwrite(p->data, 1, p->size, g.afile);
    }
    if (g.acodec == XC_AUDIO_PCM && g.wav) {
        fwrite(p->data, 1, p->size, g.wav);
        g.wav_bytes += p->size;
        g.asamples += p->size / 4;
    } else if (g.dec && g.pkt) {
        int ret;
        if (av_new_packet(g.pkt, (int) p->size) == 0) {
            memcpy(g.pkt->data, p->data, p->size);
            ret = avcodec_send_packet(g.dec, g.pkt);
            av_packet_unref(g.pkt);
            if (ret < 0) {
                if (g.adecode_errors++ < 10) logf_("audio", "decode error on packet %llu: %d", (unsigned long long) g.apackets, ret);
            } else {
                while (avcodec_receive_frame(g.dec, g.frm) == 0) {
                    g.adecoded_frames++;
                    wav_write_frame(g.frm);
                    av_frame_unref(g.frm);
                }
            }
        }
    }
    pthread_mutex_unlock(&g.lock);
}

static void cb_audio_flush(void *ctx)
{
    (void) ctx;
    pthread_mutex_lock(&g.lock);
    if (g.dec) avcodec_flush_buffers(g.dec);
    pthread_mutex_unlock(&g.lock);
    logf_("audio", "flush");
}

static void cb_volume(void *ctx, float db)
{
    (void) ctx;
    g.volume_db = db;
    logf_("audio", "volume %.1f dB%s", (double) db, db <= -144.0f ? " (mute)" : "");
}

/* ---------------------------------------------------------------- video */

static void cb_video_format(void *ctx, xc_video_codec codec, int w, int h)
{
    (void) ctx;
    pthread_mutex_lock(&g.lock);
    if (g.vframes) g.vformat_changes++;
    g.vw = w;
    g.vh = h;
    if (!g.vfile || codec != g.vcodec) {
        if (g.vfile) fclose(g.vfile);
        g.vfile = fopen(path_join(g.o.out, codec == XC_VIDEO_H265 ? "video.h265" : "video.h264"), "ab");
    }
    g.vcodec = codec;
    pthread_mutex_unlock(&g.lock);
    logf_("video", "format %s %dx%d", codec == XC_VIDEO_H265 ? "H.265" : "H.264", w, h);
}

static void cb_video(void *ctx, const xc_video_frame *f)
{
    int64_t now = xc_now_ns(), lead = f->pts_ns - now;
    (void) ctx;
    pthread_mutex_lock(&g.lock);
    if (!g.got_video) {
        g.got_video = 1;
        g.first_video_ns = now;
        g.max_pts_lead_ns = g.min_pts_lead_ns = lead;
        logf_("video", "FIRST FRAME: %zu bytes, keyframe %d, %dx%d, starts with %02x %02x %02x %02x %02x",
              f->size, f->is_keyframe, f->width, f->height, f->data[0], f->data[1], f->data[2], f->data[3],
              f->size > 4 ? f->data[4] : 0);
    }
    if (lead > g.max_pts_lead_ns) g.max_pts_lead_ns = lead;
    if (lead < g.min_pts_lead_ns) g.min_pts_lead_ns = lead;
    g.last_video_ns = now;
    g.last_pts_ns = f->pts_ns;
    g.vframes++;
    g.vbytes += f->size;
    if (f->is_keyframe) g.vkeyframes++;
    if (g.vfile) fwrite(f->data, 1, f->size, g.vfile);
    pthread_mutex_unlock(&g.lock);
}

static void cb_video_flush(void *ctx)
{
    (void) ctx;
    pthread_mutex_lock(&g.lock);
    g.vflushes++;
    if (g.vfile) fflush(g.vfile);
    pthread_mutex_unlock(&g.lock);
    logf_("video", "flush (stream reset)");
}

/* ---------------------------------------------------------------- session / log / stats */

static void cb_log(void *ctx, xc_log_level level, const char *msg)
{
    static const char *tags[] = { "debug", "info", "WARN", "ERROR" };
    (void) ctx;
    logf_(tags[level & 3], "%s", msg);
}

static void cb_connected(void *ctx, const char *name, const char *model, const char *addr)
{
    (void) ctx;
    pthread_mutex_lock(&g.lock);
    g.connected = 1;
    g.sessions++;
    snprintf(g.client_name, sizeof(g.client_name), "%s", name);
    snprintf(g.client_model, sizeof(g.client_model), "%s", model);
    snprintf(g.client_addr, sizeof(g.client_addr), "%s", addr);
    pthread_mutex_unlock(&g.lock);
    logf_("conn", "CLIENT CONNECTED: \"%s\" (%s) from %s", name, model, addr);
}

static void cb_disconnected(void *ctx, const char *reason)
{
    (void) ctx;
    g.connected = 0;
    logf_("conn", "CLIENT DISCONNECTED (%s)", reason);
}

static void cb_pin(void *ctx, const char *pin)
{
    (void) ctx;
    if (pin && pin[0]) logf_("pin", "*** ENTER THIS PIN ON THE IPHONE: %s ***", pin);
    else logf_("pin", "(PIN hidden)");
}

static void cb_stats(void *ctx, const char *json)
{
    FILE *f;
    (void) ctx;
    pthread_mutex_lock(&g.lock);
    snprintf(g.last_stats, sizeof(g.last_stats), "%s", json);
    pthread_mutex_unlock(&g.lock);
    f = fopen(path_join(g.o.out, "stats.jsonl"), "a");
    if (f) {
        fprintf(f, "{\"t\":%.3f,\"core\":%s}\n", since_start(), json);
        fclose(f);
    }
    if (!g.o.quiet_stats) {
        pthread_mutex_lock(&g.lock);
        logf_("stats", "video %llu fr (%llu key) %.1f MB | audio %llu pkt, %llu decoded, %llu err | %s",
              (unsigned long long) g.vframes, (unsigned long long) g.vkeyframes, (double) g.vbytes / 1e6,
              (unsigned long long) g.apackets, (unsigned long long) g.adecoded_frames,
              (unsigned long long) g.adecode_errors, json);
        pthread_mutex_unlock(&g.lock);
    }
}

/* ---------------------------------------------------------------- main */

static void on_signal(int sig) { (void) sig; g_stop = 1; }

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --name NAME        advertised AirPlay name (default \"xbox-cast-test\")\n"
        "  --out DIR          output directory (default ./hostrx-out), created if missing\n"
        "  --seconds N        exit after N seconds (default 0 = until Ctrl-C)\n"
        "  --builtin-mdns     advertise with castcore's own mDNS responder instead of avahi\n"
        "  --no-h265          do not advertise H.265 (default: advertised)\n"
        "  --pin              require an on-screen PIN (printed in this terminal)\n"
        "  --size WxH         advertised display size (default 1920x1080)\n"
        "  --fps N            advertised max fps (default 60)\n"
        "  --port N           RTSP port (default 7000)\n"
        "  --no-low-delay     keep the iPhone's SPS (no max_num_reorder_frames=0 rewrite)\n"
        "  --quiet-stats      do not print the 1 Hz stats line\n"
        "  --debug            UxPlay protocol debug log\n", argv0);
}

static int parse_args(int argc, char **argv, options *o)
{
    static const struct option longopts[] = {
        { "name", required_argument, 0, 'n' }, { "out", required_argument, 0, 'o' },
        { "seconds", required_argument, 0, 's' }, { "builtin-mdns", no_argument, 0, 'b' },
        { "no-h265", no_argument, 0, 'H' }, { "pin", no_argument, 0, 'P' },
        { "size", required_argument, 0, 'S' }, { "fps", required_argument, 0, 'f' },
        { "port", required_argument, 0, 'p' }, { "no-low-delay", no_argument, 0, 'L' },
        { "quiet-stats", no_argument, 0, 'q' }, { "debug", no_argument, 0, 'd' },
        { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
    int c;
    memset(o, 0, sizeof(*o));
    o->name = "xbox-cast-test";
    o->out = "hostrx-out";
    o->h265 = 1;
    o->low_delay = 1;
    o->width = 1920; o->height = 1080; o->fps = 60;
    while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (c) {
        case 'n': o->name = optarg; break;
        case 'o': o->out = optarg; break;
        case 's': o->seconds = atoi(optarg); break;
        case 'b': o->builtin_mdns = 1; break;
        case 'H': o->h265 = 0; break;
        case 'P': o->pin = 1; break;
        case 'S': if (sscanf(optarg, "%dx%d", &o->width, &o->height) != 2) return -1; break;
        case 'f': o->fps = atoi(optarg); break;
        case 'p': o->port = (unsigned short) atoi(optarg); break;
        case 'L': o->low_delay = 0; break;
        case 'q': o->quiet_stats = 1; break;
        case 'd': o->debug = 1; break;
        default: return -1;
        }
    }
    return optind == argc ? 0 : -1;
}

static void make_device_id(const char *dir, unsigned char id[6])
{
    char *p = path_join(dir, "device_id");
    FILE *f = fopen(p, "rb");
    if (f && fread(id, 1, 6, f) == 6) { fclose(f); return; }
    if (f) fclose(f);
    f = fopen("/dev/urandom", "rb");
    if (!f || fread(id, 1, 6, f) != 6) {
        for (int i = 0; i < 6; i++) id[i] = (unsigned char) (rand() & 0xff);
    }
    if (f) fclose(f);
    id[0] = (unsigned char) ((id[0] & 0xfe) | 0x02);   /* locally administered unicast */
    f = fopen(p, "wb");
    if (f) { fwrite(id, 1, 6, f); fclose(f); }
}

static void summary(void)
{
    double vsecs = g.got_video ? (double) (g.last_video_ns - g.first_video_ns) / 1e9 : 0.0;
    fprintf(stderr,
        "\n================ hostrx summary ================\n"
        "sessions (client connects)   : %llu%s%s%s\n"
        "video                        : %llu frames (%llu keyframes), %.1f MB, %s\n"
        "  average fps                : %.1f over %.1f s\n"
        "  resolution / format changes: %llu (last %dx%d), stream flushes: %llu\n"
        "  pts lead vs arrival        : %.1f .. %.1f ms\n"
        "audio                        : %llu packets (%s), %.1f KB -> %s\n"
        "  decoded                    : %llu frames, %.2f s of sound, %llu decode errors\n"
        "  last volume                : %.1f dB\n"
        "last core stats              : %s\n"
        "files in                     : %s/\n"
        "================================================\n",
        (unsigned long long) g.sessions, g.sessions ? " (last: \"" : "", g.client_name,
        g.sessions ? "\")" : "",
        (unsigned long long) g.vframes, (unsigned long long) g.vkeyframes, (double) g.vbytes / 1e6,
        g.vcodec == XC_VIDEO_H265 ? "video.h265" : (g.vframes ? "video.h264" : "(none)"),
        vsecs > 0 ? (double) g.vframes / vsecs : 0.0, vsecs,
        (unsigned long long) g.vformat_changes, g.vw, g.vh, (unsigned long long) g.vflushes,
        (double) g.min_pts_lead_ns / 1e6, (double) g.max_pts_lead_ns / 1e6,
        (unsigned long long) g.apackets, g.afile_name[0] ? g.afile_name : "none", (double) g.abytes / 1e3,
        g.wav ? "audio.wav" : "(no wav)",
        (unsigned long long) g.adecoded_frames, g.asample_rate ? (double) g.asamples / g.asample_rate : 0.0,
        (unsigned long long) g.adecode_errors, (double) g.volume_db,
        g.last_stats[0] ? g.last_stats : "(none)", g.o.out);
}

int main(int argc, char **argv)
{
    xc_config cfg;
    xc_callbacks cb;
    xc_receiver *r;
    int err = 0;
    int64_t deadline;

    memset(&g, 0, sizeof(g));
    pthread_mutex_init(&g.lock, NULL);
    g.t0 = xc_now_ns();
    if (parse_args(argc, argv, &g.o)) {
        usage(argv[0]);
        return 2;
    }
    if (mkdir(g.o.out, 0755) && errno != EEXIST) {
        fprintf(stderr, "cannot create %s: %s\n", g.o.out, strerror(errno));
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = g.o.name;
    make_device_id(g.o.out, cfg.device_id);
    cfg.display_width = g.o.width;
    cfg.display_height = g.o.height;
    cfg.display_refresh = 60;
    cfg.max_fps = g.o.fps;
    cfg.allow_h265 = g.o.h265;
    cfg.pin_mode = g.o.pin;
    cfg.port = g.o.port;
    cfg.data_dir = g.o.out;
    cfg.builtin_mdns = g.o.builtin_mdns;

    memset(&cb, 0, sizeof(cb));
    cb.mdns_publish = cb_mdns_publish;
    cb.mdns_unpublish_all = cb_mdns_unpublish_all;
    cb.on_log = cb_log;
    cb.on_client_connected = cb_connected;
    cb.on_client_disconnected = cb_disconnected;
    cb.on_pin = cb_pin;
    cb.on_video_format = cb_video_format;
    cb.on_video = cb_video;
    cb.on_video_flush = cb_video_flush;
    cb.on_audio_format = cb_audio_format;
    cb.on_audio = cb_audio;
    cb.on_audio_flush = cb_audio_flush;
    cb.on_volume = cb_volume;
    cb.on_stats = cb_stats;

    logf_("info", "hostrx with %s (API %d)", xc_version_string(), xc_api_version());
    if (!g.o.builtin_mdns) {
        g.poll = avahi_threaded_poll_new();
        g.client = g.poll ? avahi_client_new(avahi_threaded_poll_get(g.poll), 0, client_cb, NULL, &err) : NULL;
        if (!g.client) {
            fprintf(stderr, "avahi: cannot connect to avahi-daemon: %s (is it running? or use --builtin-mdns)\n",
                    avahi_strerror(err));
            return 1;
        }
        avahi_threaded_poll_start(g.poll);
    }

    r = xc_create(&cfg, &cb);
    if (!r) {
        fprintf(stderr, "xc_create failed\n");
        return 1;
    }
    if (g.o.debug) xc_set_log_level(r, XC_LOG_DEBUG);
    xc_set_low_delay_sps(r, g.o.low_delay);
    if (xc_start(r)) {
        fprintf(stderr, "xc_start failed (see the log above)\n");
        xc_destroy(r);
        return 1;
    }

    fprintf(stderr,
        "\n=================================================================\n"
        " hostrx is READY. AirPlay receiver \"%s\" is advertised (%s).\n"
        " On the iPhone (same Wi-Fi as this machine):\n"
        "   1. Open Control Center, tap Screen Mirroring.\n"
        "   2. Choose \"%s\".%s\n"
        "   3. Play something with sound for a few seconds, rotate the phone once.\n"
        "   4. Stop mirroring (Control Center > Screen Mirroring > Stop Mirroring).\n"
        " Success looks like: \"CLIENT CONNECTED\", \"FIRST FRAME\", \"libavcodec aac decoder open\",\n"
        " stats lines with growing video/audio counts and 0 decode errors.\n"
        " %s Output goes to %s/.\n"
        "=================================================================\n\n",
        g.o.name, g.o.builtin_mdns ? "builtin mDNS responder" : "avahi", g.o.name,
        g.o.pin ? "\n      Enter the PIN printed here." : "",
        g.o.seconds ? "Exits by itself after the given --seconds." : "Ctrl-C to quit.", g.o.out);

    deadline = g.o.seconds > 0 ? xc_now_ns() + (int64_t) g.o.seconds * 1000000000LL : 0;
    while (!g_stop && (!deadline || xc_now_ns() < deadline)) {
        usleep(100000);
    }
    logf_("info", "stopping");
    xc_stop(r);
    xc_destroy(r);
    if (g.poll) {
        avahi_threaded_poll_stop(g.poll);
        if (g.client) avahi_client_free(g.client);
        avahi_threaded_poll_free(g.poll);
    }

    pthread_mutex_lock(&g.lock);
    if (g.dec) {   /* drain */
        avcodec_send_packet(g.dec, NULL);
        while (g.frm && avcodec_receive_frame(g.dec, g.frm) == 0) { g.adecoded_frames++; wav_write_frame(g.frm); av_frame_unref(g.frm); }
    }
    close_decoder();
    if (g.wav) { wav_header(g.wav, g.asample_rate ? g.asample_rate : 44100, g.achannels ? g.achannels : 2, (uint32_t) g.wav_bytes); fclose(g.wav); }
    if (g.afile) fclose(g.afile);
    if (g.vfile) fclose(g.vfile);
    summary();
    pthread_mutex_unlock(&g.lock);
    return 0;
}
