# Third-party code

Every vendored or linked component, its upstream, pinned version and license. All must be GPL-3.0-compatible.

| Component | Upstream | Version | License | Used for |
|---|---|---|---|---|
| FFmpeg (via FFmpegInteropX.UWP.FFmpeg NuGet) | https://github.com/ffmpeginteropx/FFmpegInteropX | 8.1.2 | LGPL-2.1-or-later | H.264/HEVC/AAC/ALAC decode (linked DLLs) |
| moonlight-xbox (code adapted, attributed in app/src/media headers) | https://github.com/TheElixZammuto/moonlight-xbox | master (2026-10) | GPL-3.0 | D3D11VA decode on the app device, swap chain setup |
| screen-mirror-xbox (approach, Xbox mDNS fixes) | https://github.com/vproHacks/screen-mirror-xbox | master (2026-10) | GPL-3.0 | SPS reorder-delay fix, Xbox mdnsd fixes |
| airplay-xbox (reference) | https://github.com/CansDE/airplay-xbox | master (2026-10) | GPL-3.0 | reference for HW/SW decode fallback, XAudio2 |
