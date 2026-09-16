#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../domain/models.h"
namespace qm {

using ConnectionPage = AccountId;
constexpr ConnectionPage IMPORT_PAGE = 0;
enum class UsageRisk : uint8_t { Normal, Warning, High };
enum class FooterTone : uint8_t { Normal, Warning, Error };

void format_provider_summary(const ProviderStatus &status, char *output, size_t output_len);
std::vector<ConnectionPage> connection_pages(const AppSnapshot &snapshot);
const QuotaWindow *find_quota_window(const ProviderStatus &status, const char *semantic_label);
const QuotaWindow *find_model_limit(const ProviderStatus &status, const char *preferred_label);
const QuotaWindow *find_quota_window_by_duration(const ProviderStatus &status, int32_t window_minutes);
void format_window_period(const QuotaWindow *window, const char *fallback, char *output, size_t output_len);
ConnectionPage focus_after_usage_change(ConnectionPage current, const AppSnapshot &previous,
                                        const AppSnapshot &current_snapshot);
float elapsed_percent(const QuotaWindow &window, int64_t now, int window_minutes);
UsageRisk usage_risk(float used_percent, float elapsed_percent);
void format_countdown(int64_t resets_at, int64_t now, char *output, size_t output_len);
void format_codex_reset_credits(const ProviderStatus &status, char *output, size_t output_len);
FooterTone format_dashboard_footer(const ProviderStatus &status, const char *error_message,
                                   char *output, size_t output_len);

}
