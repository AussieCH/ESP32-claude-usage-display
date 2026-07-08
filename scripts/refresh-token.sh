#!/usr/bin/env bash
#
# refresh-token.sh — drop a fresh Claude subscription token into the usage proxy
# in one command.
#
# Why this exists: Anthropic rate-limits the OAuth refresh-token grant hard for
# third-party clients (persistent HTTP 429 on console.anthropic.com/v1/oauth/token
# — a known, ecosystem-wide limit, not a bug in this proxy). So the proxy's
# automatic token refresh can stay stuck for a long time, and the display freezes
# on the last values. The reliable way to get a fresh token is a *browser* login
# (`claude auth login`, the authorization_code grant), which Anthropic does NOT
# rate-limit — it just yields a token that's only valid ~8 h.
#
# This script grabs that token from the macOS Keychain and POSTs it to the proxy's
# /credentials endpoint, which writes the file AND updates the in-memory cache with
# no restart (and without touching the rate-limited refresh endpoint). Run it
# whenever the display is frozen; it takes ~5 seconds.
#
# Setup (once): set PROXY_URL and PROXY_TOKEN below, or export them, e.g.
#   export PROXY_URL=https://homeassistant.tail0c3ada.ts.net
#   export PROXY_TOKEN=<the add-on auth_token>
# Then:  ./scripts/refresh-token.sh
#
# macOS only (reads the Keychain). On Linux the token is already a file at
# ~/.claude/.credentials.json — curl that to /credentials instead.

set -euo pipefail

PROXY_URL="${PROXY_URL:-https://homeassistant.tail0c3ada.ts.net}"
PROXY_TOKEN="${PROXY_TOKEN:-}"            # the proxy add-on's auth_token (Bearer)
KEYCHAIN_SERVICE="Claude Code-credentials"
MIN_VALID_SECONDS=1800                    # re-login if the token expires within 30 min

if [ -z "$PROXY_TOKEN" ]; then
  echo "error: PROXY_TOKEN is not set — export the proxy's auth_token first:" >&2
  echo "       export PROXY_TOKEN=<add-on auth_token>" >&2
  exit 1
fi

get_cred() { security find-generic-password -s "$KEYCHAIN_SERVICE" -w 2>/dev/null || true; }

# Seconds until the token in $1 expires (negative/0 if expired or unparseable).
seconds_left() {
  printf '%s' "$1" | python3 -c '
import sys, json, time
try:
    d = json.load(sys.stdin)
    o = d.get("claudeAiOauth", d)
    print(int(o.get("expiresAt", 0) / 1000 - time.time()))
except Exception:
    print(-1)
' 2>/dev/null || echo -1
}

CRED="$(get_cred)"
LEFT=-1
[ -n "$CRED" ] && LEFT="$(seconds_left "$CRED")"

if [ "$LEFT" -lt "$MIN_VALID_SECONDS" ]; then
  echo "→ Keychain token missing or expiring (${LEFT}s left); opening browser login…"
  claude auth login --claudeai
  CRED="$(get_cred)"
  [ -n "$CRED" ] && LEFT="$(seconds_left "$CRED")"
fi

if [ -z "$CRED" ]; then
  echo "error: still no '$KEYCHAIN_SERVICE' in the Keychain — run: claude auth login --claudeai" >&2
  exit 1
fi

echo "→ pushing fresh token (valid ~$(( LEFT / 60 )) min) to $PROXY_URL/credentials …"
RESP="$(printf '%s' "$CRED" | curl -fsS -X POST "$PROXY_URL/credentials" \
  -H "Authorization: Bearer $PROXY_TOKEN" \
  -H "Content-Type: application/json" \
  --data-binary @-)"
echo "✓ proxy accepted: $RESP"
echo "  The display should refresh within a minute."
