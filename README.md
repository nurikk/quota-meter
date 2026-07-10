# quota-meter

A small Python spike that shows subscription quota data through the locally installed
official OpenAI Codex and Claude Code CLIs.

## Prerequisites

- Python 3.11 or newer
- [`codex`](https://developers.openai.com/codex/cli/) on `PATH` for OpenAI
- [`claude`](https://code.claude.com/docs/en/overview) on `PATH` for Claude
- A Unix-like platform for Claude's PTY-based interactive login

Install the project with `uv sync`, then run:

```console
uv run quota-meter login openai
uv run quota-meter login claude
uv run quota-meter login all
uv run quota-meter quotas openai
uv run quota-meter quotas claude
uv run quota-meter quotas all
uv run quota-meter quotas all --json
```

OpenAI login launches `codex app-server`, requests Codex's ChatGPT device-code login,
and displays the verification URL as a terminal QR alongside its user code. Codex owns
all credential storage and refresh behavior. Quota reads likewise use the app-server's
account and rate-limit methods rather than reading Codex tokens.

Claude has **no device authorization grant**. Its login is explicitly a QR-assisted
OAuth authorization-code flow with PKCE and a possible manually pasted code. The tool
runs the official `claude auth login --claudeai` inside a Unix PTY, relays the terminal,
and adds a QR for the authorization URL; Claude Code still owns the login itself.
Unsupported platforms receive guidance to run the official command directly.

For Claude quota reads, the tool only reads Claude Code's official OAuth credential
location: macOS Keychain service `Claude Code-credentials`, or
`${CLAUDE_CONFIG_DIR:-~/.claude}/.credentials.json` elsewhere. It never prints or writes
tokens and asks for a new login when the access token has expired. It then calls
`https://api.anthropic.com/api/oauth/usage`. **Anthropic does not document this usage
endpoint or promise its stability**, so this integration may break when Claude Code
changes. The CLI distinguishes authentication, authorization, and throttling errors.

`--json` emits the complete provider response rather than the human-oriented summary.
Avoid sharing JSON output because account or provider payloads may contain metadata.
