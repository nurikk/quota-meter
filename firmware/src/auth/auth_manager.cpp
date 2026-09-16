#include "auth_manager.h"
#ifdef ESP_PLATFORM
#include "token_store.h"
#include "../app/app_state.h"
#include "../domain/state_reducer.h"
#include "../net/http_client.h"
#include "../net/time_sync.h"
#include "../providers/claude_protocol.h"
#include "../providers/openai_protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace qm {

static constexpr char TAG[] = "quota_auth";

struct ManagedAccount {
    Account identity;
    TokenBundle bundle{};
    bool have_bundle{};
    bool credentials_rejected{};
    int64_t next_auto_refresh{};
};
static std::vector<std::unique_ptr<ManagedAccount>> accounts;
static HttpResult *http_result;
static OAuthTokens *parsed_tokens;
static TokenBundle *next_bundle;

static int64_t next_refresh_slot(ManagedAccount &account, int64_t earliest)
{
    int64_t minute = (earliest + 59) / 60;
    const int64_t parity = account.identity.provider == Provider::OpenAI ? 0 : 1;
    if (minute % 2 != parity) ++minute;
    return minute * 60;
}

static void schedule_refresh(ManagedAccount &account, int64_t earliest)
{
    const int64_t next = next_refresh_slot(account, earliest);
    account.next_auto_refresh = next;
    ESP_LOGI(TAG, "%s - %s next automatic refresh=%lld", provider_name(account.identity.provider), account.identity.name,
             static_cast<long long>(next));
}

static void log_quota(ManagedAccount &account, const ProviderStatus &status)
{
    ESP_LOGI(TAG, "%s - %s quota refreshed plan=%s windows=%u fetched=%lld", provider_name(account.identity.provider), account.identity.name,
             status.plan[0] ? status.plan : "--", status.window_count,
             static_cast<long long>(status.fetched_at));
    for (uint8_t window_index = 0; window_index < status.window_count; ++window_index) {
        const QuotaWindow &window = status.windows[window_index];
        ESP_LOGI(TAG, "%s - %s window[%u] label=%s used=%.1f%% duration=%ldm reset=%lld",
                 provider_name(account.identity.provider), account.identity.name, window_index, window.label,
                 static_cast<double>(window.used_percent), static_cast<long>(window.window_minutes),
                 static_cast<long long>(window.resets_at));
    }
}

static void publish_error(ManagedAccount &account, ErrorCode error)
{
    const AppSnapshot snapshot = app_state_get();
    ProviderStatus status = snapshot.status(account.identity.id);
    status.auth = account.credentials_rejected ? AuthState::Expired
        : account.have_bundle ? AuthState::Authenticated : AuthState::SignedOut;
    status.error = error;
    status.quota = quota_after_failure(has_cached_usage(status));
    app_state_set_account(account.identity, status);
}

static void post_json(const char *url, const char *body, const char *beta = nullptr)
{
    https_request({HttpMethod::Post, url, body, "application/json", nullptr, nullptr, beta, 16384}, http_result);
}

static bool store_refreshed_tokens(ManagedAccount &account, const OAuthTokens &tokens)
{
    secure_clear(next_bundle, sizeof(*next_bundle));
    *next_bundle = account.bundle;
    if (tokens.access_token[0]) {
        snprintf(next_bundle->oauth.access_token, sizeof(next_bundle->oauth.access_token), "%s",
                 tokens.access_token);
    }
    if (tokens.refresh_token[0]) {
        snprintf(next_bundle->oauth.refresh_token, sizeof(next_bundle->oauth.refresh_token), "%s",
                 tokens.refresh_token);
    }
    if (tokens.id_token[0]) {
        snprintf(next_bundle->oauth.id_token, sizeof(next_bundle->oauth.id_token), "%s", tokens.id_token);
    }
    next_bundle->oauth.expires_in = tokens.expires_in;
    next_bundle->refreshed_at = time(nullptr);
    next_bundle->expires_at = next_bundle->refreshed_at + tokens.expires_in;
    if (account.identity.provider == Provider::OpenAI && next_bundle->oauth.id_token[0]) {
        openai_account_id_from_jwt(next_bundle->oauth.id_token, next_bundle->account_id,
                                   sizeof(next_bundle->account_id));
    }
    if (!next_bundle->oauth.access_token[0] || !next_bundle->oauth.refresh_token[0] ||
        (account.identity.provider == Provider::OpenAI && !next_bundle->account_id[0])) {
        secure_clear(next_bundle, sizeof(*next_bundle));
        publish_error(account, ErrorCode::InvalidResponse);
        return false;
    }
    if (token_store_save(account.identity, *next_bundle) != ESP_OK) {
        secure_clear(next_bundle, sizeof(*next_bundle));
        publish_error(account, ErrorCode::Storage);
        return false;
    }
    secure_clear(&account.bundle, sizeof(account.bundle));
    account.bundle = *next_bundle;
    account.credentials_rejected = false;
    secure_clear(next_bundle, sizeof(*next_bundle));
    return true;
}

static bool refresh_token(ManagedAccount &account)
{
    if (!account.have_bundle) return false;
    ProviderStatus status = app_state_get().status(account.identity.id);
    status.auth = AuthState::Refreshing;
    app_state_set_account(account.identity, status);

    char body[4600];
    const bool built = account.identity.provider == Provider::OpenAI
        ? openai_refresh_request(account.bundle.oauth.refresh_token, body, sizeof(body))
        : claude_refresh_request(account.bundle.oauth.refresh_token, body, sizeof(body));
    if (!built) {
        publish_error(account, ErrorCode::InvalidResponse);
        return false;
    }
    post_json(account.identity.provider == Provider::OpenAI
                  ? "https://auth.openai.com/oauth/token"
                  : "https://platform.claude.com/v1/oauth/token",
              body, account.identity.provider == Provider::Claude ? CLAUDE_BETA : nullptr);
    secure_clear(body, sizeof(body));
    if (http_result->status != 200 || http_result->error != ErrorCode::None) {
        const ErrorCode error = oauth_refresh_error(*http_result);
        const uint32_t retry_after = http_result->retry_after;
        ESP_LOGW(TAG, "%s - %s token refresh rejected http=%d error=%u", provider_name(account.identity.provider), account.identity.name,
                 http_result->status, static_cast<unsigned>(error));
        secure_clear(http_result, sizeof(*http_result));
        apply_credential_failure(status, error);
        account.credentials_rejected = status.auth == AuthState::Expired;
        app_state_set_account(account.identity, status);
        schedule_refresh(account, time(nullptr) + (retry_after > 60 ? retry_after : 60));
        return false;
    }

    secure_clear(parsed_tokens, sizeof(*parsed_tokens));
    const bool parsed = parse_oauth_tokens(http_result->body, http_result->body_len,
                                           parsed_tokens, false);
    secure_clear(http_result, sizeof(*http_result));
    if (!parsed) {
        publish_error(account, ErrorCode::InvalidResponse);
        return false;
    }
    const bool saved = store_refreshed_tokens(account, *parsed_tokens);
    secure_clear(parsed_tokens, sizeof(*parsed_tokens));
    if (!saved) return false;
    status.auth = AuthState::Authenticated;
    status.error = ErrorCode::None;
    app_state_set_account(account.identity, status);
    return true;
}

static void fetch_quota(ManagedAccount &account)
{
    AppSnapshot snapshot = app_state_get();
    const ProviderStatus &before = snapshot.status(account.identity.id);
    if (!account.have_bundle || account.credentials_rejected || !can_fetch_quota(before)) return;
    ESP_LOGI(TAG, "%s - %s quota refresh starting", provider_name(account.identity.provider), account.identity.name);
    schedule_refresh(account, time(nullptr) + 60);
    if (account.bundle.expires_at <= time(nullptr) + 300 && !refresh_token(account)) {
        ESP_LOGW(TAG, "%s - %s token refresh failed", provider_name(account.identity.provider), account.identity.name);
        return;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        char bearer[4110];
        const int count = snprintf(bearer, sizeof(bearer), "Bearer %s",
                                   account.bundle.oauth.access_token);
        if (count <= 0 || static_cast<size_t>(count) >= sizeof(bearer)) {
            publish_error(account, ErrorCode::InvalidResponse);
            schedule_refresh(account, time(nullptr) + 60);
            return;
        }
        const HttpRequest request{
            HttpMethod::Get,
            account.identity.provider == Provider::OpenAI
                ? "https://chatgpt.com/backend-api/wham/usage"
                : "https://api.anthropic.com/api/oauth/usage",
            nullptr,
            nullptr,
            bearer,
            account.identity.provider == Provider::OpenAI ? account.bundle.account_id : nullptr,
            account.identity.provider == Provider::Claude ? CLAUDE_BETA : nullptr,
            16384,
        };
        https_request(request, http_result);
        secure_clear(bearer, sizeof(bearer));
        if (http_result->status == 401 && attempt == 0) {
            secure_clear(http_result, sizeof(*http_result));
            if (refresh_token(account)) continue;
            return;
        }
        break;
    }

    snapshot = app_state_get();
    ProviderStatus status = snapshot.status(account.identity.id);
    if (http_result->status != 200 || http_result->error != ErrorCode::None) {
        const int http_status = http_result->status;
        const ErrorCode error = http_result->error;
        const uint32_t retry_after = http_result->retry_after;
        secure_clear(http_result, sizeof(*http_result));
        apply_credential_failure(status, error);
        account.credentials_rejected = status.auth == AuthState::Expired;
        app_state_set_account(account.identity, status);
        ESP_LOGW(TAG, "%s - %s quota refresh failed http=%d error=%u retry_after=%u",
                 provider_name(account.identity.provider), account.identity.name, http_status, static_cast<unsigned>(error), retry_after);
        schedule_refresh(account, time(nullptr) + (retry_after > 60 ? retry_after : 60));
        return;
    }

    ProviderStatus parsed_status = status;
    const bool parsed = account.identity.provider == Provider::OpenAI
        ? parse_openai_usage(http_result->body, http_result->body_len, &parsed_status)
        : parse_claude_usage(http_result->body, http_result->body_len, &parsed_status);
    secure_clear(http_result, sizeof(*http_result));
    if (!parsed) {
        status.error = ErrorCode::InvalidResponse;
        status.quota = quota_after_failure(has_cached_usage(status));
    } else {
        status = parsed_status;
        status.auth = AuthState::Authenticated;
        status.quota = QuotaState::Fresh;
        status.error = ErrorCode::None;
        status.fetched_at = time(nullptr);
        const esp_err_t stored = quota_store_save(account.identity, status);
        if (stored != ESP_OK) {
            ESP_LOGW(TAG, "%s - %s quota cache save failed error=%d", provider_name(account.identity.provider), account.identity.name, stored);
        }
        log_quota(account, status);
    }
    schedule_refresh(account, time(nullptr) + 60);
    app_state_set_account(account.identity, status);
}

void auth_manager_init()
{
    ESP_ERROR_CHECK(token_store_init());
    http_result = static_cast<HttpResult *>(
        heap_caps_calloc(1, sizeof(HttpResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    parsed_tokens = static_cast<OAuthTokens *>(
        heap_caps_calloc(1, sizeof(OAuthTokens), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    next_bundle = static_cast<TokenBundle *>(
        heap_caps_calloc(1, sizeof(TokenBundle), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!http_result || !parsed_tokens || !next_bundle) abort();
}

void auth_worker_task(void *)
{
    std::vector<Account> stored_accounts;
    ESP_ERROR_CHECK(token_store_accounts(&stored_accounts));
    for (const auto &identity : stored_accounts) {
        std::unique_ptr<ManagedAccount> entry(new ManagedAccount{});
        entry->identity = identity;
        if (token_store_load(identity, &entry->bundle) != ESP_OK) {
            ESP_LOGE(TAG, "Account credentials could not be loaded");
            continue;
        }
        entry->have_bundle = true;
        ProviderStatus status{};
        if (quota_store_load(identity, &status) == ESP_OK) status.quota = QuotaState::Stale;
        status.auth = AuthState::Authenticated;
        app_state_set_account(identity, status);
        accounts.push_back(std::move(entry));
    }

    int64_t last_wall_time = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (app_state_get().wifi != WifiState::Connected || time_sync_start_and_wait() != ESP_OK) continue;
        const int64_t now = time(nullptr);
        if (last_wall_time > 0 && now < last_wall_time) {
            ESP_LOGW(TAG, "Wall clock moved backward from %lld to %lld; resetting refresh schedule",
                     static_cast<long long>(last_wall_time), static_cast<long long>(now));
            for (auto &entry : accounts) entry->next_auto_refresh = 0;
        }
        last_wall_time = now;
        ManagedAccount *next = nullptr;
        for (auto &entry : accounts) {
            ManagedAccount &account = *entry;
            const int64_t parity = account.identity.provider == Provider::OpenAI ? 0 : 1;
            if ((now / 60) % 2 == parity && account.have_bundle &&
                !account.credentials_rejected && now >= account.next_auto_refresh &&
                (!next || account.next_auto_refresh < next->next_auto_refresh)) {
                next = &account;
            }
        }
        if (next) fetch_quota(*next);
    }
}

} // namespace qm
#endif
