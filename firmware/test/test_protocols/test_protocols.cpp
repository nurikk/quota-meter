#ifdef PLATFORMIO
#include <unity.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <initializer_list>
#include "domain/state_reducer.h"
#include "auth/token_store.h"
#include <memory>
#include <utility>
#include "console/token_upload.h"
#include "net/http_client.h"
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
static void test_refresh_request_contracts()
{
    char request[512];
    TEST_ASSERT_TRUE(openai_refresh_request("openai-refresh", request, sizeof(request)));
    TEST_ASSERT_NOT_NULL(strstr(request, OPENAI_CLIENT_ID));
    TEST_ASSERT_NOT_NULL(strstr(request, "\"grant_type\":\"refresh_token\""));
    TEST_ASSERT_NOT_NULL(strstr(request, "\"refresh_token\":\"openai-refresh\""));
    TEST_ASSERT_TRUE(claude_refresh_request("claude-refresh", request, sizeof(request)));
    TEST_ASSERT_NOT_NULL(strstr(request, CLAUDE_CLIENT_ID));
    TEST_ASSERT_NOT_NULL(strstr(request, "\"refresh_token\":\"claude-refresh\""));
    char account_id[32]; const char *jwt = "e30.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifX0.sig"; TEST_ASSERT_TRUE(openai_account_id_from_jwt(jwt, account_id, sizeof(account_id))); TEST_ASSERT_EQUAL_STRING("acct_123", account_id);
}
static void test_oauth_and_usage_parsing()
{
    OAuthTokens tokens{}; const char *token_json = "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600}";
    TEST_ASSERT_TRUE(parse_oauth_tokens(token_json, strlen(token_json), &tokens, true)); TEST_ASSERT_EQUAL_STRING("refresh", tokens.refresh_token);
    const char *rotated = "{\"refresh_token\":\"rotated\",\"expires_in\":7200}";
    TEST_ASSERT_TRUE(parse_oauth_tokens(rotated, strlen(rotated), &tokens, false)); TEST_ASSERT_EQUAL_STRING("rotated", tokens.refresh_token);
    ProviderStatus status{}; const char *openai = "{\"plan_type\":\"plus\",\"rate_limit\":{\"primary_window\":{\"used_percent\":42,\"reset_at\":123}}}";
    TEST_ASSERT_TRUE(parse_openai_usage(openai, strlen(openai), &status)); TEST_ASSERT_EQUAL_UINT8(1, status.window_count); TEST_ASSERT_FLOAT_WITHIN(0.01, 42, status.windows[0].used_percent);
    const char *weekly = "{\"planType\":\"prolite\",\"primary\":{\"usedPercent\":2,\"windowDurationMins\":10080,\"resetsAt\":1784487499}}";
    TEST_ASSERT_TRUE(parse_openai_usage(weekly, strlen(weekly), &status));
    TEST_ASSERT_EQUAL_INT32(10080, status.windows[0].window_minutes);
    TEST_ASSERT_EQUAL_INT64(1784487499, status.windows[0].resets_at);
    TEST_ASSERT_EQUAL_PTR(&status.windows[0], find_quota_window_by_duration(status, 10080));
    char period[8];
    format_window_period(&status.windows[0], "5H", period, sizeof(period));
    TEST_ASSERT_EQUAL_STRING("7D", period);
    const char *seconds = "{\"rate_limit\":{\"primary_window\":{\"used_percent\":3,\"limit_window_seconds\":604800}}}";
    TEST_ASSERT_TRUE(parse_openai_usage(seconds, strlen(seconds), &status));
    TEST_ASSERT_EQUAL_INT32(10080, status.windows[0].window_minutes);
    const char *claude = "{\"five_hour\":{\"utilization\":12.5,\"resets_at\":\"2030-03-17T12:00:00+00:00\"},\"seven_day\":null,\"future_window\":{\"percent\":30}}";
    TEST_ASSERT_TRUE(parse_claude_usage(claude, strlen(claude), &status)); TEST_ASSERT_EQUAL_UINT8(2, status.window_count); TEST_ASSERT_EQUAL_INT64(1899979200, status.windows[0].resets_at);
    TEST_ASSERT_EQUAL_INT32(300, status.windows[0].window_minutes);
    TEST_ASSERT_FALSE(parse_claude_usage("[]", 2, &status));
}
static void test_strict_provider_parsing()
{
    ProviderStatus status{};
    strcpy(status.plan, "stale-plan");
    const char *without_plan =
        "{\"rate_limit\":{\"primary_window\":{\"used_percent\":5,\"reset_at\":100}}}";
    TEST_ASSERT_TRUE(parse_openai_usage(without_plan, strlen(without_plan), &status));
    TEST_ASSERT_EQUAL_STRING("", status.plan);
    const char *openai_trailing =
        "{\"rate_limit\":{\"primary_window\":{\"used_percent\":5}}}garbage";
    TEST_ASSERT_FALSE(parse_openai_usage(openai_trailing, strlen(openai_trailing), &status));

    const char *valid_leap =
        "{\"five_hour\":{\"utilization\":1,\"resets_at\":\"2028-02-29T12:34:56Z\"}}";
    TEST_ASSERT_TRUE(parse_claude_usage(valid_leap, strlen(valid_leap), &status));
    TEST_ASSERT_GREATER_THAN_INT64(0, status.windows[0].resets_at);
    const char *invalid_day =
        "{\"five_hour\":{\"utilization\":1,\"resets_at\":\"2027-02-29T12:34:56Z\"}}";
    TEST_ASSERT_TRUE(parse_claude_usage(invalid_day, strlen(invalid_day), &status));
    TEST_ASSERT_EQUAL_INT64(0, status.windows[0].resets_at);
    const char *short_fields =
        "{\"five_hour\":{\"utilization\":1,\"resets_at\":\"2030-1-1T1:1:1Z\"}}";
    TEST_ASSERT_TRUE(parse_claude_usage(short_fields, strlen(short_fields), &status));
    TEST_ASSERT_EQUAL_INT64(0, status.windows[0].resets_at);
    const char *claude_trailing =
        "{\"five_hour\":{\"utilization\":1}} trailing";
    TEST_ASSERT_FALSE(parse_claude_usage(claude_trailing, strlen(claude_trailing), &status));
}

static void test_retry_after_parsing()
{
    uint32_t delay = 0;
    TEST_ASSERT_TRUE(parse_retry_after("120", 0, &delay));
    TEST_ASSERT_EQUAL_UINT32(120, delay);
    TEST_ASSERT_TRUE(parse_retry_after("Sun, 06 Nov 1994 08:49:37 GMT", 784111700, &delay));
    TEST_ASSERT_EQUAL_UINT32(77, delay);
    TEST_ASSERT_TRUE(parse_retry_after("Sun, 06 Nov 1994 08:49:37 GMT", 784111800, &delay));
    TEST_ASSERT_EQUAL_UINT32(0, delay);
    TEST_ASSERT_FALSE(parse_retry_after("120 seconds", 0, &delay));
    TEST_ASSERT_FALSE(parse_retry_after("4294967296", 0, &delay));
    TEST_ASSERT_FALSE(parse_retry_after("Sun, 31 Feb 2027 08:49:37 GMT", 0, &delay));
}

static void test_cached_usage_classification()
{
    ProviderStatus status{};
    TEST_ASSERT_FALSE(has_cached_usage(status));
    status.extra_usage.present = true;
    TEST_ASSERT_TRUE(has_cached_usage(status));
    status.auth = AuthState::Refreshing;
    apply_credential_failure(status, ErrorCode::Network);
    TEST_ASSERT_EQUAL(QuotaState::Stale, status.quota);
}

static void test_claude_extended_usage_parsing()
{
    const char *json = "{\"five_hour\":{\"utilization\":12.5,\"resets_at\":\"2026-07-10T12:00:00Z\"},"
                       "\"limits\":["
                       "{\"kind\":\"weekly_limit\",\"group\":\"claude_code\",\"percent\":40,\"scope\":{\"model\":{\"display_name\":\"Fable\"}}},"
                       "{\"percent\":20,\"scope\":{\"model\":{\"name\":\"Sonnet\"}}},"
                       "{\"percent\":10,\"scope\":{\"model\":{\"id\":\"claude-opus\"}}}],"
                       "\"extra_usage\":{\"is_enabled\":true,\"monthly_limit\":25000,\"used_credits\":219,"
                       "\"utilization\":0.876,\"currency\":\"usd\",\"decimal_places\":2},"
                       "\"spend\":{\"percent\":1,\"used\":{\"amount_minor\":219,\"currency\":\"usd\",\"exponent\":2},"
                       "\"limit\":{\"amount_minor\":2500,\"currency\":\"usd\",\"exponent\":1}}}";
    ProviderStatus status{};
    TEST_ASSERT_TRUE(parse_claude_usage(json, strlen(json), &status));
    TEST_ASSERT_EQUAL_UINT8(4, status.window_count);
    TEST_ASSERT_EQUAL_STRING("Fable", status.windows[1].label);
    TEST_ASSERT_EQUAL_STRING("Sonnet", status.windows[2].label);
    TEST_ASSERT_EQUAL_STRING("claude-opus", status.windows[3].label);
    TEST_ASSERT_TRUE(status.windows[1].model_limit);
    TEST_ASSERT_EQUAL_INT32(10080, status.windows[1].window_minutes);
    TEST_ASSERT_EQUAL_PTR(&status.windows[1], find_model_limit(status, "Fable"));
    TEST_ASSERT_TRUE(status.extra_usage.present);
    TEST_ASSERT_TRUE(status.extra_usage.is_enabled);
    TEST_ASSERT_TRUE(status.extra_usage.has_utilization);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.876, status.extra_usage.utilization);
    TEST_ASSERT_TRUE(status.extra_usage.has_used_credits);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 2.19, static_cast<float>(status.extra_usage.used_credits));
    TEST_ASSERT_TRUE(status.extra_usage.has_monthly_limit);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 250, static_cast<float>(status.extra_usage.monthly_limit));
    TEST_ASSERT_EQUAL_STRING("usd", status.extra_usage.currency);
    TEST_ASSERT_TRUE(status.extra_usage.has_spend);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 2.19, static_cast<float>(status.extra_usage.spend));
    TEST_ASSERT_TRUE(status.extra_usage.has_spend_percent);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 1, status.extra_usage.spend_percent);
    TEST_ASSERT_EQUAL_UINT8(2, status.extra_usage.spend_decimal_places);
    TEST_ASSERT_EQUAL_UINT8(1, status.extra_usage.limit_decimal_places);
}

static void test_dashboard_view_model()
{
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
    tzset();
    ProviderStatus status{};
    status.window_count = 2;
    status.windows[0] = {"Secondary", 31, 2000000, true};
    status.windows[1] = {"Primary", 28, 1000000, true};
    TEST_ASSERT_EQUAL_PTR(&status.windows[1], find_quota_window(status, "primary"));
    TEST_ASSERT_EQUAL_PTR(&status.windows[0], find_quota_window(status, "Secondary"));
    TEST_ASSERT_NULL(find_quota_window(status, "Fable"));

    QuotaWindow window{"Primary", 28, 2000, true};
    TEST_ASSERT_FLOAT_WITHIN(0.01, 50, elapsed_percent(window, 1100, 30));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsageRisk::Normal), static_cast<int>(usage_risk(55, 50)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsageRisk::Warning), static_cast<int>(usage_risk(60, 50)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsageRisk::High), static_cast<int>(usage_risk(70, 50)));
    char countdown[24];
    format_countdown(200000, 100000, countdown, sizeof(countdown));
    TEST_ASSERT_EQUAL_STRING("1d 03h", countdown);
    format_countdown(103661, 100000, countdown, sizeof(countdown));
    TEST_ASSERT_EQUAL_STRING("01:01:01", countdown);
    format_countdown(704800, 100000, countdown, sizeof(countdown));
    TEST_ASSERT_EQUAL_STRING("7d 00h", countdown);

    char footer[64];
    status.fetched_at = 1710676800;
    status.error = ErrorCode::Network;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FooterTone::Warning),
                          static_cast<int>(format_dashboard_footer(status, "Network request failed", footer, sizeof(footer))));
    TEST_ASSERT_EQUAL_STRING("stale  updated 12:00:00", footer);
    status.fetched_at = static_cast<int64_t>(time(nullptr)) + 300;
    status.error = ErrorCode::None;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FooterTone::Warning),
                          static_cast<int>(format_dashboard_footer(status, nullptr, footer, sizeof(footer))));
    TEST_ASSERT_EQUAL_STRING("waiting for clock sync", footer);
    status.fetched_at = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FooterTone::Error),
                          static_cast<int>(format_dashboard_footer(status, "Network request failed", footer, sizeof(footer))));
    TEST_ASSERT_EQUAL_STRING("Network request failed", footer);
}

static void test_reducer_and_portal_helpers()
{
    AppSnapshot state{}; state.wifi = WifiState::Provisioning; TEST_ASSERT_EQUAL_INT(static_cast<int>(Screen::WifiSetup), static_cast<int>(select_screen(state)));
    state.wifi = WifiState::Connected; TEST_ASSERT_EQUAL_INT(static_cast<int>(Screen::TokenImport), static_cast<int>(select_screen(state)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QuotaState::Stale), static_cast<int>(quota_after_failure(true))); TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::Throttled), static_cast<int>(map_http_error(429))); TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::InvalidResponse), static_cast<int>(map_http_error(302)));
    char qr[128]; TEST_ASSERT_TRUE(wifi_qr_payload("name;one", "p:ass", qr, sizeof(qr))); TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:name\\;one;P:p\\:ass;;", qr);
    const char *form = "ssid=Home+Net&password=a%3Bb"; char value[32]; TEST_ASSERT_TRUE(form_value(form, strlen(form), "password", value, sizeof(value))); TEST_ASSERT_EQUAL_STRING("a;b", value);
    TEST_ASSERT_TRUE(same_origin("http://192.168.4.1", "192.168.4.1")); TEST_ASSERT_FALSE(same_origin("http://evil/", "192.168.4.1"));
    TEST_ASSERT_TRUE(constant_time_equal("password", "password")); TEST_ASSERT_FALSE(constant_time_equal("password", "Password"));
}
static AppSnapshot sample_accounts(size_t count = 3)
{
    AppSnapshot snapshot{};
    for (size_t index = 0; index < count; ++index) {
        Account account{static_cast<AccountId>(index + 1), index % 2 ? Provider::Claude : Provider::OpenAI, {}, 1};
        snprintf(account.name, sizeof(account.name), "Account %zu", index / 2 + 1);
        snapshot.accounts.push_back({account, {}});
    }
    return snapshot;
}

static void assert_pages(const AppSnapshot &state, const ConnectionPage *expected, size_t expected_count)
{
    const auto actual = connection_pages(state);
    TEST_ASSERT_EQUAL_size_t(expected_count, actual.size());
    for (size_t index = 0; index < expected_count; ++index) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(expected[index]), static_cast<int>(actual[index]));
    }
}

static AppSnapshot carousel_accounts(size_t count = 3)
{
    AppSnapshot snapshot = sample_accounts(count);
    for (auto &entry : snapshot.accounts) {
        entry.status.auth = AuthState::Authenticated;
        entry.status.quota = QuotaState::Fresh;
        entry.status.fetched_at = 1000;
        entry.status.window_count = 2;
        entry.status.windows[0] = {"Primary", 10, 2000, true, false, 300};
        entry.status.windows[1] = {"Secondary", 20, 9000, true, false, 10080};
    }
    return snapshot;
}

static void test_carousel_active_rotation_and_idle_slots()
{
    for (size_t count : {3, 4}) {
        AccountCarousel carousel;
        AppSnapshot before = carousel_accounts(count);
        ConnectionPage page = carousel.update(1, AppSnapshot{}, before, 0);
        AppSnapshot after = before;
        after.status(1).windows[0].used_percent += 1;
        after.status(2).windows[1].used_percent += 1;
        TEST_ASSERT_EQUAL(1, carousel.update(page, before, after, 0));
        TEST_ASSERT_EQUAL(1, carousel.update(page, after, after, 9999));
        for (uint32_t tick = 1; tick <= 49; ++tick) {
            page = carousel.update(page, after, after, tick * 10000);
            if (tick % 12 == 0) {
                const AccountId idle = count == 3 || (tick / 12) % 2 == 1 ? 3 : 4;
                TEST_ASSERT_EQUAL(idle, page);
                TEST_ASSERT_EQUAL(page, carousel.update(page, after, after, tick * 10000 + 9999));
            } else {
                TEST_ASSERT_TRUE(page == 1 || page == 2);
                if (tick == 1) TEST_ASSERT_EQUAL(2, page);
                if (tick == 2) TEST_ASSERT_EQUAL(1, page);
                if (tick == 13) TEST_ASSERT_EQUAL(1, page);
            }
        }
    }
}

static void test_carousel_usage_growth_detection()
{
    for (int change = 0; change < 13; ++change) {
        AccountCarousel carousel;
        AppSnapshot before = carousel_accounts();
        TEST_ASSERT_EQUAL(1, carousel.update(1, AppSnapshot{}, before, 0));
        AppSnapshot after = before;
        auto &status = after.status(2);
        switch (change) {
        case 0: status.fetched_at += 100; break;
        case 1: status.fetched_at -= 100; break;
        case 2: std::swap(status.windows[0], status.windows[1]); break;
        case 3: status.windows[0].used_percent -= 1; break;
        case 4: status.windows[0].resets_at += 1000; status.windows[0].used_percent += 1; break;
        case 5: status.quota = QuotaState::Stale; status.windows[0].used_percent += 1; break;
        case 6: status.quota = QuotaState::Error; status.windows[0].used_percent += 1; break;
        case 7: status.error = ErrorCode::Network; status.windows[0].used_percent += 1; break;
        case 8: status.windows[0].present = false; status.windows[0].used_percent += 1; break;
        case 9: status.windows[0].window_minutes = 60; status.windows[0].used_percent += 1; break;
        case 10: status.windows[0].model_limit = true; status.windows[0].used_percent += 1; break;
        case 11: strcpy(status.windows[0].label, "New window"); status.windows[0].used_percent += 1; break;
        case 12:
            std::swap(status.windows[0], status.windows[1]);
            status.windows[1].used_percent += 0.25f;
            break;
        }
        TEST_ASSERT_EQUAL(1, carousel.update(1, before, after, 1000));
        TEST_ASSERT_EQUAL(2, carousel.update(1, after, after, 10000));
        TEST_ASSERT_EQUAL(change == 12 ? 2 : 3, carousel.update(2, after, after, 20000));
    }
}

static void test_carousel_baselines_and_activity_expiry()
{
    for (QuotaState quota : {QuotaState::Fresh, QuotaState::Stale}) {
        AccountCarousel carousel;
        AppSnapshot before = carousel_accounts();
        before.status(2).quota = quota;
        TEST_ASSERT_EQUAL(1, carousel.update(1, AppSnapshot{}, before, 0));
        TEST_ASSERT_EQUAL(2, carousel.update(1, before, before, 10000));
        TEST_ASSERT_EQUAL(3, carousel.update(2, before, before, 20000));
        AppSnapshot after = before;
        after.status(2).quota = QuotaState::Fresh;
        after.status(2).windows[0].used_percent += 1;
        TEST_ASSERT_EQUAL(3, carousel.update(3, before, after, 21000));
        TEST_ASSERT_EQUAL(2, carousel.update(3, after, after, 30000));
        ConnectionPage page = 2;
        for (uint32_t now = 40000; now <= 610000; now += 10000) {
            before = after;
            after.status(2).fetched_at += 10;
            page = carousel.update(page, before, after, now);
        }
        carousel.reset_dwell(611000);
        TEST_ASSERT_EQUAL(2, carousel.update(2, after, after, 620999));
        TEST_ASSERT_EQUAL(3, carousel.update(2, after, after, 621000));
    }
    AccountCarousel carousel;
    AppSnapshot before = carousel_accounts();
    before.status(2).window_count = 0;
    before.status(2).quota = QuotaState::Loading;
    carousel.update(1, AppSnapshot{}, before, 0);
    AppSnapshot after = carousel_accounts();
    TEST_ASSERT_EQUAL(1, carousel.update(1, before, after, 1000));
    TEST_ASSERT_EQUAL(2, carousel.update(1, after, after, 10000));
    TEST_ASSERT_EQUAL(3, carousel.update(2, after, after, 20000));
}

static void test_carousel_idle_single_manual_and_wrap()
{
    for (uint32_t start : {0u, UINT32_MAX - 5000}) {
        AccountCarousel carousel;
        AppSnapshot snapshot = carousel_accounts();
        ConnectionPage page = carousel.update(1, AppSnapshot{}, snapshot, start);
        for (uint32_t tick = 1; tick <= 70; ++tick) {
            page = carousel.update(page, snapshot, snapshot, start + tick * 10000);
            TEST_ASSERT_EQUAL(tick % 3 + 1, page);
        }
        carousel.reset_dwell(start + 705000);
        TEST_ASSERT_EQUAL(3, carousel.update(3, snapshot, snapshot, start + 710000));
        TEST_ASSERT_EQUAL(3, carousel.update(3, snapshot, snapshot, start + 714999));
        TEST_ASSERT_EQUAL(1, carousel.update(3, snapshot, snapshot, start + 715000));
        carousel.reset_dwell(start + 716000);
        TEST_ASSERT_EQUAL(IMPORT_PAGE, carousel.update(IMPORT_PAGE, snapshot, snapshot, start + 900000));
    }
    AccountCarousel carousel;
    AppSnapshot snapshot = carousel_accounts(1);
    TEST_ASSERT_EQUAL(1, carousel.update(1, AppSnapshot{}, snapshot, 0));
    for (uint32_t now : {10000, 120000, 600000})
        TEST_ASSERT_EQUAL(1, carousel.update(1, snapshot, snapshot, now));
    AppSnapshot empty;
    TEST_ASSERT_EQUAL(IMPORT_PAGE, carousel.update(1, snapshot, empty, 610000));
}

static void test_carousel_active_manual_and_wrap()
{
    for (uint32_t start : {0u, UINT32_MAX - 5000}) {
        AccountCarousel carousel;
        AppSnapshot before = carousel_accounts();
        carousel.update(1, AppSnapshot{}, before, start);
        AppSnapshot after = before;
        after.status(1).windows[0].used_percent += 1;
        after.status(2).windows[0].used_percent += 1;
        carousel.update(1, before, after, start + 1000);
        carousel.reset_dwell(start + 5000);
        TEST_ASSERT_EQUAL(2, carousel.update(2, after, after, start + 14999));
        TEST_ASSERT_EQUAL(1, carousel.update(2, after, after, start + 15000));
        carousel.reset_dwell(start + 16000);
        before = after;
        after.status(1).windows[0].used_percent += 1;
        TEST_ASSERT_EQUAL(IMPORT_PAGE, carousel.update(IMPORT_PAGE, before, after, start + 20000));
        TEST_ASSERT_EQUAL(IMPORT_PAGE, carousel.update(IMPORT_PAGE, after, after, start + 120000));
        carousel.reset_dwell(start + 121000);
        TEST_ASSERT_EQUAL(2, carousel.update(2, after, after, start + 130999));
        TEST_ASSERT_EQUAL(3, carousel.update(2, after, after, start + 131000));
        TEST_ASSERT_EQUAL(1, carousel.update(3, after, after, start + 141000));
        TEST_ASSERT_EQUAL(3, carousel.update(2, after, after, start + 620000));
    }
}

static void test_carousel_scroll_deadline_and_expiration()
{
    AccountCarousel carousel;
    AppSnapshot snapshot = carousel_accounts();
    carousel.update(3, AppSnapshot{}, snapshot, 0);
    for (uint32_t now : {9750, 10000, 10250}) {
        carousel.reset_dwell(now);
        TEST_ASSERT_EQUAL(3, carousel.update(3, snapshot, snapshot, now));
    }
    carousel.reset_dwell(10500);
    TEST_ASSERT_EQUAL(IMPORT_PAGE, carousel.update(IMPORT_PAGE, snapshot, snapshot, 30000));
    carousel.reset_dwell(31000);
    TEST_ASSERT_EQUAL(2, carousel.update(2, snapshot, snapshot, 40999));
    TEST_ASSERT_EQUAL(3, carousel.update(2, snapshot, snapshot, 41000));
    AppSnapshot expired = snapshot;
    apply_credential_failure(expired.status(1), ErrorCode::Unauthorized);
    carousel.reset_dwell(51000);
    TEST_ASSERT_EQUAL(1, carousel.update(3, snapshot, expired, 51000));
}


static void test_carousel_account_changes()
{
    AccountCarousel carousel;
    AppSnapshot before = carousel_accounts();
    carousel.update(1, AppSnapshot{}, before, 0);
    AppSnapshot after = before;
    after.status(2).windows[0].used_percent += 1;
    carousel.update(1, before, after, 1000);
    TEST_ASSERT_EQUAL(2, carousel.update(1, after, after, 10000));
    before = after;
    ++after.accounts[1].account.generation;
    after.status(2).windows[0].used_percent += 1;
    TEST_ASSERT_EQUAL(3, carousel.update(2, before, after, 20000));
    before = after;
    after.accounts.erase(after.accounts.begin() + 2);
    TEST_ASSERT_EQUAL(1, carousel.update(3, before, after, 21000));
    TEST_ASSERT_EQUAL(1, carousel.update(1, after, after, 30999));
    TEST_ASSERT_EQUAL(2, carousel.update(1, after, after, 31000));
    before = after;
    after.accounts.push_back(carousel_accounts().accounts.back());
    after.status(3).windows[0].used_percent = 99;
    TEST_ASSERT_EQUAL(2, carousel.update(2, before, after, 32000));
    TEST_ASSERT_EQUAL(3, carousel.update(2, after, after, 41000));
    TEST_ASSERT_EQUAL(1, carousel.update(3, after, after, 51000));
}

static void test_connection_page_order()
{
    AppSnapshot state = sample_accounts();
    const ConnectionPage none[] = {IMPORT_PAGE};
    assert_pages(state, none, 1);

    state.status(2).auth = AuthState::Authenticated;
    const ConnectionPage claude[] = {2, IMPORT_PAGE};
    assert_pages(state, claude, 2);

    state.status(1).auth = AuthState::Refreshing;
    const ConnectionPage both[] = {1, 2, IMPORT_PAGE};
    assert_pages(state, both, 3);

    state.status(2).auth = AuthState::SignedOut;
    const ConnectionPage codex[] = {1, IMPORT_PAGE};
    assert_pages(state, codex, 2);
}


static void test_openai_reset_credits_parsing()
{
    const struct {
        const char *field;
        bool present;
        uint32_t count;
    } cases[] = {
        {",\"rate_limit_reset_credits\":{\"available_count\":1,\"applicable_available_count\":0}", true, 1},
        {",\"rate_limit_reset_credits\":{\"available_count\":0}", true, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":4294967295}", true, UINT32_MAX},
        {"", false, 0},
        {",\"rate_limit_reset_credits\":null", false, 0},
        {",\"rate_limit_reset_credits\":{}", false, 0},
        {",\"rate_limit_reset_credits\":{\"applicable_available_count\":1}", false, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":null}", false, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":\"1\"}", false, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":true}", false, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":-1}", false, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":1.5}", false, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":4294967296}", false, 0},
        {",\"rate_limit_reset_credits\":{\"available_count\":1e999}", false, 0},
    };
    for (const auto &test : cases) {
        char json[256] = "{\"rate_limit\":{\"primary_window\":{\"used_percent\":17}}";
        strcat(json, test.field);
        strcat(json, "}");
        ProviderStatus status{};
        status.reset_credits = {true, 7};
        TEST_ASSERT_TRUE(parse_openai_usage(json, strlen(json), &status));
        TEST_ASSERT_EQUAL(test.present, status.reset_credits.present);
        TEST_ASSERT_EQUAL_UINT32(test.count, status.reset_credits.available_count);
        TEST_ASSERT_FLOAT_WITHIN(0.01, 17, status.windows[0].used_percent);
    }
}

static void test_format_codex_reset_credits()
{
    ProviderStatus status{};
    char text[32];
    format_codex_reset_credits(status, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("Reset credits: --", text);
    status.reset_credits = {true, 1};
    format_codex_reset_credits(status, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("Reset credits: 1", text);
    status.reset_credits.available_count = 0;
    format_codex_reset_credits(status, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("Reset credits: 0", text);
    status.reset_credits.available_count = UINT32_MAX;
    format_codex_reset_credits(status, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("Reset credits: 4294967295", text);
}

static void test_oauth_refresh_error_classification()
{
    const struct {
        int status;
        ErrorCode transport_error;
        const char *body;
        ErrorCode expected;
    } cases[] = {
        {401, ErrorCode::Network, "", ErrorCode::Unauthorized},
        {401, ErrorCode::None, "", ErrorCode::Unauthorized},
        {400, ErrorCode::InvalidResponse, "{\"error\":\"invalid_grant\"}", ErrorCode::Unauthorized},
        {403, ErrorCode::Forbidden, "{\"error\":{\"code\":\"invalid_grant\"}}", ErrorCode::Unauthorized},
        {400, ErrorCode::None, "{\"error\":\"invalid_request\"}", ErrorCode::InvalidResponse},
        {400, ErrorCode::None, "{\"error\":\"invalid_client\"}", ErrorCode::InvalidResponse},
        {400, ErrorCode::None, "{\"error_description\":\"expired token\"}", ErrorCode::InvalidResponse},
        {400, ErrorCode::None, "not json", ErrorCode::InvalidResponse},
        {403, ErrorCode::None, "{\"error\":\"access_denied\"}", ErrorCode::Forbidden},
        {429, ErrorCode::Network, "{\"error\":\"invalid_grant\"}", ErrorCode::Throttled},
        {500, ErrorCode::None, "{\"error\":\"invalid_grant\"}", ErrorCode::InvalidResponse},
        {0, ErrorCode::Network, "", ErrorCode::Network},
        {200, ErrorCode::Network, "", ErrorCode::Network},
        {200, ErrorCode::InvalidResponse, "", ErrorCode::InvalidResponse},
        {200, ErrorCode::None, "{}", ErrorCode::None},
    };
    for (const auto &test : cases) {
        HttpResult response{};
        response.status = test.status;
        response.error = test.transport_error;
        strcpy(response.body, test.body);
        response.body_len = strlen(test.body);
        TEST_ASSERT_EQUAL(test.expected, oauth_refresh_error(response));
    }
    TEST_ASSERT_EQUAL(ErrorCode::Unauthorized, map_http_error(401, ErrorCode::Network));
    TEST_ASSERT_EQUAL(ErrorCode::Network, map_http_error(0, ErrorCode::Network));
}

static void test_expired_credential_state_and_recovery()
{
    ProviderStatus status{};
    status.auth = AuthState::Refreshing;
    status.window_count = 1;
    status.windows[0].used_percent = 17;
    status.fetched_at = 100;
    apply_credential_failure(status, ErrorCode::Unauthorized);
    TEST_ASSERT_EQUAL(AuthState::Expired, status.auth);
    TEST_ASSERT_EQUAL(QuotaState::Stale, status.quota);
    TEST_ASSERT_EQUAL(100, status.fetched_at);
    TEST_ASSERT_FALSE(can_fetch_quota(status));
    char summary[80];
    format_provider_summary(status, summary, sizeof(summary));
    TEST_ASSERT_NOT_NULL(strstr(summary, "Credentials expired"));
    for (AuthState auth : {AuthState::SignedOut, AuthState::Error}) {
        status.auth = auth;
        TEST_ASSERT_FALSE(can_fetch_quota(status));
    }
    status.auth = AuthState::Authenticated;
    TEST_ASSERT_TRUE(can_fetch_quota(status));
    status.auth = AuthState::Refreshing;
    TEST_ASSERT_TRUE(can_fetch_quota(status));
    for (ErrorCode error : {ErrorCode::Network, ErrorCode::Throttled, ErrorCode::InvalidResponse,
                            ErrorCode::Forbidden}) {
        apply_credential_failure(status, error);
        TEST_ASSERT_EQUAL(AuthState::Authenticated, status.auth);
        TEST_ASSERT_TRUE(can_fetch_quota(status));
    }
    status.window_count = 0;
    apply_credential_failure(status, ErrorCode::Unauthorized);
    TEST_ASSERT_EQUAL(QuotaState::Error, status.quota);
}

static void test_expired_session_navigation()
{
    AppSnapshot before = sample_accounts();
    before.wifi = WifiState::Connected;
    before.status(1).auth = AuthState::Authenticated;
    before.status(2).auth = AuthState::Authenticated;
    AppSnapshot after = before;
    apply_credential_failure(after.status(1), ErrorCode::Unauthorized);
    auto pages = connection_pages(after);
    TEST_ASSERT_EQUAL(Screen::Dashboard, select_screen(after));
    TEST_ASSERT_EQUAL_size_t(3, connection_pages(after).size());
    TEST_ASSERT_EQUAL(1, pages[0]);
    TEST_ASSERT_EQUAL(2, pages[1]);
    TEST_ASSERT_EQUAL(IMPORT_PAGE, pages[2]);
    AccountCarousel carousel;
    carousel.update(2, AppSnapshot{}, before, 0);
    TEST_ASSERT_EQUAL(1, carousel.update(2, before, after, 1000));
    before = after;
    after.status(2).quota = QuotaState::Fresh;
    after.status(2).fetched_at = 100;
    TEST_ASSERT_EQUAL(1, carousel.update(1, before, after, 120000));
    carousel.reset_dwell(121000);
    TEST_ASSERT_EQUAL(2, carousel.update(2, after, after, 130999));
    TEST_ASSERT_EQUAL(2, carousel.update(2, after, after, 131000));
    before = after;
    apply_credential_failure(after.status(2), ErrorCode::Unauthorized);
    TEST_ASSERT_EQUAL(2, carousel.update(1, before, after, 132000));
    TEST_ASSERT_EQUAL(2, carousel.update(IMPORT_PAGE, before, after, 132000));
    TEST_ASSERT_EQUAL(2, carousel.update(2, after, after, 800000));
    TEST_ASSERT_EQUAL_size_t(3, connection_pages(after).size());
    before = after;
    after.status(2).auth = AuthState::Authenticated;
    after.status(1).auth = AuthState::Authenticated;
    after.status(1).quota = QuotaState::Fresh;
    after.status(1).fetched_at = 200;
    TEST_ASSERT_EQUAL(1, carousel.update(2, before, after, 810000));
    after.status(1).auth = AuthState::SignedOut;
    TEST_ASSERT_EQUAL_size_t(2, connection_pages(after).size());
    TEST_ASSERT_EQUAL(2, connection_pages(after)[0]);
}

static void test_account_record_codec_and_upload_targets()
{
    Account account{};
    TEST_ASSERT_TRUE(parse_upload_account("codex", "V29yayBUZWFt", &account));
    TEST_ASSERT_EQUAL_STRING("Work Team", account.name);
    TEST_ASSERT_EQUAL(Provider::OpenAI, account.provider);
    TEST_ASSERT_TRUE(same_account_target(account, Provider::OpenAI, "Work Team"));
    TEST_ASSERT_FALSE(same_account_target(account, Provider::Claude, "Work Team"));
    TEST_ASSERT_FALSE(same_account_target(account, Provider::OpenAI, "work team"));
    TEST_ASSERT_TRUE(parse_upload_account("claude", "V29yayBUZWFt", &account));
    TEST_ASSERT_EQUAL(Provider::Claude, account.provider);
    for (const char *invalid : {"", "YQ==", "Yh", "YQ\n", "AA", "Cg", "IFdvcms", "V29yayA", "w6k", "!"}) {
        TEST_ASSERT_FALSE(parse_upload_account("codex", invalid, &account));
    }
    TEST_ASSERT_FALSE(parse_upload_account("all", "V29yaw", &account));
    TEST_ASSERT_FALSE(parse_upload_account("codex", nullptr, &account));
    char encoded[64];
    char name[34];
    memset(name, 'x', 33);
    name[33] = 0;
    TEST_ASSERT_TRUE(base64url_encode(reinterpret_cast<const uint8_t *>(name), 33, encoded, sizeof(encoded)));
    TEST_ASSERT_FALSE(parse_upload_account("codex", encoded, &account));
    name[32] = 0;
    TEST_ASSERT_TRUE(base64url_encode(reinterpret_cast<const uint8_t *>(name), 32, encoded, sizeof(encoded)));
    TEST_ASSERT_TRUE(parse_upload_account("codex", encoded, &account));

    auto original = std::make_unique<AccountRecord>();
    original->account = {42, Provider::OpenAI, "Work Team", 7};
    original->bundle.version = TokenBundle::VERSION;
    strcpy(original->bundle.oauth.access_token, "synthetic-access");
    strcpy(original->bundle.oauth.refresh_token, "synthetic-refresh");
    strcpy(original->bundle.account_id, "synthetic-provider-id");
    auto decoded = std::make_unique<AccountRecord>();
    std::vector<uint8_t> persisted(sizeof(AccountRecord));
    memcpy(persisted.data(), original.get(), persisted.size());
    TEST_ASSERT_TRUE(decode_account_record(persisted.data(), persisted.size(), decoded.get()));
    TEST_ASSERT_EQUAL(42, decoded->account.id);
    TEST_ASSERT_EQUAL(7, decoded->account.generation);
    TEST_ASSERT_EQUAL_STRING("Work Team", decoded->account.name);
    TEST_ASSERT_EQUAL_MEMORY(&original->bundle, &decoded->bundle, sizeof(TokenBundle));
    TEST_ASSERT_FALSE(decode_account_record(persisted.data(), persisted.size() - 1, decoded.get()));
    original->version = 99;
    TEST_ASSERT_FALSE(decode_account_record(original.get(), sizeof(*original), decoded.get()));
    original->version = AccountRecord::VERSION;
    original->account.provider = Provider::Claude;
    original->bundle.oauth.access_token[0] = 0;
    TEST_ASSERT_TRUE(decode_account_record(original.get(), sizeof(*original), decoded.get()));
    original->bundle.oauth.refresh_token[0] = 0;
    TEST_ASSERT_FALSE(decode_account_record(original.get(), sizeof(*original), decoded.get()));
}

static void test_dynamic_account_pages_and_isolation()
{
    for (size_t count : {1, 10, 20, 22, 300}) {
        AppSnapshot before = sample_accounts(count);
        before.wifi = WifiState::Connected;
        for (auto &entry : before.accounts) {
            entry.status.auth = AuthState::Authenticated;
            entry.status.quota = QuotaState::Fresh;
            entry.status.fetched_at = 100;
            entry.status.window_count = 1;
            entry.status.windows[0] = {"Primary", static_cast<float>(entry.account.id), 2000, true};
        }
        const auto pages = connection_pages(before);
        TEST_ASSERT_EQUAL_size_t(count + 1, pages.size());
        TEST_ASSERT_EQUAL(IMPORT_PAGE, pages.back());
        for (size_t index = 0; index < count; ++index) TEST_ASSERT_EQUAL(index + 1, pages[index]);
        AppSnapshot after = before;
        const AccountId last = static_cast<AccountId>(count);
        after.status(last).fetched_at = 300;
        AccountCarousel carousel;
        carousel.update(1, AppSnapshot{}, before, 0);
        TEST_ASSERT_EQUAL(1, carousel.update(1, before, after, 1000));
        TEST_ASSERT_EQUAL(IMPORT_PAGE, carousel.update(IMPORT_PAGE, after, after, 2000));
        apply_credential_failure(after.status(last), ErrorCode::Unauthorized);
        TEST_ASSERT_EQUAL(last, carousel.update(1, before, after, 3000));
        TEST_ASSERT_EQUAL(QuotaState::Stale, after.status(last).quota);
        TEST_ASSERT_FALSE(can_fetch_quota(after.status(last)));
        TEST_ASSERT_EQUAL_size_t(count + 1, connection_pages(after).size());
        TEST_ASSERT_EQUAL(Screen::Dashboard, select_screen(after));
        char countdown[20];
        for (size_t index = 0; index < count; ++index) {
            const auto id = static_cast<AccountId>(index + 1);
            TEST_ASSERT_EQUAL(2000, after.status(id).windows[0].resets_at);
            format_countdown(after.status(id).windows[0].resets_at, 1990, countdown, sizeof(countdown));
            TEST_ASSERT_EQUAL_STRING("00:00:10", countdown);
            if (id != last) TEST_ASSERT_EQUAL_MEMORY(&before.status(id), &after.status(id), sizeof(ProviderStatus));
        }
        TEST_ASSERT_EQUAL(last, carousel.update(last, after, after, 120000));
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_account_record_codec_and_upload_targets);
    RUN_TEST(test_dynamic_account_pages_and_isolation);
    RUN_TEST(test_oauth_refresh_error_classification);
    RUN_TEST(test_expired_credential_state_and_recovery);
    RUN_TEST(test_expired_session_navigation);
    RUN_TEST(test_base64url_vectors);
    RUN_TEST(test_refresh_request_contracts);
    RUN_TEST(test_oauth_and_usage_parsing);
    RUN_TEST(test_strict_provider_parsing);
    RUN_TEST(test_retry_after_parsing);
    RUN_TEST(test_cached_usage_classification);
    RUN_TEST(test_openai_reset_credits_parsing);
    RUN_TEST(test_format_codex_reset_credits);
    RUN_TEST(test_claude_extended_usage_parsing);
    RUN_TEST(test_dashboard_view_model);
    RUN_TEST(test_reducer_and_portal_helpers);
    RUN_TEST(test_connection_page_order);
    RUN_TEST(test_carousel_active_rotation_and_idle_slots);
    RUN_TEST(test_carousel_usage_growth_detection);
    RUN_TEST(test_carousel_baselines_and_activity_expiry);
    RUN_TEST(test_carousel_idle_single_manual_and_wrap);
    RUN_TEST(test_carousel_account_changes);
    RUN_TEST(test_carousel_active_manual_and_wrap);
    RUN_TEST(test_carousel_scroll_deadline_and_expiration);

    return UNITY_END();
}
#endif
