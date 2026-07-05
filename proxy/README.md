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

The proxy needs a Claude OAuth token. Pick one of two ways:

### Option 1 — Static token (simplest; the only practical way on macOS)

On a machine that has Claude Code, run:

```bash
claude setup-token
```

This opens a browser, you authorize, and it prints a long-lived token
(`sk-ant-oat01-…`). Set it as `STATIC_ACCESS_TOKEN` — no file and no refresh
needed. This is the recommended path when the proxy runs somewhere without its own
Claude login (e.g. a Home Assistant add-on).

> If the usage endpoint rejects this token (HTTP 401 — it is documented as scoped
> to inference), use Option 2 instead.

### Option 2 — `credentials.json` (auto-refreshing)

The proxy can read a Claude Code credentials file and refresh the access token
itself. **This file only exists on Linux** — Claude Code writes
`~/.claude/.credentials.json` there because Linux has no system keychain.

> **macOS note:** on macOS Claude Code stores its tokens in the **Keychain**, not
> in a file, so `~/.claude/.credentials.json` does **not** exist and `claude /login`
> will not create it. On macOS, use Option 1, or generate the file on the Linux
> host that will run the proxy by installing Claude Code there and logging in.

Point `CREDENTIALS_FILE` at the file. The proxy needs **write access** to it so it
can persist rotated tokens.

## Run

**Docker (with a static token):**
```bash
# set STATIC_ACCESS_TOKEN and AUTH_TOKEN in docker-compose.yml
docker compose up -d
curl -H "Authorization: Bearer <AUTH_TOKEN>" http://localhost:8787/usage
```

**Docker (with a credentials file):**
```bash
mkdir data && cp ~/.claude/.credentials.json data/   # Linux only, see above
# set AUTH_TOKEN in docker-compose.yml (e.g. `openssl rand -hex 32`)
docker compose up -d
curl -H "Authorization: Bearer <AUTH_TOKEN>" http://localhost:8787/usage
```

**Without Docker:** see the comment header in `claude-usage-proxy.service`.

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `STATIC_ACCESS_TOKEN` | *(empty)* | Use this token as-is; skips the file + refresh (Option 1) |
| `CREDENTIALS_FILE` | `~/.claude/.credentials.json` | Claude Code credentials JSON (Option 2) |
| `AUTH_TOKEN` | *(empty)* | Required Bearer token for `/usage`; if empty, only private/loopback clients are served |
| `CACHE_SECONDS` | `180` | Minimum seconds between upstream calls — do not lower |
| `PORT` / `BIND` | `8787` / `0.0.0.0` | Listen port / address |

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

On upstream errors (e.g. a 429) the proxy serves the last known result with
`"stale": true` instead of passing the error through.

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
