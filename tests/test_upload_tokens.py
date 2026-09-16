import base64
import hashlib
import json
import subprocess
import sys
import unicodedata
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
    codex = upload_tokens.read_codex_tokens(codex_path, "Work Team")
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
        ), "Research"
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
        ), "Research"
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


@pytest.mark.parametrize("config_dir", [None, "", "~/device-work", "/device/Café", "/device/Cafe\u0301"])
def test_claude_keychain_service_uses_exact_selected_directory(
    monkeypatch: pytest.MonkeyPatch, config_dir: str | None,
) -> None:
    commands: list[list[str]] = []

    def security_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        commands.append(command)
        return subprocess.CompletedProcess(command, 0, stdout="synthetic-credentials")

    monkeypatch.setattr(upload_tokens.shutil, "which", lambda _name: "/usr/bin/security")
    monkeypatch.setattr(upload_tokens.subprocess, "run", security_run)
    environment = {} if config_dir is None else {"CLAUDE_CONFIG_DIR": config_dir}
    assert upload_tokens.claude_credentials_text(environment, platform="darwin") == "synthetic-credentials"
    expected = "Claude Code-credentials"
    if config_dir:
        expected += "-" + hashlib.sha256(unicodedata.normalize("NFC", config_dir).encode()).hexdigest()[:8]
    assert commands == [["/usr/bin/security", "find-generic-password", "-s", expected, "-w"]]


def test_custom_claude_keychain_failure_only_uses_selected_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    commands: list[list[str]] = []
    (tmp_path / ".credentials.json").write_text("selected-account")

    def security_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        commands.append(command)
        return subprocess.CompletedProcess(command, 1, stdout="")

    monkeypatch.setattr(upload_tokens.shutil, "which", lambda _name: "/usr/bin/security")
    monkeypatch.setattr(upload_tokens.subprocess, "run", security_run)
    assert upload_tokens.claude_credentials_text({"CLAUDE_CONFIG_DIR": str(tmp_path)}, "darwin") == "selected-account"
    assert len(commands) == 1
    assert commands[0][3].startswith("Claude Code-credentials-")


@pytest.fixture
def credential_files(tmp_path: Path) -> tuple[Path, Path]:
    codex_path = tmp_path / "codex.json"
    codex_path.write_text(json.dumps({"tokens": {
        "access_token": jwt({"exp": 2_000_000_000}),
        "refresh_token": "synthetic-refresh", "id_token": "synthetic-id",
        "account_id": "synthetic-provider-id",
    }}))
    claude_path = tmp_path / "claude.json"
    claude_path.write_text(json.dumps({"claudeAiOauth": {
        "accessToken": "synthetic-access", "refreshToken": "synthetic-refresh",
        "expiresAt": 0,
    }}))
    return codex_path, claude_path


@pytest.mark.parametrize("provider", ["codex", "claude"])
@pytest.mark.parametrize("account", ["Work Team", "Research", "Bluefish", "Personal", "x" * 32, "quoted '\"; name"])
def test_discovery_honors_provider_name_and_explicit_file(
    credential_files: tuple[Path, Path], provider: str, account: str,
) -> None:
    codex_path, claude_path = credential_files
    sources = upload_tokens.discover_tokens(
        provider, account,
        codex_path=codex_path if provider == "codex" else None,
        claude_path=claude_path if provider == "claude" else None,
    )
    assert len(sources) == 1
    assert sources[0].provider == provider
    assert sources[0].account == account
    command = upload_tokens.token_begin_command(sources[0])
    verb, encoded_provider, encoded_name = command.rstrip().split(b" ")
    assert verb == b"token-begin"
    assert encoded_provider.decode() == provider
    assert base64.urlsafe_b64decode(encoded_name + b"=" * (-len(encoded_name) % 4)).decode() == account
    assert command.count(b"\n") == 1
    assert len(command) < 256


@pytest.mark.parametrize("provider", ["codex", "claude"])
@pytest.mark.parametrize("account", [None, "", " ", " Work", "Work ", "x" * 33, "A\nrestart", "A\rB", "A\0B", "A\tB", "A\x7fB", "Café"])
def test_invalid_account_selection_does_not_read_credentials(
    monkeypatch: pytest.MonkeyPatch, provider: str, account: str | None,
) -> None:
    def unexpected_read(*_args: object) -> None:
        pytest.fail("Invalid selection must not read credentials")

    monkeypatch.setattr(upload_tokens, "read_codex_tokens", unexpected_read)
    monkeypatch.setattr(upload_tokens, "claude_credentials_text", unexpected_read)
    with pytest.raises(ValueError, match="Account name"):
        upload_tokens.discover_tokens(provider=provider, account=account)


@pytest.mark.parametrize("provider", ["all", "unknown", ""])
def test_invalid_provider_selection(provider: str) -> None:
    with pytest.raises(ValueError, match="Provider"):
        upload_tokens.discover_tokens(provider, "Work")


@pytest.mark.parametrize("provider", ["codex", "claude"])
def test_mismatched_file_rejected_before_reading(provider: str) -> None:
    with pytest.raises(ValueError, match="requires --provider"):
        upload_tokens.discover_tokens(
            provider, "Work", codex_path=Path("missing") if provider == "claude" else None,
            claude_path=Path("missing") if provider == "codex" else None,
        )


def test_discovery_rejects_oversized_tokens(credential_files: tuple[Path, Path]) -> None:
    codex_path, _ = credential_files
    data = json.loads(codex_path.read_text())
    data["tokens"]["access_token"] = jwt({"exp": 20, "padding": "x" * 4097})
    codex_path.write_text(json.dumps(data))
    with pytest.raises(ValueError, match="access token is invalid"):
        upload_tokens.discover_tokens("codex", "Work", codex_path)


def test_token_source_repr_does_not_expose_credentials() -> None:
    source = upload_tokens.TokenSource(
        "codex", "Work Team", "secret-access", "secret-refresh", "secret-id", "account", 1
    )
    assert "secret" not in repr(source)


@pytest.mark.parametrize("account_count", [1, 10, 11])
@pytest.mark.parametrize("providers", [("codex",), ("claude",), ("codex", "claude")])
def test_send_tokens_uploads_independent_accounts_and_restarts(
    monkeypatch: pytest.MonkeyPatch, account_count: int, providers: tuple[str, ...],
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
        upload_tokens.TokenSource(provider, f"Account {index}", "access", "refresh", "id", "account", 1)
        for provider in providers for index in range(account_count)
    ]

    upload_tokens.send_tokens("target", sources)

    begins = [
        command for command in connection.commands if command.startswith(b"token-begin")
    ]
    expected = [bytes(upload_tokens.token_begin_command(source)) for source in sources]
    assert begins == expected
    assert connection.commands[-1] == b"restart\r\n"


@pytest.mark.parametrize("invalid_bundle", [False, True])
def test_main_rejects_invalid_upload_before_loading_serial(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, invalid_bundle: bool,
) -> None:
    path = tmp_path / "auth.json"
    path.write_text("{}")
    args = ["upload_tokens.py", "--provider", "codex" if invalid_bundle else "claude",
            "--account", "Work", "--codex-auth", str(path)]
    monkeypatch.setattr(sys, "argv", args)

    def unexpected_import(_name: str) -> None:
        pytest.fail("Invalid upload must not load serial")

    monkeypatch.setattr(upload_tokens.importlib, "import_module", unexpected_import)
    assert upload_tokens.main() == 1


def test_send_tokens_validates_every_bundle_before_opening_serial(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    sources = [
        upload_tokens.TokenSource("codex", "Work Team", "a", "r", "i", "account", 1),
        upload_tokens.TokenSource("codex", "Other", "a", "", "i", "account", 1),
    ]

    def unexpected_import(_name: str) -> None:
        pytest.fail("Invalid upload must not load serial")

    monkeypatch.setattr(upload_tokens.importlib, "import_module", unexpected_import)
    with pytest.raises(ValueError, match="refresh token is missing"):
        upload_tokens.send_tokens("target", sources)
