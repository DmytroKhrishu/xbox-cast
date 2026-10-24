# UI

Owner: ui. Code: `app/src/ui/`. Visual spec: `docs/design/` (HTML mockups, rendered to `docs/design/png/` with
`docs/design/render.sh`). Input mapping: `docs/controller-ux.md`.

## Files
| File | What |
|---|---|
| `MainPage.xaml/.h/.cpp/.idl` | the only page: idle, connecting, mirroring overlay, settings panel, PIN, stats, toasts, hold-B ring |
| `Bridge.h/.cpp` | the only UI code that touches platform (Settings, ReceiverHost, Diag) and media (MediaPipeline). Everything else calls `xcui::Bridge` |
| `Input.h` | VirtualKey and `/cmd` key names → logical `xcui::Key` |
| `Anim.h/.cpp` | fade/slide helpers (250–350 ms, cubic ease-out), spinner |

`XC_UI_WIRED` in `Bridge.cpp` is 0 until the bridge is adapted to the real platform/media headers. With 0 the app runs
the UI standalone: settings read/write LocalSettings directly with the contract keys, no receiver, no video.

## Screens and states
One page, stacked layers, top-most owns input: **PIN > Settings > Overlay > Mirroring > Idle** (see controller-ux).

| Screen | Mockup | Shown when |
|---|---|---|
| Idle | `png/idle.png` | no session. Name (136 px), how-to line, drifting backdrop, Network IP + version bottom-left, "Menu Settings" bottom-right |
| Connecting | `png/connecting.png` | ReceiverHost state `connecting`. Spinner + "Connecting to <client>"; the hint line fades in after 3 s |
| Mirroring | `png/overlay.png` (overlay shown) | state `mirroring`. Full-bleed video, zero chrome |
| Overlay | `png/overlay.png` | any button while mirroring; bottom bar, auto-hides after 4 s idle |
| Stats | `png/stats.png` | `showStats` and mirroring; top-right panel, refreshed 1 Hz, never takes focus |
| Settings | `png/settings.png`, `png/settings-picture.png` | Menu; in-page panel over the dimmed idle screen or the live video |
| PIN | `png/pin.png` | `on_pin` with a code; hides on an empty code or when the session ends |
| Toast | `png/toast.png` | "Mirroring ended / iPhone disconnected" (4 s); view mode / sound / view reset (1.6 s) |
| Hold ring | (in `controller-ux.md`) | B held with a session: appears at 200 ms, fills until 1000 ms |

### Settings rows
| Category | Row | Key | Values | Restart |
|---|---|---|---|---|
| Receiver | Name | `deviceName` | on-screen keyboard, 1–40 chars | yes |
| | Resolution | `resolution` | 1080p · 1440p · 4K (`2160p`) | yes |
| | Frame rate | `maxFps` | 30 fps · 60 fps | yes |
| | H.265 video | `allowH265` | On/Off | yes |
| | Require code to connect | `pinMode` | On/Off (1/0) | yes |
| Picture and Sound | Default view | `viewMode` | Fit · Fill · Zoom · Auto | no |
| | Remove black bars | `autoCrop` | On/Off | no |
| | Sound | `audioEnabled` | On/Off | no |
| | Audio delay | `audioOffsetMs` | −300..+300, 20 ms steps | no |
| | Show stats | `showStats` | On/Off | no |
| Advanced | Discovery | `mdnsMode` | Built-in (`builtin`, default) · System (`os`, experimental) | yes |
| About | version, castcore version, license line | — | read only | — |

"Restart" rows show *Applies on next connection* under the help text. The change is saved at once; the bridge asks
ReceiverHost to restart immediately when idle, or as soon as the current session ends.

## Motion and timing
| What | Timing |
|---|---|
| Focus change (fill, scale ≤ 1.06 pills / 1.03 rows, halo) | 250 ms cubic ease-out (VisualState transition) |
| Overlay in / out | fade 250 ms + slide 30 px up 300 ms / fade 250 ms |
| Settings in / out | fade 300 ms + slide 40 px from the right / 250 ms |
| PIN in / out | fade 300 ms |
| Toast | fade + 24 px slide down 300 ms, hold 1.6 s or 4 s, fade + slide up 300 ms |
| Idle → mirroring | idle layer fades out 350 ms over the first video; then the backdrop and spinner stop |
| Mirroring → idle | idle layer fades in 300 ms, backdrop restarts |
| Connecting hint | fades in 350 ms after 3 s without video |
| Hold ring | fade 150 ms, arc redrawn every 16 ms |
| Backdrop | three Composition sprite visuals with radial gradients, offset drift 43–67 s loops; running only while the idle layer is visible |

GPU budget while mirroring: no XAML or Composition animation runs (backdrop/spinner stopped, overlay/stats/settings
collapsed when hidden). The 60 Hz gamepad poll (`Windows.Gaming.Input`) runs only in the `mirroring` state and does no
layout work unless a trigger or the right stick is deflected.

## Driving the UI from the dev box
Every `/cmd` body arrives through `Diag::Command` and is handled by `MainPage::OnCommand`, using the same functions
as the pad (`PressKey` / `ReleaseKey`). With `tools/devlog.py`:
```
tools/devlog.py cmd '{"key":"Menu"}'           # open settings
tools/devlog.py cmd '{"key":"Right"}'          # into the Receiver rows
tools/devlog.py cmd '{"key":"Down"}'           # Resolution
tools/devlog.py cmd '{"key":"Right"}'          # 1080p -> 1440p
tools/devlog.py cmd '{"key":"B"}'              # back to categories
tools/devlog.py cmd '{"key":"B"}'              # close settings
tools/devlog.py cmd '{"hold":"B","ms":1200}'   # hold-B disconnect while mirroring
tools/devlog.py cmd '{"viewMode":"Fill"}'
tools/devlog.py cmd '{"setting":{"audioOffsetMs":40}}'
```
Log lines to assert on (tag `ui`): `key <K> down layer=<idle|connecting|mirroring|overlay|settings|pin>`, `cmd <json>`,
`state <a> -> <b>`, `overlay show|hide`, `settings open|close`, `setting <key>=<value>`, `pin shown|hidden`,
`toast <title>`, `hold B -> disconnect`, `network ip=<ip|none>`, `backdrop on|off`.

## Requests to platform
1. ReceiverHost: a `Restart()` (or documented Stop/Start being safe at any time from the UI thread) for the
   restart-on-idle settings; `State()` getter; the events listed in contract §3 with the client name in `Connected`.
2. Settings: `Changed` should carry the key so the UI refreshes only what changed (it works without, just re-reads all).
3. Diag: `Diag::Command` raises the raw `/cmd` JSON body (string). The UI parses `key`, `hold`, `viewMode`, `zoom`,
   `setting`, `disconnect`; platform does not need to interpret them. `GET /status` can then include the UI layer if
   you expose a hook (`MainPage` logs it on every key anyway).
4. App: navigate the root Frame to `XboxCast.MainPage` and do not set focus elsewhere; the page focuses itself.

## Requests to media
1. `MediaPipeline::ResetPan()` (centre) or document that `Pan` with a large negative delta clamps, so the UI can
   centre the view on Y and on a new session. The bridge has `ResetPan()` ready for it.
2. `Stats()` fields in a form the stats panel can show: latency ms, decoded/rendered fps, decode ms, dropped frames,
   codec + coded size, current effective mode (for Auto: whether it ended up filling or fitting).
