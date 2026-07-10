import json
import queue
import shutil
import subprocess
import threading
import time
from collections import deque
from datetime import datetime
from typing import Any, TextIO

from quota_meter.errors import QuotaMeterError
from quota_meter.qr import print_qr


class AppServer:
    REQUEST_TIMEOUT = 30.0

    def __init__(self, process: subprocess.Popen[str]):
        self.process = process
        self.next_id = 1
        self.inbox: deque[dict[str, Any]] = deque()
        self.lines: queue.Queue[str | OSError | None] = queue.Queue()
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()

    @classmethod
    def start(cls) -> "AppServer":
        executable = shutil.which("codex")
        if executable is None:
            raise QuotaMeterError("OpenAI support requires the `codex` CLI on PATH.")
        try:
            process = subprocess.Popen(
                [executable, "app-server"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                bufsize=1,
            )
        except OSError as error:
            raise QuotaMeterError(
                f"Could not start Codex app-server: {error}"
            ) from error
        server = cls(process)
        initialized = False
        try:
            server.request(
                "initialize",
                {"clientInfo": {"name": "quota-meter", "version": "0.1.0"}},
            )
            server.notify("initialized", {})
            initialized = True
            return server
        finally:
            if not initialized:
                server.close()

    def _read_stdout(self) -> None:
        if self.process.stdout is None:
            self.lines.put(OSError("stdout is unavailable"))
            self.lines.put(None)
            return
        try:
            while line := self.process.stdout.readline():
                self.lines.put(line)
        except OSError as error:
            self.lines.put(error)
        finally:
            self.lines.put(None)

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
        deadline = time.monotonic() + 2
        while self.process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()

    def __enter__(self) -> "AppServer":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def send(self, message: dict[str, Any]) -> None:
        if self.process.stdin is None:
            raise QuotaMeterError("Codex app-server stdin is unavailable.")
        try:
            self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise QuotaMeterError("Codex app-server stopped unexpectedly.") from error

    def _receive_next(self, timeout: float) -> dict[str, Any]:
        try:
            line = self.lines.get(timeout=max(timeout, 0.0))
        except queue.Empty as error:
            raise QuotaMeterError("Codex app-server timed out.") from error
        if isinstance(line, OSError):
            raise QuotaMeterError(f"Could not read Codex app-server output: {line}")
        if line is None:
            raise QuotaMeterError(
                f"Codex app-server stopped unexpectedly (exit {self.process.poll()})."
            )
        try:
            message = json.loads(line)
        except json.JSONDecodeError as error:
            raise QuotaMeterError("Codex app-server returned invalid JSON.") from error
        if not isinstance(message, dict):
            raise QuotaMeterError("Codex app-server returned an invalid message.")
        return message

    def receive(self, timeout: float = REQUEST_TIMEOUT) -> dict[str, Any]:
        if self.inbox:
            return self.inbox.popleft()
        return self._receive_next(timeout)

    def notify(self, method: str, params: dict[str, Any]) -> None:
        self.send({"method": method, "params": params})

    def request(
        self,
        method: str,
        params: dict[str, Any],
        timeout: float = REQUEST_TIMEOUT,
    ) -> Any:
        request_id = self.next_id
        self.next_id += 1
        self.send({"id": request_id, "method": method, "params": params})
        deadline = time.monotonic() + timeout
        while True:
            message = self._receive_next(deadline - time.monotonic())
            if message.get("id") != request_id:
                self.inbox.append(message)
                continue
            if "error" in message:
                error = message["error"]
                detail = error.get("message") if isinstance(error, dict) else str(error)
                raise QuotaMeterError(f"Codex {method} failed: {detail}")
            return message.get("result")


def login(output: TextIO) -> None:
    with AppServer.start() as server:
        result = server.request("account/login/start", {"type": "chatgptDeviceCode"})
        if not isinstance(result, dict):
            raise QuotaMeterError("Codex returned an invalid login response.")
        required = ("loginId", "verificationUrl", "userCode")
        if any(not isinstance(result.get(field), str) for field in required):
            raise QuotaMeterError("Codex returned an incomplete device login response.")
        print_qr(result["verificationUrl"], output)
        print(f"Open: {result['verificationUrl']}", file=output)
        print(f"User code: {result['userCode']}", file=output)
        print("Waiting for Codex login to complete…", file=output, flush=True)
        deadline = time.monotonic() + 15 * 60
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise QuotaMeterError("Codex device login timed out after 15 minutes.")
            message = server.receive(timeout=remaining)
            if message.get("method") != "account/login/completed":
                continue
            params = message.get("params")
            if (
                not isinstance(params, dict)
                or params.get("loginId") != result["loginId"]
            ):
                continue
            if not params.get("success"):
                raise QuotaMeterError(
                    f"Codex login failed: {params.get('error') or 'unknown error'}"
                )
            print("OpenAI login complete.", file=output)
            return


def read_quotas() -> dict[str, Any]:
    with AppServer.start() as server:
        account = server.request("account/read", {"refreshToken": False})
        rate_limits = server.request("account/rateLimits/read", {})
    return {"account": account, "rateLimits": rate_limits}


def _format_reset(value: Any) -> str:
    if not isinstance(value, (int, float)):
        return "unknown"
    return datetime.fromtimestamp(value).astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")


def render_quotas(payload: dict[str, Any]) -> str:
    lines = ["OpenAI Codex"]
    account_result = payload.get("account")
    account = (
        account_result.get("account") if isinstance(account_result, dict) else None
    )
    if isinstance(account, dict):
        details = [str(account.get("planType", "unknown"))]
        if account.get("email"):
            details.append(str(account["email"]))
        lines.append(f"  Account: {' — '.join(details)}")
    limits_result = payload.get("rateLimits")
    if not isinstance(limits_result, dict):
        return "\n".join(lines + ["  No rate-limit data returned."])
    buckets = limits_result.get("rateLimitsByLimitId")
    if not isinstance(buckets, dict) or not buckets:
        single = limits_result.get("rateLimits")
        buckets = {"codex": single} if isinstance(single, dict) else {}
    for bucket_id, snapshot in buckets.items():
        if not isinstance(snapshot, dict):
            continue
        name = snapshot.get("limitName") or bucket_id
        lines.append(f"  {name}:")
        for window_name in ("primary", "secondary"):
            window = snapshot.get(window_name)
            if not isinstance(window, dict):
                continue
            duration = window.get("windowDurationMins")
            duration_text = f", {duration} min window" if duration is not None else ""
            lines.append(
                f"    {window_name}: {window.get('usedPercent', '?')}% used"
                f"{duration_text}; resets {_format_reset(window.get('resetsAt'))}"
            )
        credits = snapshot.get("credits")
        if isinstance(credits, dict) and credits.get("hasCredits"):
            balance = (
                "unlimited" if credits.get("unlimited") else credits.get("balance")
            )
            lines.append(f"    credits: {balance}")
    if len(lines) == 1:
        lines.append("  No rate-limit data returned.")
    return "\n".join(lines)
