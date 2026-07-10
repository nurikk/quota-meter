#ifdef PLATFORMIO
#include <unity.h>
#include <string.h>
#include "auth/pkce.h"
#include "domain/state_reducer.h"
#include "portal/portal_logic.h"
#include "providers/claude_protocol.h"
#include "providers/openai_protocol.h"
#include "util/base64url.h"
#include "ui/view_model.h"

using namespace qm;
void setUp() {}
void tearDown() {}

static void test_base64url_vectors()
{
    char output[32]; TEST_ASSERT_TRUE(base64url_encode(reinterpret_cast<const uint8_t *>("foobar"), 6, output, sizeof(output))); TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", output);
    uint8_t decoded[16]; size_t length; TEST_ASSERT_TRUE(base64url_decode(output, decoded, sizeof(decoded), &length)); TEST_ASSERT_EQUAL_size_t(6, length); TEST_ASSERT_EQUAL_MEMORY("foobar", decoded, 6);
    TEST_ASSERT_FALSE(base64url_decode("bad+", decoded, sizeof(decoded), &length));
}
static void test_pkce_rfc7636_vector()
{
    const char *verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"; char challenge[64]; TEST_ASSERT_TRUE(pkce_challenge(verifier, challenge, sizeof(challenge))); TEST_ASSERT_EQUAL_STRING("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", challenge);
    TEST_ASSERT_TRUE(constant_time_equal("state", "state")); TEST_ASSERT_FALSE(constant_time_equal("state", "State")); TEST_ASSERT_FALSE(constant_time_equal("state", "state2"));
}
static void test_claude_callback_formats_and_state()
{
    char code[128]; TEST_ASSERT_TRUE(claude_parse_callback("abc123#expected", "expected", code, sizeof(code))); TEST_ASSERT_EQUAL_STRING("abc123", code);
    TEST_ASSERT_TRUE(claude_parse_callback("https://platform.claude.com/oauth/code/callback?code=abc%2D123&state=expected", "expected", code, sizeof(code))); TEST_ASSERT_EQUAL_STRING("abc-123", code);
    TEST_ASSERT_FALSE(claude_parse_callback("abc123#wrong", "expected", code, sizeof(code))); TEST_ASSERT_EQUAL_STRING("", code);
    TEST_ASSERT_FALSE(claude_parse_callback("code=abc123", "expected", code, sizeof(code)));
}
static void test_claude_authorize_contract()
{
    char url[1024]; TEST_ASSERT_TRUE(claude_authorize_url("challenge", "state", url, sizeof(url))); TEST_ASSERT_NOT_NULL(strstr(url, "https://claude.ai/oauth/authorize")); TEST_ASSERT_NOT_NULL(strstr(url, CLAUDE_CLIENT_ID)); TEST_ASSERT_NOT_NULL(strstr(url, "code_challenge_method=S256")); TEST_ASSERT_NULL(strstr(url, " "));
}
static void test_openai_request_and_response_contract()
{
    char request[256]; TEST_ASSERT_TRUE(openai_user_code_request(request, sizeof(request))); TEST_ASSERT_NOT_NULL(strstr(request, OPENAI_CLIENT_ID));
    const char *json = "{\"device_auth_id\":\"dev\",\"usercode\":\"ABCD-EFGH\",\"interval\":\"5\",\"future\":true}"; OpenAiDeviceCode code{};
    TEST_ASSERT_TRUE(parse_openai_device_code(json, strlen(json), &code)); TEST_ASSERT_EQUAL_STRING("dev", code.device_auth_id); TEST_ASSERT_EQUAL_UINT32(5, code.interval);
    char exchange[512]; TEST_ASSERT_TRUE(openai_exchange_request("a+b/c", "verifier", exchange, sizeof(exchange))); TEST_ASSERT_NOT_NULL(strstr(exchange, "grant_type=authorization_code")); TEST_ASSERT_NOT_NULL(strstr(exchange, "code=a%2Bb%2Fc")); TEST_ASSERT_NULL(strstr(exchange, "{\""));
    char account_id[32]; const char *jwt = "e30.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifX0.sig"; TEST_ASSERT_TRUE(openai_account_id_from_jwt(jwt, account_id, sizeof(account_id))); TEST_ASSERT_EQUAL_STRING("acct_123", account_id);
    TEST_ASSERT_FALSE(parse_openai_device_code("{\"device_auth_id\":\"x\"}", 22, &code));
}
static void test_oauth_and_usage_parsing()
{
    OAuthTokens tokens{}; const char *token_json = "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600}";
    TEST_ASSERT_TRUE(parse_oauth_tokens(token_json, strlen(token_json), &tokens, true)); TEST_ASSERT_EQUAL_STRING("refresh", tokens.refresh_token);
    const char *rotated = "{\"refresh_token\":\"rotated\",\"expires_in\":7200}";
    TEST_ASSERT_TRUE(parse_oauth_tokens(rotated, strlen(rotated), &tokens, false)); TEST_ASSERT_EQUAL_STRING("rotated", tokens.refresh_token);
    ProviderStatus status{}; const char *openai = "{\"plan_type\":\"plus\",\"rate_limit\":{\"primary_window\":{\"used_percent\":42,\"reset_at\":123}}}";
    TEST_ASSERT_TRUE(parse_openai_usage(openai, strlen(openai), &status)); TEST_ASSERT_EQUAL_UINT8(1, status.window_count); TEST_ASSERT_FLOAT_WITHIN(0.01, 42, status.windows[0].used_percent);
    const char *claude = "{\"five_hour\":{\"utilization\":12.5,\"resets_at\":\"2030-03-17T12:00:00Z\"},\"seven_day\":null,\"future_window\":{\"percent\":30}}";
    TEST_ASSERT_TRUE(parse_claude_usage(claude, strlen(claude), &status)); TEST_ASSERT_EQUAL_UINT8(2, status.window_count); TEST_ASSERT_EQUAL_INT64(1899979200, status.windows[0].resets_at);
    TEST_ASSERT_FALSE(parse_claude_usage("[]", 2, &status));
}
static void test_reducer_and_portal_helpers()
{
    AppSnapshot state{}; state.wifi = WifiState::Provisioning; TEST_ASSERT_EQUAL_INT(static_cast<int>(Screen::WifiSetup), static_cast<int>(select_screen(state)));
    state.wifi = WifiState::Connected; state.openai.auth = AuthState::AwaitingUser; TEST_ASSERT_EQUAL_INT(static_cast<int>(Screen::OpenAiCode), static_cast<int>(select_screen(state)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuotaState::Stale), static_cast<int>(quota_after_failure(true))); TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::Throttled), static_cast<int>(map_http_error(429))); TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::InvalidResponse), static_cast<int>(map_http_error(302)));
    char qr[128]; TEST_ASSERT_TRUE(wifi_qr_payload("name;one", "p:ass", qr, sizeof(qr))); TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:name\\;one;P:p\\:ass;;", qr);
    const char *form = "ssid=Home+Net&password=a%3Bb"; char value[32]; TEST_ASSERT_TRUE(form_value(form, strlen(form), "password", value, sizeof(value))); TEST_ASSERT_EQUAL_STRING("a;b", value);
    TEST_ASSERT_TRUE(same_origin("http://192.168.4.1", "192.168.4.1")); TEST_ASSERT_FALSE(same_origin("http://evil/", "192.168.4.1"));
}
static void assert_pages(const AppSnapshot &state, const ConnectionPage *expected, size_t expected_count)
{
    ConnectionPage actual[3];
    TEST_ASSERT_EQUAL_size_t(expected_count, connection_pages(state, actual, 3));
    for (size_t index = 0; index < expected_count; ++index) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(expected[index]), static_cast<int>(actual[index]));
    }
}

static void test_connection_page_order()
{
    AppSnapshot state{};
    const ConnectionPage none[] = {ConnectionPage::Add};
    assert_pages(state, none, 1);

    state.claude.auth = AuthState::Authenticated;
    const ConnectionPage claude[] = {ConnectionPage::Claude, ConnectionPage::Add};
    assert_pages(state, claude, 2);

    state.openai.auth = AuthState::Refreshing;
    const ConnectionPage both[] = {ConnectionPage::Codex, ConnectionPage::Claude, ConnectionPage::Add};
    assert_pages(state, both, 3);

    state.claude.auth = AuthState::SignedOut;
    const ConnectionPage codex[] = {ConnectionPage::Codex, ConnectionPage::Add};
    assert_pages(state, codex, 2);
}


int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_base64url_vectors);
    RUN_TEST(test_pkce_rfc7636_vector);
    RUN_TEST(test_claude_callback_formats_and_state);
    RUN_TEST(test_claude_authorize_contract);
    RUN_TEST(test_openai_request_and_response_contract);
    RUN_TEST(test_oauth_and_usage_parsing);
    RUN_TEST(test_reducer_and_portal_helpers);
    RUN_TEST(test_connection_page_order);
    return UNITY_END();
}
#endif
