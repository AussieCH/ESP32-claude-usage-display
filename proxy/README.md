# Claude Usage Proxy

A small Python proxy (standard library only) that queries Anthropic's OAuth usage
endpoint — the same data source as Claude Code's `/usage` command — and serves the
result as slim JSON for the ESP32 dashboard. Your Claude credentials stay on this
host; clients only ever see utilization percentages and reset timestamps.

> **Compliance note:** Anthropic's usage policy restricts OAuth tokens from
> Free/Pro/Max plans to Claude Code and Claude.ai. A third-party tool polling this
> endpoint is a ToS grey area. Run it only for your own account, read-only, at a
> ≥180 s interval — at your own risk.

## Credentials

The proxy needs the **subscription OAuth token** (access + refresh) — the same one
Claude Code's `/usage` command uses.

> **`claude setup-token` does not work for this.** That long-lived static token
> (`sk-ant-oat01-…`, set via `STATIC_ACCESS_TOKEN`) is scoped to inference, and the
> usage endpoint rejects it with **HTTP 403**. It's supported by the proxy for
> completeness, but for `/usage` you need the subscription token below.

### Recommended — `credentials.json` (auto-refreshing)

On a machine that has Claude Code, sign in with your subscription:

```bash
claude auth login --claudeai
claude auth status          # expect "loggedIn": true
```

Then get a `.credentials.json` (`{"claudeAiOauth":{"accessToken":…,"refreshToken":…}}`):

- **Linux:** it's already a file at `~/.claude/.credentials.json`.
- **macOS:** the token lives in the **Keychain**, not a file (`claude /login` won't
  create one). Export it:
  ```bash
  security find-generic-password -s "Claude Code-credentials" -w > credentials.json
  ```
  (Click **Allow** on the Keychain prompt.)

Point `CREDENTIALS_FILE` at the file. The proxy refreshes the access token itself,
so it needs **write access** to persist rotated tokens.

## Token renewal & recovery

The subscription **access token expires ~every 8 h**. The proxy *tries* to renew it on
its own using the refresh token — proactively `REFRESH_LEAD` (2 h) before expiry, while
the current token is still a valid fallback. When that works, it's autonomous.

> **Reality check — auto-refresh is often rate-limited.** Anthropic rate-limits the
> **refresh-token grant** (`console.anthropic.com/v1/oauth/token`) hard for third-party
> clients: it returns a persistent **429 `rate_limit_error`** (often with `retry-after: 0`)
> that can stay stuck for **days** and does **not** drain by waiting. This is a known,
> ecosystem-wide limit that hits every third-party tool polling the OAuth endpoints
> ([claude-code#30930](https://github.com/anthropics/claude-code/issues/30930)), not a
> bug in this proxy — an initial retry storm can also earn a long per-account penalty.
> The proxy defends what it can (in-memory token so a write failure can't cause
> refresh-per-request; a long `REFRESH_BACKOFF` so it pokes the endpoint at most ~every
> 4 h instead of feeding the limit), so **don't restart/rebuild the add-on repeatedly** —
> each restart drops the in-memory token and forces another refresh attempt. But no
> backoff clears the limit; only Anthropic relaxing it (or the penalty aging out) does.

**Recovery — push a fresh token (one command).** A **browser** login
(`claude auth login --claudeai`) uses a *different* grant that is **not** rate-limited
and yields a token valid ~8 h. The proxy exposes **`POST /credentials`** to install such
a token instantly — it writes the credentials file **and** updates the in-memory cache,
so no restart and no refresh call is needed:

```bash
# macOS — token lives in the Keychain
security find-generic-password -s "Claude Code-credentials" -w \
  | curl -fsS -X POST "$PROXY_URL/credentials" \
      -H "Authorization: Bearer $PROXY_TOKEN" --data-binary @-
```

`scripts/refresh-token.sh` wraps this (checks the Keychain token's expiry, re-runs the
browser login only if it's stale, then pushes) — set `PROXY_URL`/`PROXY_TOKEN` and run
it whenever the display is frozen. If the refresh limit ever clears, auto-refresh
resumes on its own with no change needed.

`POST /credentials` requires the same `AUTH_TOKEN` Bearer as `/usage`, is refused in
`STATIC_ACCESS_TOKEN` mode, and never echoes the token back (it returns only
`{ok, persisted, expiresInMin, keys}`).

## Run

**Docker (credentials file):**
```bash
mkdir data && cp /path/to/credentials.json data/.credentials.json
# set AUTH_TOKEN in docker-compose.yml (e.g. `openssl rand -hex 32`)
docker compose up -d
curl -H "Authorization: Bearer <AUTH_TOKEN>" http://localhost:8787/usage
```

**Home Assistant OS:** run the proxy as a local add-on — see the full walkthrough in
[`../docs/home-assistant.md`](../docs/home-assistant.md).

**Without Docker:** see the comment header in `claude-usage-proxy.service`.

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `CREDENTIALS_FILE` | `~/.claude/.credentials.json` | Subscription credentials JSON (recommended) |
| `AUTH_TOKEN` | *(empty)* | Required Bearer token for `/usage`; if empty, only private/loopback clients are served |
| `CACHE_SECONDS` | `600` | Seconds a normal poll may serve cached data (10 min keeps well clear of the rate limit) |
| `FORCE_MIN_SECONDS` | `30` | Floor for `GET /usage?force=1` — smallest gap between real upstream fetches even when forced |
| `BACKOFF_429` | `1800` | On a 429 from the **usage** endpoint, seconds to leave it alone (unless `Retry-After` asks for longer) |
| `REFRESH_BACKOFF` | `14400` | On a 429 from the **token-refresh** endpoint, seconds to back off. Its rate limit is a rolling window, so long backoff (4 h) lets it drain instead of being kept alive by frequent retries |
| `REFRESH_LEAD` | `7200` | Renew the access token this many seconds **before** it expires, while the current one is still a valid fallback (2 h) — see Token renewal |
| `STATIC_ACCESS_TOKEN` | *(empty)* | Static token as-is (skips file + refresh); **403s on the usage endpoint — see Credentials** |
| `PORT` / `BIND` | `8787` / `0.0.0.0` | Listen port / address |

## Immediate refresh (`GET /usage?force=1`)

A normal `GET /usage` serves the cache (up to `CACHE_SECONDS`). Adding `?force=1`
bypasses the cache and fetches fresh from Anthropic **now** — for an on-demand
refresh (e.g. a device button) — while still respecting `FORCE_MIN_SECONDS` so a
mashed button can't hit the rate limit. This lets you run a gentle cache (10 min)
and still get the latest numbers on demand.

## Response format (`GET /usage`)

```json
{
  "valid": true,
  "model": "",
  "fiveHour":     {"available": true, "utilization": 34, "resetsAt": "2026-07-04T18:00:00Z"},
  "sevenDay":     {"available": true, "utilization": 12, "resetsAt": "2026-07-08T00:00:00Z"},
  "sevenDayOpus": {"available": false, "utilization": 0, "resetsAt": ""},
  "fetchedAt": "2026-07-04T16:03:11Z"
}
```

On upstream errors the proxy serves the last known result with `"stale": true`
instead of passing the error through. On **HTTP 429** it also stops touching
upstream (even on `?force=1`) until the `Retry-After` / `BACKOFF_429` window
passes, so a rate-limit cooldown can't snowball into repeated hits.

## Reachable remotely via Tailscale Funnel

```bash
tailscale funnel --bg 8787
tailscale funnel status   # prints the public https://<node>.<tailnet>.ts.net URL
```

`AUTH_TOKEN` is then **mandatory** — Funnel makes the port publicly reachable.
Without a token the private-only guard does kick in (Funnel terminates TLS locally
and proxies from `127.0.0.1`; the proxy reads the real client from
`X-Forwarded-For` and rejects public requests), but do not rely on it — a set
`AUTH_TOKEN` is the only real access control.

The ts.net URL has a valid Let's Encrypt certificate; the firmware verifies it
against its built-in ISRG Root X1.

> **Note:** the firmware's HTTPS certificate check needs the correct time. After
> boot the ESP32 only verifies successfully once NTP has synced (~15–30 s); until
> then it may briefly show "Proxy Error". This does not apply to `http://` LAN URLs.

## Home Assistant (optional)

> Full step-by-step guide (install Tailscale in HA, run the proxy as Docker or a
> local HA add-on, expose it to the T-Display via Tailscale Funnel, REST sensor
> every 3 minutes): **[../docs/home-assistant.md](../docs/home-assistant.md)**

```yaml
rest:
  - resource: http://<proxy-host>:8787/usage
    headers:
      Authorization: "Bearer <AUTH_TOKEN>"
    scan_interval: 180   # 3 minutes — matches the proxy cache
    sensor:
      - name: "Claude Usage 5h"
        value_template: "{{ value_json.fiveHour.utilization }}"
        unit_of_measurement: "%"
      - name: "Claude Usage 7d"
        value_template: "{{ value_json.sevenDay.utilization }}"
        unit_of_measurement: "%"
```
