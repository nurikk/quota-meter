# Quota Meter ESP32 firmware

Standalone ESP-IDF 5.5/PlatformIO firmware for the JC3248W535EN
(ESP32-S3 N16R8): AXS15231B 480×320 landscape UI/touch (320×480 native
panel), 16 MB QIO flash, 8 MB OPI PSRAM, and GPIO1 backlight.

> **Prototype warning:** OpenAI and Claude subscriber quota endpoints are
> private, unsupported APIs and can change without notice. This development
> build stores provider credentials in plaintext NVS. A production device needs
> flash encryption, NVS encryption, secure boot, protected debug/download
> modes, and a manufacturing key lifecycle.

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

## Wi-Fi provisioning

On first boot the screen shows the device-specific `QuotaMeter-XXXXXX` WPA
network, its persisted random eight-digit password, and a Wi-Fi QR. Join it and
open `http://192.168.4.1`. Scan or enter a network and save. The device uses
APSTA while connecting, then keeps its settings server reachable on the station
IP. Disconnects retry with a bounded 1–60 second backoff; if saved credentials
remain unusable for 30 seconds, the setup AP returns.

Wi-Fi can also be provisioned through the USB Serial/JTAG console at 115200
baud. To avoid placing the password in shell history, copy `.env.example` to the
ignored `.env`, set `WIFI_SSID` and `WIFI_PASSWORD`, then run:

```sh
.venv/bin/python firmware/tools/provision_wifi.py --port "$ESPPORT"
```

The helper requires `pyserial` (installed with PlatformIO in the development
environment), auto-detects a single device with USB VID:PID `303A:1001` when
`--port` is omitted, and never prints the password. The interactive REPL itself
echoes typed input, so treat a live serial session as sensitive.

The screen shows the authenticated settings URL as
`http://<device-ip>/?p=<setup-password>`. Visiting it sets an
`HttpOnly; SameSite=Strict` settings cookie. Wi-Fi changes require that cookie
and a same-origin request once provisioning is complete. The portal has no
provider credential mutation routes.

To reset Wi-Fi only, use **Clear credentials & restart** on the Wi-Fi screen. A
full flash erase removes Wi-Fi, the setup password, and provider bundles.

## Provider credential upload

The USB Serial/JTAG console is the only provider credential ingress. With the
device connected, run:

```sh
.venv/bin/python firmware/tools/upload_tokens.py --port "$ESPPORT"
```

`--port` is optional when exactly one USB device with VID:PID `303A:1001` is
connected. The script reads:

1. Codex credentials from `$CODEX_HOME/auth.json`, defaulting to
   `~/.codex/auth.json`.
2. Claude credentials from macOS Keychain service `Claude Code-credentials`,
   falling back to `${CLAUDE_CONFIG_DIR:-~/.claude}/.credentials.json`.

Use `--provider codex` or `--provider claude` to upload only one provider.
`--codex-auth` and `--claude-auth` override the corresponding credential file
locations.

It validates all selected bundles before opening the serial port, uses bounded
base64url chunks, uploads Codex before Claude when both are selected, persists
each bundle, and restarts the
device. It never prints credentials. Do not run a serial monitor concurrently,
because the ESP console echoes received bytes.

At boot the firmware loads persisted bundles and cached quotas. Automatic quota
reads run once per minute. Access tokens are refreshed when needed; rotated
tokens are persisted before use. Rejected or expired credentials remain visible
as expired on the dashboard and must be replaced by rerunning the upload script.
A provider `Retry-After` longer than one minute is honored.

All upstream calls wait for plausible SNTP time, use ESP-IDF's certificate
bundle with hostname verification, and cap response bodies at 16 KiB. Tokens,
raw provider bodies, and account IDs are never returned by `/api/status` or
intentionally logged or rendered.

## Portal API

The portal only supports provisioning and status: `GET /`, `GET /portal.js`,
`GET /api/status`, `GET /api/wifi/scan`, and `POST /api/wifi`. The Wi-Fi write is
unauthenticated only while the setup AP is active; afterward it requires the
settings cookie and same-origin validation.

## Hardware validation still required

Verify RGB565 color/orientation, full-screen polled touch, GPIO1 brightness, QR
readability, captive DNS behavior, strict TLS failure behavior, reboot
persistence, refresh-token rotation, 429/stale handling, and secret-free serial
and portal traffic.
