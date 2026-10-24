/*
 * AAC-ELD decode check (xbox-cast castcore). GPL-3.0-or-later.
 *
 * AirPlay mirroring audio is AAC-ELD, 44.1 kHz stereo, 480 samples per frame, raw access units
 * (no ADTS/LATM). castcore hands the app those packets plus the AudioSpecificConfig f8 e8 50 00.
 * This test opens FFmpeg's native "aac" decoder exactly like hostrx does (extradata = config,
 * sample_rate, default channel layout) and checks:
 *   1. with castcore's AirPlay config the decoder opens as 44100 Hz / 2 ch;
 *   2. if a sample file is given (argv[1] or $XC_ELD_SAMPLE; CI uses FFmpeg's FATE conformance
 *      stream aac/er_eld1001np_44_ep0.mp4), every raw ELD access unit of it decodes without error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>

#ifndef AV_PROFILE_AAC_ELD   /* FFmpeg < 6.1 */
#define AV_PROFILE_AAC_ELD FF_PROFILE_AAC_ELD
#endif

static int failures;
#define CHECK(cond, what) do { int ok_ = (cond); printf("  %s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

/* same setup as hostrx cb_audio_format */
static AVCodecContext *open_aac(const uint8_t *cfg, int n, int rate, int channels)
{
    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_AAC);
    AVCodecContext *c = dc ? avcodec_alloc_context3(dc) : NULL;
    if (!c) return NULL;
    c->extradata = (uint8_t *) av_mallocz((size_t) n + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(c->extradata, cfg, (size_t) n);
    c->extradata_size = n;
    c->sample_rate = rate;
    av_channel_layout_default(&c->ch_layout, channels);
    if (avcodec_open2(c, dc, NULL) < 0) avcodec_free_context(&c);
    return c;
}

int main(int argc, char **argv)
{
    static const uint8_t airplay_eld[4] = { 0xf8, 0xe8, 0x50, 0x00 };   /* castcore's on_audio_format config */
    const char *sample = argc > 1 ? argv[1] : getenv("XC_ELD_SAMPLE");
    AVCodecContext *c = open_aac(airplay_eld, 4, 44100, 2);

    printf("AirPlay AAC-ELD config f8e85000\n");
    CHECK(c != NULL, "FFmpeg aac decoder opens with the ELD AudioSpecificConfig");
    if (c) {
        printf("       %d Hz, %d ch, frame_size %d, profile %d\n", c->sample_rate, c->ch_layout.nb_channels, c->frame_size,
               c->profile);
        CHECK(c->sample_rate == 44100 && c->ch_layout.nb_channels == 2, "44100 Hz stereo");
        avcodec_free_context(&c);
    }

    if (!sample || !sample[0]) {
        printf("no ELD sample given (argv[1] / XC_ELD_SAMPLE): skipping the decode part\n");
    } else {
        AVFormatContext *fmt = NULL;
        int si = -1, packets = 0, frames = 0, errors = 0, frame_size = 0;
        long samples = 0;
        printf("decoding %s\n", sample);
        CHECK(avformat_open_input(&fmt, sample, NULL, NULL) == 0 && avformat_find_stream_info(fmt, NULL) >= 0,
              "sample opens");
        if (fmt) si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (si >= 0) {
            AVCodecParameters *par = fmt->streams[si]->codecpar;
            AVPacket *pkt = av_packet_alloc();
            AVFrame *frm = av_frame_alloc();
            printf("       stream: profile %d, %d Hz, %d ch, config", par->profile, par->sample_rate, par->ch_layout.nb_channels);
            for (int i = 0; i < par->extradata_size; i++) printf(" %02x", par->extradata[i]);
            printf("\n");
            CHECK(par->profile == AV_PROFILE_AAC_ELD, "sample is AAC-ELD");
            c = open_aac(par->extradata, par->extradata_size, par->sample_rate, par->ch_layout.nb_channels);
            CHECK(c != NULL, "decoder opens with the sample's config (hostrx setup)");
            while (c && av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == si) {
                    packets++;
                    /* raw access unit, as castcore delivers it */
                    if (avcodec_send_packet(c, pkt) < 0) errors++;
                    while (avcodec_receive_frame(c, frm) == 0) {
                        frames++;
                        samples += frm->nb_samples;
                        frame_size = frm->nb_samples;
                        av_frame_unref(frm);
                    }
                }
                av_packet_unref(pkt);
            }
            printf("       %d packets -> %d frames of %d samples, %.2f s, %d errors\n", packets, frames, frame_size,
                   par->sample_rate ? (double) samples / par->sample_rate : 0.0, errors);
            CHECK(packets > 100 && errors == 0, "every access unit decodes");
            /* ELD frameLengthFlag (bit 19 = byte 2 mask 0x10 in these configs): 1 = 480 (AirPlay), 0 = 512 (FATE stream) */
            CHECK(frames >= packets - 2 &&
                  frame_size == ((par->extradata_size >= 3 && (par->extradata[2] & 0x10)) ? 480 : 512),
                  "one frame per access unit, frame length as the config says");
            av_frame_free(&frm);
            av_packet_free(&pkt);
            avcodec_free_context(&c);
        }
        avformat_close_input(&fmt);
    }
    printf(failures ? "\n%d FAILURES\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
