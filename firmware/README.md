# Quota Meter ESP32 firmware

Standalone ESP-IDF 5.5/PlatformIO prototype for the JC3248W535EN
(ESP32-S3 N16R8): AXS15231B 480×320 landscape UI/touch (320×480 native
panel), 16 MB QIO flash, 8 MB OPI PSRAM, and GPIO1 backlight.

> **Prototype warning:** OpenAI device authentication/WHAM and Claude
> subscriber OAuth/usage are private, unsupported APIs and can change without
> notice. This development build stores provider tokens in plaintext NVS. A
> production device needs flash encryption, NVS encryption, secure boot,
> protected debug/download modes, and a manufacturing key lifecycle.

## Build and test

From the repository root:

```sh
pio test -d firmware -e native
pio run -d firmware -e jc3248w535en
pio run -d firmware -e jc3248w535en -t upload --upload-port "$ESPPORT"
pio device monitor -d firmware --port "$ESPPORT" --baud 115200
```

Erase all development credentials (destructive):

```sh
pio run -d firmware -e jc3248w535en -t erase --upload-port "$ESPPORT"
```

No upload port is hardcoded. The checked-in partition table uses one factory
application, enlarged NVS, and a reserved `nvs_keys` partition; it deliberately
has no OTA slots.

## Provisioning

On first boot the screen shows the device-specific `QuotaMeter-XXXXXX` WPA
network, its persisted random eight-digit password, and a Wi-Fi QR. Join it and
open `http://192.168.4.1`. Scan or enter a network and save. The device uses
APSTA while connecting, then keeps its settings server reachable on the station
IP. Disconnects retry with a bounded 1–60 second backoff; if saved credentials
remain unusable for 30 seconds, the setup AP returns.

Wi-Fi can also be provisioned through the USB Serial/JTAG console at 115200
baud. The command parser supports quoted SSIDs and passwords:

```text
connect "SSID_NAME" "password"
```

To avoid placing the password in shell history, copy `.env.example` to the
ignored `.env`, set `WIFI_SSID` and `WIFI_PASSWORD`, then run:

```sh
.venv/bin/python firmware/tools/provision_wifi.py --port "$ESPPORT"
```

The helper requires `pyserial` (installed with PlatformIO in the development
environment), auto-detects a single ESP32-S3 USB Serial/JTAG device when
`--port` is omitted, and never prints the password. The interactive REPL itself
echoes typed input, so treat a live serial session as sensitive.

The screen shows the authenticated settings URL as
`http://<device-ip>/?p=<setup-password>`. Visiting it sets an
`HttpOnly; SameSite=Strict` settings cookie. Provider mutation routes require
that cookie and a same-origin request. The local portal is HTTP because phones
cannot validate an arbitrary per-device self-signed certificate. Claude
callback material is therefore visible to the local LAN for its short lifetime.

To reset Wi-Fi only, use **Clear credentials & restart** on the Wi-Fi screen. A
full flash erase removes Wi-Fi, the setup password, and provider bundles.

## Provider authentication

- **OpenAI:** tap Login, scan the QR, open the displayed URL, and enter the user
  code. Polling stops after 15 minutes. A successful exchange stores the
  versioned token bundle before quota access.
- **Claude:** tap Login and scan the PKCE authorization QR. After browser
  authorization, paste the complete callback URL/query or `code#state` into the
  authenticated local portal within ten minutes. State is compared exactly.
- Connected accounts each get a dedicated swipe page in Codex, Claude, Add
  Connection order. Each provider page has refresh/logout controls and
  quota/reset data with distinct stale/error states.
- Automatic quota reads run once per minute. A provider `Retry-After` value
  longer than one minute is honored.

For development, existing desktop credentials can be imported over USB without
printing them:

```sh
.venv/bin/python firmware/tools/import_tokens.py --port "$ESPPORT"
```

The helper reads Codex credentials from `$CODEX_HOME/auth.json` (default
`~/.codex/auth.json`) and Claude credentials from the same storage used by the
desktop CLI, transfers bounded base64url chunks, persists both bundles, and
restarts the device. The REPL echoes received bytes, so do not run a serial
monitor concurrently. This is only appropriate for the plaintext-NVS prototype.


All upstream calls wait for plausible SNTP time, use ESP-IDF's certificate
bundle with hostname verification, and cap response bodies at 16 KiB. One cloud
worker serializes provider HTTP and token mutation. Tokens, callback values,
verifier/state, raw provider bodies, and account IDs are never returned by
`/api/status` or intentionally logged/rendered.

## Portal API

Public provisioning/status routes are `GET /`, `GET /portal.js`,
`GET /api/status`, and `GET /api/wifi/scan`. `POST /api/wifi` is
unauthenticated only while the setup AP is active; afterward it requires the
settings cookie and same-origin validation.

Authenticated provider routes are `POST /api/auth/openai/start`,
`POST /api/auth/claude/start`, `POST /api/auth/claude/code`,
`POST /api/providers/{openai|claude}/refresh`, and
`DELETE /api/providers/{openai|claude}`.

## Hardware validation still required

Verify RGB565 color/orientation, full-screen polled touch, GPIO1 brightness, QR
readability, captive DNS behavior, strict TLS failure behavior, both live
private OAuth flows, reboot persistence, refresh-token rotation, 429/stale
handling, and secret-free serial/portal traffic.
