# AirPlay screen mirroring: protocol notes (as UxPlay implements it)

For the `core` (castcore) and `media` agents. Reference tree: UxPlay `master` @ `116e390` (2026-10-24),
<https://github.com/FDH2/UxPlay>. Paths below are relative to that repo. Everything here is read from the source or
UxPlay's README/issues; items marked **UNVERIFIED** were not confirmed from code or a capture.

## 1. Session flow

| Step | Transport | Endpoint (handler) | Notes |
|---|---|---|---|
| Discovery | mDNS | `_airplay._tcp` + `_raop._tcp` (see §2) | both point at the same TCP port |
| Capabilities | RTSP/HTTP on the main TCP port | `GET /info` → `raop_handler_info` (`lib/raop_handlers.h`) | binary plist: `audioFormats`, `audioLatencies`, `displays[0]` (width/height/refresh/maxFPS/overscanned), features, `pk`, model |
| Pairing (transient, default) | same | `POST /pair-setup` → `raop_handler_pairsetup`, `POST /pair-verify` → `raop_handler_pairverify` | Ed25519 + Curve25519 (`lib/pairing.c`, `lib/crypto.c`) |
| PIN pairing (optional) | same | `POST /pair-pin-start`, `POST /pair-setup-pin` (SRP-6a, `lib/srp.c`) | UxPlay `-pin`/`-reg`; MDM-managed iPhones *require* it (README "pin-authentication") |
| FairPlay handshake | same | `POST /fp-setup` → `raop_handler_fpsetup` → `lib/playfair/` | yields the AES key (`ekey`) used for audio and to derive video keys |
| Stream setup | same | `SETUP` → `raop_handler_setup` | 1st SETUP: `ekey`, `eiv`, timing port (NTP). 2nd SETUP: `streams[]` with `type` 110 (video) / 96 (audio) |
| Start | same | `RECORD`, then `SET_PARAMETER` (volume, progress, metadata), `GET_PARAMETER` | |
| Keep-alive | same | `POST /feedback` every ~2 s | missing feedback = client gone |
| Stop | same | `TEARDOWN` (per stream type, or typeless) | iOS 27 sends typeless TEARDOWN; UxPlay issue #540 (fixed) |

Request routing table: `lib/raop.c` `conn_request()` (~line 400-460): `/feedback`, `/pair-pin-start`, `/pair-setup-pin`,
`/pair-setup`, `/pair-verify`, `/fp-setup`, `/audioMode`, `GET /info`, `OPTIONS`, `SETUP`, `GET_PARAMETER`,
`SET_PARAMETER`, `RECORD`, `FLUSH`, `TEARDOWN`; HLS casting adds `/reverse`, `/play`, `/scrub`, `/rate`, `/stop`,
`/action`, `/playback-info`, `/getProperty`, `/setProperty`.

Ports: by default all random (bind 0). `-p` legacy ports are TCP 7100 (main) / 7000 / 7001, UDP 6000 / 6001 / 7011
(README `-p`). Xbox forbids binding 57344-65535 (see `xbox-platform.md`), so bind 0 or a fixed port in 1025-49151.

## 2. mDNS advertisement

Built in `lib/dnssd.c` (`dnssd_register_raop`, `dnssd_register_airplay`) + constants in `lib/dnssdint.h`; the built-in
responder builds the same TXT in `lib/mdnsd/dnssd_mdnsd.c` (~line 120-170).

`_raop._tcp` instance name is `<deviceid-hex>@<name>`; TXT:

| key | value (UxPlay) | meaning |
|---|---|---|
| `ch` `cn` `ss` `sr` | `2`, `0,1,2,3`, `16`, `44100` | 2 ch; PCM/ALAC/AAC/AAC-ELD; 16-bit 44.1 kHz |
| `et` | `0,3,5` | encryption: none, FairPlay, FairPlay SAPv2.5 |
| `ft` | features (same as `_airplay` `features`) | |
| `am` | `AppleTV3,2` (`lib/global.h` `GLOBAL_MODEL`) | the model the iPhone thinks it talks to |
| `vs` / `vn` / `vv` / `txtvers` | `220.68` / `65537` / `2` / `1` | source version etc. |
| `pk` | Ed25519 public key (hex) | must match `/info` |
| `pw` | `true`/`false` | password/PIN required |
| `sf` `da` `sv` `tp` `md` `rhd` | `0x4`, `true`, `false`, `UDP`, `0,1,2`, `5.6.0.0` | |

`_airplay._tcp` instance name is `<name>`; TXT: `deviceid` (MAC-style `AA:BB:..`), `features` (`0xLO,0xHI`), `flags`
`0x4`, `model`, `pk`, `pi` (a fixed UUID), `srcvers`, `vv`, `pw`.

### Feature bits that matter (set in `uxplay.cpp` ~line 2393-2455, `dnssd_set_airplay_features`)

| bit | UxPlay default | meaning / relevance for us |
|---|---|---|
| 0 | 0 (1 with `-hls`) | AirPlay video (URL casting) |
| 4 | 0 (1 with `-hls`) | HLS supported |
| 7 | 1 | screen mirroring |
| 9 | 1 | audio |
| 11 | 1 | audio packet redundancy |
| 12, 14, 22 | 1 | FairPlay auth (required: there is no non-FairPlay mirroring path) |
| 17, 15, 16 | 1 | metadata: text, artwork, progress |
| 18-21 | 1 | audio formats (19/20 "must be set for AirPlay 2 multiroom", but UxPlay does not implement multiroom) |
| 27 | 1 | supports legacy pairing (`FEATURES_1 0x5A7FFEE6` vs `0x527FFEE6` in `dnssdint.h`) |
| 30 | 1 | RAOP supported |
| 40 | 0 | buffered audio (AirPlay 2) - off |
| 41 | 0 | PTP timing - off (NTP is used) |
| **42** | 0 (1 with `-h265`) | **"Screen Multi Codec": allows H.265 mirroring** |
| 43, 46 | 0 | system / HomeKit pairing - off |
| 48, 51 | 0 | CoreUtils pairing / unified pair-setup + MFi - off (we can't do MFi) |

## 3. The `displays` plist in `GET /info` (what decides the encoded size)

`lib/raop_handlers.h` ~line 216-240 writes `displays[0]` with `uuid`, `widthPhysical`, `heightPhysical`, `width`,
`height`, `widthPixels`, `heightPixels`, `rotation`, `refreshRate`, `maxFPS`, `overscanned`. Values come from
`raop_set_plist()` (`lib/raop.c` ~line 700-760); defaults `1920x1080`, refreshRate 60, **maxFPS 30**, overscanned 0
(`lib/raop.c` ~line 620).

UxPlay README, `-s wxh`: "This is just a request made to the AirPlay client ... the **height** pixel size is the
controlling one used by the client for determining the streaming format; the **width is dynamically adjusted to the
shape of the image** (portrait or landscape format)". `-fps n` raises maxFPS (default 30; "60 fps may give you improved
video"). `-o` (overscanned) makes the client add an empty border inside the requested size; README: "don't use this
option".

## 4. Video stream (TCP, `type 110`)

File: `lib/raop_rtp_mirror.c`, thread `raop_rtp_mirror_thread()`.

- One TCP connection to the receiver's `dataPort` (returned in the SETUP reply). The iPhone connects; we accept.
- Every packet = **128-byte header** + payload. Header: bytes 0-3 payload size (little-endian), bytes 4-5 payload type,
  bytes 6-7 "option", bytes 8-15 NTP timestamp of the frame (sender clock; converted with the NTP offset, §6).
- Payload types (`packet[4]`, comment block ~line 338-365, `switch` at ~line 388):

| type | encrypted | content |
|---|---|---|
| `0x00` | yes | one frame: one or more NAL units in **AVCC** form (4-byte big-endian length prefix). IDR frames carry type 5 NALs; M1/M2 Macs prepend an SEI (type 6) |
| `0x01` | no | codec config: SPS + PPS (H.264) or VPS+SPS+PPS (H.265, option `0x1e 0x01`) **plus image size floats** in header bytes 16-63 |
| `0x02` | no | old-protocol heartbeat (AirMyPC), no payload |
| `0x05` | no | "streaming report", once per second, binary plist, no timestamp |

- Option bytes on `0x01`: `0x16 0x01` H.264 normal; `0x56 0x01` H.264 "stream stopping / client sleeps" (screen
  locked, app with DRM, etc.); `0x1e 0x01` H.265; `0x5e 0x01` H.265 stopping. UxPlay maps these to `video_pause` /
  `video_resume` callbacks.
- **Annex-B conversion**: UxPlay rewrites each 4-byte AVCC length into `00 00 00 01` in place (~line 450-475) and
  prepends the last SPS/PPS to the next IDR (same timestamp). Feed FFmpeg either Annex-B as UxPlay does, or keep AVCC and
  build `extradata` (avcC/hvcC) from the `0x01` packet. Annex-B is simpler and re-sends parameter sets with every IDR.
- **Encryption**: AES-128-CTR, one continuous keystream per connection (state carries across packets).
  `lib/mirror_buffer.c` `mirror_buffer_init_aes()`: key = SHA-512("AirPlayStreamKey" + streamConnectionID, ekey)[0:16],
  IV = SHA-512("AirPlayStreamIV" + streamConnectionID, ekey)[0:16], where `ekey` is the FairPlay-decrypted audio key
  and `streamConnectionID` comes from the `type 110` stream dict in SETUP. Decrypt with `mirror_buffer_decrypt()`.
- **Size info** (`case 0x01`, ~line 553-625): floats at header offsets 16/20 `width_source/height_source`, 40/44 (copy),
  48/52 `x_offset/y_offset`, 56/60 `width/height`. Passed to `video_set_codec_and_size` / `video_report_size`.
  Real log from a portrait iPhone at the default 1920x1080 request: `wxh = 500x1080; source 500x1080`
  (UxPlay issue #140).
- **Rotation / resize**: there is no separate event. The sender stops, sends a new `0x01` packet (new SPS/PPS, new
  size) and an IDR. The decoder must accept a mid-stream SPS change (re-init or let FFmpeg reconfigure). UxPlay #606:
  rotation also breaks MP4 recording for the same reason.
- **Packet loss**: it is TCP, so no loss, only stalls. When Wi-Fi degrades the sender drops its frame rate/bitrate;
  there is no NACK/FEC to implement. A stalled TCP read just delays frames: render latest-wins, never queue.
- B-frames: iOS sends none, but its SPS (High, POC type 0, 4 refs) has **no VUI bitstream_restriction**, so a
  conforming decoder holds up to 4+ frames for reordering. The Xbox port `vproHacks/screen-mirror-xbox` measured
  130-700 ms extra latency from this with the platform decoder and fixed it by rewriting the SPS in flight to
  `max_num_reorder_frames=0` (`docs/DEVLOG.md`, `src/app/H264Sps.cpp`). With FFmpeg use `AV_CODEC_FLAG_LOW_DELAY` and
  `flags2 |= AV_CODEC_FLAG2_FAST`/`has_b_frames=0`; verify frames come out 1-in-1-out.

## 5. Audio in mirroring (UDP RTP, `type 96`)

Files: `lib/raop_rtp.c`, `lib/raop_buffer.c`, `lib/raop_ntp.c`.

- 2nd SETUP stream dict `type 96` with `ct` (compression type: 2 = ALAC, 8 = AAC-ELD, 4 = AAC-main), `spf`
  (samples/frame: 480 AAC-ELD, 352 ALAC), `audioFormat`, `usingScreen`, `isMedia`, `controlPort`
  (`raop_handlers.h` ~line 985-1040). **Mirroring uses AAC-ELD, 44.1 kHz stereo, 480 samples/packet**
  (`raop_rtp.c` ~line 112, 560). Audio-only (speaker) mode uses ALAC 352.
- RTP over UDP on our `dataPort`; a control port for resend requests (only if the client gave `controlPort` != 0);
  payload AES-128-CBC with `ekey`/`eiv` (whole 16-byte blocks only; remainder in clear) - see `raop_buffer.c`.
- Decoding: FFmpeg's native `aac` decoder handles AAC-ELD (the Xbox port replaced fdk-aac, which is not GPL-compatible,
  with FFmpeg's decoder; `screen-mirror-xbox` `docs/DEVLOG.md`). Do not use fdk-aac.
- Timing: NTP client in `raop_ntp.c` polls the iPhone's timing port (UDP) to compute the clock offset. Video and audio
  carry sender-NTP timestamps; UxPlay's default `-vsync` mode schedules both against that clock. `-vsync no` renders
  video as it arrives ("appropriate for live streaming ... no video frames are dropped") - that is our low-latency mode.
- `audioLatencies` in `/info` reports input/output latency 0; `-al` (reported audio latency) only affects ALAC mode and
  "the client appears to ignore this" (README `-al`). There is no knob that makes iOS buffer less.

## 6. What this means for "fill the screen"

1. **The iPhone never pads to our aspect.** Height is the controlling dimension; width follows the content shape. A
   portrait iPhone at the default `1920x1080` request sends ~`500x1080` (logged in UxPlay #140; iPhone screens are
   ~19.5:9, so ~498x1080). Landscape sends ~`1920x886`-ish (full width, height reduced to keep the phone's aspect) -
   **UNVERIFIED exact numbers**; confirm with `hostrx` logs of `width_source/height_source`.
2. So black bars are ours to draw, not baked into the stream (unless `overscanned`=true, which adds a real border -
   never set it). `width/height` + `x_offset/y_offset` in the `0x01` header tell the exact picture rectangle; use them
   rather than assuming the coded size (coded height may be rounded up to 16 - crop to `width x height`).
3. **Advertising a phone-shaped display does not help**: the request is a bounding box, the height wins; asking for
   e.g. `1080x2340` would just make the portrait stream taller (more pixels, more bitrate), and landscape would get
   narrower. Advertise the TV's shape and do fill/zoom/crop in our renderer. Recommended: `1920x1080@60`, maxFPS 60.
4. "Auto-crop" for apps that letterbox *inside* the phone (16:9 video on a 19.5:9 screen in landscape) needs content
   analysis (detect black rows/cols) - the stream carries no hint.
5. Rotation changes the size: every new `0x01` packet must reset the fill/zoom geometry (and the auto-crop detector).
6. H.265 (bit 42) only yields more than 1080p if the requested height is > 1080 (README `-h265`: "Only recent Apple
   devices ... can send h265 video if a resolution with h > 1080 is requested"; `-h265` changes the default to 3840x2160
   and keeps 30 fps). It costs bandwidth and decode memory for no gain on a 1080p UI path; keep it off in v1.

## 7. Version notes (iOS 17/18/26/27)

- iOS 26.x: works with UxPlay 1.73.x (issues #547, #564 were closed as client/network problems).
- iOS 27 / iPadOS 27 betas send `combinedGetInfoWithControlSetup = true` in SETUP (new key). UxPlay #535 (open): one
  user's client never opens the data connection; another got `received type 0x01 packet with no payload` until `-h265`
  was enabled (then the client mirrored in HEVC); the maintainer reports iPadOS 27 beta 5 "seems to work fine". Treat
  **iOS 27 as the top protocol risk: be ready to enable H.265 (bit 42) and implement HEVC decode** if 27.0 final keeps
  this behaviour. Typeless TEARDOWN on iOS 27 was fixed in #540; take UxPlay >= that commit.
- Apple Music on **macOS** uses FairPlay type 2 which playfair can't do (UxPlay #570) - audio-only from Mac Music fails.

## 8. Porting facts already proven on Xbox (by others)

Two GPL-3.0 UWP ports of UxPlay `lib/` exist and both report working mirroring on real consoles in Dev Mode:

- `vproHacks/screen-mirror-xbox` (Xbox One, created 2026-10-17): mirroring + AAC-ELD audio, ALAC speaker mode, HLS/YouTube
  casting, rotation; long-session test 9,608/9,608 frames, player ~50-80 ms behind the newest frame. Its patch to
  UxPlay lib is in `docs/uxplay-lib-changes.patch`: pthread/unistd shims, OpenSSL keyfile via memory BIO (no stdio in
  UWP OpenSSL), `RAND_bytes` in `srp.c`, kernel receive timestamps disabled, mirroring socket `shutdown()` on stop
  (Windows `SO_RCVTIMEO` with a timeval = no timeout), FairPlay mode byte > 3 rejected (OOB read), a PTTH/reverse-HTTP
  bug fix, and the mDNS adapter fix (§ in `xbox-platform.md`).
- `CansDE/airplay-xbox` (Series X, created 2026-10-12): FFmpeg D3D11VA + XAudio2, UxPlay built-in `mdnsd`; reports
  200-300 ms latency with a deliberate 200 ms sync buffer, and a fix for UxPlay closing sockets in `raop_ntp.c` /
  `mdnsd_stop` while threads still use them, which "crashed the app with 0xC0000008 when it was suspended".

Take both patch sets into account before writing our own shims.
