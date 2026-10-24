# Rules for every agent working in this repo

Read this file, `docs/architecture.md` and `docs/contract.md` before touching anything.

## Open source from day one
This repo is public (GPL-3.0). It must never contain personal content:
- no LAN/tailnet IPs, hostnames, tailnet or funnel names, MAC addresses
- no emails other than the GitHub noreply address, no real names beyond the GitHub handle
- no Store product IDs, publisher CNs/GUIDs, certificates, passwords, tokens
- no host paths like `/home/<user>`, no personal device names

Identities, IDs and secrets come from CI variables/secrets (see `docs/building.md`) or a gitignored `.env`
(template: `.env.example`). Use placeholders in docs: `<xbox-lan-ip>`, `<dev-machine>`.
Run `tools/scrub-check.sh` before every commit; it must print `clean`. It reads extra private patterns from
`$SCRUB_PATTERNS_FILE` (default `~/.config/xbox-cast/scrub-patterns.txt`, never committed).

## Git
- Author: `DmytroKhrishu <33790867+DmytroKhrishu@users.noreply.github.com>` (set per commit with `-c user.name=... -c user.email=...`
  or `git config` in your worktree).
- Commit straight to `main`, small focused commits, imperative subject.
- Several agents push to the same repo. `git pull --rebase` before every push. Commit only files you own (see
  `docs/contract.md` "Ownership"). Never `git add -A` in a shared checkout; add paths explicitly.
- Push/gh as account DmytroKhrishu: `GH_TOKEN=$(gh auth token --user DmytroKhrishu) git push` /
  `GH_TOKEN=$(gh auth token --user DmytroKhrishu) gh ...`. The default gh login on the dev box is a different account.

## Licensing
- The project is **GPL-3.0** (`LICENSE`), same as UxPlay, whose AirPlay code we vendor.
- Every vendored third-party tree keeps its own LICENSE file and is listed in `THIRD_PARTY.md` with upstream URL, commit/version
  and license. Only GPL-3.0-compatible licenses (MIT/BSD/ISC/zlib/Apache-2.0/LGPL/GPL-3.0). Never vendor GPL-2.0-only code.

## The console
- The test Xbox is in RETAIL mode unless the owner says otherwise. Only he can switch to Dev Mode. Never restart/reboot it, sign
  out, or uninstall his apps. Restarting our own app via Device Portal is fine once Dev Mode is on.
- Tests on the console: short (<= 20 s of playback), then stop and leave our app on its idle screen.
- No debugger on the console: every native path must log richly (see contract "Diagnostics").

## Style
- C#: 4 spaces, file-scoped namespaces not available (C# 7.3 on .NET Native), keep it simple.
- C/C++: 4 spaces, `snake_case` for C, C++/WinRT conventions for the component.
- No formatter runs on existing files (rewrites whole files, unmergeable diffs).
- No emoji in UI. Icons: Segoe Fluent/MDL2 glyphs only where needed, always with a text label.
