# Ideas / not-now list

Things noticed but deferred until the owner has seen the app on the TV.

## UI polish (from the design review of docs/design/png, 2026-10-24)
- Mirroring overlay is dense (~11 controls + 3 controller-hint groups in one bar). Calmer: one row of view-mode pills + a
  "More" for zoom/audio delay/stats; drop the hint chips that duplicate buttons (e.g. Disconnect appears twice); "Zoom" is
  both a view mode and a stepper label.
- Overlay covers the bottom of a portrait phone; consider a narrower floating panel in the pillarbox area.
- Settings: help text sits far from the focused row; bump footer/hint text to >= 28 px; align value right edges; give every
  editable row the same affordance (chevrons / "Edit").
- Idle footer contrast slightly low for 3 m viewing.

## Features after v1 (research order, see docs/research/feasibility.md)
1. AirPlay audio from music apps (RAOP: ALAC/AAC, cover art, metadata).
2. DLNA/UPnP media renderer (try Windows PlayToReceiver on Xbox first).
3. AirPlay video casting (URL / HLS).
