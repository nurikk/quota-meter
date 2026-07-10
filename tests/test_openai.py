import json
import threading

import pytest

from quota_meter.errors import QuotaMeterError
from quota_meter.openai import AppServer, render_quotas


class InputLines:
    def __init__(self, lines: list[str]):
        self.lines = iter(lines)

    def readline(self) -> str:
        return next(self.lines, "")


class BlockingLines:
    def __init__(self):
        self.release = threading.Event()

    def readline(self) -> str:
        self.release.wait()
        return ""


class OutputLines:
    def __init__(self):
        self.lines: list[str] = []

    def write(self, line: str) -> None:
        self.lines.append(line)

    def flush(self) -> None:
        pass


class Process:
    def __init__(self, responses: list[dict[str, object]]):
        self.stdin = OutputLines()
        self.stdout: InputLines | BlockingLines = InputLines(
            [json.dumps(item) + "\n" for item in responses]
        )

    def poll(self) -> None:
        return None


def test_app_server_writes_ndjson_request_and_notification() -> None:
    process = Process([{"id": 1, "result": {"ok": True}}])
    server = AppServer(process)  # type: ignore[arg-type]

    assert server.request("initialize", {"clientInfo": {"name": "test"}}) == {
        "ok": True
    }
    server.notify("initialized", {})

    assert process.stdin.lines == [
        '{"id":1,"method":"initialize","params":{"clientInfo":{"name":"test"}}}\n',
        '{"method":"initialized","params":{}}\n',
    ]


def test_app_server_reports_protocol_error() -> None:
    process = Process([{"id": 1, "error": {"message": "not logged in"}}])
    server = AppServer(process)  # type: ignore[arg-type]

    with pytest.raises(QuotaMeterError, match="not logged in"):
        server.request("account/read", {"refreshToken": False})


def test_app_server_preserves_interleaved_notification() -> None:
    notification = {
        "method": "account/login/completed",
        "params": {"loginId": "login-1", "success": True},
    }
    process = Process([notification, {"id": 1, "result": {"loginId": "login-1"}}])
    server = AppServer(process)  # type: ignore[arg-type]

    assert server.request("account/login/start", {"type": "chatgptDeviceCode"}) == {
        "loginId": "login-1"
    }
    assert server.receive() == notification


def test_app_server_receive_times_out() -> None:
    process = Process([])
    blocking = BlockingLines()
    process.stdout = blocking
    server = AppServer(process)  # type: ignore[arg-type]

    try:
        with pytest.raises(QuotaMeterError, match="timed out"):
            server.receive(timeout=0.01)
    finally:
        blocking.release.set()


def test_openai_quota_renderer_handles_multiple_buckets() -> None:
    payload = {
        "account": {"account": {"planType": "plus", "email": "a@example.com"}},
        "rateLimits": {
            "rateLimits": {},
            "rateLimitsByLimitId": {
                "codex": {
                    "limitName": "Codex",
                    "primary": {
                        "usedPercent": 25,
                        "windowDurationMins": 300,
                        "resetsAt": 1_900_000_000,
                    },
                    "credits": {"hasCredits": True, "unlimited": False, "balance": "5"},
                }
            },
        },
    }

    rendered = render_quotas(payload)

    assert "plus — a@example.com" in rendered
    assert "25% used, 300 min window" in rendered
    assert "credits: 5" in rendered
