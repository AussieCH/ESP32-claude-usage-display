#!/usr/bin/env python3
"""Claude usage proxy for the ESP32 dashboard.

Polls Anthropic's OAuth usage endpoint (the same data source as Claude
Code's /usage command), handles access-token refresh, caches the result,
and serves it as the exact JSON schema the ESP32 firmware expects.

The Claude OAuth credentials never leave this host — clients only ever
see utilization percentages and reset timestamps.

Endpoints:
    GET /usage    normalized usage JSON (Bearer auth if AUTH_TOKEN is set)
    GET /health   liveness probe, no auth

Environment:
    PORT                 listen port                     (default 8787)
    BIND                 listen address                  (default 0.0.0.0)
    AUTH_TOKEN           required Bearer token for /usage. If unset, only
                         private/loopback clients are served; Funnel-forwarded
                         public requests are still rejected (real client is
                         read from X-Forwarded-For, trusted only from a
                         loopback peer). Set it anyway when exposing via Funnel.
    CREDENTIALS_FILE     Claude Code credentials JSON    (default ~/.claude/.credentials.json)
    STATIC_ACCESS_TOKEN  use this token as-is, skip file + refresh (optional)
    CACHE_SECONDS        min seconds between upstream calls (default 180;
                         do not lower this — the endpoint rate-limits hard)
    USER_AGENT           upstream User-Agent             (default claude-code/2.1.0)
    OAUTH_CLIENT_ID      client id for token refresh
                         (default: Claude Code's public client id)

Compliance note: Anthropic's usage policy restricts OAuth tokens from
Free/Pro/Max plans to Claude Code and Claude.ai. Polling this endpoint
from a third-party tool is a ToS grey area — run this for your own
account, at your own risk, read-only, and at gentle intervals.
"""

import hmac
import json
import os
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from ipaddress import ip_address
from pathlib import Path

PROXY_VERSION = "1.0.5"   # shown at startup, in /health, and the Server header

# ── Configuration ────────────────────────────────────────────────────

PORT             = int(os.environ.get("PORT", "8787"))
BIND             = os.environ.get("BIND", "0.0.0.0")
AUTH_TOKEN       = os.environ.get("AUTH_TOKEN", "").strip()
CREDENTIALS_FILE = Path(os.environ.get(
    "CREDENTIALS_FILE", str(Path.home() / ".claude" / ".credentials.json")))
STATIC_TOKEN     = os.environ.get("STATIC_ACCESS_TOKEN", "").strip()
CACHE_SECONDS    = max(60, int(os.environ.get("CACHE_SECONDS", "600")))
# Floor for /usage?force=1 — the smallest gap between real upstream fetches even
# when a client forces a refresh, so a mashed refresh button can't hit the limit.
FORCE_MIN_SECONDS = max(10, int(os.environ.get("FORCE_MIN_SECONDS", "30")))
# On HTTP 429, don't touch upstream again for this long (unless the response's
# Retry-After header asks for more) — keeps a rate-limit cooldown from snowballing.
BACKOFF_429      = max(60, int(os.environ.get("BACKOFF_429", "1800")))
BACKOFF_MAX      = 6 * 3600  # never back off longer than this, even if told to
USER_AGENT       = os.environ.get("USER_AGENT", "claude-code/2.1.0")
CLIENT_ID        = os.environ.get(
    "OAUTH_CLIENT_ID", "9d1c250a-e61b-44d9-88ed-5944d1962f5e")

USAGE_URL   = "https://api.anthropic.com/api/oauth/usage"
REFRESH_URL = "https://console.anthropic.com/v1/oauth/token"
BETA_HEADER = "oauth-2025-04-20"

# ── Logging ──────────────────────────────────────────────────────────

def log(msg: str) -> None:
    print(f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} {msg}", flush=True)

# ── OAuth credential handling ────────────────────────────────────────

_cred_lock = threading.Lock()


def _read_credentials() -> dict:
    with open(CREDENTIALS_FILE, "r", encoding="utf-8") as fh:
        return json.load(fh)


def _write_credentials(data: dict) -> None:
    tmp = CREDENTIALS_FILE.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
    os.chmod(tmp, 0o600)
    tmp.replace(CREDENTIALS_FILE)


def _refresh_access_token(oauth: dict) -> dict:
    """Exchange the refresh token for a new access token."""
    body = json.dumps({
        "grant_type": "refresh_token",
        "refresh_token": oauth["refreshToken"],
        "client_id": CLIENT_ID,
    }).encode()
    req = urllib.request.Request(
        REFRESH_URL, data=body, method="POST",
        headers={"Content-Type": "application/json",
                 "User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=15) as resp:
        payload = json.loads(resp.read().decode())
    oauth["accessToken"] = payload["access_token"]
    if payload.get("refresh_token"):
        oauth["refreshToken"] = payload["refresh_token"]
    oauth["expiresAt"] = int(time.time() * 1000) + \
        int(payload.get("expires_in", 3600)) * 1000
    log("[oauth] access token refreshed")
    return oauth


def get_access_token(force_refresh: bool = False) -> str:
    if STATIC_TOKEN:
        return STATIC_TOKEN
    with _cred_lock:
        data = _read_credentials()
        oauth = data.get("claudeAiOauth") or data.get("oauth") or {}
        if not oauth.get("accessToken"):
            raise RuntimeError(
                f"no accessToken in {CREDENTIALS_FILE} — log in with "
                "'claude /login' on this machine or copy the file here")
        expired = oauth.get("expiresAt", 0) <= int(time.time() * 1000) + 60_000
        if force_refresh or expired:
            oauth = _refresh_access_token(oauth)
            data["claudeAiOauth"] = oauth
            _write_credentials(data)
        return oauth["accessToken"]

# ── Upstream fetch + normalization ───────────────────────────────────

def _fetch_upstream(token: str) -> dict:
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": BETA_HEADER,
        "User-Agent": USER_AGENT,
        "Accept": "application/json",
        "Content-Type": "application/json",
    })
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode())


def _iso_from_any(value) -> str:
    """Accept ISO strings or epoch seconds/millis, return ISO 8601 UTC."""
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    if isinstance(value, (int, float)):
        ts = float(value)
        if ts > 1e12:          # milliseconds
            ts /= 1000.0
        return datetime.fromtimestamp(ts, tz=timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ")
    return ""


def _pct_from_any(value) -> int:
    # The OAuth usage endpoint returns utilization as a percentage on a 0-100
    # scale (e.g. 6.0 = 6%), so 1.0 means 1%. Do NOT treat 0 < v <= 1 as a
    # fraction to scale by 100 — that turned a real 1% into 100%.
    if value is None:
        return 0
    return max(0, min(100, round(float(value))))


def _pick(d: dict, *keys):
    for k in keys:
        if k in d and d[k] is not None:
            return d[k]
    return None


def _norm_block(block) -> dict:
    if not isinstance(block, dict):
        return {"available": False, "utilization": 0, "resetsAt": ""}
    util = _pick(block, "utilization", "utilization_pct", "used_pct", "percent")
    rst  = _pick(block, "resets_at", "reset_at", "resetsAt", "resets", "reset_time")
    return {
        "available": util is not None,
        "utilization": _pct_from_any(util),
        "resetsAt": _iso_from_any(rst),
    }


def normalize(raw: dict) -> dict:
    five = _pick(raw, "five_hour", "fiveHour", "session")
    week = _pick(raw, "seven_day", "sevenDay", "weekly")
    opus = _pick(raw, "seven_day_opus", "sevenDayOpus", "weekly_opus", "seven_day_sonnet_opus")
    out = {
        "valid": True,
        "model": str(_pick(raw, "model", "current_model") or ""),
        "fiveHour": _norm_block(five),
        "sevenDay": _norm_block(week),
        "sevenDayOpus": _norm_block(opus),
        "fetchedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    return out

# ── Cache ────────────────────────────────────────────────────────────

_cache_lock = threading.Lock()
_cache: dict = {"body": None, "raw": None, "ts": 0.0, "stale": False, "retry_after": 0.0}


def _stale() -> dict:
    """A copy of the cached body flagged as stale."""
    body = dict(_cache["body"])
    body["stale"] = True
    return body


def _ratelimit_headers(e: "urllib.error.HTTPError") -> dict:
    """The Retry-After / anthropic-ratelimit-* headers from a response, for logging."""
    if not e.headers:
        return {}
    return {k: v for k, v in e.headers.items()
            if "ratelimit" in k.lower() or k.lower() == "retry-after"}


def _retry_after_seconds(e: "urllib.error.HTTPError") -> float:
    """How long to back off: prefer Retry-After, else the latest
    anthropic-ratelimit-*-reset timestamp, else BACKOFF_429. Clamped to BACKOFF_MAX."""
    now = datetime.now(timezone.utc)
    hdrs = e.headers if e.headers else {}

    ra = hdrs.get("Retry-After")
    if ra:
        ra = ra.strip()
        if ra.isdigit():
            return min(float(ra), BACKOFF_MAX)
        try:
            return max(0.0, min((parsedate_to_datetime(ra) - now).total_seconds(), BACKOFF_MAX))
        except (TypeError, ValueError):
            pass

    # Anthropic returns reset timestamps per limited dimension — wait for the latest.
    latest = 0.0
    for k, v in (hdrs.items() if hdrs else []):
        if k.lower().startswith("anthropic-ratelimit") and k.lower().endswith("-reset"):
            try:
                when = datetime.fromisoformat(v.strip().replace("Z", "+00:00"))
                if when.tzinfo is None:
                    when = when.replace(tzinfo=timezone.utc)
                latest = max(latest, (when - now).total_seconds())
            except ValueError:
                pass
    if latest > 0:
        return min(latest, BACKOFF_MAX)

    return float(BACKOFF_429)


def get_usage(force: bool = False) -> dict:
    with _cache_lock:
        now = time.time()

        # In a rate-limit cooldown, never touch upstream (even on force) until the
        # backoff expires — otherwise repeated polls keep hitting 429 and hold the
        # cooldown open. Serve stale if we have any; if not (cold start during a
        # rate limit), surface an error but STILL don't poll upstream.
        if now < _cache["retry_after"]:
            if _cache["body"]:
                return _stale()
            raise RuntimeError(
                f"rate limited — retrying upstream in {int(_cache['retry_after'] - now)}s")

        # Serve cache unless it's older than the window — or, for a forced
        # refresh, older than the (much smaller) force floor.
        if _cache["body"]:
            window = FORCE_MIN_SECONDS if force else CACHE_SECONDS
            if now - _cache["ts"] < window:
                return _cache["body"]

        try:
            token = get_access_token()
            try:
                raw = _fetch_upstream(token)
            except urllib.error.HTTPError as e:
                if e.code == 401 and not STATIC_TOKEN:
                    log("[upstream] 401 — forcing token refresh")
                    token = get_access_token(force_refresh=True)
                    raw = _fetch_upstream(token)
                else:
                    raise
            body = normalize(raw)
            _cache.update(body=body, raw=raw, ts=now, stale=False, retry_after=0.0)
            log(f"[upstream] ok — 5h {body['fiveHour']['utilization']}% / "
                f"7d {body['sevenDay']['utilization']}%")
            return body

        except urllib.error.HTTPError as e:
            _cache["ts"] = now
            _cache["stale"] = True
            if e.code == 429:
                wait = _retry_after_seconds(e)
                _cache["retry_after"] = now + wait
                log(f"[upstream] HTTP 429 from {getattr(e, 'url', '?')} — "
                    f"backing off {int(wait)}s; "
                    f"rate-limit headers: {_ratelimit_headers(e) or 'none'}")
            else:
                log(f"[upstream] HTTP {e.code}: {e.reason}")
            if _cache["body"]:
                return _stale()
            raise
        except Exception as e:                      # noqa: BLE001
            log(f"[upstream] error: {e}")
            if _cache["body"]:
                # Same back-off as the HTTPError path: push the next attempt
                # out a full cache window so a sustained outage (DNS/network
                # down) can't make every client request hit upstream.
                _cache["ts"] = now
                _cache["stale"] = True
                return _stale()
            raise

# ── HTTP server ──────────────────────────────────────────────────────

def _is_private(addr: str) -> bool:
    try:
        ip = ip_address(addr)
        return ip.is_private or ip.is_loopback
    except ValueError:
        return False


def _is_loopback(addr: str) -> bool:
    try:
        return ip_address(addr).is_loopback
    except ValueError:
        return False


class Handler(BaseHTTPRequestHandler):
    server_version = f"claude-usage-proxy/{PROXY_VERSION}"

    def _send(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _effective_client_ip(self) -> str:
        """Real client IP, honoring X-Forwarded-For ONLY when the direct peer
        is loopback. Tailscale Funnel/Serve terminates TLS locally and reverse-
        proxies to us, so every Funnel request arrives from 127.0.0.1 with the
        real (often public) client in X-Forwarded-For — without this, the
        no-token fallback below would treat all Funnel traffic as local.
        We trust XFF only from a loopback peer, so a direct attacker on a
        0.0.0.0 bind cannot spoof a private address (their peer isn't loopback).
        """
        peer = self.client_address[0]
        if _is_loopback(peer):
            xff = self.headers.get("X-Forwarded-For", "")
            if xff:
                return xff.split(",")[0].strip()
        return peer

    def _authorized(self) -> bool:
        if AUTH_TOKEN:
            hdr = self.headers.get("Authorization", "")
            # Constant-time compare so the token can't be recovered by timing.
            return hmac.compare_digest(hdr, f"Bearer {AUTH_TOKEN}")
        # No token configured → private/loopback clients only. Funnel-forwarded
        # public requests are rejected here because _effective_client_ip()
        # surfaces the real X-Forwarded-For client, not the loopback proxy hop.
        return _is_private(self._effective_client_ip())

    def do_GET(self) -> None:                       # noqa: N802
        if self.path.split("?")[0] == "/health":
            self._send(200, {"ok": True, "version": PROXY_VERSION,
                             "cacheAge": round(time.time() - _cache["ts"])})
            return
        if self.path.split("?")[0] != "/usage":
            self._send(404, {"error": "not found"})
            return
        if not self._authorized():
            self._send(401, {"error": "unauthorized"})
            return
        force = self.path.split("?", 1)[1] if "?" in self.path else ""
        force = "force=1" in force or "force=true" in force
        try:
            self._send(200, get_usage(force=force))
        except Exception as e:                      # noqa: BLE001
            self._send(502, {"valid": False, "error": str(e)})

    def log_message(self, fmt, *args):              # quiet default access log
        pass


def main() -> int:
    if not STATIC_TOKEN and not CREDENTIALS_FILE.exists():
        log(f"[fatal] {CREDENTIALS_FILE} not found and STATIC_ACCESS_TOKEN unset")
        return 1
    if not AUTH_TOKEN:
        log("[warn] AUTH_TOKEN not set — /usage restricted to private/loopback "
            "clients (Funnel-forwarded public requests are rejected via "
            "X-Forwarded-For). Set AUTH_TOKEN anyway before exposing via "
            "Tailscale Funnel — it is the only real access control.")
    log(f"[start] claude-usage-proxy v{PROXY_VERSION} — listening on {BIND}:{PORT}, "
        f"cache {CACHE_SECONDS}s, "
        f"credentials: {'static token' if STATIC_TOKEN else CREDENTIALS_FILE}")
    ThreadingHTTPServer((BIND, PORT), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
