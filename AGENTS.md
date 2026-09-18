# Repository guide

## Scope

This repository contains two related components:

- `src/quota_meter/`: Python CLI for inspecting desktop Codex and Claude quotas.
- `firmware/`: ESP32-S3 firmware for the JC3248W535EN quota display.

Keep changes focused on the component named by the task. Do not commit generated
PlatformIO output, captured provider responses, credentials, or device-specific
configuration.

## Agent setup and maintenance runbook

1. **Match the request.** Read [README.md](README.md) for user prompts and
   [firmware/README.md](firmware/README.md) for authoritative hardware, Wi-Fi and
   credential procedures. Full setup or an explicit update authorizes flashing;
   build-only never does. Continue without routine confirmations, but leave
   browser account selection, consent, login and 2FA to the user. Never erase
   flash, clear Wi-Fi or run `accounts-clear` without an explicit request.
2. **Bootstrap from the repository root.** If missing, install
   [uv](https://docs.astral.sh/uv/getting-started/installation/), then run `uv sync`
   (Python 3.11+). Install the official
   [Codex CLI](https://developers.openai.com/codex/cli/) and/or
   [Claude Code](https://code.claude.com/docs/en/overview) for requested providers.
   `uv sync` does **not** install PlatformIO or pyserial: use
   `uv tool run --from platformio==6.1.19 pio ...` for builds and
   `uv run --with pyserial python firmware/tools/<helper>.py ...` for USB helpers.
   Use the tested PlatformIO Core version above; 6.2.0 fails with the current
   ESP32 platform's SCons setup.

   Before native tests, check host `gcc` and `g++`; PlatformIO does not install
   them. Follow the OS-specific compiler prerequisites in the firmware guide.
   Its shell examples use a POSIX shell; adapt them to the user's OS and terminal.

   Desktop-only work uses `uv run quota-meter login openai|claude|all` and
   `uv run quota-meter quotas openai|claude|all` (choose one provider argument);
   it does not need firmware tools or device-owned sessions.
3. **Build and select hardware as needed.** Only the JC3248W535EN is supported.
   For build/setup/update requests, build with the command below; the artifact is
   `firmware/.pio/build/jc3248w535en/firmware.bin`. Build-only stops there. For
   account/Wi-Fi-only tasks, skip building but use the same target checks below.
   Before any device write, inspect USB ports with
   `uv tool run --from platformio==6.1.19 pio device list --json-output`. Match USB
   Serial/JTAG VID:PID **303A:1001**, as both provisioning helpers do. Select the
   sole compatible connected board automatically; never hardcode a port, select
   Bluetooth, or target a known mismatched board (the USB ID alone is not a board
   model identifier). If none is present, stop at the built artifact and ask the
   user to plug in the correct board; if multiple candidates remain, ask which
   port. Set `ESPPORT` to the discovered/selected port and use the firmware guide's
   explicit `--upload-port` command, not PlatformIO's unrestricted auto-selection.
   Preserve NVS on updates; do not erase or change partitions. Recheck ports after
   reconnecting. Close serial monitors before flashing or provisioning.
4. **Configure Wi-Fi only if needed/requested.** Preserve working saved settings.
   Use the existing ignored `.env` with
   `uv run --with pyserial python firmware/tools/provision_wifi.py --env-file .env --port "$ESPPORT"`.
   If absent, copy `.env.example` only to a new `.env` and ask the user to fill it
   locally, or guide them through the setup portal. Never overwrite an existing
   `.env`, print its contents, or ask for passwords in chat. Keep serial monitors
   closed: the console echoes secrets received by both USB helpers.
5. **Add or renew only requested accounts.** Ask for provider and local label if
   missing. The uploader requires `--provider codex|claude` (not `openai`) and
   `--account`; names are case-sensitive, 1–32 printable ASCII characters without
   edge spaces. The same provider/name replaces credentials; another name adds
   an account. Follow the firmware guide's dedicated-session commands: fresh
   `CODEX_HOME` with file auth or fresh `CLAUDE_CONFIG_DIR`, never default desktop
   credentials. Have the user confirm the intended browser account; a label is
   not identity verification. Use the exact same Claude directory string at login
   and upload. After upload, never reuse, re-upload old credentials from, or log out
   a device-owned session; renew with another fresh login and the same provider/name.
   Only the USB uploader transfers tokens; never inspect/print credentials in chat.
6. **Verify and diagnose without secrets.** An upload acknowledgement is not quota
   success. After USB helpers finish, use the serial console's `status` command
   to get the device IP and connection state. Check the screen and
   `http://<device-ip>/api/status` for the intended provider/name, auth/quota state
   and a fresh `fetched_at`. Allow normal polling:
   Codex even minutes, Claude odd minutes, longer with many accounts or backoff.
   Check connectivity and SNTP time before blaming login; respect `Retry-After`
   and cached/stale quotas on 429 or network errors. Do not immediately relogin for
   stale data. During diagnosis, suggest renewal only for a confirmed
   rejected/expired login; do not renew without a request. Report build/upload/status
   evidence separately, and state any unverified steps.

## Firmware architecture

- `firmware/src/app`: shared application state and startup.
- `firmware/src/auth`: persisted credential loading, refresh, and quota worker.
- `firmware/src/console`: USB Serial/JTAG provisioning and credential-upload
  commands.
- `firmware/src/domain`: provider-neutral state models and reducers.
- `firmware/src/net`: HTTPS and time synchronization.
- `firmware/src/portal`: Wi-Fi provisioning and read-only status portal.
- `firmware/src/providers`: provider request/response contracts.
- `firmware/src/ui`: LVGL rendering and view-model logic.

USB Serial/JTAG commands `token-begin`, `token-chunk`, and `token-commit` are the
only provider credential ingress. Do not add provider tokens to HTTP routes or
UI controls, and do not restore device-initiated authorization flows. Keep
provider-specific behavior behind the provider modules.

Never log, render, or return access tokens, refresh tokens, ID tokens, account
IDs, Wi-Fi passwords, or raw provider responses. Preserve credential rejection,
refresh-token rotation, cached quota fallback, and reboot persistence behavior.

## Development commands

Run from the repository root:

```sh
uv run pytest -q
uv run ruff check .
uv tool run --from platformio==6.1.19 pio test -d firmware -e native
uv tool run --from platformio==6.1.19 pio run -d firmware -e jc3248w535en
```

Run focused tests before the full suite. Firmware hardware upload requires a
setup/update request and the compatible-target selection in step 3; a build
request alone never authorizes flashing.

## Style

Match the surrounding code. Prefer small, direct changes and strongly typed
models. Keep C++ code compatible with the configured ESP-IDF toolchain and the
native Unity test environment. Write Python for the version declared in
`pyproject.toml` and keep Ruff clean. Add focused regression tests for behavior
changes.
