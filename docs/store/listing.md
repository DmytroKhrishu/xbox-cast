# Store listing: Airtime (English, en-US)

Copy-paste source for Partner Center → product → Store listings → English. Every block below is exactly the text to
paste (plain text, no Markdown inside the fences). Character counts are Python `len()` of the fenced text (script
under "Counting" at the end); re-run it after any edit.

Field limits are from Microsoft Learn: [Add and edit Store listing info](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/add-and-edit-store-listing-info)
(description 10,000, short description 1,000 with ~270 shown, "What's new" 1,500, features 20 × 200) and
[Add additional information](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/add-additional-information)
(keywords 7 × 40 chars, ≤ 21 words total; copyright 200; additional license terms: one URL shows as a link;
"Developed by" 255).

<!--
Trademark rules followed (Apple, "Guidelines for Using Apple Trademarks and Copyrights",
https://www.apple.com/legal/intellectual-property/guidelinesfor3rdparties.html):
- "Authorized Use" 2 (Compatibility): an Apple word mark may describe compatibility only if (a) it is not part of the
  product name -> the product is "Airtime"; (b) it is used in a referential phrase ("for", "for use with", "compatible
  with") -> "Receive AirPlay screen mirroring from your iPhone or iPad on your Xbox"; (c) it is less prominent than the
  product name; (d) the product really works with it (verified with a real iPhone); (e) no sense of endorsement -> the
  disclaimer at the end of the description.
- "Rules for Proper Use" 1 and 3: trademarks are adjectives followed by a generic term -> "AirPlay screen mirroring",
  never "AirPlay" as a noun or verb ("to AirPlay"), never plural/possessive ("iPhone's" avoided).
- "Unauthorized Use" 2: no Apple logo or Apple-owned graphic symbol anywhere -> no AirPlay/Screen Mirroring glyph in the
  screenshots or art, no photos or renders of iPhones in the art ("Depictions" 2b: only real photos, and only with care;
  we use none).
- "Proper Trademark Notice" 2 (distribution outside the US): no (TM)/(R) symbols; use the credit notice
  "___ is a trademark of Apple Inc." -> the disclaimer line and the Copyright field.
Microsoft: "Xbox" is used the same referential way ("on your Xbox", "for Xbox"); never in the product name.
Microsoft also asks for no URLs or HTML in the description (FAQ on the Store listing page above), so the source link
lives in the Website / Additional license terms fields, not in the text.
-->

## Product name
`Airtime` (the reserved name; select it in the Product name drop-down).

## Short description (156 chars; the first ~270 are shown)
```
Mirror your iPhone or iPad to the TV through your Xbox, with sound and very low latency. Free, open source, no account, no ads, works on your local network.
```
Written to differ from the description's first paragraph (Microsoft recommends this, because both are shown).

## Description (2,225 chars)
```
Airtime turns your console into a screen mirroring receiver. Receive AirPlay screen mirroring from your iPhone or iPad on your Xbox: open Control Center on the phone, tap Screen Mirroring, choose your console, and your screen and sound appear on the TV within seconds.

Built for the living room
Mirroring runs with very low latency, so photos, videos, games and presentations feel immediate. Video is decoded in hardware in H.264 or H.265, up to 4K at 60 frames per second on a 4K-capable console and TV. Sound comes along, kept in sync with the picture by an adaptive buffer that rides out busy Wi-Fi. Turn the phone and the picture turns with it.

Fill the TV your way
Phones never send a TV-shaped picture, so Airtime has four view modes. Fit shows everything. Fill covers the whole screen. Zoom lets you zoom in with the triggers and pan with the right stick. Auto finds the black bars that apps add, for example a widescreen video inside a portrait phone, and crops them away so the picture fills the TV.

Made for the controller
Everything works from the controller: switch view modes with the bumpers, zoom with the triggers, nudge the audio delay if your TV lags behind, show live stats, or hold B to disconnect. In Settings you can rename the receiver, pick 1080p, 1440p or 4K and 30 or 60 fps, and require a code shown on the TV before a device can connect.

Private by design
Airtime works only on your local network. No account, no sign-in, no ads, no in-app purchases, no tracking. Nothing you mirror is recorded, stored or sent anywhere. Airtime is free and open-source software under the GNU General Public License version 3.

Good to know
Keep Airtime open while you mirror: it does not run in the background, and starting a game closes it. The phone or tablet and the console must be on the same network. One device can mirror at a time. Some apps block mirroring of protected video; their picture stays black.

Not supported yet: playing music to the console as an AirPlay speaker, and sending a video link from an app (AirPlay video casting). Mirroring from a Mac has not been tested.

AirPlay, iPhone and iPad are trademarks of Apple Inc. Airtime is not affiliated with or endorsed by Apple or Microsoft.
```

## What's new in this version
Microsoft: "If this is the first time you're submitting your app, leave this field blank." **Leave it blank for 1.0.**
Use this from the first update on (template, 209 chars of 1,500):
```
First release: AirPlay screen mirroring from iPhone and iPad with sound, H.264 and H.265 up to 4K at 60 fps, Fit, Fill, Zoom and Auto view modes, audio delay, optional connection code and a live stats overlay.
```

## Product features (16 of 20; each ≤ 200; paste without bullets)
```
Screen mirroring from iPhone and iPad, with sound
Very low latency with hardware video decoding
H.264 and H.265 video up to 4K at 60 fps (4K needs a 4K-capable console and TV)
Sound kept in sync with the picture, with an adjustable audio delay
Adaptive audio buffer for smooth sound over Wi-Fi
Follows the phone when you rotate it
Four view modes: Fit, Fill, Zoom and Auto
Auto removes black bars, for example a widescreen video inside a portrait phone
Zoom with the triggers, pan with the right stick
Optional code on the TV that a device must enter before it can connect
Rename the receiver to tell several consoles apart
Choose 1080p, 1440p or 4K, and 30 or 60 fps
Live stats: latency, frame rate and bitrate
Made for the controller, no keyboard or mouse needed
Local network only: no account, no sign-in, no tracking
Free and open source (GNU GPL version 3), no ads, no in-app purchases
```

## Keywords (7 keywords, 16 words; each ≤ 40 chars, ≤ 21 words total)
Keywords are not displayed; they only feed Store search. "AirPlay" is used referentially (what the app is
compatible with), like the description.
```
AirPlay receiver
screen mirroring
mirror iPhone
mirror iPad
cast screen to TV
wireless display
screen share
```

## Copyright and trademark info (132 chars of 200)
```
© 2026 DmytroKhrishu. AirPlay, iPhone and iPad are trademarks of Apple Inc. Xbox is a trademark of the Microsoft group of companies.
```

## Additional license terms
A single URL, so the Store shows it as a link:
```
https://github.com/DmytroKhrishu/xbox-cast/blob/main/LICENSE
```
Why not blank: blank means Microsoft's Standard Application License Terms only. The app is GPL-3.0, whose terms
(source availability, the right to modify and redistribute) are additional to the Store license, so the GPL is
linked explicitly. Third-party licenses: `THIRD_PARTY.md`, also summarised in the app's Settings > About.

## Developed by
```
DmytroKhrishu
```
("Published by" always shows the publisher display name.)

## Website (Properties → Support info)
```
https://github.com/DmytroKhrishu/xbox-cast
```

## Category
**Primary: Utilities + tools. Secondary: Entertainment.**

Microsoft's table ([Categories and subcategories](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/categories-and-subcategories)):
Utilities + tools = "apps which assist user in solving problems or completing specific tasks" with examples like
"remote controls"; Entertainment = "streaming movies and videos, live sports". Airtime has no content of its own and
streams nothing from the internet: it is a display tool for the user's own device, like a wireless HDMI cable, so it
belongs in Utilities + tools. Entertainment as the optional secondary category catches people who think of it as "watch
my phone on the TV". Precedent: screen-mirror-xbox, another UxPlay-based receiver, is listed in Utilities & tools
(docs/research/feasibility.md). Photo + video was considered and rejected (its examples are capture/editing apps).
Never pick Games: that choice can't be undone later.

## Additional system requirements (Store listing; each ≤ 200, ≤ 11 per list)
Minimum hardware:
```
An iPhone or iPad with Screen Mirroring, on the same local network as the console
Xbox Series X|S (tested with iOS 27.2)
```
Recommended hardware:
```
For 4K: a 4K TV
Console on wired Ethernet or 5 GHz Wi-Fi for the smoothest picture
```
Basis: Microsoft, [4K video playback for UWP apps on Xbox](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/hevc-xbox):
"4K and HDR10 video playback is supported on the Xbox One S onwards (the original Xbox One is restricted to 1080p)"
and the original Xbox One decodes HEVC in software only (same page), so on it 1080p with H.265 video off is the safe
setting. Tested on Xbox Series S (Series X runs the same app) with iOS 27.2; Xbox One consoles are untested, so the
listing names only Series X|S.
Properties → System requirements (hardware checkboxes): optional; leave blank, the controller is the only input.

## Screenshots (Xbox device family)
Xbox: PNG, 1920×1080 up to 3840×2160, ≤ 50 MB, up to 8, caption ≤ 200 each
([Screenshots and images](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/screenshots-and-images)).
Capture at 1920×1080 on the console (or 3840×2160 if the TV is 4K; both are accepted). Upload in this order:

| # | Screen | Set-up | Caption |
|---|---|---|---|
| 1 | Idle screen | Receiver named e.g. "Living Room" (not the owner's console name) | `Pick this name in Screen Mirroring on your iPhone or iPad.` |
| 2 | Mirroring, landscape, full screen | Phone in landscape showing the owner's own photo or video, View mode Fill, no overlay | `Your phone on the TV with sound and very low latency.` |
| 3 | Mirroring with the overlay open | Press A; focus on the Auto pill | `Fit, Fill, Zoom or Auto. Auto removes black bars.` |
| 4 | Auto mode, portrait phone | Portrait phone playing a 16:9 video; Auto has cropped the bars | `A widescreen video in a portrait phone fills the TV.` |
| 5 | Zoom | Zoom 2.0, panned with the right stick, overlay showing the zoom stepper | `Zoom in with the triggers, pan with the right stick.` |
| 6 | Settings, Receiver | Menu → Receiver; focus on Resolution = 4K | `Up to 4K at 60 fps with H.265.` |
| 7 | Settings, Picture and Sound | Focus on Audio delay | `Fine-tune the audio delay for your TV.` |
| 8 | Connection code | Receiver → Require code to connect = On, then connect | `Optional code before a device can connect.` |

Rules for the mirrored content in shots 2–5 and 8: only content the owner made (own photos, own video, a home
screen with no third-party app icons is better avoided too). No Apple logo, no Apple app UI that shows Apple-owned
graphics, no third-party app branding, no faces of other people, no notifications. Crop nothing: Microsoft wants
real screenshots of the app. If the console capture comes out as JXR (HDR) instead of PNG, convert to PNG before upload.
Stats overlay (View) is optional as an extra shot; keep it out of the hero shots.

## Store logos and Xbox art
Owned by the platform/art child (`docs/store/art/`). Partner Center needs for Xbox: 2:3 poster (720×1080 or
1440×2160, title in the top two-thirds, required), 1:1 box art (1080×1080 or 2160×2160, title in the top two-thirds,
required), 1:1 app tile icon 300×300 (strongly recommended), 16:9 super hero art 1920×1080 or 3840×2160 with no text
(recommended), and the Xbox images (branded key art 584×800, titled hero 1920×1080, featured promotional square
1080×1080).

## Trailer
None for 1.0.

## Counting
Re-run after any edit (counts include spaces and line breaks between paragraphs):
```
python3 - <<'EOF'
import re
t = open("docs/store/listing.md", encoding="utf-8").read()
for i, b in enumerate(re.findall(r"```\n(.*?)\n```", t, re.S)):
    lines = b.split("\n")
    print(i, len(b), "chars,", len(lines), "lines, longest", max(map(len, lines)))
EOF
```
