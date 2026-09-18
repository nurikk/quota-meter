#include "view_model.h"
#include <algorithm>
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
                                 : status.auth == AuthState::Expired
                                       ? "Credentials expired"
                                       : status.auth == AuthState::Error ? "Error" : "Import required";
    const char *freshness = status.quota == QuotaState::Stale
                                ? "stale"
                                : status.quota == QuotaState::Fresh ? "fresh" : "no quota";
    snprintf(output, length, "%s | %s%s%s", auth, freshness, status.plan[0] ? " | " : "", status.plan);
}

static bool is_connected(const ProviderStatus &status)
{
    return status.auth == AuthState::Authenticated || status.auth == AuthState::Refreshing;
}

std::vector<ConnectionPage> connection_pages(const AppSnapshot &snapshot)
{
    std::vector<ConnectionPage> pages;
    for (const auto &entry : snapshot.accounts) {
        if (is_connected(entry.status) || entry.status.auth == AuthState::Expired) pages.push_back(entry.account.id);
    }
    pages.push_back(IMPORT_PAGE);
    return pages;
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

static bool usage_grew(const ProviderStatus &before, const ProviderStatus &after)
{
    if (!is_connected(after) || after.quota != QuotaState::Fresh || after.error != ErrorCode::None ||
        (before.quota != QuotaState::Fresh && before.quota != QuotaState::Stale)) return false;
    for (uint8_t index = 0; index < after.window_count; ++index) {
        const auto &window = after.windows[index];
        if (!window.present) continue;
        for (uint8_t old_index = 0; old_index < before.window_count; ++old_index) {
            const auto &old = before.windows[old_index];
            if (old.present && same_label(old.label, window.label) &&
                old.model_limit == window.model_limit && old.window_minutes == window.window_minutes &&
                old.resets_at == window.resets_at && window.used_percent > old.used_percent) return true;
        }
    }
    return false;
}

static ConnectionPage next_page(const std::vector<ConnectionPage> &pages, ConnectionPage current)
{
    const auto found = std::find(pages.begin(), pages.end(), current);
    return found == pages.end() || found + 1 == pages.end() ? pages.front() : *(found + 1);
}

void AccountCarousel::reset_dwell(uint32_t now_ms)
{
    dwell_started_ms_ = now_ms;
}

ConnectionPage AccountCarousel::update(ConnectionPage current, const AppSnapshot &previous,
                                        const AppSnapshot &snapshot, uint32_t now_ms)
{
    constexpr uint32_t DWELL_MS = 10000;
    constexpr uint32_t IDLE_INTERVAL_MS = 120000;
    constexpr uint32_t ACTIVITY_MS = 600000;
    if (!initialized_) {
        initialized_ = true;
        reset_dwell(now_ms);
        idle_slot_ms_ = now_ms;
    }
    activity_.erase(std::remove_if(activity_.begin(), activity_.end(), [&](const Activity &activity) {
        const auto *entry = snapshot.find(activity.id);
        return !entry || entry->account.generation != activity.generation || !is_connected(entry->status);
    }), activity_.end());
    std::vector<ConnectionPage> eligible;
    std::vector<ConnectionPage> active;
    std::vector<ConnectionPage> idle;
    ConnectionPage expired = IMPORT_PAGE;
    for (const auto &entry : snapshot.accounts) {
        const auto *before = previous.find(entry.account.id);
        if (expired == IMPORT_PAGE && entry.status.auth == AuthState::Expired &&
            (!before || before->status.auth != AuthState::Expired ||
             before->account.generation != entry.account.generation)) expired = entry.account.id;
        if (!is_connected(entry.status)) continue;
        auto activity = std::find_if(activity_.begin(), activity_.end(), [&](const Activity &value) {
            return value.id == entry.account.id;
        });
        if (activity == activity_.end()) {
            activity_.push_back({entry.account.id, entry.account.generation});
            activity = activity_.end() - 1;
        } else if (before && before->account.generation == entry.account.generation &&
                   usage_grew(before->status, entry.status)) {
            activity->last_growth_ms = now_ms;
            activity->active = true;
        }
        if (activity->active && static_cast<uint32_t>(now_ms - activity->last_growth_ms) >= ACTIVITY_MS)
            activity->active = false;
        eligible.push_back(entry.account.id);
        (activity->active ? active : idle).push_back(entry.account.id);
    }
    if (active.empty()) {
        idle_slot_ms_ = now_ms;
        idle_due_ = false;
    } else if (static_cast<uint32_t>(now_ms - idle_slot_ms_) >= IDLE_INTERVAL_MS) {
        idle_due_ = true;
    }
    if (expired != IMPORT_PAGE) {
        reset_dwell(now_ms);
        return expired;
    }
    const auto *shown = snapshot.find(current);
    if (current == IMPORT_PAGE || (shown && shown->status.auth == AuthState::Expired)) return current;
    if (!shown || !is_connected(shown->status)) {
        reset_dwell(now_ms);
        return eligible.empty() ? connection_pages(snapshot).front() : eligible.front();
    }
    if (std::find(active.begin(), active.end(), current) != active.end()) last_active_ = current;
    if (static_cast<uint32_t>(now_ms - dwell_started_ms_) < DWELL_MS) return current;
    reset_dwell(now_ms);
    if (active.empty()) return next_page(eligible, current);
    if (idle_due_ && !idle.empty()) {
        last_idle_ = next_page(idle, last_idle_);
        idle_slot_ms_ = now_ms;
        idle_due_ = false;
        return last_idle_;
    }
    return next_page(active, last_active_);
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
