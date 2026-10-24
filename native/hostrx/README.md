# hostrx: Linux test receiver for castcore

hostrx runs castcore on a Linux machine so an iPhone can mirror to it. It writes what arrives to files and decodes
the audio with libavcodec, which proves the whole receive path (discovery, pairing, FairPlay, decryption, framing,
audio config) without the Xbox.

## Build

```sh
sudo apt-get install cmake pkg-config libssl-dev libplist-dev libavahi-client-dev \
                     libavcodec-dev libavformat-dev libavutil-dev
cmake -S native -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build            # optional: unit tests
```

## iPhone test (the owner's part)

The iPhone must be on the same LAN / Wi-Fi as the Linux machine. avahi-daemon must be running (it normally is).
Open TCP 7000 and UDP (any port, castcore uses ephemeral ports) if a firewall is active.

```sh
./build/hostrx --name xbox-cast-test --out /tmp/hostrx-out --seconds 60
```

Then on the iPhone: Control Center > Screen Mirroring > **xbox-cast-test**. Play a video with sound for ~20 s,
rotate the phone once, then Stop Mirroring. hostrx exits by itself after 60 s (or Ctrl-C) and prints a summary.

Variants:

| Option | Use |
|---|---|
| `--builtin-mdns` | advertise with castcore's own responder (the Xbox default) instead of avahi |
| `--no-h265` | do not offer HEVC (iPhone then always sends H.264) |
| `--pin` | require the on-screen PIN (printed in the terminal) |
| `--size 3840x2160 --fps 60` | advertise a 4K display (newer iPhones then choose HEVC) |
| `--no-low-delay` | deliver the iPhone's SPS unmodified |
| `--debug` | UxPlay's protocol log (RTSP requests, pairing steps) |
| `--quiet-stats` | no per-second stats line |

### What success looks like

```
[   0.008] info  advertised "xbox-cast-test" through mdns_publish
 hostrx is READY. ...
[  12.345] conn  CLIENT CONNECTED: "<iPhone name>" (iPhone16,1) from <phone-ip>
[  12.400] audio format aac-eld 44100 Hz 2 ch, codec config f8e85000
[  12.401] audio libavcodec aac decoder open: 44100 Hz, 2 ch, ...
[  12.600] video format H.264 1920x1080          (or H.265; size follows the phone's aspect)
[  12.601] video FIRST FRAME: ... keyframe 1, ..., starts with 00 00 00 01 67
[  13.000] stats video 31 fr (1 key) ... | audio 43 pkt, 43 decoded, 0 err | {"connected":true,...}
...
[  25.000] video format H.264 1080x1920           (after rotating)
[  30.000] conn  CLIENT DISCONNECTED (closed)
```

and in the summary: video frames > 0 at roughly the phone's frame rate, audio packets > 0 with **0 decode errors**,
`audio.wav` with the sound you played. The video file plays with `ffplay /tmp/hostrx-out/video.h264` (or `.h265`),
the audio with `ffplay /tmp/hostrx-out/audio.wav`.

## Output files (`--out DIR`, default `./hostrx-out`)

| File | Content |
|---|---|
| `video.h264` / `video.h265` | Annex-B elementary stream exactly as `on_video` delivered it (parameter sets on keyframes) |
| `audio.aac-eld` (`.alac`, `.aac-lc`, `.pcm`) | compressed packets, each prefixed by a 4-byte big-endian length |
| `audio.wav` | the audio decoded by libavcodec, s16le |
| `stats.jsonl` | castcore's 1 Hz `on_stats` JSON, one line per second |
| `castcore-ed25519.pem`, `device_id` | the receiver identity (kept between runs, so the iPhone sees the same device) |

## Self-test without an iPhone

```sh
./build/hostrx --name xbox-cast-test --out /tmp/hrx --seconds 15 &
avahi-browse -rt _airplay._tcp      # shows "xbox-cast-test" with deviceid/features/pk/... TXT
avahi-browse -rt _raop._tcp         # shows "<DEVICEID>@xbox-cast-test"
# GET /info must be RTSP/1.0 (UxPlay treats HTTP/1.1 requests as the HLS video channel, so plain curl gets nothing):
printf 'GET /info RTSP/1.0\r\nCSeq: 1\r\nX-Apple-ProtocolVersion: 1\r\n\r\n' | nc -q1 127.0.0.1 7000 | head -c 200
# -> "RTSP/1.0 200 OK ... Content-Type: application/x-apple-binary-plist ... Content-Length: 884" + bplist00...
```

`ctest` covers the same without the network: `test_castcore` (TXT records, GET /info plist, identity persistence),
`test_h264_sps` (low-delay SPS rewrite decoded by FFmpeg), `test_aac_eld` (FFmpeg decodes AAC-ELD with castcore's
config; set `XC_ELD_SAMPLE=<FFmpeg FATE aac/er_eld1001np_44_ep0.mp4>` for the full decode).

`--builtin-mdns` works next to avahi-daemon on the same machine (both bind UDP 5353 with SO_REUSEADDR); the services
then resolve to host `<hostname>-xc.local`.
