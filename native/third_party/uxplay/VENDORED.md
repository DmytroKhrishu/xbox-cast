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

| File | Patch | Why |
|---|---|---|
| `lib/mdnsd/mdnsd.c` | accept adapters whose description contains "Virtual" when it is "Durango Virtual Network ... Adapter"; fallback interface when none matched | Xbox exposes its real NICs that way (fix from screen-mirror-xbox `docs/uxplay-lib-changes.patch`) |
| `lib/mdnsd/mdnsd.c` | join the responder thread before closing its sockets | closing a socket under a blocked `select()` crashes with 0xC0000008 on UWP suspend (screen-mirror-xbox) |
| `lib/mdnsd/dnssd_mdnsd.c` | advertise host `<host>-xc.local`, never `<host>.local` | the console/OS already owns its own name; a second responder for it causes conflicts (screen-mirror-xbox / airplay-xbox) |
| `lib/mdnsd/dnssd_mdnsd.c`, `lib/dnssd.h` | `dnssd_build_records()` (TXT records without starting the responder), `dnssd_get_host_name()` | castcore's `mdns_publish` mode hands the exact same TXT records to the app |
| `lib/crypto.c` | ed25519 key file read/written through memory BIOs | OpenSSL's `BIO_new_fp` needs the applink shim on Windows builds |
| `lib/srp.c` | no CryptoAPI seeding on Windows | `CryptAcquireContext` is not available to UWP apps; OpenSSL 3 seeds itself (BCryptGenRandom) |
| `lib/raop_ntp.c` | comment only: kernel RX timestamps not used on Windows | documents the UWP limitation |
| `lib/raop_rtp_mirror.c` | `IPPROTO_TCP` guard; on Windows the accepted mirror stream socket is non-blocking (FIONBIO) instead of `SO_RCVTIMEO` | Winsock reports a receive timeout as WSAETIMEDOUT, which upstream treats as fatal: video froze on the first pause in the stream while audio played on |
| `lib/raop_rtp_mirror.c` | report the size of every codec packet (not only the first) | castcore emits `on_video_format` on rotation |
| `lib/raop.h`, `lib/raop_handlers.h` | optional `report_client_address` callback | `on_client_connected` gets the client address |
| `lib/fairplay_playfair.c` | bound-check the FairPlay message index | out-of-bounds read on malformed fp-setup |
| `lib/compat.h` | MSVC >= 2015 uses the real `snprintf` | `_snprintf` does not NUL-terminate |

Generate the exact diff with `git diff 6702e29 -- native/third_party/uxplay` (6702e29 = pristine import).

## How to bump
See `docs/core.md`, "Bumping UxPlay".
