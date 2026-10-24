#!/usr/bin/env bash
# Renders every docs/design/*.html mockup to docs/design/png/<name>.png at 1920x1080 with a headless Chromium.
# CHROME may point at any chromium/chrome binary; defaults to a Playwright headless shell if present.
set -eu
cd "$(dirname "$0")"
chrome="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium_headless_shell-*/chrome-*/chrome-headless-shell 2>/dev/null | tail -1)}"
[ -x "$chrome" ] || { echo "set CHROME to a chromium binary" >&2; exit 1; }
mkdir -p png
for f in ${1:-*.html}; do
  n="${f%.html}"
  "$chrome" --headless --disable-gpu --hide-scrollbars --force-device-scale-factor=1 --window-size=1920,1080 \
    --virtual-time-budget=500 --allow-file-access-from-files --screenshot="png/$n.png" "file://$PWD/$f" >/dev/null 2>&1
  echo "png/$n.png"
done
