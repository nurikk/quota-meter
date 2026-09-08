# Repository guide

## Scope

This repository contains two related components:

- `src/quota_meter/`: Python CLI for inspecting desktop Codex and Claude quotas.
- `firmware/`: ESP32-S3 firmware for the JC3248W535EN quota display.

Keep changes focused on the component named by the task. Do not commit generated
PlatformIO output, captured provider responses, credentials, or device-specific
configuration.

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
pio test -d firmware -e native
pio run -d firmware -e jc3248w535en
```

Run focused tests before the full suite. Firmware hardware upload is explicit
and must use a caller-provided port; never assume a connected board is the
correct target.

## Style

Match the surrounding code. Prefer small, direct changes and strongly typed
models. Keep C++ code compatible with the configured ESP-IDF toolchain and the
native Unity test environment. Write Python for the version declared in
`pyproject.toml` and keep Ruff clean. Add focused regression tests for behavior
changes.
