import argparse
import base64
import importlib
import json
import math
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from quota_meter.claude import _credential_text


def find_esp32s3_port(list_ports: Any) -> str:
    matches = [
        port.device
        for port in list_ports.comports()
        if port.vid == 0x303A and port.pid == 0x1001
    ]
    if len(matches) != 1:
        raise RuntimeError(
            "Specify --port; expected one ESP32-S3 USB Serial/JTAG device"
        )
    return matches[0]


@dataclass(frozen=True)
class TokenSource:
    provider: str
    access_token: str
    refresh_token: str
    id_token: str
    account_id: str
    expires_at: int


def jwt_expiry(token: str) -> int:
    parts = token.split(".")
    if len(parts) < 2:
        raise ValueError("OAuth access token has no JWT expiry")
    payload = parts[1] + "=" * (-len(parts[1]) % 4)
    try:
        document = json.loads(base64.urlsafe_b64decode(payload))
    except (ValueError, json.JSONDecodeError) as error:
        raise ValueError("OAuth access token has an invalid JWT expiry") from error
    expiry = document.get("exp") if isinstance(document, dict) else None
    if not isinstance(expiry, int) or expiry <= 0:
        raise ValueError("OAuth access token has no valid JWT expiry")
    return expiry


def _required_text(document: dict[str, Any], name: str, description: str) -> str:
    value = document.get(name)
    if not isinstance(value, str) or not value:
        raise ValueError(f"{description} is missing")
    return value


def _parse_json(text: str, description: str) -> Any:
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise ValueError(f"{description} contains invalid JSON") from error


def read_codex_tokens(path: Path) -> TokenSource:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError("Codex auth storage could not be read") from error
    document = _parse_json(text, "Codex auth storage")
    tokens = document.get("tokens") if isinstance(document, dict) else None
    if not isinstance(tokens, dict):
        raise ValueError("Codex auth storage has no token bundle")
    access = _required_text(tokens, "access_token", "Codex access token")
    return TokenSource(
        provider="codex",
        access_token=access,
        refresh_token=_required_text(tokens, "refresh_token", "Codex refresh token"),
        id_token=_required_text(tokens, "id_token", "Codex ID token"),
        account_id=_required_text(tokens, "account_id", "Codex account ID"),
        expires_at=jwt_expiry(access),
    )


def read_claude_tokens(text: str) -> TokenSource:
    document = _parse_json(text, "Claude auth storage")
    oauth = document.get("claudeAiOauth") if isinstance(document, dict) else None
    if not isinstance(oauth, dict):
        raise ValueError("Claude auth storage has no OAuth token bundle")
    expires_at = oauth.get("expiresAt")
    if (
        isinstance(expires_at, bool)
        or not isinstance(expires_at, (int, float))
        or not math.isfinite(expires_at)
        or expires_at <= 0
    ):
        raise ValueError("Claude access token expiry is missing")
    try:
        expires_at_seconds = int(expires_at / 1000)
    except (OverflowError, ValueError) as error:
        raise ValueError("Claude access token expiry is invalid") from error
    return TokenSource(
        provider="claude",
        access_token=_required_text(oauth, "accessToken", "Claude access token"),
        refresh_token=_required_text(oauth, "refreshToken", "Claude refresh token"),
        id_token="",
        account_id="",
        expires_at=expires_at_seconds,
    )


def field_commands(field: str, value: str, chunk_size: int = 120) -> list[bytearray]:
    raw = bytearray(value.encode("utf-8"))
    commands: list[bytearray] = []
    for offset in range(0, len(raw), chunk_size):
        encoded = base64.urlsafe_b64encode(raw[offset : offset + chunk_size]).rstrip(
            b"="
        )
        commands.append(
            bytearray(
                b"token-chunk "
                + field.encode()
                + b" "
                + str(offset).encode()
                + b" "
                + encoded
                + b"\r\n"
            )
        )
        encoded = bytearray(encoded)
        encoded[:] = b"\0" * len(encoded)
    raw[:] = b"\0" * len(raw)
    return commands


def _read_until(connection: Any, markers: tuple[bytes, ...], timeout: float) -> bytes:
    pending = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pending.extend(connection.read(connection.in_waiting or 1))
        for marker in markers:
            if marker in pending:
                result = marker
                pending[:] = b"\0" * len(pending)
                return result
    pending[:] = b"\0" * len(pending)
    raise TimeoutError("The device did not acknowledge the token import")


def _send_command(connection: Any, command: bytearray, expected: bytes) -> None:
    connection.write(command)
    connection.flush()
    command[:] = b"\0" * len(command)
    marker = _read_until(connection, (expected, b"ERROR:"), 10)
    if marker != expected:
        raise RuntimeError("The device rejected the token import")


def send_tokens(port: str, sources: list[TokenSource]) -> None:
    serial = importlib.import_module("serial")
    with serial.Serial(
        port=port, baudrate=115200, timeout=0.2, write_timeout=2
    ) as connection:
        connection.write(b"\r\n")
        connection.flush()
        _read_until(connection, (b"quota-meter>",), 10)
        for source in sources:
            _send_command(
                connection,
                bytearray(f"token-begin {source.provider}\r\n".encode()),
                b"OK: token import started.",
            )
            fields = (
                ("access", source.access_token),
                ("refresh", source.refresh_token),
                ("id", source.id_token),
                ("account", source.account_id),
            )
            for field, value in fields:
                for command in field_commands(field, value):
                    _send_command(connection, command, b"OK: token chunk accepted.")
            _send_command(
                connection,
                bytearray(f"token-commit {source.expires_at}\r\n".encode()),
                b"OK: token bundle saved.",
            )
        _send_command(connection, bytearray(b"restart\r\n"), b"OK: restarting.")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Import desktop OAuth tokens into Quota Meter over USB"
    )
    parser.add_argument("--port")
    parser.add_argument("--provider", choices=("all", "codex", "claude"), default="all")
    parser.add_argument(
        "--codex-auth",
        type=Path,
        default=Path(os.environ.get("CODEX_HOME", "~/.codex")).expanduser()
        / "auth.json",
    )
    args = parser.parse_args()

    try:
        sources: list[TokenSource] = []
        if args.provider in {"all", "codex"}:
            sources.append(read_codex_tokens(args.codex_auth))
        if args.provider in {"all", "claude"}:
            sources.append(read_claude_tokens(_credential_text()))
        list_ports = importlib.import_module("serial.tools.list_ports")
        send_tokens(args.port or find_esp32s3_port(list_ports), sources)
    except (
        ImportError,
        OSError,
        RuntimeError,
        TimeoutError,
        ValueError,
        json.JSONDecodeError,
    ) as error:
        sys.stderr.write(f"Token import failed: {error}\n")
        return 1
    sys.stdout.write("OAuth tokens imported; Quota Meter is restarting.\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
