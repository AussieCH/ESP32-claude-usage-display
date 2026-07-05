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
| `BACKOFF_429` | `1800` | On HTTP 429, seconds to leave upstream alone (unless `Retry-After` asks for longer) so a rate-limit cooldown can't snowball |
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
