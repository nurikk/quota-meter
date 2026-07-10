#include "view_model.h"
#include <stdio.h>
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
    if (is_connected(snapshot.openai)) ordered[count++] = ConnectionPage::Codex;
    if (is_connected(snapshot.claude)) ordered[count++] = ConnectionPage::Claude;
    ordered[count++] = ConnectionPage::Add;

    const size_t copied = count < capacity ? count : capacity;
    for (size_t index = 0; index < copied; ++index) pages[index] = ordered[index];
    return count;
}
}
