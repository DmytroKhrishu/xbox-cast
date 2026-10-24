# Vendored: UxPlay protocol library (`lib/`)

- Upstream: https://github.com/FDH2/UxPlay
- Commit: `116e39069e40f09350a372233937a55c890a02f8` (master, 2026-10-24, "(cosmetic) code cleanup")
- License: GPL-3.0 (`LICENSE`); parts carry their own notices: `lib/llhttp/LICENSE-MIT` (MIT),
  `lib/playfair/LICENSE.md` (GPL-3.0), Juho Vähä-Herttua's RPiPlay/AirPlayServer files (LGPL-2.1+ headers),
  `lib/mdnsd/` by kgbook (LGPL-2.1+ header).

## Why master and not the latest tag (v1.73.7, 2026-09-04)
v1.73.7 predates two things we need:
- `lib/mdnsd/` (self-contained mDNS responder, 69783a0 and follow-ups): the only advertising path proven on Xbox.
- 546820c "fix for missing TEARDOWN 110 on iOS 27 beta".
Bump to the next tag (v1.74) once it is released.

## What was taken
Everything under upstream `lib/` except:
- `lib/dns_sd/` (Bonjour/Avahi dns_sd backend; castcore uses `lib/mdnsd/` or the app's `mdns_publish` callback)
- the upstream `CMakeLists.txt` files (castcore has its own build, `native/CMakeLists.txt`)
Not taken at all: `uxplay.cpp`, `renderers/` (GStreamer), docs, Bluetooth LE beacon.

## Local patches
Every change is marked `/* xbox-cast: ... */` in the source. List: see the section below (kept in sync with
`git log -- native/third_party/uxplay`).

(none yet: this commit is the pristine upstream tree)

## How to bump
See `docs/core.md`, "Bumping UxPlay".
