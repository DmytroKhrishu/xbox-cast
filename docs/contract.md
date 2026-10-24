# Shared contract (read before fanning out; change only via the orchestrator)

## Ownership (commit only inside your area; ask the orchestrator for anything else)
| Area | Owner | Paths |
|---|---|---|
| Orchestration, top-level docs | orchestrator | `README.md`, `AGENTS.md`, `docs/architecture.md`, `docs/contract.md`, `THIRD_PARTY.md` (others append rows via their report or a single-row commit), `.gitignore`, `.env.example`, `tools/scrub-check.sh` |
| Research | research | `docs/research/**` |
| Protocol core | core | `native/**`, `.github/workflows/core.yml`, `docs/core.md` |
| App platform + CI | platform | `app/XboxCast.vcxproj`, `app/Package.appxmanifest`, `app/App.*`, `app/pch.*`, `app/Assets/**`, `app/src/core/**`, `app/src/diag/**`, `app/src/settings/**`, `.github/workflows/build*.yml`, `tools/deploy.py`, `tools/devlog.py`, `docs/building.md` |
| Media pipeline | media | `app/src/media/**`, `docs/media.md` |
| UI | ui | `app/src/ui/**`, `app/*.xaml` pages other than `App.xaml`, `docs/controller-ux.md` |

`app/XboxCast.vcxproj` uses wildcards (`src\**\*.cpp`, `src\**\*.h`, `src\**\*.xaml` as Page, `src\**\*.idl` as Midl) so media/ui
add files without editing the project. If you need a project/package change (NuGet, capability, define), ask platform via
the orchestrator.

## 1. castcore C API (`native/include/castcore.h`, owner: core)
Pure C, no platform headers in the public header. Thread-safe entry points. All callbacks may fire on core threads; the
app must not block in them for more than ~1 ms (copy and queue).

```c
#define XC_API_VERSION 1

typedef enum { XC_LOG_DEBUG, XC_LOG_INFO, XC_LOG_WARN, XC_LOG_ERROR } xc_log_level;
typedef enum { XC_VIDEO_H264 = 1, XC_VIDEO_H265 = 2 } xc_video_codec;
typedef enum { XC_AUDIO_AAC_ELD = 1, XC_AUDIO_ALAC = 2, XC_AUDIO_AAC_LC = 3, XC_AUDIO_PCM = 4 } xc_audio_codec;

typedef struct { const char *key; const char *value; } xc_txt_entry;

typedef struct {
    const char    *name;            /* advertised name, UTF-8 */
    unsigned char  device_id[6];    /* stable random id (app persists it); used as AirPlay deviceid / RAOP MAC prefix */
    int            display_width;   /* advertised display (what iOS encodes for), e.g. 1920 */
    int            display_height;  /* e.g. 1080 */
    int            display_refresh; /* 60 */
    int            max_fps;         /* 60 */
    int            allow_h265;      /* advertise HEVC (4K-capable senders) */
    int            pin_mode;        /* 0 = none, 1 = on-screen PIN every connection */
    unsigned short port;            /* 0 = default 7000 */
    const char    *data_dir;        /* writable dir for the persistent ed25519 identity / pairing data */
    int            builtin_mdns;    /* 1 = castcore advertises itself with its own mDNS responder (UDP 5353, multicast,
                                       SO_REUSEADDR) and never calls mdns_publish. Fallback for platforms where the OS
                                       DNS-SD API can't advertise (app picks it via setting `mdnsMode`). */
} xc_config;

typedef struct {
    const uint8_t *data;            /* Annex-B, start codes, SPS/PPS(/VPS) prepended on keyframes and format changes */
    size_t         size;
    int64_t        pts_ns;          /* presentation time on the xc_now_ns() clock */
    int            is_keyframe;
    xc_video_codec codec;
    int            width, height;   /* coded size from the stream (0 if unknown) */
} xc_video_frame;

typedef struct {
    const uint8_t *data;            /* one decrypted compressed access unit (raw AAC/ALAC, no ADTS) or PCM s16le */
    size_t         size;
    int64_t        pts_ns;          /* xc_now_ns() clock */
} xc_audio_packet;

typedef struct {
    void *ctx;
    /* platform services (required) */
    int  (*mdns_publish)(void *ctx, const char *service_type /* "_airplay._tcp" | "_raop._tcp" */,
                         const char *instance_name, unsigned short port,
                         const xc_txt_entry *txt, size_t txt_count);   /* return 0 on success */
    void (*mdns_unpublish_all)(void *ctx);
    /* events (all optional) */
    void (*on_log)(void *ctx, xc_log_level level, const char *msg);
    void (*on_client_connected)(void *ctx, const char *client_name, const char *client_model, const char *client_addr);
    void (*on_client_disconnected)(void *ctx, const char *reason);
    void (*on_pin)(void *ctx, const char *pin);              /* show on screen; NULL/"" = hide */
    void (*on_video_format)(void *ctx, xc_video_codec codec, int width, int height);
    void (*on_video)(void *ctx, const xc_video_frame *frame);
    void (*on_video_flush)(void *ctx);                       /* stream reset (rotation/teardown): drop queued frames */
    void (*on_audio_format)(void *ctx, xc_audio_codec codec, int sample_rate, int channels,
                            const uint8_t *codec_config, size_t codec_config_size); /* AudioSpecificConfig / ALAC magic cookie */
    void (*on_audio)(void *ctx, const xc_audio_packet *packet);
    void (*on_audio_flush)(void *ctx);
    void (*on_volume)(void *ctx, float volume_db);            /* AirPlay volume: -144 = mute, -30..0 */
    void (*on_stats)(void *ctx, const char *json);            /* ~1 Hz: bitrate, fps in, packet loss, ntp offset */
} xc_callbacks;

typedef struct xc_receiver xc_receiver;

int          xc_api_version(void);
int64_t      xc_now_ns(void);                                 /* monotonic clock used for every pts */
xc_receiver *xc_create(const xc_config *cfg, const xc_callbacks *cb);  /* NULL on failure (logged via on_log) */
int          xc_start(xc_receiver *r);                        /* binds sockets, publishes mDNS; 0 = ok */
void         xc_disconnect(xc_receiver *r);                   /* drop the current client, keep advertising */
void         xc_stop(xc_receiver *r);                         /* unpublish + close everything */
void         xc_destroy(xc_receiver *r);
const char  *xc_version_string(void);                         /* "castcore x.y (uxplay <tag>)" */
```
Core may add functions/fields at the END of structs (bump `XC_API_VERSION`); never reorder.
Later (not v1): AirPlay URL/HLS video callbacks, cover art/metadata for audio-only AirPlay.

## 2. Native build output (owner: core; consumed by platform's CI)
`pwsh native/build-uwp.ps1 -Arch x64 -Config Release` produces:
```
native/out/uwp-x64/include/castcore.h
native/out/uwp-x64/lib/castcore.lib        (+ every static dependency lib it needs, e.g. libcrypto.lib, plist.lib)
native/out/uwp-x64/lib/link.txt            (one lib file name per line: what the app must link)
native/out/uwp-x64/bin/*.dll               (runtime DLLs to package, if any; prefer static)
```
The app project reads `link.txt`. OpenSSL comes from vcpkg (`x64-uwp`, manifest `native/vcpkg.json`), cached in CI.
Linux: `cmake -S native -B build && cmake --build build` builds `castcore` + `hostrx`.

## 3. App internals (C++/WinRT, namespace `XboxCast`)
- `app/src/core/ReceiverHost.h` (platform): owns `xc_receiver`, `DnssdPublisher`. Exposes to the rest of the app:
  ```cpp
  struct IVideoSink { virtual void OnFormat(xc_video_codec, int w, int h) = 0; virtual void OnFrame(const xc_video_frame&) = 0; virtual void OnFlush() = 0; };
  struct IAudioSink { virtual void OnFormat(xc_audio_codec, int rate, int ch, const uint8_t* cfg, size_t n) = 0;
                      virtual void OnPacket(const xc_audio_packet&) = 0; virtual void OnFlush() = 0; virtual void OnVolume(float db) = 0; };
  // ReceiverHost::SetSinks(IVideoSink*, IAudioSink*); events (winrt::event) for Connected/Disconnected/Pin/Stats/State.
  ```
  Sinks copy data and return immediately.
- `app/src/media/MediaPipeline.h` (media): implements both sinks.
  ```cpp
  class MediaPipeline : public IVideoSink, public IAudioSink {
   public:
    void Attach(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel);  // UI thread
    void SetViewMode(ViewMode m);   // Fit | Fill | Zoom | Auto
    void SetZoom(float z);          // 1.0 .. 3.0, applies in Zoom mode
    void Pan(float dx, float dy);   // normalized, clamped to content
    void SetAutoCrop(bool on);
    void SetAudioEnabled(bool on); void SetAudioOffsetMs(int ms);
    MediaStats Stats() const;       // decode/render fps, decode ms, queue depths, latency estimate, crop rect, codec, size
    void Stop();                    // release decoder + swap chain resources (back to idle)
  };
  enum class ViewMode { Fit, Fill, Zoom, Auto };   // Auto = auto-crop black bars, then Fill if the crop fits within ~8% loss, else Fit
  ```
  Transitions between modes animate (~300 ms ease) in the renderer, never pop.
- Wiring: `ReceiverHost::Instance()` is a process-wide singleton started by `App` on launch (and stopped on suspend,
  restarted on resume). `MainPage` (ui) owns one `MediaPipeline`, calls `Attach(panel)` and
  `ReceiverHost::Instance().SetSinks(&pipeline, &pipeline)`, and subscribes to ReceiverHost events for state changes.
- Bootstrap of `MainPage`: platform commits a minimal placeholder `app/src/ui/MainPage.{idl,xaml,h,cpp}` (runtimeclass
  `XboxCast.MainPage`, a full-screen `SwapChainPanel x:Name="VideoPanel"` + a TextBlock) so the app builds and launches.
  After that commit, ui owns those files. Nobody renames `XboxCast.MainPage` or `VideoPanel`.
- Package/link deps platform puts in the project for everyone: NuGet `Microsoft.Windows.CppWinRT`,
  `FFmpegInteropX.UWP.FFmpeg` 8.1.2 (headers under `include/`, import libs + DLLs under `runtimes/win-x64/native`, DLLs
  packaged next to the exe), link `d3d11.lib dxgi.lib d3dcompiler.lib xaudio2.lib ws2_32.lib` + castcore `link.txt`.
  Shaders: `app/src/media/*.hlsl` compiled by FxCompile (platform adds the item type wildcard).
- Compile guard: until castcore links in CI, ReceiverHost builds behind `XC_HAVE_CASTCORE` (defined by the project when
  `native/out/uwp-x64/lib/link.txt` exists); without it the app runs with no receiver (idle screen only).
- `app/src/settings/Settings.h` (platform): typed accessors over `ApplicationData::Current().LocalSettings()` + `Changed` event.

### Settings keys (LocalSettings, owner: platform; UI edits them, ReceiverHost/Media react)
| Key | Type | Default | Meaning |
|---|---|---|---|
| `deviceName` | string | `"Xbox"` | advertised name |
| `deviceId` | string (12 hex) | random on first run | stable id |
| `resolution` | string | `"1080p"` | `1080p` \| `1440p` \| `2160p` (advertised display size) |
| `maxFps` | int | 60 | 30 \| 60 |
| `allowH265` | bool | true | |
| `pinMode` | int | 0 | 0 none, 1 PIN on screen |
| `viewMode` | string | `"Auto"` | Fit \| Fill \| Zoom \| Auto |
| `zoom` | double | 1.0 | |
| `autoCrop` | bool | true | |
| `audioEnabled` | bool | true | |
| `audioOffsetMs` | int | 0 | -300..300, lip-sync trim |
| `showStats` | bool | false | stats overlay |
| `devServer` | bool | true in dev builds, false in Store builds | diagnostics HTTP server |
| `mdnsMode` | string | `"auto"` | `os` (DnssdServiceInstance) \| `builtin` (castcore responder) \| `auto` (os, fall back to builtin if registration fails) |

### Window / XAML rules (platform sets them in App, everyone relies on them)
- `ApplicationView::SetDesiredBoundsMode(UseCoreWindow)`: draw edge to edge (no Xbox overscan margins) so video is full-bleed.
  UI keeps its own 5% TV-safe margins.
- `ApplicationViewScaling::TrySetDisableLayoutScaling(true)`: layout in 1920×1080 effective pixels (not Xbox's 200% 960×540).
  Media still sizes the swap chain from `SwapChainPanel` ActualSize × CompositionScaleX/Y and handles `CompositionScaleChanged`.
- `Application::RequiresPointerMode(WhenRequested)`, `FocusVisualKind(Reveal)` off → UI draws its own calm focus visuals.
- Keep the screen awake while mirroring with `DisplayRequest`, created lazily on the first session (never before
  `Window::Activate`, that freezes the splash on Xbox).

## 4. Diagnostics (owner: platform) — there is no debugger on the console
- `Diag::Log(level, tag, fmt...)` → in-memory ring buffer (~4000 lines) + `OutputDebugString`. castcore `on_log` is routed
  here with tag `core`. Format: `2026-01-01T12:00:00.123 I media decoder opened h264 1920x886 d3d11va`.
- Dev HTTP server on the console, TCP **7099** (LAN only, enabled by `devServer`):
  - `GET /status` → JSON: version, state (`idle|connecting|mirroring`), client, settings, media stats, memory.
  - `GET /log?since=<seq>` → `{seq, lines:[...]}`.
  - `POST /cmd` JSON: `{"key":"A|B|X|Y|LB|RB|Up|Down|Left|Right|Menu|View"}`, `{"viewMode":"Fill"}`, `{"zoom":1.25}`,
    `{"setting":{"k":"v"}}`, `{"disconnect":true}` → `{ok:true}`. Lets agents drive the app without a controller.
- `tools/devlog.py` (platform): tails `/log`, prints `/status`, sends `/cmd`. Console address from `XBOX_HOST` in `.env`.
- Never bake any address into a build. The dev box reaches the console, not the other way round.

## 5. UI contract (owner: ui) — Apple-like, calm
- Idle screen: device name large, one line of how-to ("On your iPhone: Control Center → Screen Mirroring → <name>"),
  subtle animated backdrop, small status (network name/IP for troubleshooting, version) bottom corner. No trademarks as
  branding; plain descriptive text only.
- Mirroring: full-bleed video, zero chrome. Any button press shows a translucent bottom overlay (auto-hides after 4 s):
  view mode pills (Fit · Fill · Zoom · Auto), zoom +/-, audio offset, stats toggle, Disconnect.
- Controller (details in `docs/controller-ux.md`): A = show overlay / select, B = hide overlay → (on idle) nothing,
  LB/RB = cycle view mode, LT/RT = zoom out/in, right stick or D-pad (when zoomed, overlay hidden) = pan, View = stats,
  Menu = settings, hold B 1 s = disconnect.
- Motion: fades/slides 250–350 ms; focus scale ≤ 1.06; TV-safe margins (5% horizontal, 5% vertical).
- PIN overlay when `on_pin` fires.

## 6. Logging tags
`app`, `core`, `mdns`, `net`, `video`, `render`, `audio`, `ui`, `settings`, `diag`.
