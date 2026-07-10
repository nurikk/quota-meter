import argparse
import importlib
import sys
import time
from pathlib import Path
from typing import Any


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


def read_credentials(path: Path) -> tuple[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        values[key.strip()] = value
    ssid = values.get("WIFI_SSID", "")
    password = values.get("WIFI_PASSWORD", "")
    if not ssid or len(ssid.encode("utf-8")) > 32:
        raise ValueError("WIFI_SSID must contain 1-32 UTF-8 bytes")
    if len(password.encode("utf-8")) > 64:
        raise ValueError("WIFI_PASSWORD must contain at most 64 UTF-8 bytes")
    return ssid, password


def console_quote(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def send_credentials(port: str, ssid: str, password: str) -> None:
    serial = importlib.import_module("serial")
    with serial.Serial(
        port=port, baudrate=115200, timeout=0.2, write_timeout=2
    ) as connection:
        connection.write(b"\r\n")
        connection.flush()
        deadline = time.monotonic() + 10
        pending = bytearray()
        while time.monotonic() < deadline and b"quota-meter>" not in pending:
            pending.extend(connection.read(connection.in_waiting or 1))
        pending.clear()
        command = bytearray(
            f"connect {console_quote(ssid)} {console_quote(password)}\r\n".encode()
        )
        connection.write(command)
        connection.flush()
        command[:] = b"\0" * len(command)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            pending.extend(connection.read(connection.in_waiting or 1))
            if b"OK: Wi-Fi credentials saved" in pending:
                pending.clear()
                return
            if b"ERROR:" in pending:
                pending.clear()
                raise RuntimeError("The device rejected the Wi-Fi command")
        pending.clear()
        raise TimeoutError("The device did not acknowledge the Wi-Fi command")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Provision Quota Meter Wi-Fi over USB serial"
    )
    parser.add_argument(
        "--env-file", type=Path, default=Path(__file__).resolve().parents[2] / ".env"
    )
    parser.add_argument("--port")
    args = parser.parse_args()
    try:
        ssid, password = read_credentials(args.env_file)
        list_ports = importlib.import_module("serial.tools.list_ports")
        port = args.port or find_esp32s3_port(list_ports)
        send_credentials(port, ssid, password)
    except (ImportError, OSError, RuntimeError, TimeoutError, ValueError) as error:
        sys.stderr.write(f"Provisioning failed: {error}\n")
        return 1
    sys.stdout.write("Wi-Fi credentials sent successfully.\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
