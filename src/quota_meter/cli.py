import argparse
import json
import sys
from typing import Any, Sequence, TextIO

from quota_meter import claude, openai
from quota_meter.errors import QuotaMeterError

PROVIDERS = ("openai", "claude")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="quota-meter")
    subparsers = parser.add_subparsers(dest="command", required=True)
    login_parser = subparsers.add_parser("login", help="log in through an official CLI")
    login_parser.add_argument("provider", choices=(*PROVIDERS, "all"))
    quotas_parser = subparsers.add_parser("quotas", help="show subscription quotas")
    quotas_parser.add_argument("provider", choices=(*PROVIDERS, "all"))
    quotas_parser.add_argument("--json", action="store_true", dest="as_json")
    return parser


def _selected(provider: str) -> tuple[str, ...]:
    return PROVIDERS if provider == "all" else (provider,)


def run(args: argparse.Namespace, output: TextIO | None = None) -> None:
    destination: TextIO = output if output is not None else sys.stdout
    if args.command == "login":
        for provider in _selected(args.provider):
            if provider == "openai":
                openai.login(destination)
            else:
                claude.login(destination)
        return
    payloads: dict[str, Any] = {}
    for provider in _selected(args.provider):
        payloads[provider] = (
            openai.read_quotas() if provider == "openai" else claude.read_quotas()
        )
    if args.as_json:
        result: Any = payloads if args.provider == "all" else payloads[args.provider]
        json.dump(result, destination, indent=2, sort_keys=True)
        print(file=destination)
        return
    rendered = []
    for provider, payload in payloads.items():
        renderer = (
            openai.render_quotas if provider == "openai" else claude.render_quotas
        )
        rendered.append(renderer(payload))
    print("\n\n".join(rendered), file=destination)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        run(build_parser().parse_args(argv))
    except KeyboardInterrupt:
        print("error: cancelled", file=sys.stderr)
        return 130
    except QuotaMeterError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
