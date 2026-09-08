import base64
import json
import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from types import SimpleNamespace

import pytest

SCRIPT = Path(__file__).parents[1] / "firmware" / "tools" / "upload_tokens.py"
SPEC = spec_from_file_location("upload_tokens", SCRIPT)
assert SPEC and SPEC.loader
upload_tokens = module_from_spec(SPEC)
sys.modules[SPEC.name] = upload_tokens
SPEC.loader.exec_module(upload_tokens)


def jwt(payload: dict[str, object]) -> str:
    encoded = base64.urlsafe_b64encode(json.dumps(payload).encode()).rstrip(b"=")
    return f"header.{encoded.decode()}.signature"


def test_reads_sanitized_desktop_token_shapes(tmp_path: Path) -> None:
    codex_path = tmp_path / "auth.json"
    codex_path.write_text(
        json.dumps(
            {
                "tokens": {
                    "access_token": jwt({"exp": 2_000_000_000}),
                    "refresh_token": "codex-refresh",
                    "id_token": "codex-id",
                    "account_id": "account-1",
                }
            }
        )
    )
    codex = upload_tokens.read_codex_tokens(codex_path)
    assert codex.provider == "codex"
    assert codex.expires_at == 2_000_000_000
    assert codex.account_id == "account-1"

    claude = upload_tokens.read_claude_tokens(
        json.dumps(
            {
                "claudeAiOauth": {
                    "accessToken": "claude-access",
                    "refreshToken": "claude-refresh",
                    "expiresAt": 2_000_000_000_000,
                }
            }
        )
    )
    assert claude.provider == "claude"
    assert claude.expires_at == 2_000_000_000
    assert claude.id_token == ""

    stale_claude = upload_tokens.read_claude_tokens(
        json.dumps(
            {
                "claudeAiOauth": {
                    "accessToken": "",
                    "refreshToken": "claude-refresh",
                    "expiresAt": 0,
                }
            }
        )
    )
    assert stale_claude.access_token == ""
    assert stale_claude.expires_at == 0


def test_field_commands_are_bounded_and_round_trip() -> None:
    value = "token-value-" * 40
    commands = upload_tokens.field_commands("access", value)
    reconstructed = bytearray()
    for expected_offset, command in zip(
        range(0, len(value), 120), commands, strict=True
    ):
        assert len(command) < 256
        _, field, offset, encoded = command.rstrip().split(b" ")
        assert field == b"access"
        assert offset == str(expected_offset).encode()
        reconstructed.extend(
            base64.urlsafe_b64decode(encoded + b"=" * (-len(encoded) % 4))
        )
    assert reconstructed.decode() == value


def test_device_discovery_requires_one_matching_usb_device() -> None:
    list_ports = SimpleNamespace(
        comports=lambda: [
            SimpleNamespace(device="ignored", vid=0x239A, pid=0x8125),
            SimpleNamespace(device="target", vid=0x303A, pid=0x1001),
        ]
    )
    assert upload_tokens.find_device_port(list_ports) == "target"

    list_ports.comports = list
    with pytest.raises(RuntimeError, match="exactly one"):
        upload_tokens.find_device_port(list_ports)

    list_ports.comports = lambda: [
        SimpleNamespace(device="a", vid=0x303A, pid=0x1001),
        SimpleNamespace(device="b", vid=0x303A, pid=0x1001),
    ]
    with pytest.raises(RuntimeError, match="exactly one"):
        upload_tokens.find_device_port(list_ports)


def test_codex_path_honors_codex_home(tmp_path: Path) -> None:
    assert upload_tokens.codex_auth_path({"CODEX_HOME": str(tmp_path)}) == (
        tmp_path / "auth.json"
    )


def test_claude_credentials_fall_back_to_config_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    credentials = tmp_path / ".credentials.json"
    credentials.write_text("sanitized")
    monkeypatch.setattr(upload_tokens.shutil, "which", lambda _name: None)
    assert (
        upload_tokens.claude_credentials_text(
            {"CLAUDE_CONFIG_DIR": str(tmp_path)}, platform="darwin"
        )
        == "sanitized"
    )


def test_discovery_order_is_codex_then_claude(monkeypatch: pytest.MonkeyPatch) -> None:
    codex = upload_tokens.TokenSource("codex", "a", "r", "i", "account", 1)
    claude = upload_tokens.TokenSource("claude", "a", "r", "", "", 1)
    monkeypatch.setattr(upload_tokens, "codex_auth_path", lambda: Path("auth.json"))
    monkeypatch.setattr(upload_tokens, "read_codex_tokens", lambda _path: codex)
    monkeypatch.setattr(upload_tokens, "claude_credentials_text", lambda: "{}")
    monkeypatch.setattr(upload_tokens, "read_claude_tokens", lambda _text: claude)
    assert [source.provider for source in upload_tokens.discover_tokens()] == [
        "codex",
        "claude",
    ]


def test_discovery_honors_provider_and_path_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    codex_path = tmp_path / "codex.json"
    claude_path = tmp_path / "claude.json"
    claude_path.write_text("claude")
    codex = upload_tokens.TokenSource("codex", "a", "r", "i", "account", 2)
    claude = upload_tokens.TokenSource("claude", "a", "r", "", "", 2)
    monkeypatch.setattr(upload_tokens, "read_codex_tokens", lambda path: codex if path == codex_path else None)
    monkeypatch.setattr(upload_tokens, "read_claude_tokens", lambda text: claude if text == "claude" else None)

    assert upload_tokens.discover_tokens("codex", codex_path) == [codex]
    assert upload_tokens.discover_tokens("claude", claude_path=claude_path) == [claude]


def test_discovery_accepts_stale_tokens_for_device_refresh(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    stale = upload_tokens.TokenSource("codex", "a", "r", "i", "account", 10)
    monkeypatch.setattr(upload_tokens, "read_codex_tokens", lambda _path: stale)
    assert upload_tokens.discover_tokens("codex", Path("auth.json")) == [stale]


def test_discovery_rejects_oversized_tokens(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    oversized = upload_tokens.TokenSource("codex", "a" * 4097, "r", "i", "account", 20)
    monkeypatch.setattr(upload_tokens, "read_codex_tokens", lambda _path: oversized)
    with pytest.raises(ValueError, match="access token is invalid"):
        upload_tokens.discover_tokens("codex", Path("auth.json"))


def test_token_source_repr_does_not_expose_credentials() -> None:
    source = upload_tokens.TokenSource(
        "codex", "secret-access", "secret-refresh", "secret-id", "account", 1
    )
    assert "secret" not in repr(source)


def test_send_tokens_uploads_in_order_and_restarts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class Connection:
        def __init__(self) -> None:
            self.responses = bytearray()
            self.commands: list[bytes] = []

        def __enter__(self) -> "Connection":
            return self

        def __exit__(
            self,
            exception_type: type[BaseException] | None,
            exception: BaseException | None,
            traceback: object | None,
        ) -> None:
            return None

        @property
        def in_waiting(self) -> int:
            return len(self.responses)

        def write(self, command: bytes | bytearray) -> None:
            written = bytes(command)
            self.commands.append(written)
            if written == b"\r\n":
                self.responses.extend(b"quota-meter>")
            elif written.startswith(b"token-begin"):
                self.responses.extend(b"OK: token upload started.")
            elif written.startswith(b"token-chunk"):
                self.responses.extend(b"OK: token chunk accepted.")
            elif written.startswith(b"token-commit"):
                self.responses.extend(b"OK: token bundle saved.")
            elif written == b"restart\r\n":
                self.responses.extend(b"OK: restarting.")

        def flush(self) -> None:
            return None

        def read(self, size: int) -> bytes:
            result = bytes(self.responses[:size])
            del self.responses[:size]
            return result

    connection = Connection()
    serial = SimpleNamespace(Serial=lambda **_kwargs: connection)
    monkeypatch.setattr(upload_tokens.importlib, "import_module", lambda _name: serial)
    sources = [
        upload_tokens.TokenSource("codex", "access", "refresh", "id", "account", 1),
        upload_tokens.TokenSource("claude", "access", "refresh", "", "", 2),
    ]

    upload_tokens.send_tokens("target", sources)

    begins = [
        command for command in connection.commands if command.startswith(b"token-begin")
    ]
    assert begins == [b"token-begin codex\r\n", b"token-begin claude\r\n"]
    assert connection.commands[-1] == b"restart\r\n"
