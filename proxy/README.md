# Claude Usage Proxy

Kleiner Python-Proxy (nur Stdlib), der Anthropics OAuth-Usage-Endpoint abfragt
— dieselbe Datenquelle wie der `/usage`-Befehl in Claude Code — und das Ergebnis
als schlankes JSON für das ESP32-Dashboard bereitstellt. Die Claude-Credentials
bleiben auf diesem Host; Clients sehen nur Prozentwerte und Reset-Zeiten.

> **Compliance-Hinweis:** Anthropics Nutzungsrichtlinie beschränkt OAuth-Tokens
> von Free/Pro/Max-Plänen auf Claude Code und Claude.ai. Ein Drittanbieter-Tool,
> das diesen Endpoint pollt, bewegt sich in einer ToS-Grauzone. Nur für den
> eigenen Account, read-only, mit ≥180 s Intervall betreiben — auf eigenes Risiko.

## Voraussetzungen

Claude-Code-Credentials auf dem Proxy-Host, eine der beiden Varianten:

1. **credentials.json** (empfohlen, mit automatischem Token-Refresh):
   Auf einem Rechner mit Claude Code `~/.claude/.credentials.json` kopieren
   (macOS: falls die Datei fehlt, liegen die Credentials im Keychain — dann
   einmalig auf dem Proxy-Host `claude /login` ausführen). Der Proxy braucht
   **Schreibzugriff** auf die Datei, um rotierte Tokens zu persistieren.
2. **Statisches Token:** `claude setup-token` erzeugt ein langlebiges OAuth-Token
   → als `STATIC_ACCESS_TOKEN` setzen. Kein Refresh nötig. Falls der Usage-
   Endpoint dieses Token ablehnt (es ist laut Doku auf Inferenz gescoped),
   Variante 1 verwenden.

## Start

**Docker:**
```bash
mkdir data && cp ~/.claude/.credentials.json data/
# AUTH_TOKEN in docker-compose.yml setzen (z.B. `openssl rand -hex 32`)
docker compose up -d
curl -H "Authorization: Bearer <AUTH_TOKEN>" http://localhost:8787/usage
```

**Ohne Docker:** siehe Kommentarkopf in `claude-usage-proxy.service`.

## Antwortformat (`GET /usage`)

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

Bei Upstream-Fehlern (z.B. 429) liefert der Proxy den letzten bekannten Stand
mit `"stale": true` aus, statt einen Fehler durchzureichen.

## Extern erreichbar via Tailscale Funnel

```bash
tailscale funnel --bg 8787
tailscale funnel status   # zeigt die öffentliche https://<node>.<tailnet>.ts.net-URL
```

`AUTH_TOKEN` ist dann **Pflicht** — Funnel macht den Port öffentlich erreichbar.
Ohne Token greift zwar der Private-Only-Schutz (Funnel terminiert TLS lokal und
proxyt von `127.0.0.1`; der Proxy liest den echten Client aus `X-Forwarded-For`
und weist öffentliche Requests ab), aber verlass dich nicht darauf — ein
gesetztes `AUTH_TOKEN` ist die einzige echte Zugriffskontrolle.

Die ts.net-URL hat ein gültiges Let's-Encrypt-Zertifikat; die Firmware prüft es
gegen die eingebaute ISRG Root X1.

> **Hinweis:** Die HTTPS-Zertifikatsprüfung der Firmware braucht die korrekte
> Uhrzeit. Nach dem Boot verifiziert das ESP32 erst nach NTP-Sync (~15–30 s)
> erfolgreich; bis dahin kann kurz „Proxy Error" erscheinen. Bei `http://`-URLs
> im LAN entfällt das.

## Home Assistant (optional)

> Ausführliche Schritt-für-Schritt-Anleitung (Tailscale in HA installieren, Proxy
> als Docker oder lokales HA-Add-on betreiben, via Tailscale Funnel extern fürs
> T-Display erreichbar machen, REST-Sensor alle 3 Minuten):
> **[../docs/home-assistant.md](../docs/home-assistant.md)**

```yaml
rest:
  - resource: http://<proxy-host>:8787/usage
    headers:
      Authorization: "Bearer <AUTH_TOKEN>"
    scan_interval: 180   # 3 Minuten — passt zum Proxy-Cache
    sensor:
      - name: "Claude Usage 5h"
        value_template: "{{ value_json.fiveHour.utilization }}"
        unit_of_measurement: "%"
      - name: "Claude Usage 7d"
        value_template: "{{ value_json.sevenDay.utilization }}"
        unit_of_measurement: "%"
```
