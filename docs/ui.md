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

`Bridge.cpp` is wired to platform's `Settings`, `ReceiverHost` and `Diag`. Media is picked up with
`__has_include("media/MediaPipeline.h")`: until that header lands the UI runs without video (media calls only log).

## Screens and states
One page, stacked layers, top-most owns input: **PIN > Settings > Overlay > Mirroring > Idle** (see controller-ux).

| Screen | Mockup | Shown when |
|---|---|---|
| Idle | `png/idle.png` | no session, receiver ready. Name (136 px), how-to line, drifting backdrop, Network IP + version bottom-left, controls legend bottom-right. No status line: ready is the normal case |
| Idle, receiver not available | `png/idle-error.png` | receiver Stopped with a `LastError` (castcore not built in, xc_create/xc_start failed). Plain-text line "Screen Mirroring is not available" above the name, the how-to line becomes the problem text, legend shrinks to Menu Settings. Polled every 2 s while idle |
| Connecting | `png/connecting.png` | ReceiverHost state `connecting`. Spinner + "Connecting to <client>"; the hint line fades in after 3 s |
| Mirroring | `png/overlay.png` (overlay shown) | state `mirroring`. Full-bleed video, zero chrome |
| Overlay | `png/overlay.png`, `png/overlay-zoom.png` | any button while mirroring; opaque bottom panel, auto-hides after 4 s idle |
| Stats | `png/stats.png` | `showStats` and mirroring; opaque top-right panel, refreshed 1 Hz, never takes focus |
| Settings | `png/settings.png`, `png/settings-picture.png` | Menu; opaque in-page panel (`#FF07070B`) over the idle screen or the live video |
| PIN | `png/pin.png` | `on_pin` with a code; opaque layer; hides on an empty code or when the session ends |
| Toast | `png/toast.png` | "Mirroring ended / iPhone disconnected" (4 s); view mode / zoom / sound / view reset (1.6 s) |

### Nothing see-through over video
The owner found translucent panels over the live picture unreadable. Every surface drawn over video is opaque:
overlay, stats, toast and hold ring use `PanelSolidBrush` `#FF14141B` (toast `#FF1E1E28`), Settings and PIN cover the
screen with `#FF07070B`. No full-width scrim/gradient behind the overlay. No decorative dots: a dot or colour may only
appear where it carries state (the selected view-mode pill, switches).

### Overlay content
One control row: view-mode pills **Fit · Fill · Zoom · Auto**; the zoom stepper (− value +) right after the pills
**only while the mode is Zoom** and with no caption of its own (so "Zoom" is never both a mode and a label);
**Audio delay** − value + at the right end. One hint row top-right: X Sound, View Stats, B Close, hold B Disconnect.
Stats and Disconnect exist only as button hints (no focusable duplicates).

### Controller glyphs
Hints are a controller glyph plus a text label, always both (house rule: no icon without a label). The glyphs come
from the system icon font, `FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets"` (Fluent Icons first, MDL2 Assets as
fallback; both ship the same code points; the Xbox shell has both). Glyph 36 px in `TextBrush`, label 28 px
SemiBold in `Text2Brush`, 4 px between paired glyphs, 8 px glyph → label, 44 px between hints.

| Button | Name in the font | Code point |
|---|---|---|
| A | ButtonA | `U+F093` |
| B | ButtonB | `U+F094` |
| Y | ButtonY | `U+F095` |
| X | ButtonX | `U+F096` |
| Menu | ButtonMenu | `U+EDE3` |
| View | ButtonView2 | `U+EECA` |
| LT | TriggerLeft | `U+F10A` |
| RT | TriggerRight | `U+F10B` |
| LB | BumperLeft | `U+F10C` |
| RB | BumperRight | `U+F10D` |

Source: Microsoft Learn, "Segoe MDL2 Assets icons"
(https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-ui-symbol-font) and "Segoe Fluent Icons font"
(https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-fluent-icons-font); both tables list every code
point above with the name shown. "Segoe Xbox MDL2 Symbol" / "Segoe Xbox Symbol" are not documented for UWP apps, so
they are not used. Row accessories use the same font: Edit `U+E70F`, ChevronLeft `U+E76B`, ChevronRight `U+E76C`.
The mockups draw look-alike stand-ins (`docs/design/glyphs.js`) because the dev box has neither font.

### Controls legend (idle)
Bottom-right, one row, same baseline as the Network / Version corner: **LB RB** View mode · **LT RT** Zoom ·
**View** Stats · **Menu** Settings · Hold **B** Disconnect. While idle LB/RB and LT/RT set the view the next session
starts with (toast confirms); View toggles stats. Hold B only acts during a session.
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
| About | version, castcore version, license line | — | read only | — |

`mdnsMode` has no row: built-in is the default and the only mode proven on Xbox, and "Discovery" meant nothing to the
owner. It stays reachable for testing: `tools/devlog.py cmd '{"setting":{"mdnsMode":"os"}}'`. With it the Advanced
category is gone (it had no other content).

Every row ends in a fixed 60 px accessory slot, so all values share one right edge: **pencil** (Edit, A opens the
on-screen keyboard: Name), **‹ ›** (Choice / Delay rows), or the **switch**. Accessories are always visible (45 %
opacity at rest, full on focus), so editability reads before focusing. The footer names what A does on the focused
row: A **Edit** / **Change** / **Turn on or off** / **Reset** / **Open** (category), then B **Back**.

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
`toast <title>`, `hold B -> disconnect`, `network ip=<ip|none>`, `backdrop on|off`,
`idle receiver status: ready|<LastError>`.

## Requests to platform
None blocking. Already covered by what landed: ReceiverHost restarts itself on receiver keys (debounced 300 ms),
`Settings::Changed` carries the key, `Diag::Command` forwards the raw `/cmd` body. Nice to have: a public generic
`Settings::Get(key)` (the bridge reads through `ToJson()` today, about 14 LocalSettings reads per lookup).

## Requests to media
1. `MediaPipeline::ResetPan()` (centre). Until then the bridge centres with `Pan(-10,-10)` then `Pan(0.5,0.5)`, relying
   on Pan being clamped to the content (contract says "clamped to content").
2. Register a `media` status provider (`Diag::SetStatusProvider("media", ...)`): the stats panel shows its fields next
   to castcore's on_stats JSON. Useful: latency ms, decoded/rendered fps, decode ms, dropped frames, codec + size,
   effective mode for Auto.
3. `MediaPipeline` must be default-constructible; the bridge creates it on page load, calls `Attach(VideoPanel)` and
   `ReceiverHost::SetSinks(p, p)`, and `Stop()` when a session ends.
