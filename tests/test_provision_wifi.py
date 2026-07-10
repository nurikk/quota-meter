from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

SCRIPT = Path(__file__).parents[1] / "firmware" / "tools" / "provision_wifi.py"
SPEC = spec_from_file_location("provision_wifi", SCRIPT)
assert SPEC and SPEC.loader
provision_wifi = module_from_spec(SPEC)
SPEC.loader.exec_module(provision_wifi)


def test_read_credentials(tmp_path: Path) -> None:
    env_file = tmp_path / ".env"
    env_file.write_text('WIFI_SSID="Office Wi-Fi"\nWIFI_PASSWORD=p#a ss\\word\n')

    expected = ("Office Wi-Fi", "p#a ss\\word")
    assert provision_wifi.read_credentials(env_file) == expected


def test_console_quote_escapes_repl_metacharacters() -> None:
    assert provision_wifi.console_quote('name "one"\\two') == (
        '"name \\"one\\"\\\\two"'
    )
