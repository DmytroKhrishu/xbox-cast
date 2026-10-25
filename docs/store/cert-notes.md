# Certification notes: Airtime

Two texts for Partner Center. Both are kept for later submissions, so update them whenever the first-launch flow
changes.

1. **Additional testing information** (product-level page: product → Additional testing information; Save
   description). The Credentials table stays empty: the app has no sign-in.
2. **Restricted capabilities → hevcPlayback** (submission → Submission options). Approval is normally asked once and
   not repeated on updates (Microsoft Learn,
   [Manage submission options](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/manage-submission-options#restricted-capabilities)).

Before pasting: the Store package must be built with `XcStoreBuild=true` (MSBuild log line
`xbox-cast: ... store=true`), which turns the developer diagnostics server off. The text below states that.

## 1. Additional testing information (1,917 chars; kept under 2,000)
```
Airtime is a screen mirroring receiver: it lets an iPhone or iPad on the same local network mirror its screen and sound to the TV through the Xbox (AirPlay screen mirroring). No account, sign-in, credentials, internet service or purchase is needed. It works only on the local network.

Without an iPhone or iPad (everything except the mirroring itself):
1. Launch Airtime. The idle screen shows the receiver name (default "Xbox"), how to connect, the network address and version (bottom left) and controller hints (bottom right).
2. Press Menu to open Settings. Up/Down moves between the categories Receiver, Picture and Sound, and About; Right or A enters a category; B goes back; B on the category list closes Settings.
3. Change values (Left/Right on Resolution, A on a switch, A on Name opens the keyboard). Relaunch the app: the values are kept.
4. Settings > About shows the version and the license information.
5. On the idle screen, LB/RB and LT/RT set the starting view mode (a message confirms); View toggles stats.

With an iPhone or iPad on the same network as the console:
6. On the phone, open Control Center, tap Screen Mirroring and choose the name shown on the TV. Screen and sound appear within seconds; rotating the phone rotates the picture.
7. While mirroring: A opens the control bar (view modes Fit, Fill, Zoom, Auto; audio delay), LB/RB switch view modes, LT/RT zoom, the right stick pans when zoomed, X turns sound on/off, Y resets the view, View shows stats, hold B for one second disconnects.
8. Optional: Settings > Receiver > Require code to connect = On; the next connection shows a 4-digit code on the TV that the phone must enter.

Restricted capability hevcPlayback: used for hardware H.265/HEVC decoding and 4K output of the mirrored picture; see the justification in Submission options. Because of it the app is closed when a game starts (expected). The app has no background audio.
```

Checks behind the steps (keep them true): idle text and legend `docs/ui.md` "Screens and states", "Controls legend";
Settings navigation and persistence `docs/controller-ux.md` "Settings"; overlay and pad mapping
`docs/controller-ux.md` "Mapping"; PIN `docs/core.md` (4-digit code per connection with `pin_mode = 1`).
No minimum iOS version is stated: only a current iPhone has been tested.

## 2. hevcPlayback justification (939 chars)
```
Airtime receives AirPlay screen mirroring from iPhone and iPad and shows it on the TV. Current iPhones send the mirrored screen as H.265/HEVC when the receiver offers it, at up to 3840x2160 and 60 frames per second (we receive 3840x1768 landscape and 994x2160 portrait streams from a current iPhone). hevcPlayback is needed for three things: hardware HEVC decoding through Direct3D 11 video acceleration on the console, 4K display output for the decoded frames, and the larger memory budget that the 4K decode surfaces, the swap chain and the decoder's reference frames need. The app plays no protected (DRM) content and stores nothing. Users can lower the resolution to 1080p or turn H.265 off in Settings. We accept the documented side effects: the app is closed when a game starts and cannot play audio in the background; it has no background audio feature. The app is open source (GPL-3.0); the source is linked from the Store listing.
```

Sources: memory budget and side effects, Microsoft Learn
[4K video playback for UWP apps on Xbox](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/hevc-xbox)
("allocated 3.25GB of memory. This is 2GB more", "your app will be suspended and closed" when a game launches);
stream sizes from the first 4K console test (2026-10-25: HEVC 3840x1768 landscape / 994x2160 portrait at 60 fps,
d3d11va decode about 1.4 ms per frame, 0 errors); owner decision `docs/decisions.md`
(2026-10-24, no background audio, 4K ranks higher).
