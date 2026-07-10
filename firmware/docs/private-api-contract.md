# Unsupported/private provider contracts

Both integrations below are **unsupported private APIs**, not stable public
OAuth/usage APIs. They may change or be blocked without notice. This prototype
freezes contracts observed from Codex 0.132.0 and Claude Code 2.1.206 during
July 2026 research. Sanitized fixtures live in `test/fixtures/`; they contain no
bearer tokens, refresh tokens, PKCE verifier/state, or account identity.

## OpenAI

Public client ID: `app_EMoamEEZ73f0CkXaXp7hrann`.

1. POST the JSON `client_id` to
   `https://auth.openai.com/api/accounts/deviceauth/usercode`.
2. Poll `https://auth.openai.com/api/accounts/deviceauth/token` with
   `device_auth_id` and `user_code`; HTTP 403/404 mean pending.
3. Exchange at `https://auth.openai.com/oauth/token` using
   `application/x-www-form-urlencoded`, the authorization-code grant, returned
   verifier, and redirect `https://auth.openai.com/deviceauth/callback`.
4. Refresh at the same endpoint with a JSON refresh-token grant.
5. Read `https://chatgpt.com/backend-api/wham/usage` with a bearer token and
   `ChatGPT-Account-Id` extracted from the ID token's
   `https://api.openai.com/auth.chatgpt_account_id` claim.

## Claude

Public client ID: `9d1c250a-e61b-44d9-88ed-5944d1962f5e`.

- Authorize: `https://claude.ai/oauth/authorize`
- Manual callback: `https://platform.claude.com/oauth/code/callback`
- Token: `https://platform.claude.com/v1/oauth/token`
- Usage: `https://api.anthropic.com/api/oauth/usage`
- Beta header: `anthropic-beta: oauth-2025-04-20`
- Scopes: `org:create_api_key user:profile user:inference`
  `user:sessions:claude_code user:mcp_servers user:file_upload`
- Manual input accepts the displayed `code#state` or the exact callback
  URL/query containing both values. State must match exactly.

PKCE is S256 with random 64-byte verifier material and random 32-byte state
material, encoded as base64url without padding. The pending login expires after
ten minutes.
