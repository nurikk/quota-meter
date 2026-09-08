# Unsupported/private provider contracts

Both integrations below use **unsupported private APIs**, not stable public
usage APIs. They may change or be blocked without notice. This prototype freezes
contracts observed from Codex 0.132.0 and Claude Code 2.1.206 during July 2026
research. Sanitized fixtures live in `test/fixtures/`; they contain no bearer or
refresh tokens or account identity.

The device does not initiate provider authorization. Provider credentials are
created and owned by the official desktop tools, then uploaded locally over USB.

## OpenAI Codex

Public client ID: `app_EMoamEEZ73f0CkXaXp7hrann`.

- Refresh: `POST https://auth.openai.com/oauth/token` with a JSON refresh-token
  grant.
- Usage: `GET https://chatgpt.com/backend-api/wham/usage` with a bearer token and
  `ChatGPT-Account-Id` extracted from the ID token's
  `https://api.openai.com/auth.chatgpt_account_id` claim.

## Claude

Public client ID: `9d1c250a-e61b-44d9-88ed-5944d1962f5e`.

- Refresh: `POST https://platform.claude.com/v1/oauth/token` with a JSON
  refresh-token grant.
- Usage: `GET https://api.anthropic.com/api/oauth/usage`.
- Beta header: `anthropic-beta: oauth-2025-04-20`.
