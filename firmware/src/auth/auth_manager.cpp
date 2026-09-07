#include "auth_manager.h"
#ifdef ESP_PLATFORM
#include "pkce.h"
#include "token_store.h"
#include "../app/app_state.h"
#include "../domain/state_reducer.h"
#include "../net/http_client.h"
#include "../net/time_sync.h"
#include "../providers/claude_protocol.h"
#include "../providers/openai_protocol.h"
#include "../providers/cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace qm {

static constexpr char TAG[] = "quota_auth";

static const char *provider_name(Provider provider)
{
    return provider == Provider::OpenAI ? "Codex" : "Claude";
}

enum class CommandType : uint8_t { Start, Callback, Refresh, Logout };
struct Command { CommandType type; Provider provider; char callback[2049]; };

static QueueHandle_t queue_handle;
static std::atomic_bool cancelled[2];
static TokenBundle bundles[2]{};
static bool have_bundle[2]{};
static bool credentials_rejected[2]{};
static char claude_state[129]{};
static char claude_verifier[129]{};
static int64_t claude_pending_until;
static int64_t next_auto_refresh[2]{};
static HttpResult *http_result;
static OAuthTokens *parsed_tokens;
static TokenBundle *next_bundle;

static size_t index(Provider provider) { return provider == Provider::OpenAI ? 0 : 1; }

static int64_t next_refresh_slot(Provider provider, int64_t earliest)
{
    int64_t minute = (earliest + 59) / 60;
    const int64_t parity = provider == Provider::OpenAI ? 0 : 1;
    if (minute % 2 != parity) ++minute;
    return minute * 60;
}

static void schedule_refresh(Provider provider, int64_t earliest)
{
    const int64_t next = next_refresh_slot(provider, earliest);
    next_auto_refresh[index(provider)] = next;
    ESP_LOGI(TAG, "%s next automatic refresh=%lld", provider_name(provider),
             static_cast<long long>(next));
}

static void log_quota(Provider provider, const ProviderStatus &status)
{
    ESP_LOGI(TAG, "%s quota refreshed plan=%s windows=%u fetched=%lld", provider_name(provider),
             status.plan[0] ? status.plan : "--", status.window_count,
             static_cast<long long>(status.fetched_at));
    for (uint8_t window_index = 0; window_index < status.window_count; ++window_index) {
        const QuotaWindow &window = status.windows[window_index];
        ESP_LOGI(TAG, "%s window[%u] label=%s used=%.1f%% duration=%ldm reset=%lld",
                 provider_name(provider), window_index, window.label,
                 static_cast<double>(window.used_percent), static_cast<long>(window.window_minutes),
                 static_cast<long long>(window.resets_at));
    }
}

static void publish_error(Provider provider, ErrorCode error)
{
    AppSnapshot snapshot = app_state_get();
    ProviderStatus status = provider == Provider::OpenAI ? snapshot.openai : snapshot.claude;
    status.auth = credentials_rejected[index(provider)] ? AuthState::Expired
        : have_bundle[index(provider)] ? AuthState::Authenticated : AuthState::Error;
    status.error = error;
    status.quota = quota_after_failure(status.window_count > 0);
    app_state_set_provider(provider, status);
}

static void post_json(const char *url, const char *body, const char *beta = nullptr)
{
    https_request({HttpMethod::Post, url, body, "application/json", nullptr, nullptr, beta, 16384}, http_result);
}

static bool store_tokens(Provider provider, OAuthTokens &tokens)
{
    size_t i = index(provider);
    secure_clear(next_bundle, sizeof(*next_bundle));
    if (have_bundle[i]) *next_bundle = bundles[i];
    next_bundle->version = TokenBundle::VERSION;
    if (tokens.access_token[0]) snprintf(next_bundle->oauth.access_token, sizeof(next_bundle->oauth.access_token), "%s", tokens.access_token);
    if (tokens.refresh_token[0]) snprintf(next_bundle->oauth.refresh_token, sizeof(next_bundle->oauth.refresh_token), "%s", tokens.refresh_token);
    if (tokens.id_token[0]) snprintf(next_bundle->oauth.id_token, sizeof(next_bundle->oauth.id_token), "%s", tokens.id_token);
    next_bundle->oauth.expires_in = tokens.expires_in;
    next_bundle->refreshed_at = time(nullptr);
    next_bundle->expires_at = next_bundle->refreshed_at + tokens.expires_in;
    if (provider == Provider::OpenAI && next_bundle->oauth.id_token[0]) {
        openai_account_id_from_jwt(next_bundle->oauth.id_token, next_bundle->account_id, sizeof(next_bundle->account_id));
    }
    if (!next_bundle->oauth.access_token[0] || !next_bundle->oauth.refresh_token[0] ||
        (provider == Provider::OpenAI && !next_bundle->account_id[0])) {
        secure_clear(next_bundle, sizeof(*next_bundle));
        publish_error(provider, ErrorCode::InvalidResponse);
        return false;
    }
    if (token_store_save(provider, *next_bundle) != ESP_OK) {
        secure_clear(next_bundle, sizeof(*next_bundle));
        publish_error(provider, ErrorCode::Storage);
        return false;
    }
    secure_clear(&bundles[i], sizeof(bundles[i]));
    bundles[i] = *next_bundle;
    have_bundle[i] = true;
    credentials_rejected[i] = false;
    secure_clear(next_bundle, sizeof(*next_bundle));
    return true;
}

static bool refresh_token(Provider provider)
{
    size_t i = index(provider);
    if (!have_bundle[i]) return false;
    ProviderStatus status = provider == Provider::OpenAI ? app_state_get().openai : app_state_get().claude;
    status.auth = AuthState::Refreshing;
    app_state_set_provider(provider, status);
    char body[4600];
    bool built = provider == Provider::OpenAI
        ? openai_refresh_request(bundles[i].oauth.refresh_token, body, sizeof(body))
        : claude_refresh_request(bundles[i].oauth.refresh_token, body, sizeof(body));
    if (!built) {
        publish_error(provider, ErrorCode::InvalidResponse);
        return false;
    }
    post_json(provider == Provider::OpenAI ? "https://auth.openai.com/oauth/token" : "https://platform.claude.com/v1/oauth/token",
              body, provider == Provider::Claude ? CLAUDE_BETA : nullptr);
    secure_clear(body, sizeof(body));
    if (http_result->status != 200 || http_result->error != ErrorCode::None) {
        const ErrorCode error = oauth_refresh_error(*http_result);
        const uint32_t retry_after = http_result->retry_after;
        ESP_LOGW(TAG, "%s token refresh rejected http=%d error=%u", provider_name(provider),
                 http_result->status, static_cast<unsigned>(error));
        secure_clear(http_result, sizeof(*http_result));
        apply_credential_failure(status, error);
        credentials_rejected[i] = status.auth == AuthState::Expired;
        app_state_set_provider(provider, status);
        schedule_refresh(provider, time(nullptr) + (retry_after > 60 ? retry_after : 60));
        return false;
    }
    secure_clear(parsed_tokens, sizeof(*parsed_tokens));
    bool parsed = parse_oauth_tokens(http_result->body, http_result->body_len, parsed_tokens, false);
    secure_clear(http_result, sizeof(*http_result));
    if (!parsed) {
        publish_error(provider, ErrorCode::InvalidResponse);
        return false;
    }
    bool saved = store_tokens(provider, *parsed_tokens);
    secure_clear(parsed_tokens, sizeof(*parsed_tokens));
    if (!saved) return false;
    status.auth = AuthState::Authenticated;
    status.error = ErrorCode::None;
    app_state_set_provider(provider, status);
    return true;
}

static void fetch_quota(Provider provider)
{
    size_t i = index(provider);
    AppSnapshot snapshot = app_state_get();
    const ProviderStatus &before = provider == Provider::OpenAI ? snapshot.openai : snapshot.claude;
    if (!have_bundle[i] || credentials_rejected[i] || !can_fetch_quota(before)) return;
    ESP_LOGI(TAG, "%s quota refresh starting", provider_name(provider));
    schedule_refresh(provider, time(nullptr) + 60);
    if (bundles[i].expires_at <= time(nullptr) + 300 && !refresh_token(provider)) {
        ESP_LOGW(TAG, "%s token refresh failed", provider_name(provider));
        return;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        char bearer[4110];
        int count = snprintf(bearer, sizeof(bearer), "Bearer %s", bundles[i].oauth.access_token);
        if (count <= 0 || static_cast<size_t>(count) >= sizeof(bearer)) {
            publish_error(provider, ErrorCode::InvalidResponse);
            ESP_LOGE(TAG, "%s bearer header could not be created", provider_name(provider));
            schedule_refresh(provider, time(nullptr) + 60);
            return;
        }
        HttpRequest request{HttpMethod::Get,
            provider == Provider::OpenAI ? "https://chatgpt.com/backend-api/wham/usage" : "https://api.anthropic.com/api/oauth/usage",
            nullptr, nullptr, bearer, provider == Provider::OpenAI ? bundles[i].account_id : nullptr,
            provider == Provider::Claude ? CLAUDE_BETA : nullptr, 16384};
        https_request(request, http_result);
        secure_clear(bearer, sizeof(bearer));
        if (http_result->status == 401 && attempt == 0) {
            ESP_LOGW(TAG, "%s quota request unauthorized; refreshing token", provider_name(provider));
            secure_clear(http_result, sizeof(*http_result));
            if (refresh_token(provider)) continue;
            return;
        }
        break;
    }

    snapshot = app_state_get();
    ProviderStatus status = provider == Provider::OpenAI ? snapshot.openai : snapshot.claude;
    if (http_result->status != 200 || http_result->error != ErrorCode::None) {
        const int http_status = http_result->status;
        ErrorCode error = http_result->error;
        uint32_t retry_after = http_result->retry_after;
        secure_clear(http_result, sizeof(*http_result));
        apply_credential_failure(status, error);
        credentials_rejected[i] = status.auth == AuthState::Expired;
        app_state_set_provider(provider, status);
        const int64_t earliest = time(nullptr) + (retry_after > 60 ? retry_after : 60);
        ESP_LOGW(TAG, "%s quota refresh failed http=%d error=%u retry_after=%u",
                 provider_name(provider), http_status, static_cast<unsigned>(error), retry_after);
        schedule_refresh(provider, earliest);
        return;
    }
    ProviderStatus parsed_status = status;
    bool parsed = provider == Provider::OpenAI
        ? parse_openai_usage(http_result->body, http_result->body_len, &parsed_status)
        : parse_claude_usage(http_result->body, http_result->body_len, &parsed_status);
    secure_clear(http_result, sizeof(*http_result));
    if (!parsed) {
        status.error = ErrorCode::InvalidResponse;
        status.quota = quota_after_failure(status.window_count > 0);
        ESP_LOGW(TAG, "%s quota response could not be parsed", provider_name(provider));
    } else {
        status = parsed_status;
        status.auth = AuthState::Authenticated;
        status.quota = QuotaState::Fresh;
        status.error = ErrorCode::None;
        status.fetched_at = time(nullptr);
        const esp_err_t stored = quota_store_save(provider, status);
        if (stored != ESP_OK) {
            ESP_LOGW(TAG, "%s quota cache save failed error=%d", provider_name(provider), stored);
        }
        log_quota(provider, status);
    }
    schedule_refresh(provider, time(nullptr) + 60);
    app_state_set_provider(provider, status);
}

static void start_openai()
{
    cancelled[0] = false;
    ProviderStatus status{};
    status.auth = AuthState::Starting;
    app_state_set_provider(Provider::OpenAI, status);
    char body[128];
    if (!openai_user_code_request(body, sizeof(body))) {
        publish_error(Provider::OpenAI, ErrorCode::InvalidResponse);
        return;
    }
    post_json("https://auth.openai.com/api/accounts/deviceauth/usercode", body);
    secure_clear(body, sizeof(body));
    if (http_result->status != 200 || http_result->error != ErrorCode::None) {
        ErrorCode error = http_result->error;
        secure_clear(http_result, sizeof(*http_result));
        publish_error(Provider::OpenAI, error);
        return;
    }
    OpenAiDeviceCode device{};
    bool parsed = parse_openai_device_code(http_result->body, http_result->body_len, &device);
    secure_clear(http_result, sizeof(*http_result));
    if (!parsed) {
        publish_error(Provider::OpenAI, ErrorCode::InvalidResponse);
        return;
    }
    status.auth = AuthState::AwaitingUser;
    snprintf(status.login_url, sizeof(status.login_url), "%s", OPENAI_DEVICE_URL);
    snprintf(status.user_code, sizeof(status.user_code), "%s", device.user_code);
    app_state_set_provider(Provider::OpenAI, status);
    int64_t deadline = time(nullptr) + 900;
    while (time(nullptr) < deadline && !cancelled[0]) {
        vTaskDelay(pdMS_TO_TICKS(device.interval * 1000));
        char poll_body[512];
        if (!openai_poll_request(device, poll_body, sizeof(poll_body))) break;
        post_json("https://auth.openai.com/api/accounts/deviceauth/token", poll_body);
        secure_clear(poll_body, sizeof(poll_body));
        if (http_result->status == 403 || http_result->status == 404) {
            secure_clear(http_result, sizeof(*http_result));
            continue;
        }
        if (http_result->status != 200 || http_result->error != ErrorCode::None) {
            ErrorCode error = http_result->error;
            secure_clear(http_result, sizeof(*http_result));
            publish_error(Provider::OpenAI, error);
            return;
        }
        cJSON *root = cJSON_ParseWithLength(http_result->body, http_result->body_len);
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "authorization_code");
        cJSON *challenge = cJSON_GetObjectItemCaseSensitive(root, "code_challenge");
        cJSON *verifier = cJSON_GetObjectItemCaseSensitive(root, "code_verifier");
        if (!cJSON_IsString(code) || !cJSON_IsString(challenge) || !cJSON_IsString(verifier) ||
            strlen(code->valuestring) > 4096 || strlen(verifier->valuestring) > 128) {
            cJSON_Delete(root);
            secure_clear(http_result, sizeof(*http_result));
            publish_error(Provider::OpenAI, ErrorCode::InvalidResponse);
            return;
        }
        char exchange[4600];
        bool built = openai_exchange_request(code->valuestring, verifier->valuestring, exchange, sizeof(exchange));
        cJSON_Delete(root);
        secure_clear(http_result, sizeof(*http_result));
        if (!built) {
            publish_error(Provider::OpenAI, ErrorCode::InvalidResponse);
            return;
        }
        https_request({HttpMethod::Post, "https://auth.openai.com/oauth/token", exchange,
                       "application/x-www-form-urlencoded", nullptr, nullptr, nullptr, 16384}, http_result);
        secure_clear(exchange, sizeof(exchange));
        if (http_result->status != 200 || http_result->error != ErrorCode::None) {
            ErrorCode error = http_result->error;
            secure_clear(http_result, sizeof(*http_result));
            publish_error(Provider::OpenAI, error);
            return;
        }
        secure_clear(parsed_tokens, sizeof(*parsed_tokens));
        parsed = parse_oauth_tokens(http_result->body, http_result->body_len, parsed_tokens, true);
        secure_clear(http_result, sizeof(*http_result));
        if (!parsed) {
            publish_error(Provider::OpenAI, ErrorCode::InvalidResponse);
            return;
        }
        bool saved = store_tokens(Provider::OpenAI, *parsed_tokens);
        secure_clear(parsed_tokens, sizeof(*parsed_tokens));
        if (!saved) return;
        status.auth = AuthState::Authenticated;
        status.error = ErrorCode::None;
        app_state_set_provider(Provider::OpenAI, status);
        fetch_quota(Provider::OpenAI);
        return;
    }
    publish_error(Provider::OpenAI, cancelled[0] ? ErrorCode::None : ErrorCode::Timeout);
}

static void start_claude()
{
    cancelled[1] = false;
    ProviderStatus status{};
    status.auth = AuthState::Starting;
    app_state_set_provider(Provider::Claude, status);
    char challenge[64];
    if (!pkce_generate(claude_verifier, sizeof(claude_verifier), claude_state, sizeof(claude_state), challenge, sizeof(challenge)) ||
        !claude_authorize_url(challenge, claude_state, status.login_url, sizeof(status.login_url))) {
        secure_clear(claude_verifier, sizeof(claude_verifier));
        secure_clear(claude_state, sizeof(claude_state));
        publish_error(Provider::Claude, ErrorCode::InvalidResponse);
        return;
    }
    claude_pending_until = time(nullptr) + 600;
    status.auth = AuthState::AwaitingUser;
    app_state_set_provider(Provider::Claude, status);
}

static void claude_callback(const char *callback)
{
    if (time(nullptr) > claude_pending_until || cancelled[1]) {
        publish_error(Provider::Claude, ErrorCode::Timeout);
        return;
    }
    char code[1025];
    if (!claude_parse_callback(callback, claude_state, code, sizeof(code))) {
        publish_error(Provider::Claude, ErrorCode::StateMismatch);
        return;
    }
    ProviderStatus status = app_state_get().claude;
    status.auth = AuthState::Exchanging;
    app_state_set_provider(Provider::Claude, status);
    char body[5000];
    bool built = claude_exchange_request(code, claude_state, claude_verifier, body, sizeof(body));
    secure_clear(code, sizeof(code));
    secure_clear(claude_verifier, sizeof(claude_verifier));
    secure_clear(claude_state, sizeof(claude_state));
    if (!built) {
        publish_error(Provider::Claude, ErrorCode::InvalidResponse);
        return;
    }
    post_json("https://platform.claude.com/v1/oauth/token", body, CLAUDE_BETA);
    secure_clear(body, sizeof(body));
    if (http_result->status != 200 || http_result->error != ErrorCode::None) {
        ErrorCode error = http_result->error;
        secure_clear(http_result, sizeof(*http_result));
        publish_error(Provider::Claude, error);
        return;
    }
    secure_clear(parsed_tokens, sizeof(*parsed_tokens));
    bool parsed = parse_oauth_tokens(http_result->body, http_result->body_len, parsed_tokens, true);
    secure_clear(http_result, sizeof(*http_result));
    if (!parsed) {
        publish_error(Provider::Claude, ErrorCode::InvalidResponse);
        return;
    }
    bool saved = store_tokens(Provider::Claude, *parsed_tokens);
    secure_clear(parsed_tokens, sizeof(*parsed_tokens));
    if (!saved) return;
    status.auth = AuthState::Authenticated;
    status.error = ErrorCode::None;
    app_state_set_provider(Provider::Claude, status);
    fetch_quota(Provider::Claude);
}

static bool enqueue(Command &command)
{
    return queue_handle && xQueueSend(queue_handle, &command, pdMS_TO_TICKS(50)) == pdTRUE;
}

void auth_manager_init()
{
    queue_handle = xQueueCreate(8, sizeof(Command));
    http_result = static_cast<HttpResult *>(heap_caps_calloc(1, sizeof(HttpResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    parsed_tokens = static_cast<OAuthTokens *>(heap_caps_calloc(1, sizeof(OAuthTokens), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    next_bundle = static_cast<TokenBundle *>(heap_caps_calloc(1, sizeof(TokenBundle), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!queue_handle || !http_result || !parsed_tokens || !next_bundle) abort();
}

bool auth_enqueue_start(Provider provider)
{
    Command command{};
    command.type = CommandType::Start;
    command.provider = provider;
    return enqueue(command);
}

bool auth_enqueue_callback(const char *callback)
{
    if (!callback || strlen(callback) > 2048) return false;
    Command command{};
    command.type = CommandType::Callback;
    command.provider = Provider::Claude;
    snprintf(command.callback, sizeof(command.callback), "%s", callback);
    bool ok = enqueue(command);
    secure_clear(command.callback, sizeof(command.callback));
    return ok;
}

bool auth_enqueue_refresh(Provider provider)
{
    Command command{};
    command.type = CommandType::Refresh;
    command.provider = provider;
    return enqueue(command);
}

bool auth_enqueue_logout(Provider provider)
{
    cancelled[index(provider)] = true;
    Command command{};
    command.type = CommandType::Logout;
    command.provider = provider;
    return enqueue(command);
}

void auth_worker_task(void *)
{
    for (Provider provider : {Provider::OpenAI, Provider::Claude}) {
        size_t i = index(provider);
        if (token_store_load(provider, &bundles[i]) == ESP_OK) {
            if (provider == Provider::OpenAI && !bundles[i].account_id[0]) {
                token_store_remove(provider);
                secure_clear(&bundles[i], sizeof(bundles[i]));
                continue;
            }
            have_bundle[i] = true;
            ProviderStatus status{};
            if (quota_store_load(provider, &status) == ESP_OK) status.quota = QuotaState::Stale;
            status.auth = AuthState::Authenticated;
            app_state_set_provider(provider, status);
            ESP_LOGI(TAG, "%s credentials loaded cached_windows=%u fetched=%lld",
                     provider_name(provider), status.window_count,
                     static_cast<long long>(status.fetched_at));
        }
    }
    int64_t last_wall_time = 0;
    while (true) {
        Command command{};
        if (xQueueReceive(queue_handle, &command, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (command.type == CommandType::Logout) {
                size_t i = index(command.provider);
                token_store_remove(command.provider);
                secure_clear(&bundles[i], sizeof(bundles[i]));
                have_bundle[i] = false;
                credentials_rejected[i] = false;
                next_auto_refresh[i] = 0;
                ProviderStatus status{};
                status.auth = AuthState::SignedOut;
                app_state_set_provider(command.provider, status);
                continue;
            }
            if (command.type == CommandType::Refresh && credentials_rejected[index(command.provider)]) continue;
            if (app_state_get().wifi != WifiState::Connected || time_sync_start_and_wait() != ESP_OK) {
                publish_error(command.provider, ErrorCode::Network);
                secure_clear(command.callback, sizeof(command.callback));
                continue;
            }
            if (command.type == CommandType::Start) {
                command.provider == Provider::OpenAI ? start_openai() : start_claude();
            } else if (command.type == CommandType::Callback) {
                claude_callback(command.callback);
                secure_clear(command.callback, sizeof(command.callback));
            } else if (command.type == CommandType::Refresh) {
                fetch_quota(command.provider);
            }
        } else if (app_state_get().wifi == WifiState::Connected && time_sync_start_and_wait() == ESP_OK) {
            int64_t now = time(nullptr);
            if (last_wall_time > 0 && now < last_wall_time) {
                ESP_LOGW(TAG, "Wall clock moved backward from %lld to %lld; resetting refresh schedule",
                         static_cast<long long>(last_wall_time), static_cast<long long>(now));
                next_auto_refresh[0] = 0;
                next_auto_refresh[1] = 0;
            }
            last_wall_time = now;
            Provider provider = (now / 60) % 2 == 0 ? Provider::OpenAI : Provider::Claude;
            size_t i = index(provider);
            if (have_bundle[i] && !credentials_rejected[i] && now >= next_auto_refresh[i]) fetch_quota(provider);
        }
    }
}

} // namespace qm
#endif
