# Feasibility: what xbox-cast can receive on a retail Xbox (UWP Store app)

Decides what we build after v1 (AirPlay mirroring + audio). Evidence date: 2026-10-24. Platform details and sources for
networking/memory/decode are in [`xbox-platform.md`](xbox-platform.md); AirPlay wire details in
[`airplay-mirroring.md`](airplay-mirroring.md). **UNVERIFIED** = not confirmed by docs, code or a public report.
Effort = agent-days on top of v1, including console test time.

## Summary

| # | Protocol | Verdict | Effort (after v1) | Store/legal risk | Do it? |
|---|---|---|---|---|---|
| 1 | AirPlay screen mirroring (H.264, 1080p60, audio, PIN) | **Possible** (proven on Xbox by 2 OSS ports + 4 Store apps) | v1 | Medium (FairPlay via playfair, "AirPlay" trademark) | v1 |
| 1b | AirPlay mirroring H.265 / 4K | **Partial** (decode yes; 4K output needs `hevcPlayback`; few senders) | 2-3 | Low | Only if iOS 27 forces HEVC |
| 2a | AirPlay audio (RAOP/AirPlay 1: ALAC, metadata, cover art) | **Possible** (UxPlay; shown on Xbox) | 2-3 | Same as 1 | **Yes, next** |
| 2b | AirPlay 2 buffered audio, multi-room, HomeKit | **Not possible** (PTP ports 319/320 not bindable; NQPTP is GPL-2.0-only) | - | - | No |
| 3a | AirPlay video casting (URL/HLS: YouTube app, Safari) | **Partial** (works for YouTube/HLS + direct URLs; app-by-app) | 4-6 | Medium | Yes, after 2a |
| 3b | FairPlay-DRM casting (Apple TV+, Disney+ ...) | **Not possible** | - | - | No |
| 4 | Miracast receiver (Windows/Android "Cast/Connect") | **Not possible** (no Wi-Fi Direct for apps; MS-MICE discovery needs Wi-Fi beacons) | 0.5 probe | Low | Probe only |
| 5 | Google Cast / Chromecast receiver | **Not possible** legally (Google-signed device cert; only replayed stolen signatures work) | - | **High** | No |
| 6 | DLNA/UPnP Media Renderer | **Possible** | 4-6 (2-3 if `PlayToReceiver` works) | Low | **Yes** (covers Windows "Cast to Device", Android DLNA apps) |
| 7a | YouTube "Link with TV code" / DIAL | **Partial** technically, not shippable (needs YouTube stream extraction, ToS) | 8-12 + upkeep | High | No (official YouTube Xbox app exists) |
| 7b | Spotify Connect (librespot) | **Partial** technically, not worth it | 5-8 | High | No (official Spotify app) |
| 7c | Android screen mirroring | **Not possible** without Cast; only via our own sender app | 10+ | Low | Later, maybe |

Recommended order after v1: **2a AirPlay audio → 6 DLNA DMR → 3a AirPlay video casting → (1b HEVC if needed)**.

---

## 1. AirPlay screen mirroring (iOS/iPadOS/macOS)

| Aspect | Finding |
|---|---|
| Verdict | **Possible.** Two GPL-3.0 UWP ports of UxPlay `lib/` mirror real iPhones on real consoles: [vproHacks/screen-mirror-xbox](https://github.com/vproHacks/screen-mirror-xbox) (Xbox One; video, AAC-ELD audio, rotation, 9,608/9,608 frames over 5 min, ~50-80 ms render lag) and [CansDE/airplay-xbox](https://github.com/CansDE/airplay-xbox) (Series X; FFmpeg D3D11VA). Both Dev Mode; screen-mirror-xbox says it is "published with a private audience" in the Store (its claim, not verified) |
| Basis | [UxPlay](https://github.com/FDH2/UxPlay) @ `116e390`, GPL-3.0. Latest release v1.73.7 (2026-09-04) |
| `lib/` separable? | **Yes.** `lib/` has no GStreamer; renderers live in `renderers/` and talk to `lib` through `raop_callbacks_t` (`video_process`, `audio_process`, `video_set_codec_and_size`, `video_pause/resume`, ...). Both Xbox ports compiled `lib/` without `renderers/` |
| Deps + licenses | OpenSSL 3 (Apache-2.0), libplist (LGPL-2.1+), llhttp (MIT, vendored `lib/llhttp/LICENSE-MIT`), playfair (GPL-3.0, `lib/playfair/LICENSE.md`), built-in mDNS `lib/mdnsd` (UxPlay, GPL-3.0). All GPL-3.0 compatible. FFmpeg (LGPL build) for decode. **Do not use fdk-aac** (not GPL-compatible; screen-mirror-xbox replaced it with FFmpeg's AAC-ELD decoder) |
| Codecs / fps | H.264 default; default request 1920x1080, maxFPS 30 → we advertise 60 (`raop_set_plist("maxFPS", 60)`) |
| H.265 / 4K | Feature bit 42 + request height > 1080. "Only recent Apple devices (M1/M2 Macs or iPads, and some iPhones) can send h265 ... wired ethernet preferred ... might be required" (UxPlay README `-h265`). HEVC HW decode exists on Series S; 4K *output* modes tied to `hevcPlayback` (see platform doc). Low value for a phone mirror |
| Audio in mirroring | AAC-ELD 44.1 kHz stereo over RTP/UDP, NTP-timed. Works on Xbox (both ports) |
| PIN / password | Supported by UxPlay (`/pair-pin-start`, SRP, `-pin`, `-pw`); MDM-managed iPhones require PIN pairing (README) |
| iOS versions | iOS 26.x fine. **iOS 27** adds `combinedGetInfoWithControlSetup`; UxPlay #535 (open) has one client that never opens the data connection and one that only worked with `-h265`; maintainer: iPadOS 27 beta 5 "seems to work fine". Typeless TEARDOWN fixed (#540). Keep HEVC as a contingency |
| Known UxPlay limits | No DRM content (video black, audio only); macOS Apple Music uses FairPlay type 2 → unsupported (#570); one client at a time by default; iOS 27 open issues; HLS path fragile (#550, #596) |
| Store risk | **Medium.** Precedent: 4 AirPlay-receiver apps live on Xbox (§8). Risks: "AirPlay"/Apple trademarks in name/icon (screen-mirror-xbox removed an AirPlay-like icon), playfair's "legal status is unclear" (UxPlay README), certification testers need an iPhone (write test notes) |

## 2. AirPlay audio (music apps)

| Aspect | AirPlay 1 / RAOP (realtime ALAC) | AirPlay 2 (buffered audio, multi-room, HomeKit) |
|---|---|---|
| What iPhone does | Falls back to it for receivers that don't advertise AP2 buffered audio; Music/Spotify/podcasts work (screen-mirror-xbox: Music with cover art, "Works on Xbox") | Needs features 40 (buffered), 41 (PTP), 46 (HomeKit) |
| UxPlay | ALAC 44.1k, metadata text/artwork/progress, volume. "AirPlay2 multi-room audio streaming is not supported: use shairport-sync" (README) | No |
| shairport-sync | n/a | Yes, with NQPTP. **License:** not MIT as a whole: `COPYING` says "refer to the individual source files"; headers of `shairport.c`, `player.c`, `rtsp.c`, `ptp-utilities.c` are MIT; bundled parts BSD/MIT (`LICENSES`). GPL-3.0 compatible |
| NQPTP | n/a | **GPL-2.0-only** ("version 2." in `nqptp.c`, `LICENSE` = GPLv2) → **cannot be vendored** (AGENTS.md). Needs "exclusive access to ports 319 and 320" (README) |
| UWP feasibility | **Possible** | **Not possible**: Xbox UWP ports must be in [1025, 49151] for fixed binds (Known issues doc) → PTP 319/320 out (bind attempt **UNVERIFIED**), AppContainer can't take exclusive ports anyway; plus a from-scratch PTP follower and a Linux-centric player port |
| Effort | 2-3 (port UxPlay ALAC path + FFmpeg ALAC decoder + now-playing UI) | n/a |
| Notes | Use FFmpeg's ALAC decoder, not Apple's: Apple's reference decoder crashed the Xbox app on a damaged packet (screen-mirror-xbox DEVLOG). Background playback: see platform doc §8 | |

## 3. AirPlay video casting and DRM

| Aspect | Finding |
|---|---|
| URL/HLS casting | UxPlay `-hls` (feature bits 0 + 4): `/play`, `/scrub`, `/rate`, `/playback-info`, reverse HTTP (PTTH) + FCUP playlist requests. "Only streaming from the YouTube iOS app ... is currently supported" in UxPlay (README); screen-mirror-xbox added Safari-style direct URLs on Xbox |
| Xbox specifics | Play via `MediaPlayer` + `AdaptiveMediaSource`; feed playlists from memory (no loopback HTTP); YouTube's packed-audio renditions must be rewrapped as MPEG-TS (`HlsAudio.cpp`), fix UxPlay's PTTH reverse-response bug, ad→video transition ~10 s (open TODO) - all in screen-mirror-xbox DEVLOG |
| Verdict | **Partial**: works for YouTube + plain URLs; other apps (TikTok, Twitch) vary (CansDE README: TikTok sends no video request) |
| Effort | 4-6 |
| FairPlay DRM (Apple TV+, Disney+, Prime ...) | **Not possible.** FairPlay Streaming keys are only released to Apple-certified receivers; there is no open implementation and none can exist without Apple's key material. In mirroring, protected content is blanked by iOS: "Apple video-DRM ... cannot be decrypted by UxPlay ... only the unprotected audio will be streamed" (UxPlay README); "DRM-protected apps ... only cast to Apple-certified receivers" (screen-mirror-xbox README). Netflix dropped AirPlay entirely in 2019 (**UNVERIFIED**, no source fetched) |
| Store risk | Medium (same as 1); YouTube content is played from YouTube's own URLs the phone sends, no extraction |

## 4. Miracast receiver

| Aspect | Finding |
|---|---|
| `Windows.Media.Miracast.MiracastReceiver` | Windows 10 1903+; "Miracast requires the device to support Wi-Fi Direct" ([API docs](https://learn.microsoft.com/en-us/uwp/api/windows.media.miracast.miracastreceiver)) |
| Xbox | "WiFiDirect APIs are not supported on Xbox", Wi-Fi APIs too ([UWP features not supported on Xbox](https://learn.microsoft.com/en-us/uwp/extension-sdks/uwp-limitations-on-xbox)). Whether `MiracastReceiver.GetStatus()` reports Wi-Fi Direct available on a Series S is **UNVERIFIED** → 0.5-day probe (log `MiracastReceiverStatus.WiFiStatus` / `ListeningStatus`) |
| Microsoft "Wireless Display" app on Xbox | Microsoft's own first-party receiver for Windows/Android Miracast; first-party apps get system access we don't. Current availability and how it works: **UNVERIFIED** (web search rate-limited during this research) |
| MS-MICE (Miracast over Infrastructure) | Sink registers `<name>._display._tcp` via mDNS and listens on TCP **7250** (doable), **but** sources discover sinks through a Wi-Fi WSC vendor-extension IE in **beacons/probe responses** ([MS-MICE](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-mice/9598ca72-d937-466c-95f6-70401bb10bdb) §1, §3.2.5) - an app can't put IEs in the console's Wi-Fi frames. Then RTSP + DTLS-protected Wi-Fi Display stream (spec is public; no GPL-compatible sink implementation found) |
| Verdict | **Not possible** for a third-party app unless the probe shows `MiracastReceiver` live on Xbox |
| Store precedent | AirServer claims Miracast on Xbox; AirReceiver: "Wireless Display depend on the WiFi device, It's off by default ... check whether it works" - consistent with "doesn't work on Xbox" (inference) |

## 5. Google Cast / Chromecast receiver

| Aspect | Finding |
|---|---|
| Device auth | Sender sends `AuthChallenge`; receiver must answer with a signature by a **Google-issued device certificate** chain. Chrome refuses to stream otherwise ([Shanocast write-up](https://xakcop.com/post/shanocast/)) |
| Shanocast approach | Openscreen (BSD-3, Chromium) cast_streaming receiver + **private key and precomputed signatures extracted from the AirReceiver Android app** (which replays 48-hour peer-cert signatures, probably from a rooted Chromecast). Repo has **no license**. Shipping this = redistributing someone else's extracted key material → copyright/DMCA + Store takedown risk |
| What works even with auth | Chrome tab/desktop mirroring (cast_streaming). Android "Cast screen" probably the same path (**UNVERIFIED**). App casting (YouTube, Netflix, Spotify) needs the receiver to run Google's receiver apps in a Cast web runtime - not available |
| Store precedent | AirReceiver (softmedia) claims Google Cast on Xbox; AirServer claims Google Cast. Proves Microsoft cert doesn't check this; does **not** make it legal |
| Verdict | **Not possible** (legally). Technically Partial (mirroring only) |

## 6. DLNA / UPnP Media Renderer (DMR)

| Aspect | Finding |
|---|---|
| Protocol | SSDP (UDP 1900 multicast 239.255.255.250, `NOTIFY`/`M-SEARCH`) + HTTP device description + SOAP services AVTransport, RenderingControl, ConnectionManager. Renderer fetches the media URL itself |
| UWP on Xbox | Multicast + inbound TCP work for mDNS (same Winsock path), so SSDP should too (**UNVERIFIED** on 1900; fallback: `DatagramSocket` with `MulticastOnly`). Playback via `MediaPlayer` (H.264/HEVC/AAC/MP3/FLAC per platform). No loopback issue: sender's server is remote |
| `PlayToReceiver` (WinRT) | "create a custom software Play To receiver that is a digital media renderer" ([docs](https://learn.microsoft.com/en-us/uwp/api/windows.media.playto.playtoreceiver)); not on the Xbox unsupported list (only `Casting` and `DialProtocol` are). Works on Xbox: **UNVERIFIED** (0.5-day probe) - if yes, DMR is ~2 days |
| Senders | Windows "Cast to Device" / Play To (DLNA), BubbleUPnP/Web Video Caster on Android, many NAS/media apps |
| OSS | [pupnp](https://github.com/pupnp/pupnp) (BSD-3, C) - compatible. [gmrender-resurrect](https://github.com/hzeller/gmrender-resurrect) GPL-2.0 per GitHub ("or later" **UNVERIFIED** → don't vendor until checked). A minimal own SSDP+SOAP stack in castcore is ~1.5k LOC |
| Store precedent | "Play in" (DLNA/Windows/Android/iOS content) on Xbox Store |
| Verdict / effort | **Possible**, 4-6 days own stack (2-3 with `PlayToReceiver`). Low risk |

## 7. Other targets

| Target | Finding | Verdict |
|---|---|---|
| YouTube Cast / DIAL / "Link with TV code" | Receiver side reimplemented by [yt-cast-receiver](https://github.com/patrickkfkan/yt-cast-receiver) (Node; `package.json` says MIT; DIAL server + lounge API + manual pairing). DIAL needs SSDP + HTTP (`DialProtocol` API unsupported on Xbox → own stack). The killer: the receiver must *play* YouTube itself, which needs stream URL extraction (yt-dlp-style, breaks often, against YouTube ToS). The official YouTube app on Xbox already accepts casts (**UNVERIFIED** that it supports Link with TV code today) | Not worth it |
| Spotify Connect | [librespot](https://github.com/librespot-org/librespot) MIT, Rust; "only works with Spotify Premium" (README). Rust → UWP target is tier 3 (**UNVERIFIED** effort). Spotify's developer terms likely forbid unofficial Connect devices (terms page didn't load: **UNVERIFIED**). Official Spotify Xbox app exists | No |
| Plex / Jellyfin | "Cast" there = remote-controlling a client app; official Xbox clients exist (e.g. [jellyfin-xbox](https://github.com/jellyfin/jellyfin-xbox)). Not a receiver protocol | Out of scope |
| Android screen mirroring | Stock Android only mirrors via Google Cast (blocked, §5); Miracast removed from stock Android (vendor-specific, e.g. Samsung Smart View: **UNVERIFIED**). EasyCast claims Android mirroring on Xbox, presumably via its own sender app. Our route would be an Android companion app (MediaProjection + our own RTP/H.264) | Later, 10+ days |
| Windows PCs | "Cast to Device" = DLNA (§6). "Connect/Project" = Miracast (§4, not possible). AirPlay senders for Windows exist and UxPlay handles the old protocol (`0x02` heartbeats, AirMyPC) | Via §6 |

## 8. Store precedents (Xbox device family, Store catalog API 2026-10-24)

Product IDs intentionally omitted (repo rule). Ratings = stars (count).

| App (publisher) | Claims | Rating | Last update | Proves |
|---|---|---|---|---|
| AirServer Xbox Edition | AirPlay + Google Cast + Miracast; "720p or 1080p @60 FPS"; "extended background audio support" ([airserver.com/Xbox](https://www.airserver.com/Xbox)) | - | - | AirPlay mirroring and background AirPlay audio pass Store cert on Xbox; Miracast claim may be Xbox One-era (**UNVERIFIED** today) |
| AirPlay - Screen Mirroring Receiver (cjw1115) | iPhone/iPad mirroring + audio to PC/Xbox, "low latency" | 1.8 (19) | 2026-09-28 | Indie AirPlay receivers are being certified in 2026 |
| AirPlay Screen Mirrorring Receiver (Hao Cai) | iPhone/Mac mirroring | 1.7 (566) | 2026-06-29 | same; low ratings = room for a good one |
| EasyCast - Screen Mirroring Receiver (Hao Cai) | iPhone + Android mirroring, "low latency" | 1.8 (529) | 2026-10-13 | Android path likely via own sender app (**UNVERIFIED**) |
| AirReceiver (softmedia) | AirPlay, Google Cast, Wireless Display (Wi-Fi-dependent, off by default) | 2.7 (204) | 2026-03-31 | Cast receivers get through cert (via replayed signatures per Shanocast analysis) |
| Play in (Shivin Shrivastava) | DLNA / Windows / Android / iOS content playback | 2.0 (5) | 2026-05-22 | DLNA renderer allowed |
| Screen Mirror Server (vproHacks, GPL-3.0, UxPlay) | AirPlay mirroring, speaker, video | - | - | Its README: published "with a private audience", category Utilities & tools, non-game UWP. An open-source UxPlay/playfair app can be submitted |

Conclusion: AirPlay mirroring/audio/video receivers are both technically possible and accepted by Microsoft on Xbox;
every existing one is poorly rated, so quality (latency, fill modes, UI) is the differentiator.

## Decisions for the owner

1. **After v1, build AirPlay audio (2a) then DLNA (6)?** Recommended; both low-risk and cheap.
2. **Background audio vs 4K:** `backgroundMediaPlayback` (AirPlay music while gaming, as AirServer does) and
   `hevcPlayback` (4K/HDR output, 3.25 GB) are mutually exclusive on Xbox. Recommend background audio; no `hevcPlayback`.
3. **Name/branding:** avoid "AirPlay" as the first word of the Store name and Apple's glyph; say "AirPlay receiver" in
   the description. Owner's call on how close to the line.
4. **Google Cast:** confirm "no" (legal exposure is the owner's, and the repo is public).
5. **mDNS default:** builtin responder first (see platform doc §1); requires a contract tweak to `mdnsMode`.
