# Home Assistant + Tailscale setup

This is the exact, tested setup for running the usage proxy on **Home Assistant OS**
and reaching it from anywhere with the T-Display via **Tailscale Funnel** — one URL
that works both on your home LAN and remotely (e.g. from the office).

## How it fits together

The ESP32 **cannot** join your tailnet (there is no Tailscale client for it), so
remote access goes through **Tailscale Funnel** — a public HTTPS URL with a valid
Let's Encrypt certificate, which the firmware verifies against its built-in ISRG
Root X1. Home Assistant can read the same proxy locally.

```
                        Tailscale Funnel (public HTTPS, Let's Encrypt cert)
  T-Display S3  ─────────────────────────────────────────────►  Proxy add-on :8787  ──►  api.anthropic.com
  (not on tailnet)   https://<node>.<tailnet>.ts.net/usage         on Home Assistant       /api/oauth/usage
                                                                       ▲
  Home Assistant  ──── local: http://<ha-ip>:8787/usage ───────────────┘   (optional REST sensor, every 180 s)
```

> **Same LAN only?** If the T-Display always sits on the same network as Home
> Assistant, skip Tailscale entirely and point the device at
> `http://<ha-ip>:8787/usage`. Funnel is only needed to reach it from another network.

**Prerequisites**
- Home Assistant OS (this guide is written for it; aarch64/amd64).
- A **Claude subscription** (Pro/Max) — the OAuth token below needs it.
- A [Tailscale](https://tailscale.com) account (free tier is enough).
- The **Advanced SSH & Web Terminal** add-on — we use its shell and Docker access.
  Install it, set an `ssh` password (or key) in its config, and turn **Protection
  mode → OFF** (that grants the Docker socket access we need for the Funnel step).

---

## 1. Get the Claude credentials

> **Don't use `claude setup-token`.** That long-lived token is scoped to inference
> and the usage endpoint rejects it with **HTTP 403**. You need the **subscription
> OAuth token** (access + refresh) — the same one Claude Code's `/usage` uses.

On a machine that has Claude Code, sign in with your subscription:

```bash
claude auth login --claudeai
claude auth status          # expect "loggedIn": true
```

Now get a `.credentials.json` (format: `{"claudeAiOauth":{"accessToken":…,"refreshToken":…,…}}`):

- **Linux:** it's already a file at `~/.claude/.credentials.json` — use it directly.
- **macOS:** the token is stored in the **Keychain**, not a file. Export it:
  ```bash
  security find-generic-password -s "Claude Code-credentials" -w > credentials.json
  ```
  Click **Allow** on the Keychain prompt. `credentials.json` now holds the blob.

Put the file on Home Assistant at **`/share/claude/.credentials.json`**. Easiest via
the Web Terminal — paste the content:

```bash
mkdir -p /share/claude
cat > /share/claude/.credentials.json     # paste, then Enter, then Ctrl-D
chmod 600 /share/claude/.credentials.json
python3 -c "import json;print('ok:', list(json.load(open('/share/claude/.credentials.json'))['claudeAiOauth'].keys()))"
```

The proxy needs **write** access to this file so it can persist refreshed tokens
(handled by the `map` in the add-on below).

---

## 2. Install Tailscale in Home Assistant

1. **Settings → Add-ons → Add-on Store**, search **Tailscale** (if missing, add the
   repo `https://github.com/hassio-addons/repository` via ⋮ → *Repositories*).
2. Install and start it; open its **Log/Web UI** for the login URL, authorize the node.
3. In its **Configuration**, set **`userspace_networking: false`**. This is required
   so Tailscale can reach the proxy's host port (8787) for Funnel. Restart the add-on.
4. Leave **"Share Home Assistant with Serve or Funnel" OFF** — that option funnels
   Home Assistant's own UI (port 8123), which is *not* what the T-Display needs.

### Enable Funnel for the tailnet

1. Admin console → [DNS](https://login.tailscale.com/admin/dns): enable **MagicDNS**
   and **HTTPS Certificates**.
2. Admin console → [Access Controls](https://login.tailscale.com/admin/acls): allow
   Funnel via a top-level `nodeAttrs` block (works alongside the `grants` syntax):
   ```json
   "nodeAttrs": [
     { "target": ["autogroup:member"], "attr": ["funnel"] }
   ]
   ```

---

## 3. Create the proxy as a local add-on

In the Web Terminal, create the add-on folder and pull the proxy script from this repo:

```bash
mkdir -p /addons/claude-usage-proxy && cd /addons/claude-usage-proxy
wget -O claude_usage_proxy.py https://raw.githubusercontent.com/AussieCH/ESP32-claude-usage-display/main/proxy/claude_usage_proxy.py

cat > config.yaml <<'EOF'
name: Claude Usage Proxy
version: "1.0.3"
slug: claude_usage_proxy
description: Serves Claude usage as JSON for the ESP32 dashboard
arch:
  - aarch64
  - amd64
init: false
ports:
  "8787/tcp": 8787
map:
  - type: share
    read_only: false
options:
  auth_token: ""
  cache_seconds: 600
schema:
  auth_token: str?
  cache_seconds: int
EOF

cat > Dockerfile <<'EOF'
FROM python:3.12-alpine
COPY claude_usage_proxy.py /claude_usage_proxy.py
COPY run.sh /run.sh
RUN chmod a+x /run.sh
CMD [ "/run.sh" ]
EOF

cat > run.sh <<'EOF'
#!/bin/sh
C=/data/options.json
export AUTH_TOKEN="$(python3 -c "import json;print(json.load(open('$C')).get('auth_token',''))")"
export CACHE_SECONDS="$(python3 -c "import json;print(json.load(open('$C')).get('cache_seconds',600))")"
export CREDENTIALS_FILE="/share/claude/.credentials.json"
exec python3 /claude_usage_proxy.py
EOF
chmod +x run.sh
```

> Notes on these choices: the plain `python:3.12-alpine` image + reading
> `/data/options.json` avoids guessing HA base-image/bashio versions, so it builds
> deterministically. The `map` uses the **object form** (`type: share`), which is
> what current Supervisor versions expect. The proxy is stdlib-only, so there is
> nothing to `pip install`.

**Install & configure:**

1. **Settings → Add-ons → Add-on Store → ⋮ → Reload.** A **Local add-ons** section
   appears with **Claude Usage Proxy** → open it → **Install** (builds the image).
2. In **Configuration**, set **`auth_token`** to a strong random string
   (`openssl rand -hex 32`). This is the Bearer token clients send — **not** your
   Claude token. Save.
3. **Start**, and check the **Log** for:
   ```
   [start] claude-usage-proxy v1.0.3 — listening on 0.0.0.0:8787, cache 600s, credentials: /share/claude/.credentials.json
   ```

> **Gotcha — always Reload before Rebuild.** Local add-on `config.yaml` changes
> (options, `ports`, `map`) are only re-read when you **Reload** the Add-on Store.
> A "Rebuild" alone rebuilds the image but keeps the old config, so mount changes
> like the `map` silently don't apply. Sequence after any `config.yaml` edit:
> **Reload → Rebuild → Start**, and bump `version` to help HA notice.

**Verify** (Web Terminal — use the HA host IP, *not* `localhost`; each add-on is its
own container):

```bash
curl -H "Authorization: Bearer <auth_token>" http://<ha-ip>:8787/usage
```

Expect `{"valid": true, "fiveHour": {…}, "sevenDay": {…}, …}`. A `403` here means the
wrong (setup-)token — see section 1.

---

## 4. Expose the proxy via Tailscale Funnel

In the Web Terminal (Advanced SSH & Web Terminal with Docker access):

```bash
TS=$(docker ps --format '{{.Names}}' | grep -i tailscale | head -1)
docker exec "$TS" /opt/tailscale funnel --bg 8787
docker exec "$TS" /opt/tailscale funnel status
```

> The Tailscale add-on ships the CLI at **`/opt/tailscale`** — it is *not* on `$PATH`,
> so call it with the full path.

`funnel status` prints your public URL, proxying `/` → `127.0.0.1:8787`:

```
https://<node>.<tailnet>.ts.net (Funnel on)
|-- / proxy http://127.0.0.1:8787
```

**Verify from any network** (the URL is public; auth still protects `/usage`):

```bash
curl https://<node>.<tailnet>.ts.net/health                                   # {"ok": true}
curl -H "Authorization: Bearer <auth_token>" https://<node>.<tailnet>.ts.net/usage
```

Funnel started with `--bg` is **persistent** and survives reboots. If you ever
rebuild/reset the Tailscale add-on, re-run the `funnel --bg 8787` command.

---

## 5. Point the T-Display at it

Device portal (`http://192.168.4.1`) → **Connection Settings**:

- **Usage Proxy URL** → `https://<node>.<tailnet>.ts.net/usage`
- **Proxy Bearer Token** → your `auth_token`
- Leave the Claude cookie empty → **Save**

This one URL works both at home and remotely. Right after boot the device may show
`Proxy Error` for ~15–30 s until NTP sets the clock (the HTTPS certificate check
needs the correct time), then the usage values appear.

---

## 6. Optional — REST sensor in Home Assistant

To also show the numbers on HA dashboards, read the proxy **locally** (`scan_interval:
180` = 3 min, matching the proxy cache, so HA and the ESP32 share one upstream poll).

In `secrets.yaml`:
```yaml
claude_proxy_auth: "Bearer <auth_token>"
```

In `configuration.yaml`:
```yaml
rest:
  - resource: http://<ha-ip>:8787/usage
    scan_interval: 180
    headers:
      Authorization: !secret claude_proxy_auth
    sensor:
      - name: "Claude Usage 5h"
        unique_id: claude_usage_5h
        value_template: "{{ value_json.fiveHour.utilization }}"
        unit_of_measurement: "%"
      - name: "Claude Usage 7d"
        unique_id: claude_usage_7d
        value_template: "{{ value_json.sevenDay.utilization }}"
        unit_of_measurement: "%"
```

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `curl /usage` → `{"valid": false, "error": "HTTP Error 403: Forbidden"}` | A `claude setup-token` was used — it's scoped to inference. Use the subscription `.credentials.json` (section 1). |
| Add-on log: `.credentials.json not found` although the file exists | The `map` wasn't applied. **Reload** the Add-on Store, then **Rebuild**, then **Start** (use the object-form `map`). |
| `docker exec … "tailscale": executable file not found` | The binary is at `/opt/tailscale` — call it with the full path. |
| `curl localhost:8787` from the Web Terminal → connection reset | Each add-on is a separate container; `localhost` isn't the proxy. Use `http://<ha-ip>:8787`. |
| Display shows `Proxy Error` right after boot | NTP time not set yet (needed for the cert check) — resolves in ~30 s. |
| `funnel status` shows a URL but requests hang | The Tailscale add-on can't reach the proxy port. Confirm **`userspace_networking: false`** and that the proxy publishes `8787` on the host. |

> **Deployed differently?** On Home Assistant Supervised/Container (a host with
> Docker + a shell), you can run the proxy container directly and use host-level
> `tailscale funnel 8787` instead of the add-on gymnastics — see
> [`../proxy/README.md`](../proxy/README.md).
