import argparse
import base64
import binascii
import hashlib
import importlib
import json
import math
import os
import shutil
import subprocess
import sys
import time
import unicodedata
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

USB_VID = 0x303A
USB_PID = 0x1001


@dataclass(frozen=True, repr=False)
class TokenSource:
    provider: str
    account: str
    access_token: str
    refresh_token: str
    id_token: str
    account_id: str
    expires_at: int


def find_device_port(list_ports: Any) -> str:
    matches = sorted(
        port.device
        for port in list_ports.comports()
        if port.vid == USB_VID and port.pid == USB_PID
    )
    if len(matches) != 1:
        raise RuntimeError(
            "Specify --port; expected exactly one USB Serial/JTAG device "
            "with VID:PID 303A:1001"
        )
    return matches[0]


def jwt_expiry(token: str) -> int:
    parts = token.split(".")
    if len(parts) < 2:
        raise ValueError("Codex access token has no JWT expiry")
    payload = parts[1] + "=" * (-len(parts[1]) % 4)
    try:
        document = json.loads(base64.urlsafe_b64decode(payload))
    except (binascii.Error, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("Codex access token has an invalid JWT expiry") from error
    expiry = document.get("exp") if isinstance(document, dict) else None
    if isinstance(expiry, bool) or not isinstance(expiry, int) or expiry <= 0:
        raise ValueError("Codex access token has no valid JWT expiry")
    return expiry


def _required_text(document: dict[str, Any], name: str, description: str) -> str:
    value = document.get(name)
    if not isinstance(value, str) or not value:
        raise ValueError(f"{description} is missing")
    return value


def _optional_text(document: dict[str, Any], name: str, description: str) -> str:
    value = document.get(name, "")
    if not isinstance(value, str):
        raise ValueError(f"{description} is invalid")
    return value


def _parse_json(text: str, description: str) -> Any:
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise ValueError(f"{description} contains invalid JSON") from error


def codex_auth_path(environment: Mapping[str, str] = os.environ) -> Path:
    home = environment.get("CODEX_HOME")
    return (Path(home).expanduser() if home else Path.home() / ".codex") / "auth.json"


def read_codex_tokens(path: Path, account: str) -> TokenSource:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(f"Codex credentials could not be read at {path}") from error
    document = _parse_json(text, "Codex credential storage")
    tokens = document.get("tokens") if isinstance(document, dict) else None
    if not isinstance(tokens, dict):
        raise ValueError("Codex credential storage has no token bundle")
    access = _required_text(tokens, "access_token", "Codex access token")
    return TokenSource(
        provider="codex",
        account=account,
        access_token=access,
        refresh_token=_required_text(tokens, "refresh_token", "Codex refresh token"),
        id_token=_required_text(tokens, "id_token", "Codex ID token"),
        account_id=_required_text(tokens, "account_id", "Codex account ID"),
        expires_at=jwt_expiry(access),
    )


def claude_credentials_text(
    environment: Mapping[str, str] = os.environ,
    platform: str = sys.platform,
) -> str:
    config_value = environment.get("CLAUDE_CONFIG_DIR", "")
    service = "Claude Code-credentials"
    if config_value:
        normalized = unicodedata.normalize("NFC", config_value).encode("utf-8")
        service += "-" + hashlib.sha256(normalized).hexdigest()[:8]
    if platform == "darwin":
        security = shutil.which("security")
        if security:
            try:
                result = subprocess.run(
                    [
                        security,
                        "find-generic-password",
                        "-s",
                        service,
                        "-w",
                    ],
                    text=True,
                    capture_output=True,
                    check=False,
                )
            except OSError:
                result = None
            if result is not None and result.returncode == 0 and result.stdout:
                return result.stdout

    config_dir = Path(config_value or "~/.claude").expanduser()
    path = config_dir / ".credentials.json"
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(f"Claude credentials could not be read at {path}") from error


def read_claude_tokens(text: str, account: str) -> TokenSource:
    document = _parse_json(text, "Claude credential storage")
    oauth = document.get("claudeAiOauth") if isinstance(document, dict) else None
    if not isinstance(oauth, dict):
        raise ValueError("Claude credential storage has no OAuth token bundle")
    expires_at = oauth.get("expiresAt")
    if (
        isinstance(expires_at, bool)
        or not isinstance(expires_at, (int, float))
        or not math.isfinite(expires_at)
        or expires_at < 0
    ):
        raise ValueError("Claude access token expiry is missing or invalid")
    return TokenSource(
        provider="claude",
        account=account,
        access_token=_optional_text(oauth, "accessToken", "Claude access token"),
        refresh_token=_required_text(oauth, "refreshToken", "Claude refresh token"),
        id_token="",
        account_id="",
        expires_at=int(expires_at / 1000),
    )


def validate_token_source(source: TokenSource) -> None:
    validate_target(source.provider, source.account)
    fields = {
        "access token": (source.access_token, 4096),
        "refresh token": (source.refresh_token, 4096),
        "ID token": (source.id_token, 4096),
        "account ID": (source.account_id, 127),
    }
    if not source.refresh_token:
        raise ValueError(f"{source.provider.capitalize()} refresh token is missing")
    if source.provider == "codex" and (
        not source.access_token or not source.id_token or not source.account_id
    ):
        raise ValueError("Codex credential bundle is incomplete")
    for name, (value, maximum) in fields.items():
        if "\0" in value or len(value.encode("utf-8")) > maximum:
            raise ValueError(f"{source.provider.capitalize()} {name} is invalid")
    if source.expires_at < 0 or source.expires_at > 0xFFFFFFFF:
        raise ValueError(f"{source.provider.capitalize()} access token expiry is invalid")


def validate_target(provider: str, account: str) -> None:
    if provider not in ("codex", "claude"):
        raise ValueError("Provider must be codex or claude")
    if (
        not isinstance(account, str)
        or not 1 <= len(account) <= 32
        or account != account.strip(" ")
        or any(not 32 <= ord(char) <= 126 for char in account)
    ):
        raise ValueError("Account name must be 1-32 printable ASCII characters without edge spaces")


def discover_tokens(
    provider: str,
    account: str,
    codex_path: Path | None = None,
    claude_path: Path | None = None,
) -> list[TokenSource]:
    validate_target(provider, account)
    if provider == "codex":
        if claude_path is not None:
            raise ValueError("--claude-auth requires --provider claude")
        source = read_codex_tokens(codex_path or codex_auth_path(), account)
    else:
        if codex_path is not None:
            raise ValueError("--codex-auth requires --provider codex")
        claude_text = (
            claude_path.read_text(encoding="utf-8")
            if claude_path is not None
            else claude_credentials_text()
        )
        source = read_claude_tokens(claude_text, account)
    validate_token_source(source)
    return [source]


def token_begin_command(source: TokenSource) -> bytearray:
    validate_target(source.provider, source.account)
    name = base64.urlsafe_b64encode(source.account.encode("ascii")).rstrip(b"=")
    return bytearray(b"token-begin " + source.provider.encode("ascii") + b" " + name + b"\r\n")


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
    raw[:] = b"\0" * len(raw)
    return commands


def _read_until(connection: Any, markers: tuple[bytes, ...], timeout: float) -> bytes:
    pending = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pending.extend(connection.read(connection.in_waiting or 1))
        for marker in markers:
            if marker in pending:
                pending[:] = b"\0" * len(pending)
                return marker
    pending[:] = b"\0" * len(pending)
    raise TimeoutError("The device did not acknowledge the token upload")


def _send_command(connection: Any, command: bytearray, expected: bytes) -> None:
    connection.write(command)
    connection.flush()
    command[:] = b"\0" * len(command)
    marker = _read_until(connection, (expected, b"ERROR:"), 10)
    if marker != expected:
        raise RuntimeError("The device rejected the token upload")


def send_tokens(port: str, sources: list[TokenSource]) -> None:
    for source in sources:
        validate_token_source(source)
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
                token_begin_command(source),
                b"OK: token upload started.",
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
        description="Upload local Codex and Claude credentials over USB"
    )
    parser.add_argument("--port")
    parser.add_argument(
        "--provider", choices=("codex", "claude"), required=True
    )
    parser.add_argument("--account", required=True)
    parser.add_argument("--codex-auth", type=Path)
    parser.add_argument("--claude-auth", type=Path)
    args = parser.parse_args()

    try:
        sources = discover_tokens(
            provider=args.provider,
            codex_path=args.codex_auth,
            claude_path=args.claude_auth,
            account=args.account,
        )
        list_ports = importlib.import_module("serial.tools.list_ports")
        send_tokens(args.port or find_device_port(list_ports), sources)
    except (ImportError, OSError, RuntimeError, TimeoutError, ValueError) as error:
        sys.stderr.write(f"Token upload failed: {error}\n")
        return 1
    providers = " and ".join(f"{source.provider} - {source.account}" for source in sources)
    sys.stdout.write(f"{providers} credentials uploaded; Quota Meter is restarting.\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
