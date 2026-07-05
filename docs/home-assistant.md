# Home Assistant + Tailscale setup

This guide sets up the usage proxy on a Home Assistant host, makes it reachable
from **anywhere** for the T-Display via Tailscale Funnel, and surfaces the same
numbers inside Home Assistant with a REST sensor polling every 3 minutes.

It covers everything you asked for, in the order you actually do it:

1. [Add Tailscale to Home Assistant](#1-add-tailscale-to-home-assistant)
2. [Run the proxy on the Home Assistant host](#2-run-the-proxy-on-the-home-assistant-host)
3. [Expose the proxy externally via Tailscale Funnel](#3-expose-the-proxy-via-tailscale-funnel) (for the T-Display)
4. [REST sensor in Home Assistant, every 3 minutes](#4-rest-sensor-in-home-assistant-every-3-minutes)

---

## How it fits together

The ESP32 **cannot** join your tailnet (there is no Tailscale client for it), so
remote access goes through **Tailscale Funnel** — a public HTTPS URL with a valid
Let's Encrypt certificate, which the firmware verifies against its built-in ISRG
Root X1. Home Assistant reads the same proxy locally.

```
                        Tailscale Funnel  (public HTTPS, Let's Encrypt cert)
  T-Display S3  ───────────────────────────────────────────────►  Proxy :8787  ──►  api.anthropic.com
  (not on tailnet)   https://<node>.<tailnet>.ts.net/usage          on HA host        /api/oauth/usage
                                                                        ▲
  Home Assistant  ──── local: http://localhost:8787/usage ─────────────┘   (REST sensor, every 180 s)
```

> **On the same LAN only?** If the T-Display lives on the same network as Home
> Assistant, you don't need Tailscale at all — point the device straight at
> `http://<ha-ip>:8787/usage` and skip parts 1 and 3. Funnel is only for reaching
> the device from a *different* network.

**Prerequisites**
- A Home Assistant install (HA OS, Supervised, or Container).
- A Claude OAuth token for the proxy. Running the proxy on Home Assistant (with no
  Claude login of its own), the simplest is a **static token**: on a machine that
  has Claude Code, run `claude setup-token` and copy the `sk-ant-oat01-…` string.
  See [Credentials](#credentials) below. (A `~/.claude/.credentials.json` file is
  the other option, but on macOS that file doesn't exist — tokens live in the
  Keychain — so the static token is the practical choice here.)
- A [Tailscale](https://tailscale.com) account (free tier is enough).

---

## 1. Add Tailscale to Home Assistant

On **HA OS / Supervised** the easiest route is the community add-on:

1. **Settings → Add-ons → Add-on Store**, search for **Tailscale**. If it isn't
   listed, add the repository `https://github.com/hassio-addons/repository` via
   the store's ⋮ menu → *Repositories*, then search again.
2. **Install** the Tailscale add-on, then **Start** it.
3. Open the add-on **Log** (or **Web UI**) — it prints an authentication URL.
   Open it, log in, and approve the node. Home Assistant now appears in your
   [Tailscale admin console](https://login.tailscale.com/admin/machines).
4. In the add-on **Configuration**, it's worth enabling:
   - `userspace_networking: false` (needs the `NET_ADMIN` capability, but gives
     normal networking — fine on most installs)
   - Leave `accept_routes`, exit-node options off unless you use them.

On **Home Assistant Container** (or a companion Raspberry Pi / NAS), install
Tailscale on the host OS instead:

```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up
```

Verify with `tailscale status` — the host should show a `100.x.y.z` tailnet IP.

---

## 2. Run the proxy on the Home Assistant host

The proxy (`proxy/claude_usage_proxy.py`) authenticates to Anthropic, serves a
slim JSON on port **8787**, and keeps your credentials on the HA host.

### Credentials

The proxy needs a Claude OAuth token. Running it on Home Assistant, the practical
choice is a **static token** — no file, no second host:

1. On a machine that has Claude Code, run `claude setup-token`.
2. Authorize in the browser; copy the printed `sk-ant-oat01-…`.
3. Set it as the proxy's `STATIC_ACCESS_TOKEN` (env var, or `static_access_token`
   in the add-on config below).

> The alternative is an auto-refreshing `~/.claude/.credentials.json`, but that
> file only exists on **Linux** — on macOS the tokens live in the Keychain and
> `claude /login` won't create it. So on a Mac, use the static token. If the usage
> endpoint rejects the static token (HTTP 401), fall back to a credentials file
> generated on a Linux host. See [`../proxy/README.md`](../proxy/README.md).

### Option A — Docker (HA Container, Supervised, or a companion Pi/NAS)

If Docker is available on the host, this is the simplest:

```bash
git clone https://github.com/AussieCH/ESP32-claude-usage-display.git
cd ESP32-claude-usage-display/proxy
# In docker-compose.yml set:
#   STATIC_ACCESS_TOKEN: "sk-ant-oat01-..."   (from `claude setup-token`)
#   AUTH_TOKEN:          "..."                 (e.g. openssl rand -hex 32)
docker compose up -d
curl -H "Authorization: Bearer <AUTH_TOKEN>" http://localhost:8787/usage
```

See [`../proxy/README.md`](../proxy/README.md) for the full env-var reference, the
credentials-file variant, and a systemd alternative.

### Option B — Home Assistant OS local add-on (no host Docker access)

HA OS doesn't let you run arbitrary Docker, but it runs **local add-ons**. Using
the **Advanced SSH & Web Terminal** or **Samba** add-on, create
`/addons/claude-usage-proxy/` with these four files:

**`config.yaml`**
```yaml
name: Claude Usage Proxy
version: "1.0.0"
slug: claude_usage_proxy
description: Polls Anthropic usage and serves JSON for the ESP32 dashboard
arch: [aarch64, amd64, armv7]
init: false
ports:
  "8787/tcp": 8787
# map: [share:rw]        # only needed for the credentials-file variant (Linux)
options:
  auth_token: ""
  static_access_token: ""
  cache_seconds: 180
schema:
  auth_token: str?
  static_access_token: str?
  cache_seconds: int
```

**`Dockerfile`**
```dockerfile
ARG BUILD_FROM
FROM ${BUILD_FROM}
RUN apk add --no-cache python3
COPY claude_usage_proxy.py /claude_usage_proxy.py
COPY run.sh /run.sh
RUN chmod a+x /run.sh
CMD [ "/run.sh" ]
```

**`run.sh`**
```sh
#!/usr/bin/with-contenv bashio
export AUTH_TOKEN="$(bashio::config 'auth_token')"
export STATIC_ACCESS_TOKEN="$(bashio::config 'static_access_token')"
export CACHE_SECONDS="$(bashio::config 'cache_seconds')"
# For the credentials-file variant instead of a static token, drop the line above
# and use: export CREDENTIALS_FILE="/share/claude/.credentials.json"
exec python3 /claude_usage_proxy.py
```

**`claude_usage_proxy.py`** — copy it from this repo's `proxy/` folder.

Then:
1. **Settings → Add-ons → Add-on Store → ⋮ → Check for updates** — the
   *Local add-ons* section now lists **Claude Usage Proxy**. Install it.
2. In its **Configuration**, set `static_access_token` (from `claude setup-token`)
   and `auth_token` (a strong random string), then start it.
3. Check the **Log** for `[start] listening on 0.0.0.0:8787, ... credentials: static token`.

---

## 3. Expose the proxy via Tailscale Funnel

This is what lets the T-Display reach the proxy from **outside** your LAN. Funnel
publishes port 8787 as `https://<node>.<tailnet>.ts.net/` with a real certificate.

1. **Enable Funnel for your tailnet** in the admin console: [Access controls](https://login.tailscale.com/admin/acls)
   → make sure MagicDNS and HTTPS certificates are on, and the node is allowed to
   use Funnel (a `nodeAttrs` entry granting `funnel`). See Tailscale's
   [Funnel docs](https://tailscale.com/kb/1223/funnel).
2. From a shell where Tailscale runs (the Tailscale add-on's terminal, or the host
   for a package install), expose the proxy:

   ```bash
   tailscale funnel --bg 8787
   tailscale funnel status   # prints the public https://<node>.<tailnet>.ts.net URL
   ```

   > If Tailscale runs as the **add-on** (a separate container) and the proxy runs
   > as another add-on/container, point Funnel at the proxy by address instead of a
   > bare port, e.g. `tailscale funnel --bg 8787 http://<proxy-host>:8787`, where
   > `<proxy-host>` is reachable from the Tailscale container (the host IP or the
   > add-on hostname). On a single host install (Option A/host Tailscale) the bare
   > `tailscale funnel --bg 8787` is enough.
3. Test from your phone on mobile data:
   `https://<node>.<tailnet>.ts.net/usage` with the `Authorization: Bearer …` header.

> ⚠️ **Funnel makes the port public.** `AUTH_TOKEN` is mandatory here — it is the
> only real access control. (The proxy also rejects Funnel-forwarded public
> requests when no token is set, but don't rely on that.)

### Point the T-Display at it

In the device portal (`http://192.168.4.1`, Connection Settings):
- **Usage Proxy URL** → `https://<node>.<tailnet>.ts.net/usage`
- **Proxy Bearer Token** → your `AUTH_TOKEN`
- Leave the Claude cookie empty.

HTTPS is verified against the built-in ISRG Root X1. Right after boot the device
may briefly show `Proxy Error` until NTP sets the clock (certificate validity
needs the correct time).

---

## 4. REST sensor in Home Assistant (every 3 minutes)

To also see the numbers on your HA dashboards, add a REST sensor that reads the
proxy **locally** (no Funnel needed for HA itself). `scan_interval: 180` = 3 min,
which matches the proxy's cache window, so HA and the ESP32 share one upstream poll.

In `secrets.yaml`:
```yaml
claude_proxy_auth: "Bearer <AUTH_TOKEN>"
```

In `configuration.yaml`:
```yaml
rest:
  - resource: http://localhost:8787/usage   # or http://<ha-ip>:8787/usage
    scan_interval: 180                       # 3 minutes
    headers:
      Authorization: !secret claude_proxy_auth
    sensor:
      - name: "Claude Usage 5h"
        unique_id: claude_usage_5h
        value_template: "{{ value_json.fiveHour.utilization }}"
        unit_of_measurement: "%"
        icon: mdi:clock-outline
      - name: "Claude Usage 7d"
        unique_id: claude_usage_7d
        value_template: "{{ value_json.sevenDay.utilization }}"
        unit_of_measurement: "%"
        icon: mdi:calendar-week
      - name: "Claude Usage 5h Reset"
        unique_id: claude_usage_5h_reset
        value_template: "{{ value_json.fiveHour.resetsAt }}"
        device_class: timestamp
      - name: "Claude Usage Model"
        unique_id: claude_usage_model
        value_template: "{{ value_json.model }}"
      - name: "Claude Usage Stale"
        unique_id: claude_usage_stale
        value_template: "{{ value_json.stale | default(false) }}"
```

> If the proxy runs as a **local add-on**, use `http://<ha-ip>:8787/usage` (or the
> add-on hostname) instead of `localhost` — add-ons are separate containers.

Restart Home Assistant (or **Developer Tools → YAML → REST**), then check
**Developer Tools → States** for `sensor.claude_usage_5h`.

### Optional: keep the cadence gentle

The proxy rate-limits itself to one upstream call per `CACHE_SECONDS` (≥180 s) no
matter how many clients poll it, so the ESP32 and this sensor together still make
one call every 3 minutes. Don't lower `scan_interval` below 180 — you'd only get
cached values anyway, and the [usage endpoint rate-limits hard](../proxy/README.md).

---

## Troubleshooting

| Symptom | Check |
|---|---|
| T-Display shows `Proxy Error` | `curl https://<node>.ts.net/usage` from mobile data; wait ~30 s after boot for NTP; verify the Bearer token matches |
| Funnel URL returns 401 | `AUTH_TOKEN` set on the proxy but the client's `Authorization: Bearer …` is missing or wrong |
| REST sensor is `unknown` | `curl -H "Authorization: Bearer …" http://<ha-ip>:8787/usage` from the HA host; check the proxy add-on/container log |
| Proxy log shows `no accessToken` | `.credentials.json` missing/empty — copy it from a Claude Code machine into the mapped path |
| `value_json.stale` is `true` | The proxy is serving cached data because the upstream call failed (often a 429) — usually transient |
