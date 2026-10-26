# castcore: the AirPlay receiver core

`native/` builds **castcore**, a static C11 library that turns a machine into an AirPlay screen-mirroring + audio
receiver, plus **hostrx**, a Linux test receiver. The app (UWP, `app/`) links castcore through the C API in
`native/include/castcore.h` (= `docs/contract.md` section 1) and renders what castcore hands it.

castcore is a thin wrapper around the protocol library of [UxPlay](https://github.com/FDH2/UxPlay) (GPL-3.0),
vendored in `native/third_party/uxplay/lib/`. UxPlay does RTSP, pairing (SRP / ed25519), FairPlay, the mirror and
audio RTP streams, decryption, the NTP timing channel and the mDNS TXT records. castcore adds: the `xc_*` API, Annex-B
framing guarantees, the low-delay SPS rewrite, the clock mapping, persistent identity, a supervisor thread (stats,
heartbeat, resets) and the choice between the app's mDNS publisher and UxPlay's builtin responder.

```
 iPhone ──RTSP/TCP 7000──> httpd (UxPlay)  ──callbacks──> castcore.c ──xc_callbacks──> app (MediaPipeline)
        ──mirror TCP────> raop_rtp_mirror ─┘    │
        ──audio UDP─────> raop_rtp ────────┘    ├─ h264_sps.c   (low-delay SPS rewrite)
        <─NTP UDP──────── raop_ntp              └─ supervisor thread (1 Hz stats, heartbeat, resets)
 mDNS:  mdns_publish(app)  or  builtin_mdns=1 → UxPlay lib/mdnsd (UDP 5353)
```

## Build

| Target | Command | Output |
|---|---|---|
| Linux (castcore, hostrx, tests) | `cmake -S native -B build && cmake --build build && ctest --test-dir build` | `build/libcastcore.a`, `build/hostrx` |
| UWP x64 | `pwsh native/build-uwp.ps1 -Arch x64 -Config Release` | `native/out/uwp-x64/{include,lib,bin,licenses}` + `lib/link.txt` |

Linux needs `libssl-dev libplist-dev libavahi-client-dev libavcodec-dev libavformat-dev libavutil-dev` (FFmpeg only
for hostrx and the tests). The UWP script finds Visual Studio with vswhere, takes vcpkg from `VCPKG_ROOT`,
`VCPKG_INSTALLATION_ROOT` (GitHub runners) or clones it, installs `native/vcpkg.json` (openssl, libplist) for the
`x64-uwp-static-md` triplet (static libs, dynamic CRT `/MD` as UWP requires), configures CMake with
`CMAKE_SYSTEM_NAME=WindowsStore`, builds castcore plus `castcore_linkcheck.dll` (a DLL linked with `/APPCONTAINER`
against the whole public API, so an unresolved symbol or a non-app-partition import fails in the core build, not in
the app build) and installs. `lib/castcore.lib` is self-contained: castcore + UxPlay + OpenSSL libcrypto + libplist
merged with `lib.exe`, and the system import libraries (`ws2_32 iphlpapi bcrypt crypt32`) are recorded in it with
`#pragma comment(lib)`. So `link.txt` is the single line `castcore.lib`, and the script re-links the link-check
object against that file alone (plus `WindowsApp.lib`, `/APPCONTAINER`) to prove the app needs nothing else.
(A multi-line `link.txt` reached link.exe as one file name `a.lib;b.lib;...` through the app project's property
function, run 40139956250; a single entry sidesteps that.) No runtime DLLs: `bin/` is empty. CI: `.github/workflows/core.yml` (`linux` job: build + `ctest`; `uwp` job: the
script above, vcpkg binary cache in `actions/cache`, artifact `castcore-uwp-x64`).

MSVC compiles UxPlay as C11 with `src/compat/msvc/` (a pthread subset on SRW locks / condition variables, `usleep`,
a force-included header for `ssize_t` / `strcasecmp`), taken from
[screen-mirror-xbox](https://github.com/vproHacks/screen-mirror-xbox) (GPL-3.0). `WINAPI_FAMILY=WINAPI_FAMILY_APP`
is set explicitly (Ninja does not set it). Every Win32 call in the Windows paths was checked against the SDK
partitions: `GetAdaptersAddresses`, `GetIfEntry2`, `FormatMessageA`, `GetSystemTimeAsFileTime`, `WSAIoctl`,
`QueryPerformanceCounter` and `gethostname` (no partition guard in winsock2.h) are available to apps; CryptoAPI is
not (srp.c patch: OpenSSL seeds from BCryptGenRandom). Locally the Windows
paths are syntax-checked with mingw-w64 gcc (`-DWIN32 -DWINAPI_FAMILY=2`) before pushing.

## Public API behaviour

- `xc_create` validates the config (name 1..63 bytes, `mdns_publish` required unless `builtin_mdns`, `pin_mode` 0/1)
  and copies it. Zero display fields default to 1920x1080@60, max 60 fps.
- `xc_start` creates the identity, binds TCP `port` (default 7000; UDP ports are ephemeral), advertises, starts the
  supervisor thread. Returns -1 (logged) if the port is taken or advertising fails.
- `xc_disconnect` drops every connection (restarts the RTSP server on the same port), keeps advertising.
- `xc_stop` stops the supervisor, withdraws the services (`mdns_unpublish_all` or responder goodbye), destroys the
  RTSP server (joins every connection thread) and the responder. Safe to call twice; `xc_destroy` calls it.
- No callback fires after `xc_stop` returns.
- Additive API (no struct change, `XC_API_VERSION` stays 1): `xc_set_log_level` (DEBUG also enables UxPlay's
  protocol log), `xc_set_low_delay_sps`.
- API 2 (callbacks appended to `xc_callbacks`): audio-only AirPlay. `on_audio_stream(started, audio_only)` from the
  audio SETUP (`audio_only` = `usingScreen` false and not AAC-ELD) and the audio TEARDOWN; `on_track` from the DMAP
  `SET_PARAMETER` (`src/dmap.c`: `minm` title, `asar` artist, `asal` album); `on_cover_art` (JPEG/PNG bytes);
  `on_progress` from `progress: start/curr/end` (RTP 44.1 kHz ticks -> seconds). iOS sends progress at track start,
  seek and resume only, so the app advances the position by the audio it receives.

## Threading model

castcore owns no thread of the app. Callbacks come from UxPlay's threads and castcore's supervisor; the app must not
block in them (copy and return) and must not call `xc_stop`/`xc_destroy` from inside a callback.

| Thread | Created by | Fires |
|---|---|---|
| caller of `xc_start` | app | `mdns_publish` (twice: `_airplay._tcp` then `_raop._tcp`), `on_log` |
| caller of `xc_stop` | app | `mdns_unpublish_all`, `on_log` |
| httpd thread (one, `select()` over all RTSP connections) | UxPlay `httpd_start` | `on_client_connected`, `on_client_disconnected`, `on_pin`, `on_audio_format` (at RTSP SETUP), `on_video_flush` (client replaced / teardown), `on_log` |
| mirror thread (one per mirroring session, TCP) | UxPlay `raop_rtp_mirror` | `on_video_format`, `on_video`, `on_video_flush` (stream shutdown), `on_pin("")` |
| audio thread (one per session, UDP) | UxPlay `raop_rtp` | `on_audio`, `on_audio_flush`, `on_volume` |
| NTP thread | UxPlay `raop_ntp` | `on_log` only |
| supervisor (1 Hz) | castcore | `on_stats`, heartbeat warnings, connection restarts (`on_client_disconnected` follows from the httpd thread) |
| builtin mDNS responder | UxPlay `lib/mdnsd` | nothing (answers the network only) |

`on_video` and `on_audio` run on different threads concurrently. castcore's own state is behind one mutex that is
never held while a callback runs.

## Video

`on_video` delivers one access unit per call, **Annex-B** (`00 00 00 01` start codes). UxPlay's mirror code
(`raop_rtp_mirror.c`) receives AVCC/HVCC (length-prefixed) frames, decrypts them (AES-CTR) and rewrites every length
prefix into a start code in place; on a codec packet (type 1: SPS/PPS, or VPS/SPS/PPS for HEVC) it keeps the parameter
sets and **prepends them to the next frame**, which is the IDR that follows. So every keyframe castcore delivers
starts with its parameter sets (verified in `raop_rtp_mirror.c`, unchanged). castcore marks `is_keyframe` from the
NAL types (H.264 IDR=5, HEVC IRAP 16..21), and drops frames UxPlay flags as undecryptable (`video_invalid` in stats).

`on_video_format(codec, w, h)` fires before the first frame and whenever codec or size changes (rotation, sender
switching H.264/HEVC). Size comes from the SPS (cropped coded size) when parseable, else from the codec packet's
`widthSource/heightSource`. A local UxPlay patch reports the size of every codec packet, not only the first.

### Low-delay SPS rewrite (H.264, default on)

The iPhone's SPS has a VUI **without** `bitstream_restriction`, so a conforming decoder must assume up to
`max_dec_frame_buffering` frames of reordering and may hold frames back. screen-mirror-xbox measured 130-700 ms of
extra latency on Xbox Media Foundation from exactly this, and fixed it by rewriting the SPS. iOS never sends B
slices, so castcore (`src/h264_sps.c`, a C port of screen-mirror-xbox's `H264Sps`) re-encodes the SPS of delivered
frames with `bitstream_restriction_flag = 1`, `max_num_reorder_frames = 0`, `max_dec_frame_buffering = num_ref_frames`
(everything else bit-identical, emulation prevention redone). The rewrite is cached per distinct input SPS. If a B
slice is ever seen, castcore stops rewriting for that session and logs it. `xc_set_low_delay_sps(r, 0)` delivers
the sender's SPS untouched. HEVC is passed through unchanged (no measurement of the same problem yet).

`tests/test_h264_sps.c` proves it against FFmpeg: a real x264 stream with an iOS-like SPS (VUI without restriction,
and no VUI at all) is rewritten, re-parsed, and decoded by libavcodec with all 10 frames out and zero packets of
output delay.

## Audio

AirPlay mirroring audio is negotiated at RTSP SETUP; castcore calls `on_audio_format` from there with:

| AirPlay `ct` | codec | config passed | notes |
|---|---|---|---|
| 8 | `XC_AUDIO_AAC_ELD` | AudioSpecificConfig `f8 e8 50 00` (ELD, 44.1 kHz, stereo, 480 samples/frame) | screen mirroring |
| 2 | `XC_AUDIO_ALAC` | 36-byte ALAC magic cookie (`alac` atom: 352 spf, 16 bit, 2 ch, 44.1 kHz) | AirPlay audio (music apps) |
| 4 | `XC_AUDIO_AAC_LC` | AudioSpecificConfig `12 10` | |
| 1 | `XC_AUDIO_PCM` | none | castcore byte-swaps L16 big-endian to s16le |

`on_audio` delivers one decrypted access unit per call: raw AAC (no ADTS/LATM) or ALAC frame, as FFmpeg's `aac` /
`alac` decoders take them with the config above as extradata. `tests/test_aac_eld.c` opens FFmpeg's decoder with
castcore's ELD config exactly as hostrx does (44.1 kHz stereo) and, in CI, decodes FFmpeg's FATE AAC-ELD
conformance stream (`aac/er_eld1001np_44_ep0.mp4`) access unit by access unit with zero errors.

`on_volume(dB)`: AirPlay volume, -144 = mute, else -30..0. castcore does not apply it.

## Clock mapping (`pts_ns`)

`xc_now_ns()` is monotonic nanoseconds (Linux `CLOCK_MONOTONIC`; Windows `QueryPerformanceCounter`).

UxPlay times every frame with its NTP channel: it keeps a filtered offset between the sender's clock and the local
**realtime** clock (`raop_ntp`, `CLOCK_REALTIME` / `GetSystemTimePreciseAsFileTime`) and converts each frame's sender
timestamp into `ntp_time_local` = the local realtime nanoseconds at which the sender wants the frame presented.
castcore moves that onto the monotonic clock at delivery:

```
pts_ns = ntp_time_local - realtime_now + xc_now_ns()
```

i.e. it keeps UxPlay's "present in N ms" distance and re-expresses it on the monotonic clock, sampled at callback
time, so realtime jumps (NTP sync, user clock change) cannot shift presentation. Frames without a timestamp get
`pts_ns = xc_now_ns()`. For mirroring, iOS stamps frames very close to capture time, so `pts_ns - xc_now_ns()` at
arrival is about the network delay (negative = already late); a low-latency renderer presents video on arrival and
uses pts only to pace audio. `on_stats` reports `net_latency_ms` (arrival minus local presentation time of the last
frame) and `ntp_offset_ms` (sender clock minus local realtime).

## mDNS

Two modes, chosen by `cfg.builtin_mdns`:

**`builtin_mdns = 0`**: castcore asks UxPlay's dnssd module for the TXT records without starting any responder
(local patch `dnssd_build_records`) and calls `mdns_publish` once per service. hostrx implements it with
avahi-client.

**`builtin_mdns = 1`** (Xbox default, setting `mdnsMode`): UxPlay's own responder (`lib/mdnsd`, LGPL-2.1+, kgbook's
mdnsd) runs in a thread, joins 224.0.0.251:5353, answers PTR/SRV/TXT/A for both services and announces/goodbyes.
It is the advertising path both public Xbox ports of UxPlay use (screen-mirror-xbox, airplay-xbox). Local patches:
the "Durango Virtual Network Adapter" filter fix, host name `<host>-xc.local` (never the machine's own name), and
joining the thread before closing its sockets (0xC0000008 on suspend). **Coexistence with another responder on
5353 works**: on the Linux box avahi-daemon owns 5353, the responder binds with `SO_REUSEADDR` and avahi-browse
resolves both services from it (to `<host>-xc.local` and the LAN address), and the records disappear after
`xc_stop` (goodbye). This is what happens on Xbox too, where the OS has its own mDNS stack.

Records (from hostrx on Linux, `--size 1920x1080 --fps 60`, h265 on, pin off; `deviceid` / `pk` vary):

```
_airplay._tcp  "<name>"  port 7000
  deviceid=02:11:22:33:44:55  features=0x527FFEE6,0x400  pw=false  flags=0x4  model=AppleTV3,2
  pk=<32-byte ed25519 public key, hex>  pi=2e388006-13ba-4041-9a67-25dd4a43d536  srcvers=220.68  vv=2
_raop._tcp     "021122334455@<name>"  port 7000
  ch=2 cn=0,1,2,3 da=true et=0,3,5 vv=2 ft=0x527FFEE6,0x400 am=AppleTV3,2 md=0,1,2 rhd=5.6.0.0 pw=false
  sr=44100 ss=16 sv=false tp=UDP txtvers=1 sf=0x4 vs=220.68 vn=65537 pk=<same key>
```

Features are UxPlay's defaults (`uxplay.cpp start_dnssd`): bits 0 and 4 (video/HLS) off, bit 27 (legacy pairing)
off, bit 42 (SupportsScreenMultiCodec, H.265) = `allow_h265` -> word 2 `0x400` when on, `0x0` when off. With
`pin_mode = 1`, `pw=true`, `flags=0x...` gets the password bit and a random 4-digit PIN is shown through `on_pin` at
every connection (UxPlay `pin_pw` 3). The display size / refresh / max fps are not in TXT; iOS reads them from
`GET /info` (`displays[0].widthPixels/heightPixels/refreshRate/maxFPS`).

## Identity

`raop_init2` gets `<data_dir>/castcore-ed25519.pem`; UxPlay creates the ed25519 key on first start and reuses it
afterwards, so `pk` (and with it iOS's remembered pairing) is stable across restarts. Empty `data_dir` = a key derived
from the device id (UxPlay default). The PEM is read/written through memory BIOs (local patch) because vcpkg's UWP
OpenSSL has no `BIO_new_fp`. `device_id` comes from the app (persisted by it).

## Supervisor, resets, stats

The supervisor wakes every 100 ms and once per second: emits `on_stats`, counts seconds since the client's last
`/feedback` heartbeat (iOS sends one every 2 s) and, after 15 s without one (UxPlay's `MISSED_FEEDBACK_LIMIT`), drops
all connections. A protocol reset (`conn_reset`, e.g. the iOS 27 missing-TEARDOWN case) restarts the RTSP server the
same way. `on_stats` JSON:

```json
{"connected":true,"video_codec":"h264","width":1920,"height":1080,"video_fps":59.8,"video_kbps":8123,
 "video_frames":1234,"video_invalid":0,"format_changes":1,"audio_codec":8,"audio_pps":91.9,"audio_kbps":256,
 "audio_packets":2300,"volume_db":-12.5,"net_latency_ms":4.1,"ntp_offset_ms":-0.215,"missed_feedback_s":0}
```

## Local patches to UxPlay

Every change is marked `/* xbox-cast: ... */`; the table is in `native/third_party/uxplay/VENDORED.md`, the exact
diff is `git diff 6702e29 -- native/third_party/uxplay` (6702e29 = pristine import of upstream `116e390`). Summary:
Xbox mdnsd fixes (adapter filter, `-xc` host name, thread join before socket close), TXT-only record building for
`mdns_publish`, memory-BIO key file, no CryptoAPI on UWP, Winsock fixes, codec-packet size on every packet, client
address callback, a FairPlay bounds check, `snprintf` on MSVC.

## Bumping UxPlay

1. Clone upstream at the new tag; diff its `lib/` against `6702e29` (our pristine import) to see what changed.
2. Replace `native/third_party/uxplay/lib/` with the new `lib/` minus `dns_sd/` and the CMake files; commit that alone
   ("vendor UxPlay lib/ at <tag> (pristine)").
3. Re-apply our patches: `git diff 6702e29 <last-patched-commit> -- native/third_party/uxplay | git apply -3`, fix
   conflicts, keep every `/* xbox-cast: */` marker. Update `VENDORED.md` (tag, commit, patch table) and
   `XC_VERSION_STRING` in `src/castcore.c`.
4. Check `raop.h` callback signatures against `castcore.c` and new TXT keys/feature bits against `uxplay.cpp
   start_dnssd`; run `ctest`, then hostrx with an iPhone.

## Known limits / open issues

- **iOS 27**: UxPlay #535 (missing TEARDOWN 110) is handled on master (546820c, included). Watch UxPlay for more.
- No HLS / "AirPlay video" (YouTube URL casting): features bits 0/4 are off, the request is ignored.
- One client at a time; a new client replaces the current one (UxPlay "nohold").
- Audio format is fixed at 44.1 kHz stereo as negotiated by AirPlay; no 48 kHz mode.
- The builtin responder names the host `<gethostname()>-xc.local`; on Xbox that is the console's network name plus
  `-xc` (screen-mirror-xbox uses the same scheme with `-AirPlay`).
- The HEVC SPS is not rewritten for low delay (H.264 only).
- No kernel RX timestamps on Windows (UWP); NTP uses user-space receive times (UxPlay's default elsewhere).
