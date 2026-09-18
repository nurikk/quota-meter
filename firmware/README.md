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

For agent-led setup, start with the [copy-paste prompts](../README.md#let-a-coding-agent-set-it-up)
and [agent runbook](../AGENTS.md#agent-setup-and-maintenance-runbook).
Install [uv](https://docs.astral.sh/uv/getting-started/installation/). Native tests
also require host `gcc` and `g++` on `PATH`; PlatformIO does not install them:

- macOS: install Xcode Command Line Tools with `xcode-select --install`.
- Debian/Ubuntu: install `build-essential` with your package manager.
- Windows: follow [PlatformIO's native compiler setup](https://docs.platformio.org/en/latest/platforms/native.html#installation)
  for MSYS2 and its compiler `PATH`.

The shell examples use a POSIX shell. Adapt them to your terminal on Windows.
Then run from the repository root (`uv sync` alone does not install PlatformIO
or pyserial):

```sh
uv sync
uv tool run --from platformio==6.1.19 pio test -d firmware -e native
uv tool run --from platformio==6.1.19 pio run -d firmware -e jc3248w535en
```

These commands pin the tested PlatformIO Core 6.1.19. Core 6.2.0 currently fails
with this ESP32 platform's SCons setup (`SCons.Tool.FortranCommon` is missing).


Build-only stops at `firmware/.pio/build/jc3248w535en/firmware.bin`. For requested
setup/updates, list ports with
`uv tool run --from platformio==6.1.19 pio device list --json-output` and set `ESPPORT`
to the sole compatible JC3248W535EN USB Serial/JTAG port (VID:PID `303A:1001`).
Never hardcode a port or select Bluetooth/known mismatched boards; this USB ID is
shared by other ESP32 boards. Ask which port only if ambiguous. With no compatible
board connected, stop at the build and ask the user to plug in the correct board.
Then flash without erasing Wi-Fi or accounts:

```sh
uv tool run --from platformio==6.1.19 pio run -d firmware -e jc3248w535en -t upload --upload-port "$ESPPORT"
```

Optional diagnostics, only when neither provisioning helper is running:

```sh
uv tool run --from platformio==6.1.19 pio device monitor -d firmware --port "$ESPPORT" --baud 115200
```

Close the monitor before flashing or provisioning: the console echoes secret
input. Never erase flash or clear accounts unless explicitly requested. The
checked-in partition table uses one factory application, enlarged NVS, and a
reserved `nvs_keys` partition; it deliberately has no OTA slots.

## Wi-Fi provisioning

On first boot the screen shows the device-specific `QuotaMeter-XXXXXX` WPA
network, its persisted random eight-digit password, and a Wi-Fi QR. Join it and
open `http://192.168.4.1`. Scan or enter a network and save. The device uses
APSTA while connecting, then keeps its settings server reachable on the station
IP. Disconnects retry with a bounded 1–60 second backoff; if saved credentials
remain unusable for 30 seconds, the setup AP returns.

Wi-Fi can also be provisioned through the USB Serial/JTAG console at 115200
baud. Use the existing ignored `.env`; if absent, copy `.env.example` to a new
`.env` and have the user set `WIFI_SSID` and `WIFI_PASSWORD` locally. Never overwrite
an existing `.env`, print it, or ask for passwords in chat. Alternatively use the
setup portal above. Close any serial monitor, then run:

```sh
uv run --with pyserial python firmware/tools/provision_wifi.py --env-file .env --port "$ESPPORT"
```

`uv run --with pyserial` supplies the helper's serial dependency without adding it
to the project. The helper auto-detects a single device with USB VID:PID `303A:1001`
when `--port` is omitted and never prints the password. The interactive REPL itself
echoes typed input, so treat a live serial session as sensitive.

The screen shows the authenticated settings URL as
`http://<device-ip>/?p=<setup-password>`. Visiting it sets an
`HttpOnly; SameSite=Strict` settings cookie. Wi-Fi changes require that cookie
and a same-origin request once provisioning is complete. The portal has no
provider credential mutation routes.

To reset Wi-Fi only, use **Clear credentials & restart** on the Wi-Fi screen. A
full flash erase removes Wi-Fi, the setup password, and provider bundles.

## Provider credential upload

The USB Serial/JTAG console is the only provider credential ingress. Both Codex
and Claude support independently named accounts, each on its own swipe page.
There is no fixed account count: add one, ten, or more as NVS/PSRAM capacity permits.
Storage failures reject the upload explicitly without publishing a new account.

`--provider codex|claude` and `--account 'Work Team'` are required. Names are
case-sensitive, 1–32 printable ASCII characters, with internal spaces allowed
but no leading/trailing spaces or control characters. Unicode names are not
supported by the display font. Long display names scroll within their label.
Reusing the exact provider and name replaces that account's credentials while
preserving its opaque internal identity; a different name creates another account.
The same name on different providers identifies independent accounts. Names do
not verify the upstream login identity. There is no rename/remove interface.

Metadata and tokens are persisted together in one NVS blob, addressed by generated
internal IDs, never raw names or provider account IDs. Cached quotas are tied to
an upload generation: refresh-token rotation retains the cache, but uploading a
replacement login invalidates old cached usage. Generation checks prevent an
in-flight worker from overwriting newly uploaded credentials or cached quotas.
Uploads take effect after the uploader's restart.

Deployed `qm_openai` and `qm_claude` bundles and valid cached quotas are imported
without erasing their original data, each using the generic name `Default` under
its provider. Upload to that exact provider/name to replace an imported account.
Startup storage errors fail explicitly instead of erasing NVS automatically.

### Dedicated Codex device sessions

Create fresh, separate logins for the device, not copies of desktop `auth.json`.
[Codex authentication](https://developers.openai.com/codex/auth) supports
`CODEX_HOME` and `cli_auth_credentials_store="file"`. Start with fresh directories
and have the user confirm the intended account in each browser login, completing
consent and any 2FA themselves. With the official Codex CLI installed, run in one
shell (replace `Personal` with the desired local label):

```sh
mkdir -p "$HOME/.codex-meter"
CODEX_SESSION=$(mktemp -d "$HOME/.codex-meter/session.XXXXXX")
CODEX_HOME="$CODEX_SESSION" codex -c 'cli_auth_credentials_store="file"' login
uv run --with pyserial python firmware/tools/upload_tokens.py --port "$ESPPORT" \
  --provider codex --account Personal --codex-auth "$CODEX_SESSION/auth.json"
```

These commands leave the existing desktop session in `~/.codex` alone.
After handing credentials to the device, do not run Codex or `codex logout` in
these device-owned directories, or upload their old token files again. The device
owns refresh-token rotation; sharing a login with a desktop client can cause
rotation conflicts. Codex 0.154.0 revokes the existing session on logout and before
browser/device login in the same `CODEX_HOME`. To replace expired credentials, create another fresh,
dedicated directory/login and upload to the same provider/name. The account name is a
local label, not verification of which account you logged into.

### Dedicated Claude device sessions

Use a fresh `CLAUDE_CONFIG_DIR` for each independently logged-in device account;
never share a rotating session between independent clients. With the official
Claude Code CLI installed, run in one shell; have the user confirm the intended
browser account and complete consent and any 2FA:

```sh
mkdir -p "$HOME/.claude-meter"
CLAUDE_SESSION=$(mktemp -d "$HOME/.claude-meter/session.XXXXXX")
CLAUDE_CONFIG_DIR="$CLAUDE_SESSION" claude auth login --claudeai
CLAUDE_CONFIG_DIR="$CLAUDE_SESSION" \
  uv run --with pyserial python firmware/tools/upload_tokens.py --port "$ESPPORT" \
  --provider claude --account 'Research Team'
```

Use the exact same `CLAUDE_CONFIG_DIR` string for login and upload. On macOS,
Claude stores custom-directory credentials under `Claude Code-credentials-` plus
the first eight hex characters of SHA-256 of that raw directory string after NFC
normalization (not its expanded/resolved path). The uploader selects only that
service; it never falls back to the desktop's bare Keychain service. File fallback
is the selected directory's `.credentials.json`. `--claude-auth PATH` instead
reads that explicit file and bypasses Keychain. Leave device-owned sessions alone
after upload; use a fresh directory/login when replacing credentials.

`--port` is optional when exactly one USB device with VID:PID `303A:1001` is
connected. Without explicit file overrides, the script reads:

1. Codex credentials from `$CODEX_HOME/auth.json`, defaulting to
   `~/.codex/auth.json`.
2. Claude credentials from the directory-specific macOS Keychain service above
   (bare `Claude Code-credentials` only when `CLAUDE_CONFIG_DIR` is unset/empty),
   falling back to `${CLAUDE_CONFIG_DIR:-~/.claude}/.credentials.json`.

`--codex-auth` and `--claude-auth` override file locations for the selected
provider. Mismatched provider/file flags are rejected before reading credentials.
For hardware setup, always use the dedicated-session workflows above for both
providers, never the default desktop credential locations.

The serial protocol starts with `token-begin <codex|claude> <base64url-name>`;
for example, `token-begin codex V29yayBUZWFt` targets `Work Team`. The unpadded
base64url encoding avoids command injection through names. `token-chunk` and
`token-commit` follow; metadata alone never creates a persisted account.

Only on an explicit request, the USB command `accounts-clear` deletes all provider
credentials and cached quotas, including legacy imports, then restarts. Wi-Fi and
setup settings are preserved.
This is logical NVS deletion, not a forensic secure erase of flash.

The script validates the target and bundle before opening the serial port, uses
bounded base64url chunks, persists the bundle, and restarts the device. It never
prints credentials. Do not run a serial monitor concurrently, because the ESP
console echoes received bytes.

At boot the firmware loads each account's persisted bundle and cached quota.
Codex accounts refresh in even minutes and Claude in odd minutes (each account
normally every two minutes), with independent retry deadlines. Every five seconds,
the worker selects the oldest due account eligible for that minute and fetches it.
Slow requests cannot let earlier accounts monopolize their provider's turn;
refresh intervals grow with account count and network latency. Access tokens are refreshed
when needed; rotated tokens are persisted before use. Rejected or
expired credentials remain visible on their own page and must be replaced by
rerunning the upload script. Each account independently honors `Retry-After` and
retains cached quotas on failures.

After the USB helpers finish, open the serial monitor above and enter `status`
to discover the device IP, Wi-Fi state, and per-account status. Close the monitor
again before any later credential upload.


After upload, verify the intended provider/name on the display and at
`http://<device-ip>/api/status`: check auth/quota state and a fresh `fetched_at`
after the provider's polling turn. Upload success alone is not proof of working
quotas. If stale, check Wi-Fi and SNTP time, then allow polling/backoff; 429 or a
network failure is not a reason to immediately log in again. Report pending or
blocked verification instead of claiming success.

All upstream calls wait for plausible SNTP time, use ESP-IDF's certificate
bundle with hostname verification, and cap response bodies at 16 KiB. Tokens,
raw provider bodies, and account IDs are never returned by `/api/status` or
intentionally logged or rendered.

## Portal API

The portal only supports provisioning and status: `GET /`, `GET /portal.js`,
`GET /api/status`, `GET /api/wifi/scan`, and `POST /api/wifi`. The Wi-Fi write is
unauthenticated only while the setup AP is active; afterward it requires the
settings cookie and same-origin validation. `/api/status` reports
an `accounts` collection with `provider`, `name`, `auth`, `quota`, `plan`,
`window_count`, and `fetched_at` descriptors; it has no token input routes.

## Hardware validation still required

Verify RGB565 color/orientation, full-screen polled touch, GPIO1 brightness, QR
readability, captive DNS behavior, strict TLS failure behavior, reboot
persistence, refresh-token rotation, 429/stale handling, and secret-free serial
and portal traffic.
