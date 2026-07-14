#include "wifi_console.h"
#ifdef ESP_PLATFORM
#include "../app/app_state.h"
#include "../auth/token_store.h"
#include "../portal/wifi_portal.h"
#include "../util/base64url.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace qm {
namespace {

TokenBundle *import_bundle;
Provider import_provider;
bool import_active;
size_t field_lengths[4];

enum class ImportField : uint8_t { Access, Refresh, Id, Account };

bool parse_provider(const char *value, Provider *provider)
{
    if (strcmp(value, "codex") == 0) {
        *provider = Provider::OpenAI;
        return true;
    }
    if (strcmp(value, "claude") == 0) {
        *provider = Provider::Claude;
        return true;
    }
    return false;
}

bool parse_field(const char *value, ImportField *field)
{
    if (strcmp(value, "access") == 0) *field = ImportField::Access;
    else if (strcmp(value, "refresh") == 0) *field = ImportField::Refresh;
    else if (strcmp(value, "id") == 0) *field = ImportField::Id;
    else if (strcmp(value, "account") == 0) *field = ImportField::Account;
    else return false;
    return true;
}

char *field_buffer(ImportField field, size_t *capacity, size_t **length)
{
    const size_t index = static_cast<size_t>(field);
    *length = &field_lengths[index];
    switch (field) {
    case ImportField::Access:
        *capacity = sizeof(import_bundle->oauth.access_token);
        return import_bundle->oauth.access_token;
    case ImportField::Refresh:
        *capacity = sizeof(import_bundle->oauth.refresh_token);
        return import_bundle->oauth.refresh_token;
    case ImportField::Id:
        *capacity = sizeof(import_bundle->oauth.id_token);
        return import_bundle->oauth.id_token;
    case ImportField::Account:
        *capacity = sizeof(import_bundle->account_id);
        return import_bundle->account_id;
    }
    return nullptr;
}

bool parse_size(const char *text, size_t *value)
{
    char *end = nullptr;
    unsigned long parsed = strtoul(text, &end, 10);
    if (!text[0] || !end || *end || parsed > SIZE_MAX) return false;
    *value = static_cast<size_t>(parsed);
    return true;
}

int connect_command(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: connect \"SSID\" \"password\"\n");
        return 1;
    }
    if (strlen(argv[1]) > 32 || strlen(argv[2]) > 64) {
        printf("ERROR: SSID or password is too long.\n");
        secure_clear(argv[2], strlen(argv[2]));
        return 1;
    }
    bool accepted = wifi_connect_and_save(argv[1], argv[2]);
    secure_clear(argv[2], strlen(argv[2]));
    printf(accepted ? "OK: Wi-Fi credentials saved; connecting.\n" : "ERROR: Wi-Fi credentials were not saved.\n");
    return accepted ? 0 : 1;
}

int token_begin_command(int argc, char **argv)
{
    Provider provider;
    if (argc != 2 || !parse_provider(argv[1], &provider)) {
        printf("Usage: token-begin <codex|claude>\n");
        return 1;
    }
    secure_clear(import_bundle, sizeof(*import_bundle));
    secure_clear(field_lengths, sizeof(field_lengths));
    import_provider = provider;
    import_active = true;
    printf("OK: token import started.\n");
    return 0;
}

int token_chunk_command(int argc, char **argv)
{
    ImportField field;
    size_t offset;
    if (argc != 4 || !import_active || !parse_field(argv[1], &field) || !parse_size(argv[2], &offset)) {
        if (argc == 4) secure_clear(argv[3], strlen(argv[3]));
        printf("ERROR: invalid token chunk.\n");
        return 1;
    }

    uint8_t decoded[160];
    size_t written = 0;
    const bool decoded_ok = base64url_decode(argv[3], decoded, sizeof(decoded), &written);
    secure_clear(argv[3], strlen(argv[3]));

    size_t capacity;
    size_t *length;
    char *destination = field_buffer(field, &capacity, &length);
    const bool valid = decoded_ok && offset == *length && written > 0 && offset + written < capacity &&
                       memchr(decoded, 0, written) == nullptr;
    if (!valid) {
        secure_clear(decoded, sizeof(decoded));
        printf("ERROR: invalid token chunk.\n");
        return 1;
    }

    memcpy(destination + offset, decoded, written);
    *length += written;
    destination[*length] = '\0';
    secure_clear(decoded, sizeof(decoded));
    printf("OK: token chunk accepted.\n");
    return 0;
}

int token_commit_command(int argc, char **argv)
{
    size_t expires_at;
    if (argc != 2 || !import_active || !parse_size(argv[1], &expires_at) || expires_at > INT64_MAX) {
        printf("ERROR: invalid token commit.\n");
        return 1;
    }

    const time_t now = time(nullptr);
    import_bundle->version = TokenBundle::VERSION;
    import_bundle->expires_at = static_cast<int64_t>(expires_at);
    import_bundle->refreshed_at = now;
    const int64_t remaining = import_bundle->expires_at - now;
    import_bundle->oauth.expires_in = remaining > UINT32_MAX ? UINT32_MAX : remaining > 0 ? remaining : 0;

    const bool complete = import_bundle->oauth.access_token[0] && import_bundle->oauth.refresh_token[0] &&
                          (import_provider != Provider::OpenAI || import_bundle->account_id[0]);
    const esp_err_t result = complete ? token_store_save(import_provider, *import_bundle) : ESP_ERR_INVALID_ARG;
    secure_clear(import_bundle, sizeof(*import_bundle));
    secure_clear(field_lengths, sizeof(field_lengths));
    import_active = false;
    if (result != ESP_OK) {
        printf("ERROR: token bundle was not saved.\n");
        return 1;
    }
    printf("OK: token bundle saved.\n");
    return 0;
}

void print_provider_status(const char *name, const ProviderStatus &status)
{
    printf("%s auth=%u quota=%u error=%u plan=%s windows=%u fetched=%lld\n", name,
           static_cast<unsigned>(status.auth), static_cast<unsigned>(status.quota),
           static_cast<unsigned>(status.error), status.plan[0] ? status.plan : "--",
           status.window_count, static_cast<long long>(status.fetched_at));
    for (uint8_t index = 0; index < status.window_count; ++index) {
        const QuotaWindow &window = status.windows[index];
        printf("  window[%u] label=%s used=%.1f%% duration=%ldm reset=%lld model=%u\n", index,
               window.label, static_cast<double>(window.used_percent),
               static_cast<long>(window.window_minutes), static_cast<long long>(window.resets_at),
               window.model_limit ? 1U : 0U);
    }
    if (status.extra_usage.present) {
        printf("  extra enabled=%u utilization=%s%.3f%%\n", status.extra_usage.is_enabled ? 1U : 0U,
               status.extra_usage.has_utilization ? "" : "unavailable ",
               static_cast<double>(status.extra_usage.utilization));
    }
}

int status_command(int argc, char **)
{
    if (argc != 1) return 1;
    const AppSnapshot snapshot = app_state_get();
    printf("Wi-Fi state=%u ip=%s time=%lld\n", static_cast<unsigned>(snapshot.wifi), snapshot.sta_ip,
           static_cast<long long>(time(nullptr)));
    print_provider_status("Codex", snapshot.openai);
    print_provider_status("Claude", snapshot.claude);
    return 0;
}


int restart_command(int argc, char **)
{
    if (argc != 1) return 1;
    printf("OK: restarting.\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

void register_command(const char *name, const char *help, const char *hint, esp_console_cmd_func_t function)
{
    const esp_console_cmd_t command = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = function,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&command));
}

} // namespace

void wifi_console_init()
{
    import_bundle = static_cast<TokenBundle *>(
        heap_caps_calloc(1, sizeof(TokenBundle), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!import_bundle) abort();

    register_command("connect", "Save Wi-Fi credentials and connect", "<SSID> <password>", connect_command);
    register_command("token-begin", "Begin a local OAuth token import", "<codex|claude>", token_begin_command);
    register_command("token-chunk", "Append an encoded OAuth token chunk", "<field> <offset> <base64url>",
                     token_chunk_command);
    register_command("token-commit", "Persist the imported OAuth token bundle", "<expires-at>", token_commit_command);
    register_command("status", "Show non-secret connection state", nullptr, status_command);
    register_command("restart", "Restart Quota Meter", nullptr, restart_command);
    ESP_ERROR_CHECK(esp_console_register_help_command());

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "quota-meter> ";
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t device_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = nullptr;
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

} // namespace qm
#endif
