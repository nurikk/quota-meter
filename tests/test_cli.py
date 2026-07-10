import io

from quota_meter import cli
from quota_meter.errors import QuotaMeterError


def test_cli_returns_error_when_codex_is_missing(monkeypatch, capsys) -> None:
    monkeypatch.setattr("quota_meter.openai.shutil.which", lambda _name: None)

    result = cli.main(["quotas", "openai"])

    assert result == 1
    assert "requires the `codex` CLI" in capsys.readouterr().err


def test_cli_reports_provider_error_without_network(monkeypatch, capsys) -> None:
    def fail() -> None:
        raise QuotaMeterError("credentials unavailable")

    monkeypatch.setattr("quota_meter.claude.read_quotas", fail)

    result = cli.main(["quotas", "claude"])

    assert result == 1
    assert "credentials unavailable" in capsys.readouterr().err


def test_cli_json_preserves_full_provider_payload(monkeypatch, capsys) -> None:
    payload = {"five_hour": None, "unknown_future_field": {"value": 1}}
    monkeypatch.setattr("quota_meter.claude.read_quotas", lambda: payload)

    result = cli.main(["quotas", "claude", "--json"])

    assert result == 0
    assert '"unknown_future_field"' in capsys.readouterr().out


def test_run_all_human_output(monkeypatch) -> None:
    monkeypatch.setattr("quota_meter.openai.read_quotas", lambda: {})
    monkeypatch.setattr("quota_meter.claude.read_quotas", lambda: {})
    output = io.StringIO()
    args = cli.build_parser().parse_args(["quotas", "all"])

    cli.run(args, output)

    assert "OpenAI Codex" in output.getvalue()
    assert "Claude" in output.getvalue()
