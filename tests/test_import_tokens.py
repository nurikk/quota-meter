import base64
import json
import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

SCRIPT = Path(__file__).parents[1] / "firmware" / "tools" / "import_tokens.py"
SPEC = spec_from_file_location("import_tokens", SCRIPT)
assert SPEC and SPEC.loader
import_tokens = module_from_spec(SPEC)
sys.modules[SPEC.name] = import_tokens
SPEC.loader.exec_module(import_tokens)


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
    codex = import_tokens.read_codex_tokens(codex_path)
    assert codex.provider == "codex"
    assert codex.expires_at == 2_000_000_000
    assert codex.account_id == "account-1"

    claude = import_tokens.read_claude_tokens(
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


def test_field_commands_are_bounded_and_round_trip() -> None:
    value = "token-value-" * 40
    commands = import_tokens.field_commands("access", value)
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
