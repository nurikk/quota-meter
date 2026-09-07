#include "view_model.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
namespace qm {
void format_provider_summary(const ProviderStatus &status, char *output, size_t length)
{
    const char *auth = status.auth == AuthState::Authenticated
                           ? "Connected"
                           : status.auth == AuthState::Refreshing
                                 ? "Refreshing"
                                 : status.auth == AuthState::Starting || status.auth == AuthState::Exchanging
                                       ? "Working"
                                       : status.auth == AuthState::AwaitingUser
                                             ? "Waiting for login"
                                             : status.auth == AuthState::Expired ? "Session expired"
                                             : status.auth == AuthState::Error ? "Error" : "Sign in required";
    const char *freshness = status.quota == QuotaState::Stale
                                ? "stale"
                                : status.quota == QuotaState::Fresh ? "fresh" : "no quota";
    snprintf(output, length, "%s | %s%s%s", auth, freshness, status.plan[0] ? " | " : "", status.plan);
}

static bool is_connected(const ProviderStatus &status)
{
    return status.auth == AuthState::Authenticated || status.auth == AuthState::Refreshing;
}

size_t connection_pages(const AppSnapshot &snapshot, ConnectionPage *pages, size_t capacity)
{
    ConnectionPage ordered[3];
    size_t count = 0;
    if (is_connected(snapshot.openai) || snapshot.openai.auth == AuthState::Expired) ordered[count++] = ConnectionPage::Codex;
    if (is_connected(snapshot.claude) || snapshot.claude.auth == AuthState::Expired) ordered[count++] = ConnectionPage::Claude;
    ordered[count++] = ConnectionPage::Add;

    const size_t copied = count < capacity ? count : capacity;
    for (size_t index = 0; index < copied; ++index) pages[index] = ordered[index];
    return count;
}

static bool same_label(const char *left, const char *right)
{
    while (*left || *right) {
        while (*left && !isalnum(static_cast<unsigned char>(*left))) ++left;
        while (*right && !isalnum(static_cast<unsigned char>(*right))) ++right;
        if (tolower(static_cast<unsigned char>(*left)) != tolower(static_cast<unsigned char>(*right))) return false;
        if (*left) ++left;
        if (*right) ++right;
    }
    return true;
}

const QuotaWindow *find_quota_window(const ProviderStatus &status, const char *semantic_label)
{
    if (!semantic_label) return nullptr;
    for (uint8_t index = 0; index < status.window_count; ++index) {
        if (status.windows[index].present && same_label(status.windows[index].label, semantic_label)) return &status.windows[index];
    }
    return nullptr;
}

const QuotaWindow *find_model_limit(const ProviderStatus &status, const char *preferred_label)
{
    const QuotaWindow *fallback = nullptr;
    for (uint8_t index = 0; index < status.window_count; ++index) {
        const QuotaWindow &window = status.windows[index];
        if (!window.present || !window.model_limit) continue;
        if (preferred_label && same_label(window.label, preferred_label)) return &window;
        if (!fallback) fallback = &window;
    }
    return fallback;
}

const QuotaWindow *find_quota_window_by_duration(const ProviderStatus &status, int32_t window_minutes)
{
    for (uint8_t index = 0; index < status.window_count; ++index) {
        const QuotaWindow &window = status.windows[index];
        if (window.present && window.window_minutes == window_minutes) return &window;
    }
    return nullptr;
}

void format_window_period(const QuotaWindow *window, const char *fallback, char *output, size_t length)
{
    if (!output || length == 0) return;
    const int32_t minutes = window ? window->window_minutes : 0;
    if (minutes > 0 && minutes % 1440 == 0) snprintf(output, length, "%ldD", static_cast<long>(minutes / 1440));
    else if (minutes > 0 && minutes % 60 == 0) snprintf(output, length, "%ldH", static_cast<long>(minutes / 60));
    else if (minutes > 0) snprintf(output, length, "%ldM", static_cast<long>(minutes));
    else snprintf(output, length, "%s", fallback ? fallback : "--");
}

static bool quota_updated(const ProviderStatus &before, const ProviderStatus &after)
{
    return after.quota == QuotaState::Fresh && after.fetched_at > 0 &&
           after.fetched_at != before.fetched_at;
}

ConnectionPage focus_after_usage_change(ConnectionPage current, const AppSnapshot &previous,
                                        const AppSnapshot &current_snapshot)
{
    const auto login_active = [](AuthState auth) {
        return auth == AuthState::Starting || auth == AuthState::AwaitingUser || auth == AuthState::Exchanging;
    };
    if (login_active(current_snapshot.openai.auth) || login_active(current_snapshot.claude.auth)) return current;
    if (current_snapshot.openai.auth == AuthState::Expired && previous.openai.auth != AuthState::Expired)
        return ConnectionPage::Codex;
    if (current_snapshot.claude.auth == AuthState::Expired && previous.claude.auth != AuthState::Expired)
        return ConnectionPage::Claude;
    if ((current == ConnectionPage::Codex && current_snapshot.openai.auth == AuthState::Expired) ||
        (current == ConnectionPage::Claude && current_snapshot.claude.auth == AuthState::Expired)) return current;
    if (current == ConnectionPage::Add) return current;
    const bool openai_updated = quota_updated(previous.openai, current_snapshot.openai);
    const bool claude_updated = quota_updated(previous.claude, current_snapshot.claude);
    if (!openai_updated && !claude_updated) return current;
    if (openai_updated && claude_updated) {
        return current_snapshot.claude.fetched_at > current_snapshot.openai.fetched_at
                   ? ConnectionPage::Claude : ConnectionPage::Codex;
    }
    return openai_updated ? ConnectionPage::Codex : ConnectionPage::Claude;
}

float elapsed_percent(const QuotaWindow &window, int64_t now, int window_minutes)
{
    if (window.resets_at <= 0 || window_minutes <= 0) return 0;
    const int64_t duration = static_cast<int64_t>(window_minutes) * 60;
    const int64_t elapsed = now - (window.resets_at - duration);
    if (elapsed <= 0) return 0;
    if (elapsed >= duration) return 100;
    return static_cast<float>(elapsed * 100.0 / duration);
}

UsageRisk usage_risk(float used, float elapsed)
{
    const float lead = used - elapsed;
    if (lead >= 20.0f) return UsageRisk::High;
    if (lead >= 10.0f) return UsageRisk::Warning;
    return UsageRisk::Normal;
}

void format_countdown(int64_t resets_at, int64_t now, char *output, size_t length)
{
    if (!output || length == 0) return;
    if (resets_at <= 0) { snprintf(output, length, "--:--:--"); return; }
    int64_t remaining = resets_at > now ? resets_at - now : 0;
    const int64_t days = remaining / 86400;
    remaining %= 86400;
    const int64_t hours = remaining / 3600;
    if (days > 0) {
        snprintf(output, length, "%lldd %02lldh", static_cast<long long>(days),
                 static_cast<long long>(hours));
    } else {
        const int64_t minutes = remaining % 3600 / 60;
        const int64_t seconds = remaining % 60;
        snprintf(output, length, "%02lld:%02lld:%02lld", static_cast<long long>(hours),
                 static_cast<long long>(minutes), static_cast<long long>(seconds));
    }
}


void format_codex_reset_credits(const ProviderStatus &status, char *output, size_t length)
{
    if (!output || length == 0) return;
    if (status.reset_credits.present) {
        snprintf(output, length, "Reset credits: %lu", static_cast<unsigned long>(status.reset_credits.available_count));
    } else {
        snprintf(output, length, "Reset credits: --");
    }
}

FooterTone format_dashboard_footer(const ProviderStatus &status, const char *error_message,
                                   char *output, size_t length)
{
    if (!output || length == 0) return FooterTone::Normal;
    if (status.fetched_at > 0) {
        const time_t now = time(nullptr);
        if (status.fetched_at > static_cast<int64_t>(now) + 60) {
            snprintf(output, length, "waiting for clock sync");
            return FooterTone::Warning;
        }
        time_t value = static_cast<time_t>(status.fetched_at);
        struct tm local{};
        localtime_r(&value, &local);
        const char *prefix = status.error == ErrorCode::None ? "updated " : "stale  updated ";
        snprintf(output, length, "%s%02d:%02d:%02d", prefix, local.tm_hour, local.tm_min, local.tm_sec);
        return status.error == ErrorCode::None ? FooterTone::Normal : FooterTone::Warning;
    }
    if (error_message) {
        snprintf(output, length, "%s", error_message);
        return FooterTone::Error;
    }
    snprintf(output, length, "waiting for quota data");
    return FooterTone::Normal;
}
}
