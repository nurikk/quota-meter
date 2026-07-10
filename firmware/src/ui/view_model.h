#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../domain/models.h"
namespace qm {

enum class ConnectionPage : uint8_t { Codex, Claude, Add };

void format_provider_summary(const ProviderStatus &status, char *output, size_t output_len);
size_t connection_pages(const AppSnapshot &snapshot, ConnectionPage *pages, size_t capacity);

}
