#!/usr/bin/env bash
# Fails if the tree (tracked + staged files) contains personal or secret-looking content. Prints "clean" when OK.
# Generic patterns live here; private ones (owner's hostnames, names, IDs) come from $SCRUB_PATTERNS_FILE, one ERE per line,
# default ~/.config/xbox-cast/scrub-patterns.txt (never committed).
set -u
cd "$(git rev-parse --show-toplevel)"
pf="${SCRUB_PATTERNS_FILE:-$HOME/.config/xbox-cast/scrub-patterns.txt}"
excl=(':!LICENSE' ':!native/third_party/**' ':!tools/scrub-check.sh' ':!**/package-lock.json')
generic=(
  '\b192\.168\.[0-9]{1,3}\.[0-9]{1,3}\b'
  '\b10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\b'
  '\b172\.(1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3}\b'
  '\b100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.[0-9]{1,3}\.[0-9]{1,3}\b'
  '\.ts\.net\b'
  '/home/[a-z]'
  '/Users/[a-z]'
  'BEGIN [A-Z ]*PRIVATE KEY'
  'CN=[0-9A-F]{8}-[0-9A-F]{4}-'
  '\b9[A-Z0-9]{11}\b'
  'ghp_[A-Za-z0-9]{20,}'
  '[A-Za-z0-9._%+-]+@(gmail|outlook|hotmail|icloud|yahoo|proton)\.'
)
bad=0
check() {
  local out
  out=$(git grep -nIiE -e "$1" --cached -- . "${excl[@]}" 2>/dev/null)
  if [ -n "$out" ]; then echo "== pattern: $2"; echo "$out" | head -20; bad=1; fi
}
for p in "${generic[@]}"; do check "$p" "$p"; done
if [ -f "$pf" ]; then
  n=0
  while IFS= read -r p; do
    [ -z "$p" ] && continue; case "$p" in \#*) continue;; esac
    n=$((n+1)); check "$p" "private pattern #$n"
  done < "$pf"
else
  echo "warning: no private pattern file at $pf (generic checks only)"
fi
# staged binary files that look like certs/keys
if git diff --cached --name-only | grep -qiE '\.(pfx|p12|pem|key|cer|crt|jks|snk)$'; then
  echo "== staged certificate/key file"; git diff --cached --name-only | grep -iE '\.(pfx|p12|pem|key|cer|crt|jks|snk)$'; bad=1
fi
if [ "$bad" = 0 ]; then echo clean; else exit 1; fi
