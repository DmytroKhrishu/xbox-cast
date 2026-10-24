#!/bin/sh
# xbox-cast media harness: generate test fixtures with the ffmpeg CLI (nothing binary is committed).
# Usage: make_fixtures.sh <out-dir>
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
OUT=${1:?out dir}
mkdir -p "$OUT"
cd "$OUT"
FF="ffmpeg -hide_banner -loglevel error -y"

# iOS-like H.264: High profile, 4 refs, POC type 0 (x264 only uses POC type 0 when B-frames are allowed, so allow one
# but bias it away: no B slice is ever coded; ReorderFix would log one). x264 writes a bitstream_restriction; the
# harness strips it again to reproduce exactly what the iPhone sends (no restriction).
X264_IOS="-c:v libx264 -profile:v high -preset veryfast -x264-params bframes=1:b-adapt=1:b-bias=-100:b-pyramid=none:ref=4:keyint=60:scenecut=0 -pix_fmt yuv420p -bsf:v h264_mp4toannexb"

# landscape phone 1920x886 (the iPhone frame), 60 frames
[ -f land.h264 ] || $FF -f lavfi -i testsrc2=s=1920x886:r=60:d=1 $X264_IOS -f h264 land.h264
# portrait phone ~500x1080 (rotation: width follows the phone), 30 frames
[ -f port.h264 ] || $FF -f lavfi -i testsrc2=s=496x1080:r=60:d=0.5 $X264_IOS -f h264 port.h264
# rotation mid-stream: landscape -> portrait -> landscape (each part starts with SPS/PPS + IDR)
[ -f rotate.h264 ] || cat land.h264 port.h264 land.h264 > rotate.h264

# HEVC Main and Main10 (P010 path), Annex-B
[ -f main.hevc ] || $FF -f lavfi -i testsrc2=s=1280x720:r=60:d=0.5 -c:v libx265 -preset ultrafast -x265-params log-level=error:bframes=0 -pix_fmt yuv420p -f hevc main.hevc
[ -f main10.hevc ] || $FF -f lavfi -i testsrc2=s=1280x720:r=60:d=0.5 -c:v libx265 -preset ultrafast -x265-params log-level=error:bframes=0 -pix_fmt yuv420p10le -f hevc main10.hevc

# --- crop detection material (encoded + decoded for real compression noise) ---
X264_Q="-c:v libx264 -preset veryfast -crf 20 -bf 0 -pix_fmt yuv420p -bsf:v h264_mp4toannexb -f h264"
# 16:9 video inside a landscape phone frame (pillarbox): 1576x886 content in 1920x886
[ -f crop_pillar.h264 ] || $FF -f lavfi -i testsrc2=s=1576x886:r=30:d=2 -vf "pad=1920:886:172:0:black" $X264_Q crop_pillar.h264
# 16:9 video inside a portrait phone frame (letterbox): 496x280 content in 496x1080
[ -f crop_portrait_video.h264 ] || $FF -f lavfi -i testsrc2=s=496x280:r=30:d=2 -vf "pad=496:1080:0:400:black" $X264_Q crop_portrait_video.h264
# 2.39:1 movie letterboxed in a 16:9 frame, plus film grain
[ -f crop_scope.h264 ] || $FF -f lavfi -i "mandelbrot=s=1920x804:r=30,noise=alls=6:allf=t" -t 2 -vf "pad=1920:1080:0:138:black" $X264_Q crop_scope.h264
# very dark scene with grain inside letterbox bars: must keep the bars, never crop the dark content
[ -f crop_dark.h264 ] || $FF -f lavfi -i "testsrc2=s=1920x804:r=30,eq=brightness=-0.42:contrast=0.25,noise=alls=10:allf=t" -t 2 -vf "pad=1920:1080:0:138:black" $X264_Q crop_dark.h264
# full-frame content (no bars): must stay full
[ -f crop_full.h264 ] || $FF -f lavfi -i testsrc2=s=1920x886:r=30:d=2 $X264_Q crop_full.h264
# fade to black and back (all-black frames carry no information): bright 0-1 s, fade out 1-1.5, black 1.5-2.5,
# fade in 2.5-3, bright to 4 s. (Two chained fade filters would black out the start; use one time expression.)
[ -f crop_fade.h264 ] || $FF -f lavfi -i testsrc2=s=1576x886:r=30:d=4 -vf "eq=eval=frame:brightness='if(lt(t,1),0,if(lt(t,1.5),-2*(t-1),if(lt(t,2.5),-1,if(lt(t,3),-2*(3-t),0))))',pad=1920:886:172:0:black" $X264_Q crop_fade.h264

# --- audio ---
# AAC-LC and ALAC (352 frames/packet like AirPlay) in MP4 so the harness can read raw access units + config
[ -f aaclc.m4a ] || $FF -f lavfi -i "sine=f=440:r=44100:d=1" -ac 2 -c:a aac -b:a 128k aaclc.m4a
[ -f alac.m4a ] || $FF -f lavfi -i "sine=f=440:r=44100:d=1" -ac 2 -c:a alac -frame_size 352 -sample_fmt s16p alac.m4a
# AAC-ELD: no GPL-compatible ELD encoder in ffmpeg (libfdk_aac is absent/non-free); use FFmpeg's own FATE conformance
# sample (ER AAC-ELD, 44.1 kHz). Optional: skipped when offline.
if [ ! -f eld.mp4 ]; then
    curl -fsSL --max-time 60 -o eld.mp4.part https://fate-suite.ffmpeg.org/aac/er_eld1001np_44_ep0.mp4 && mv eld.mp4.part eld.mp4 || rm -f eld.mp4.part
fi
echo "fixtures ready in $OUT"
