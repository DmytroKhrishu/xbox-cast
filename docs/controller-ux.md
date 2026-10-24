# Controller UX

Binding for the UI (`app/src/ui/**`). Visual spec: `docs/design/png/*.png`. Timings and screens: `docs/ui.md`.

## Layers
The UI is one page with stacked layers. Exactly one layer owns input at a time, top-most first:

| Layer | When | Owns input while visible |
|---|---|---|
| **PIN** | castcore `on_pin` fired with a code | yes, above everything |
| **Settings** | Menu pressed | yes |
| **Overlay** | a button pressed while mirroring | yes |
| **Mirroring** | a session is active (connected, video may or may not have started) | when nothing above is open |
| **Idle** | no session | when nothing above is open |

`Connecting` is the Mirroring layer before the first frame: it shows a spinner and the same input rules apply.
The **Stats** panel is not a layer: it never takes focus and is toggled with View from anywhere except PIN.

## Mapping
"Tap" means press and release. B actions fire on **release** whenever a session is active, so a tap and the
hold-to-disconnect gesture never both fire. Everywhere else, actions fire on press. D-pad and stick/D-pad
directions auto-repeat on hold (system key repeat).

| Input | Idle | Mirroring (overlay hidden) | Overlay | Settings | PIN |
|---|---|---|---|---|---|
| **A** | nothing | show overlay | activate focused control | activate focused row (toggle / next value / edit name) | nothing |
| **B** tap | nothing (the app never exits itself; Home leaves it) | nothing | hide overlay | rows → back to the category list; category list → close Settings | cancel: drop the connecting client |
| **B** hold 1 s | nothing | disconnect | disconnect | disconnect (only with a session) | same as tap |
| **X** | nothing | sound on/off (toast "Sound off" / "Sound on") | same | nothing | nothing |
| **Y** | nothing | reset view: zoom 1.0, pan centred | same | nothing | nothing |
| **LB / RB** | nothing | previous / next view mode, toast with the mode name | same, the View pills follow | nothing | nothing |
| **LT / RT** (analog) | nothing | zoom out / in, speed ∝ pressure; switches the view mode to Zoom first | same | nothing | nothing |
| **Right stick** | nothing | pan, when zoom > 1.0 | nothing | nothing | nothing |
| **D-pad / left stick** | nothing | zoom > 1.0: pan one step (5% of the view); otherwise show overlay | move focus inside the overlay row | move focus (see Settings) | nothing |
| **View** | nothing | stats on/off | stats on/off | stats on/off | nothing |
| **Menu** | open Settings | open Settings (video keeps playing under a dim) | open Settings (overlay closes) | close Settings | nothing |

View modes cycle in this order: **Fit → Fill → Zoom → Auto → Fit** (RB forward, LB backward). The chosen mode, the
zoom level, stats and sound are remembered (settings keys `viewMode`, `zoom`, `showStats`, `audioEnabled`).
Pan resets to centre on every new session and on Y.

### Overlay (bottom bar)
One focus row, left to right: View pills **Fit · Fill · Zoom · Auto**, Zoom **−** / **+** (0.25 steps, 1.0–3.0),
Audio delay **−** / **+** (20 ms steps, −300..300), **Stats** (switch), **Disconnect**.
- Opens with focus on the current view mode pill. Up/Down do nothing (single row), Left/Right stop at the ends.
- Auto-hides 4 s after the last input. Any input inside the overlay restarts the timer.
- A on Disconnect disconnects immediately (it is a deliberate press on a focused button; hold B is the shortcut).

### Settings (in-page panel)
Left: category list (**Receiver**, **Picture and Sound**, **Advanced**, **About**) and the help text of the
focused row. Right: that category's rows. Nothing scrolls.
- Opens with focus on the first category. Up/Down move through categories; the right pane follows focus.
- Right (or A) on a category → first row. B in rows → back to the category. B on a category → close.
- Up/Down move through rows. Left from a row that has no value to adjust (Name, About) → back to the category.
- **Choice rows** (Resolution, Frame rate, Default view, Discovery): Left/Right step the value, A steps forward and
  wraps. Chevrons show on the focused row.
- **Switch rows** (H.265, Require code, Remove black bars, Sound, Show stats): A toggles, Left = Off, Right = On.
  The row always shows the state as text ("On"/"Off") plus the switch.
- **Audio delay**: Left/Right −/+ 20 ms, clamped to −300..300; A resets to 0.
- **Name**: A opens the system on-screen keyboard; Enter/accept saves (trimmed, 1–40 chars, empty restores the
  previous name); B in the keyboard cancels.
- Every change is saved and applied immediately. Rows marked *Applies on next connection* (Name, Resolution,
  Frame rate, H.265, Require code, Discovery) restart the receiver once no session is active.

### Hold B to disconnect
Only while a session exists. 200 ms after B goes down a ring fades in at the bottom centre with the label
"Hold to disconnect"; it fills over the remaining 800 ms. Releasing early fades the ring out (150 ms) and the release
counts as a tap. At 1000 ms the ring completes, the session is dropped and the release is swallowed.

## Driving the UI without a controller (dev server)
`POST /cmd` on the console's dev server (contract §4, `tools/devlog.py`). The UI handles these exactly like the
pad, through the same code path:

| JSON | Same as |
|---|---|
| `{"key":"A"}` … one of `A B X Y LB RB LT RT Up Down Left Right Menu View` | tap of that button (press + release). `LT`/`RT` = one 0.25 zoom step |
| `{"hold":"B","ms":1200}` | hold that button for `ms` ms, then release (any key name above; `ms` 50..5000) |
| `{"viewMode":"Fill"}`, `{"zoom":1.25}` | set view mode / zoom directly |
| `{"setting":{"audioOffsetMs":40}}` | write a setting; the UI refreshes like after a pad change |
| `{"disconnect":true}` | disconnect |

Examples: `tools/devlog.py cmd '{"key":"Menu"}'`, `tools/devlog.py cmd '{"hold":"B","ms":1200}'`.
Every handled command logs `ui` lines (`ui key A down layer=overlay focus=pill.Auto`), so a test can assert on the log.
