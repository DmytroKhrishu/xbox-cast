# Owner decisions

| Date | Decision |
|---|---|
| 2026-10-24 | License GPL-3.0 (UxPlay-derived). |
| 2026-10-24 | No background audio. 4K/HEVC ranks above it, so the app may use the `hevcPlayback` restricted capability (HEVC decode, more memory, 4K output; the app is suspended when a game launches, which is acceptable). |
| 2026-10-24 | Google Cast / Chromecast receiver: not built. |
| 2026-10-24 | Working name "Xbox Cast" for now; final product name later (Store name must avoid trademarks). |
| 2026-10-25 | Product name **Airtime** (reserved in Partner Center). Repo, namespaces and project files keep the xbox-cast/XboxCast names. Tested target: Xbox Series X|S + iOS 27.2; Xbox One not claimed. |
| 2026-10-24 | mDNS: castcore's built-in responder is the default (proven on Xbox by two UxPlay ports). |
