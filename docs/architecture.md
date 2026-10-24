# Architecture

Working title **xbox-cast** (product name TBD, no trademarks in it). A casting receiver for Xbox consoles, shipped as a
UWP app: Dev Mode sideload first, Microsoft Store (Private audience) after.

## v1 goal
iPhone/iPad/Mac **screen mirroring with audio** to the Xbox, lowest latency we can get, full-screen with fit / fill /
zoom / auto-crop, calm Apple-like UI driven by the controller. No time limits, no paywalls.

## Big picture

```
 iPhone ──mDNS──▶ Xbox advertises _airplay._tcp + _raop._tcp (DnssdServiceInstance)
        ──TCP 7000 RTSP/HTTP──▶ castcore (vendored UxPlay lib: pairing, FairPlay setup, RTSP, RTP)
        ──TCP mirror stream──▶ decrypt ─▶ H.264/H.265 Annex-B ─▶ FFmpeg d3d11va decode ─▶ D3D11 renderer ─▶ SwapChainPanel
        ──UDP audio─────────▶ decrypt ─▶ AAC-ELD / ALAC / AAC ─▶ FFmpeg decode ─▶ XAudio2
```

### Layers
| Layer | Where | Language | Builds on |
|---|---|---|---|
| Protocol core (`castcore`) | `native/` | C (C11) + vendored UxPlay `lib/`, libplist, llhttp, playfair | Linux (CMake, for the host harness/tests) and UWP x64 (CMake `WindowsStore` + vcpkg `x64-uwp` OpenSSL) |
| Host harness `hostrx` | `native/hostrx/` | C | Linux only: avahi mDNS, dumps decrypted streams to files. Lets us prove the protocol with a real iPhone before the console is in Dev Mode |
| App | `app/` | C++/WinRT, XAML (Windows.UI.Xaml) | UWP x64, MSVC, GitHub Actions windows runner |
| Media pipeline | `app/src/media/` | C++/WinRT + D3D11 + FFmpeg (FFmpegInteropX.UWP.FFmpeg NuGet: UWP-built FFmpeg 8.1 headers, import libs, DLLs, d3d11va enabled) + XAudio2 | UWP |
| UI | `app/src/ui/` + XAML | C++/WinRT XAML | UWP |
| Dev tools | `tools/` | Python 3 stdlib | Linux dev box: deploy via Device Portal, read app logs, drive the app |

Why these choices:
- **UxPlay's lib** is the maintained, GPL-3.0 AirPlay mirroring + audio receiver (successor of RPiPlay), already
  portable C with a Windows path. We vendor it (pinned tag) and swap its mDNS/dns_sd layer for a platform callback.
- **C++/WinRT single app** (no C#): one toolchain, no .NET Native/interop layer between the protocol core and D3D11.
- **FFmpeg d3d11va + our own D3D11 renderer** instead of `MediaPlayer`: MediaPlayer buffers (hundreds of ms). Direct
  decode → present is the moonlight-xbox approach, proven low-latency on Xbox. It also lets us crop/zoom/pan on the GPU.
  Fallback if d3d11va misbehaves: Media Foundation H.264 MFT with `CODECAPI_AVLowLatencyMode`.
- **XAudio2** for audio: low-latency push of PCM buffers, available to UWP on Xbox.
- **No WebView2**: the UI is a few screens; XAML is lighter on memory and latency than a browser.
- Later protocols (DLNA renderer, AirPlay URL/HLS video) play media files, not low-latency streams: they will use
  `MediaPlayerElement` + FFmpegInteropX like JellyShell (4K HDR, every container/codec). See `docs/research/feasibility.md`.

## Runtime structure of the app
```
App (C++/WinRT)
 ├─ ReceiverHost          owns castcore, maps Settings → xc_config, routes callbacks
 │    ├─ DnssdPublisher   implements xc_platform.mdns_* with Windows.Networking.ServiceDiscovery.Dnssd
 │    └─ Session          one active mirroring session (castcore allows one)
 ├─ Media
 │    ├─ VideoDecoder     FFmpeg h264/hevc + d3d11va, low-delay flags, frame queue depth 1
 │    ├─ VideoRenderer    D3D11 swap chain on a SwapChainPanel, NV12→RGB shader, view transform (fit/fill/zoom/pan/auto-crop)
 │    ├─ CropDetector     samples luma every 500 ms, finds black borders, debounced
 │    ├─ AudioDecoder     FFmpeg aac(eld)/alac
 │    └─ AudioOutput      XAudio2 source voice, small jitter buffer, volume from sender
 ├─ UI
 │    ├─ MainPage         Idle screen ⇄ Mirroring view (SwapChainPanel) + overlays
 │    ├─ SettingsPage
 │    └─ Input            gamepad → actions (see docs/controller-ux.md)
 ├─ Settings              ApplicationData LocalSettings, typed accessors, change events
 └─ Diag                  ring-buffer log + LAN dev HTTP server (status/log/cmd) for agents (no debugger on Xbox)
```

## Build & ship
- CI: `.github/workflows/build.yml` on `windows-2025-vs2026` (fallback `windows-2025` if the C++ UWP workload is missing):
  `native/build-uwp.ps1` (CMake + vcpkg x64-uwp) → castcore libs → msbuild app → signed sideload `.msix` artifact.
- Signing: stable self-signed cert in repo secrets (`SIGNING_PFX_B64`, `SIGNING_PFX_PASSWORD`), so updates install in place.
- Versions: `0.1.<run_number + VERSION_OFFSET>.0`.
- Store: `build-store.yml` later (unsigned `.msixupload` + Store-identity test msix), identity from repo variables.
- Deploy: `tools/deploy.py` (Device Portal REST, Dev Mode only).

## Milestones (gates)
0. CI builds a signed C++/WinRT hello app + castcore links for x64-uwp. Host harness receives an iPhone mirror on Linux
   and dumps playable H.264 + AAC-ELD files.
1. Dev Mode: app advertises, iPhone lists it, mirroring shows video with audio on the TV.
2. Latency + quality pass (60 fps, H.265 if available, measured glass-to-glass), fit/fill/zoom/auto-crop, polished UI.
3. Store build, Private audience submission.
4. Long tail per `docs/research/feasibility.md` (AirPlay audio-only + URL video, DLNA renderer, ...).
