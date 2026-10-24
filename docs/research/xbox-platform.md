# Xbox platform constraints (UWP app, retail Store)

Constraints that bind xbox-cast's design. Sources linked inline. **UNVERIFIED** = not confirmed by docs, code or a
public report; needs a console test.

## 1. mDNS / Bonjour advertising (the #1 risk)

### Evidence

| Fact | Source |
|---|---|
| Two independent GPL-3.0 UWP ports of UxPlay advertise with **UxPlay's built-in responder** (`lib/mdnsd/`, Winsock UDP 5353, `SO_REUSEADDR`, `IP_ADD_MEMBERSHIP` 224.0.0.251) and iPhones discover them on real consoles: Xbox One (Dev Mode, OS 26100) and Xbox Series X (Dev Mode) | [vproHacks/screen-mirror-xbox `docs/DEVLOG.md`](https://github.com/vproHacks/screen-mirror-xbox/blob/main/docs/DEVLOG.md) ("Discovery (mDNS, built-in responder) - Works on Xbox"), [CansDE/airplay-xbox README](https://github.com/CansDE/airplay-xbox) ("UxPlay's built-in mdnsd responder (no Bonjour needed)"; "discovery in the iPhone's Control Center (mDNS) ... tested") |
| Both needed fixes: (a) UxPlay's Windows adapter filter rejects adapters whose description contains "Virtual", but on Xbox the real NICs are "Durango Virtual Network ... Adapter"; (b) use a host name like `<host>-AirPlay.local` so it never collides with the console's own mDNS host record; (c) `mdnsd_stop` closed sockets while the thread still used them → `0xC0000008` crash on suspend | screen-mirror-xbox [`docs/uxplay-lib-changes.patch`](https://github.com/vproHacks/screen-mirror-xbox/blob/main/docs/uxplay-lib-changes.patch) (`mdnsd.c` ~line 370, `dnssd_mdnsd.c` ~line 68); CansDE README "Known fixes" |
| (b) also implies the console **runs its own mDNS responder** on 5353, i.e. we share the port | same patch comment |
| Manifest of both: `internetClient`, `internetClientServer`, `privateNetworkClientServer`, nothing restricted | `src/app/Package.appxmanifest`, `package/AppxManifest.xml` |
| UWP documents port sharing for multicast: `DatagramSocketControl.MulticastOnly = true` lets a `DatagramSocket` "coexist with other Win32 or UWP multicast sockets bound to the same address/port" | [Sockets (UWP)](https://learn.microsoft.com/en-us/windows/uwp/networking/sockets) "Port sharing for DatagramSocket" |
| `DnssdServiceInstance.RegisterStreamSocketListenerAsync(StreamSocketListener[, NetworkAdapter])`: only takes a WinRT `StreamSocketListener`; the docs don't say whether the advertised port is the listener's or the `Port` property | [API page](https://learn.microsoft.com/en-us/uwp/api/windows.networking.servicediscovery.dnssd.dnssdserviceinstance.registerstreamsocketlistenerasync) |
| No public report of `DnssdServiceInstance` working (or failing) on Xbox; no GitHub code search hit for `DnssdServiceInstance` + `_airplay._tcp`/`_raop._tcp`/`_googlecast._tcp` | GitHub code search 2026-10-24 |
| Store apps that advertise AirPlay/Cast on Xbox exist (AirServer, AirReceiver, cjw1115, Hao Cai), so *some* mechanism works in retail. Which one is closed-source/unknown | see `feasibility.md` §8 |
| Dev Mode vs retail network stack difference: none documented. The only documented retail difference relevant to us is package-file encryption | [Known issues](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/known-issues) |

### Ranked recommendation

1. **Built-in responder (castcore `builtin_mdns=1`) as the default.** It is the only path proven on Xbox hardware (two
   consoles, two codebases, Oct 2026). Port UxPlay's `lib/mdnsd` with the three fixes above. Bind `0.0.0.0:5353` with
   `SO_REUSEADDR` before `bind`, join 224.0.0.251 on the LAN interface, set `IP_MULTICAST_IF`, answer only for our
   instance names and our own `<name>-xc.local` host record, send unsolicited announcements on start and goodbyes
   (TTL 0) on suspend/stop. Re-announce on resume and on network change. Retail-mode behaviour is **UNVERIFIED** (no
   reason to expect a difference; prove it with the first Store/private-audience build).
2. **`DnssdServiceInstance` as the fallback**, only if (1) fails on retail. Workable design: create a WinRT
   `StreamSocketListener` bound to port P purely as the registration handle, and run our real Winsock TCP server on
   P as well is **not possible** (the port is taken). So either (a) accept connections in the `StreamSocketListener`
   and bridge each `StreamSocket` into castcore (castcore's accept loop becomes a callback; needs a contract change
   and a copy per byte), or (b) test whether setting `DnssdServiceInstance.Port` to our Winsock port and registering
   with an unrelated listener advertises our port (**UNVERIFIED**, undocumented). Either way TXT records are fine
   (`TextAttributes`).
3. Hybrid "auto" (OS first, builtin on failure), as `docs/contract.md` `mdnsMode=auto` describes, is the wrong order:
   a successful-but-invisible OS registration (common on consoles/AppContainers) would never fall back. **Suggest the
   contract owner flips `auto` to builtin-first**, or default `mdnsMode` to `builtin`.

Always: keep both services on the same TCP port; refresh TXT (`pw`, features) by re-announcing; name collisions →
append " (2)".

## 2. Winsock / networking in a UWP app on Xbox

| Topic | Constraint | Source |
|---|---|---|
| Inbound LAN TCP/UDP | Needs `privateNetworkClientServer` ("inbound and outbound access to home and work networks through the firewall"); add `internetClientServer` too (both ports declare both) | [Capabilities](https://learn.microsoft.com/en-us/windows/uwp/packaging/app-capability-declarations), [Sockets](https://learn.microsoft.com/en-us/windows/uwp/networking/sockets) |
| Winsock allowed | "you can also use Windows Sockets 2 (Winsock) in a Windows app" | Sockets page |
| **Blocked ports** | UWP on Xbox "restricted from binding to ports in the range [57344, 65535] ... traffic can be silently dropped". Bind 0, or fixed ports in [1025, 49151] | [Known issues](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/known-issues) |
| Ephemeral ports | Bind 0 *may* return a port ≥ 57344? **UNVERIFIED**; safest: pick from a fixed range (e.g. 7000-7100 legacy AirPlay ports, or 35000+) and retry | - |
| Loopback | Sockets between two apps on the same machine (127.0.0.1 or own IP) are blocked. This includes the system media player fetching from our own HTTP server: screen-mirror-xbox had to feed HLS playlists from memory because "UWP apps cannot use the loopback HTTP channel UxPlay normally uses" | Sockets page; screen-mirror-xbox DEVLOG |
| Multicast | Receive/send on 224.0.0.251:5353 works (§1). SSDP 239.255.255.250:1900 for DLNA: same mechanism, **UNVERIFIED** on Xbox | - |
| Network info APIs | `NetworkInformation`, `NetworkUsage`, `ConnectivityInterval` are "not supported on Xbox" → enumerate interfaces with `GetAdaptersAddresses` (works, used by both ports) | [UWP features not supported on Xbox](https://learn.microsoft.com/en-us/uwp/extension-sdks/uwp-limitations-on-xbox) |
| Also unsupported | Wi-Fi Direct, Wi-Fi, Bluetooth, `Windows.Media.Casting`, `DialProtocol`, `Graphics.Capture` | same page |
| Kernel receive timestamps | disabled under UWP in the screen-mirror port | its patch |
| OpenSSL | UWP builds have no stdio → load/save keys through memory BIOs; use `RAND_bytes` | its patch |

## 3. Resources: app vs game vs "media app"

| Mode | Memory | GPU | CPU | How you get it | Source |
|---|---|---|---|---|---|
| App (default) | **1 GB** foreground, 128 MB background | share of 45% | share of 2-4 cores | default | [System resources](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/system-resource-allocation) |
| App + `hevcPlayback` (rescap) | **3.25 GB** ("2GB more") | same as app (per moonlight-xbox dev test) | same | manifest; Store approval required (Jellyfin, moonlight-xbox ship it) | [4K video playback on Xbox](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/hevc-xbox); [moonlight-xbox #112 comment](https://github.com/TheElixZammuto/moonlight-xbox/issues/112#issuecomment-5832281877) |
| App + `expandedResources` (rescap) | ≥ 4 GB exclusive partition, exclusive cores ("Game Mode resources") | **UNVERIFIED** | exclusive | manifest; Store approval unknown | [Capabilities](https://learn.microsoft.com/en-us/windows/uwp/packaging/app-capability-declarations) |
| Game | 5 GB | full | 4 exclusive + 2 shared | Dev Mode: Dev Home → View details → App type: Game (Dev only). Store: "UWP based games are no longer accepted in the Xbox Store" | System resources; [FAQ](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/frequently-asked-questions); CansDE README step 5 |

`hevcPlayback` side effects: 4K/HDR video modes; **app is closed when a game launches, can't coexist with a game, so
no background music** ("apps will need to choose between ... background music and ... 4K video"). moonlight-xbox
declares it; a dev found it makes the console treat it as a "Media App" that ignores the Dev Home Game/App toggle, and
that swapping it for `expandedResources` kept HEVC decode working but lost HDR, with no measurable decode-speed gain for
H.264/HEVC (#112). moonlight-xbox does **not** register as a game in the Store.

**Implication for us:** 1 GB is plenty (screen-mirror-xbox: 15-19 MB over 5 min of 1080p). Stay a plain app; no
restricted capability in v1. Consider `hevcPlayback` only for a future 4K/HDR or H.265-heavy mode, knowing it kills
background audio.

## 4. Video decode

| Topic | Finding | Source |
|---|---|---|
| FFmpeg + D3D11VA in a UWP app | Works: moonlight-xbox (`Streaming/FFmpegDecoder.cpp`, `av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)` sharing the renderer's `ID3D11Device` with lock callbacks), CansDE/airplay-xbox (D3D11VA + software fallback, D3D11 video processor output) | repos |
| Platform decoder alternative | screen-mirror-xbox uses `MediaStreamSource` → `MediaPlayer` (HW H.264). Needed an SPS rewrite (no reorder) to stop 130-700 ms buffering | its DEVLOG |
| D3D feature level for apps on Series X|S | D3D11 and D3D12, FL 11.0 (Xbox One apps: D3D11 FL 10.1 only) | System resources |
| H.264 HW limits (platform player) | 8-bit 4:2:0, Baseline/Main/High, level ≤ 5.2 | [gelatinarm codec table](https://github.com/gitman-101111/gelatinarm) (Jellyfin client, measured) |
| HEVC HW | Xbox One S and later (original One: software only). 8/10-bit 4:2:0, level ≤ 6.1 | hevc-xbox doc; gelatinarm table |
| Does HEVC decode need `hevcPlayback`? | Apparently not on Series consoles: HEVC decode kept working with `expandedResources` instead (single report). Plain-app HEVC via D3D11VA without any rescap: **UNVERIFIED** | moonlight-xbox #112 |
| Low latency config | moonlight-xbox: decoder on its own thread, `SetMaximumFrameLatency(1)`, frame-latency waitable object, precise `Present()` timing; 60 fps uses HW vsync (PR #215). For AirPlay add `AV_CODEC_FLAG_LOW_DELAY` and check 1-in-1-out | [PR #215](https://github.com/TheElixZammuto/moonlight-xbox/pull/215) |

## 5. Output resolution / 4K

- moonlight-xbox renders through a XAML `SwapChainPanel` with a **composition swap chain sized to the HDMI mode in raw
  pixels** (`HdmiDisplayInformation::GetCurrentDisplayMode()->ResolutionWidthInRawPixels`, min 1920x1080;
  `Common/DeviceResources.cpp` `uwp_get_width/height`), and users stream 4K HDR with it (#197: "4K and HDR work
  properly"). So "XAML is stuck at 1080p" is about XAML *layout*, not about what a swap chain can present. Whether this
  needs `hevcPlayback` (moonlight has it) is **UNVERIFIED**; the hevc-xbox doc ties "4K video modes" to it.
- **Worth it for mirroring? No, not in v1.** H.264 mirroring is ≤ 1080 tall; a portrait phone is ~500x1080. 4K needs
  H.265 (feature bit 42) plus a >1080 request and Apple says only recent devices send it (UxPlay README `-h265`).
  Scaling 1080p → 4K is done by the console/TV anyway. Revisit with Mac/iPad 4K mirroring.

## 6. Audio

| Option | Status on Xbox app | Notes |
|---|---|---|
| XAudio2 | Works (screen-mirror-xbox, CansDE both output AAC-ELD/ALAC via FFmpeg → XAudio2) | simplest; CansDE adds a 200 ms buffer for A/V sync |
| WASAPI (`ActivateAudioInterfaceAsync`) | Works: moonlight-xbox uses miniaudio's WASAPI backend, `ma_wasapi_usage_pro_audio`, 10-50 ms configurable buffer (default 30) after a 50-100 ms delay report | [PR #286](https://github.com/TheElixZammuto/moonlight-xbox/pull/286), `Streaming/AudioPlayer.cpp` |
| AudioGraph / MediaPlayer | Documented to keep playing in background with `backgroundMediaPlayback` | Capabilities doc |

Recommendation: WASAPI shared mode (or XAudio2) with a ~30-60 ms jitter buffer in mirroring; audio is the master clock
only for audio-only mode.

## 7. Latency (what to expect)

| Measurement | Value | Source |
|---|---|---|
| screen-mirror-xbox (Xbox One, platform decoder) | player ~50-80 ms behind newest received frame, no drift, 0 dropped of 9,608 frames | DEVLOG |
| CansDE/airplay-xbox (Series X, FFmpeg D3D11VA) | 200-300 ms total "a deliberate buffer for smooth audio and lip-synced video" | README |
| AirServer Xbox | marketing: "720p or 1080p @60 FPS", no latency figure | [airserver.com/Xbox](https://www.airserver.com/Xbox) |
| Glass-to-glass AirPlay mirroring (phone encode + Wi-Fi + decode + TV) | **UNVERIFIED** - no trustworthy published number found. Measure: film phone stopwatch + TV together | - |

Knobs we control: render on arrival (`-vsync no` equivalent), decoder low-delay, swap chain max latency 1, advertise
maxFPS 60, TV ALLM/game mode (an app can't trigger ALLM; moonlight #112), small audio buffer. Knobs we don't: iOS
encoder buffering (no receiver-side "low latency" request exists; `audioLatencies` is ignored per UxPlay README `-al`).

## 8. Background, suspend, discoverability

- A UWP app on Xbox is suspended a few seconds after the user leaves it (screen-mirror-xbox README: "receives only while
  it is open ... reappears on the phone as soon as you open it again"). Handle suspend: send mDNS goodbyes, close
  sessions, stop threads cleanly (CansDE's 0xC0000008 crash).
- **Background audio**: `backgroundMediaPlayback` keeps `MediaPlayer`/`AudioGraph` audible and "app lifetime will be
  extended automatically while playback is occurring"; background memory cap 128 MB. AirServer advertises "extended
  background audio support" on Xbox, which proves a Store receiver can keep AirPlay audio running behind a game. Whether
  XAudio2/WASAPI output and our sockets keep running in that state is **UNVERIFIED** (likely need AudioGraph or
  MediaPlayer with a custom source). Incompatible with `hevcPlayback`.
- **Discoverable while not running: not possible.** No resident services for Store apps; background tasks can't host
  a responder; mDNS must be answered live. The user must open the app (or keep it running in background audio mode).
