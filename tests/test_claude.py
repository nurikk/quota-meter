import io
import json
import os

import pytest

from quota_meter.claude import (
    _find_auth_url,
    login,
    parse_credentials,
    read_quotas,
    render_quotas,
)
from quota_meter.errors import QuotaMeterError


def test_parse_credentials_returns_unexpired_access_token() -> None:
    text = json.dumps({"claudeAiOauth": {"accessToken": "secret", "expiresAt": 2_000}})

    assert parse_credentials(text, now_ms=1_000) == "secret"


def test_parse_credentials_rejects_expired_token_without_exposing_it() -> None:
    text = json.dumps(
        {"claudeAiOauth": {"accessToken": "do-not-print", "expiresAt": 999}}
    )

    with pytest.raises(QuotaMeterError, match="expired") as caught:
        parse_credentials(text, now_ms=1_000)

    assert "do-not-print" not in str(caught.value)


def test_parse_credentials_rejects_wrong_shape() -> None:
    with pytest.raises(QuotaMeterError, match="claudeAiOauth"):
        parse_credentials("{}", now_ms=1_000)


def test_find_auth_url_in_terminal_output() -> None:
    output = (
        b"Open \x1b[4mhttps://claude.ai/oauth/authorize?code=true&state=abc\x1b[0m now"
    )

    assert _find_auth_url(output) == (
        "https://claude.ai/oauth/authorize?code=true&state=abc"
    )


def test_claude_login_fails_cleanly_without_posix_pty(monkeypatch) -> None:
    monkeypatch.setattr("quota_meter.claude.os.name", "nt")

    with pytest.raises(QuotaMeterError, match="unsupported on this platform"):
        login(io.StringIO())


@pytest.mark.skipif(os.name != "posix", reason="requires POSIX pseudo-terminals")
def test_claude_login_relays_input_and_drains_output(monkeypatch, tmp_path) -> None:
    executable = tmp_path / "fake-claude"
    executable.write_text(
        "#!/usr/bin/env python3\n"
        "import os\n"
        "print('https://claude.ai/oauth/authorize?code=true&state=test', flush=True)\n"
        "os.read(0, 2)\n"
        "print('final-output', flush=True)\n",
        encoding="utf-8",
    )
    executable.chmod(0o755)
    monkeypatch.setattr(
        "quota_meter.claude.shutil.which", lambda _name: str(executable)
    )
    input_fd, writer_fd = os.pipe()
    os.write(writer_fd, b"x\n")
    os.close(writer_fd)
    output = io.StringIO()
    try:
        with os.fdopen(input_fd, "rb", buffering=0, closefd=False) as input_stream:
            login(output, input_stream)
    finally:
        os.close(input_fd)

    assert "final-output" in output.getvalue()
    assert "Scan this authorization URL" in output.getvalue()


@pytest.mark.skipif(os.name != "posix", reason="requires POSIX pseudo-terminals")
def test_claude_login_restores_parent_terminal(monkeypatch, tmp_path) -> None:
    import pty
    import termios

    executable = tmp_path / "fake-claude"
    executable.write_text(
        "#!/usr/bin/env python3\nprint('done', flush=True)\n",
        encoding="utf-8",
    )
    executable.chmod(0o755)
    monkeypatch.setattr(
        "quota_meter.claude.shutil.which", lambda _name: str(executable)
    )
    input_master, input_slave = pty.openpty()
    original_terminal = termios.tcgetattr(input_slave)
    try:
        with os.fdopen(input_slave, "rb", buffering=0, closefd=False) as input_stream:
            login(io.StringIO(), input_stream)
        assert termios.tcgetattr(input_slave) == original_terminal
    finally:
        os.close(input_master)
        os.close(input_slave)


def test_read_quotas_parses_json_and_sends_oauth_headers(monkeypatch) -> None:
    class Response(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *_args: object) -> None:
            pass

    captured = {}

    def opener(request, timeout):
        captured["request"] = request
        captured["timeout"] = timeout
        return Response(b'{"five_hour":{"utilization":10}}')

    credentials = json.dumps(
        {"claudeAiOauth": {"accessToken": "secret", "expiresAt": 9_999_999_999_999}}
    )
    monkeypatch.setattr("quota_meter.claude._credential_text", lambda: credentials)

    result = read_quotas(opener)

    assert result["five_hour"]["utilization"] == 10
    assert captured["request"].get_header("Authorization") == "Bearer secret"
    assert captured["request"].get_header("Anthropic-beta") == "oauth-2025-04-20"
    assert captured["timeout"] == 15


def test_claude_quota_renderer_handles_current_limits_and_spend() -> None:
    payload = {
        "five_hour": {"utilization": 12.5, "resets_at": "2026-07-10T12:00:00Z"},
        "seven_day_opus": {"utilization": 20, "resets_at": None},
        "models": {"sonnet": {"utilization": 30, "resets_at": "later"}},
        "limits": [
            {
                "kind": "weekly_limit",
                "group": "claude_code",
                "percent": 40,
                "resets_at": "soon",
                "scope": {"model": {"display_name": "Sonnet"}},
            }
        ],
        "extra_usage": {
            "is_enabled": True,
            "monthly_limit": 10_000,
            "used_credits": 300,
            "utilization": 3,
            "currency": "usd",
            "decimal_places": 2,
        },
        "spend": {
            "percent": 3,
            "used": {"amount_minor": 300, "currency": "usd", "exponent": 2},
            "limit": {"amount_minor": 10_000, "currency": "usd", "exponent": 2},
        },
    }

    rendered = render_quotas(payload)

    assert "5 hour: 12.5% used" in rendered
    assert "seven day opus: 20% used" in rendered
    assert "models / sonnet: 30% used" in rendered
    assert "claude code / weekly limit / Sonnet: 40% used" in rendered
    assert "extra usage: enabled (3% used)" in rendered
    assert "spend: 3.00 USD / 100.00 USD, 3% used" in rendered
