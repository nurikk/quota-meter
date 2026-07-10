import errno
import json
import os
import re
import select
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from decimal import Decimal
from pathlib import Path
from typing import Any, BinaryIO, TextIO

from quota_meter.errors import QuotaMeterError
from quota_meter.qr import print_qr

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
OAUTH_BETA = "oauth-2025-04-20"
_AUTH_URL = re.compile(
    rb"https://[^\s\x1b]+/(?:oauth/authorize|cai/oauth/authorize)[^\s\x1b]*"
)


def _find_auth_url(data: bytes) -> str | None:
    match = _AUTH_URL.search(data)
    if match is None:
        return None
    return match.group().decode("utf-8", errors="replace").rstrip(".,;)")


def _write_terminal(output: TextIO, data: bytes) -> None:
    buffer = getattr(output, "buffer", None)
    if buffer is not None:
        buffer.write(data)
        buffer.flush()
    else:
        output.write(data.decode("utf-8", errors="replace"))
        output.flush()


def login(output: TextIO = sys.stdout, input_stream: BinaryIO | None = None) -> None:
    if os.name != "posix":
        raise QuotaMeterError(
            "Claude QR-assisted OAuth PKCE/manual-code login requires a Unix PTY "
            "and is unsupported on this platform. Run `claude auth login --claudeai` directly."
        )

    import fcntl
    import pty
    import signal
    import termios
    import tty

    executable = shutil.which("claude")
    if executable is None:
        raise QuotaMeterError(
            "Claude support requires the official `claude` CLI on PATH."
        )
    terminal_input: BinaryIO = (
        input_stream if input_stream is not None else sys.stdin.buffer
    )
    input_fd = terminal_input.fileno()
    pid, master_fd = pty.fork()
    if pid == 0:
        try:
            os.execv(executable, [executable, "auth", "login", "--claudeai"])
        except OSError:
            os._exit(127)

    print(
        "Starting QR-assisted OAuth PKCE/manual-code login "
        "(this is not device authorization).",
        file=output,
        flush=True,
    )
    saved_terminal: list[Any] | None = None
    saved_winch: Any = None
    input_open = True
    recent = b""
    qr_printed = False
    child_waited = False

    def resize_child(*_args: object) -> None:
        if not os.isatty(input_fd):
            return
        try:
            size = fcntl.ioctl(input_fd, termios.TIOCGWINSZ, b"\0" * 8)
            fcntl.ioctl(master_fd, termios.TIOCSWINSZ, size)
        except OSError:
            pass

    try:
        if os.isatty(input_fd):
            saved_terminal = termios.tcgetattr(input_fd)
            tty.setraw(input_fd)
            resize_child()
            saved_winch = signal.getsignal(signal.SIGWINCH)
            signal.signal(signal.SIGWINCH, resize_child)

        while True:
            readers = [master_fd]
            if input_open:
                readers.append(input_fd)
            readable, _, _ = select.select(readers, [], [])
            if input_open and input_fd in readable:
                data = os.read(input_fd, 4096)
                if data:
                    os.write(master_fd, data)
                else:
                    input_open = False
            if master_fd not in readable:
                continue
            try:
                data = os.read(master_fd, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            _write_terminal(output, data)
            recent = (recent + data)[-32768:]
            if not qr_printed:
                url = _find_auth_url(recent)
                if url:
                    print("\nScan this authorization URL:", file=output)
                    print_qr(url, output)
                    qr_printed = True

        _, status = os.waitpid(pid, 0)
        child_waited = True
    except KeyboardInterrupt:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        os.waitpid(pid, 0)
        child_waited = True
        raise QuotaMeterError("Claude login cancelled.") from None
    finally:
        if saved_winch is not None:
            signal.signal(signal.SIGWINCH, saved_winch)
        if saved_terminal is not None:
            termios.tcsetattr(input_fd, termios.TCSAFLUSH, saved_terminal)
        os.close(master_fd)
        if not child_waited:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            os.waitpid(pid, 0)

    return_code = os.waitstatus_to_exitcode(status)
    if return_code != 0:
        raise QuotaMeterError(f"Claude login failed (exit {return_code}).")
    print("Claude login complete.", file=output)


def _credential_text(platform: str = sys.platform) -> str:
    if platform == "darwin":
        executable = shutil.which("security")
        if executable is None:
            raise QuotaMeterError("macOS Keychain command `security` was not found.")
        result = subprocess.run(
            [
                executable,
                "find-generic-password",
                "-s",
                "Claude Code-credentials",
                "-w",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise QuotaMeterError(
                "Claude OAuth credentials were not found in macOS Keychain; "
                "run `quota-meter login claude`."
            )
        return result.stdout
    config_dir = Path(os.environ.get("CLAUDE_CONFIG_DIR", "~/.claude")).expanduser()
    path = config_dir / ".credentials.json"
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError as error:
        raise QuotaMeterError(
            f"Claude OAuth credentials were not found at {path}; "
            "run `quota-meter login claude`."
        ) from error
    except OSError as error:
        raise QuotaMeterError(
            f"Could not read Claude credentials at {path}: {error}"
        ) from error


def parse_credentials(text: str, now_ms: int | None = None) -> str:
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        raise QuotaMeterError(
            "Claude credential storage contains invalid JSON."
        ) from error
    oauth = document.get("claudeAiOauth") if isinstance(document, dict) else None
    if not isinstance(oauth, dict):
        raise QuotaMeterError(
            "Claude credential storage has no `claudeAiOauth` login; "
            "run `quota-meter login claude`."
        )
    access_token = oauth.get("accessToken")
    if not isinstance(access_token, str) or not access_token:
        raise QuotaMeterError(
            "Claude OAuth credentials have no access token; run `quota-meter login claude`."
        )
    expires_at = oauth.get("expiresAt")
    if not isinstance(expires_at, (int, float)):
        raise QuotaMeterError(
            "Claude OAuth credentials have no valid expiry; run `quota-meter login claude`."
        )
    if now_ms is None:
        try:
            now_ms = int(time.time() * 1000)
        except (OSError, OverflowError) as error:
            raise QuotaMeterError("Could not determine the current time.") from error
    if expires_at <= now_ms:
        raise QuotaMeterError(
            "Claude OAuth credentials are expired; run `quota-meter login claude` to re-login."
        )
    return access_token


def read_quotas(opener: Any = urllib.request.urlopen) -> dict[str, Any]:
    token = parse_credentials(_credential_text())
    request = urllib.request.Request(
        USAGE_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": OAUTH_BETA,
            "Accept": "application/json",
            "User-Agent": "quota-meter/0.1.0",
        },
    )
    try:
        with opener(request, timeout=15) as response:
            data = json.load(response)
    except urllib.error.HTTPError as error:
        guidance = {
            401: "Claude OAuth credential is invalid or expired; re-login.",
            403: "Claude rejected this account or OAuth scope; re-login with --claudeai.",
            429: "Claude usage endpoint is rate limited; try again later.",
        }.get(error.code, f"Claude usage endpoint returned HTTP {error.code}.")
        raise QuotaMeterError(guidance) from error
    except urllib.error.URLError as error:
        raise QuotaMeterError(
            f"Could not reach Claude usage endpoint: {error.reason}"
        ) from error
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise QuotaMeterError("Claude usage endpoint returned invalid JSON.") from error
    if not isinstance(data, dict):
        raise QuotaMeterError("Claude usage endpoint returned an invalid payload.")
    return data


def _money_text(value: Any) -> str | None:
    if not isinstance(value, dict):
        return None
    amount_minor = value.get("amount_minor")
    exponent = value.get("exponent")
    currency = value.get("currency")
    if (
        not isinstance(amount_minor, (int, float))
        or isinstance(amount_minor, bool)
        or not isinstance(exponent, int)
        or isinstance(exponent, bool)
        or exponent < 0
        or exponent > 8
        or not isinstance(currency, str)
        or not currency
    ):
        return None
    amount = Decimal(str(amount_minor)) / (Decimal(10) ** exponent)
    return f"{amount:.{exponent}f} {currency.upper()}"


def _limit_label(limit: dict[str, Any], index: int) -> str:
    group = limit.get("group")
    kind = limit.get("kind")
    base = limit.get("name") or limit.get("id") or group or kind or f"limit {index}"
    parts = [str(base).replace("_", " ")]
    if group and kind and kind != group and base != kind:
        parts.append(str(kind).replace("_", " "))
    scope = limit.get("scope")
    model = scope.get("model") if isinstance(scope, dict) else None
    if isinstance(model, dict):
        model_name = model.get("display_name") or model.get("name") or model.get("id")
        if model_name:
            parts.append(str(model_name))
    return " / ".join(parts)


def _window_line(label: str, window: dict[str, Any], indent: str = "  ") -> str:
    utilization = window.get("percent", window.get("utilization"))
    utilization_text = "unknown" if utilization is None else f"{utilization}% used"
    reset = window.get("resets_at")
    reset_text = f"; resets {reset}" if reset else ""
    return f"{indent}{label}: {utilization_text}{reset_text}"


def render_quotas(payload: dict[str, Any]) -> str:
    lines = ["Claude"]
    rendered: set[str] = set()
    known = (("five_hour", "5 hour"), ("seven_day", "7 day"))
    for key, label in known:
        window = payload.get(key)
        if isinstance(window, dict):
            lines.append(_window_line(label, window))
            rendered.add(key)
    for key, value in payload.items():
        if (
            key in rendered
            or key in {"extra_usage", "limits", "spend"}
            or not isinstance(value, dict)
        ):
            continue
        if "utilization" in value or "percent" in value or "resets_at" in value:
            lines.append(_window_line(key.replace("_", " "), value))
        else:
            for model, window in value.items():
                if isinstance(window, dict) and (
                    "utilization" in window
                    or "percent" in window
                    or "resets_at" in window
                ):
                    lines.append(
                        _window_line(
                            f"{key.replace('_', ' ')} / {str(model).replace('_', ' ')}",
                            window,
                        )
                    )
    limits = payload.get("limits")
    if isinstance(limits, list):
        for index, limit in enumerate(limits, start=1):
            if isinstance(limit, dict):
                lines.append(_window_line(_limit_label(limit, index), limit))
    extra = payload.get("extra_usage")
    if isinstance(extra, dict):
        enabled = "enabled" if extra.get("is_enabled") else "disabled"
        details = []
        if extra.get("utilization") is not None:
            details.append(f"{extra['utilization']}% used")
        spend = payload.get("spend")
        if not isinstance(spend, dict):
            exponent = extra.get("decimal_places")
            currency = extra.get("currency")
            used = _money_text(
                {
                    "amount_minor": extra.get("used_credits"),
                    "exponent": exponent,
                    "currency": currency,
                }
            )
            limit = _money_text(
                {
                    "amount_minor": extra.get("monthly_limit"),
                    "exponent": exponent,
                    "currency": currency,
                }
            )
            if used and limit:
                details.append(f"{used} / {limit}")
        suffix = f" ({', '.join(details)})" if details else ""
        lines.append(f"  extra usage: {enabled}{suffix}")
    spend = payload.get("spend")
    if isinstance(spend, dict):
        used = _money_text(spend.get("used"))
        limit = _money_text(spend.get("limit"))
        details = []
        if used and limit:
            details.append(f"{used} / {limit}")
        if spend.get("percent") is not None:
            details.append(f"{spend['percent']}% used")
        if details:
            lines.append(f"  spend: {', '.join(details)}")
    if len(lines) == 1:
        lines.append("  No quota windows returned.")
    return "\n".join(lines)
