#include "token_store.h"

#include <string.h>

namespace qm {
void secure_clear(void *value, size_t length)
{
    volatile unsigned char *p = static_cast<volatile unsigned char *>(value);
    while (length--) *p++ = 0;
}

bool valid_token_bundle(Provider provider, const TokenBundle &bundle)
{
    if (provider != Provider::OpenAI && provider != Provider::Claude) return false;
    return bundle.version == TokenBundle::VERSION && bundle.oauth.refresh_token[0] &&
        memchr(bundle.oauth.access_token, 0, sizeof(bundle.oauth.access_token)) &&
        memchr(bundle.oauth.refresh_token, 0, sizeof(bundle.oauth.refresh_token)) &&
        memchr(bundle.oauth.id_token, 0, sizeof(bundle.oauth.id_token)) &&
        memchr(bundle.account_id, 0, sizeof(bundle.account_id)) &&
        (provider != Provider::OpenAI || (bundle.oauth.access_token[0] && bundle.account_id[0]));
}

bool decode_account_record(const void *data, size_t length, AccountRecord *record)
{
    if (!data || !record || length != sizeof(*record)) return false;
    if (data != record) memcpy(record, data, length);
    const bool valid = record->version == AccountRecord::VERSION && record->account.id &&
        record->account.generation && valid_account_name(record->account.name) &&
        valid_token_bundle(record->account.provider, record->bundle);
    if (!valid) secure_clear(record, sizeof(*record));
    return valid;
}
}

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <memory>
#include <stdio.h>

namespace qm {
namespace {
constexpr char ACCOUNTS_NS[] = "qm_accounts";
SemaphoreHandle_t store_mutex;
struct StoreLock {
    StoreLock() { xSemaphoreTake(store_mutex, portMAX_DELAY); }
    ~StoreLock() { xSemaphoreGive(store_mutex); }
};
struct RecordDeleter {
    void operator()(AccountRecord *record) const { secure_clear(record, sizeof(*record)); delete record; }
};
using RecordPtr = std::unique_ptr<AccountRecord, RecordDeleter>;

void account_key(AccountId id, char kind, char *key)
{
    snprintf(key, 16, "%c%08lx", kind, static_cast<unsigned long>(id));
}

constexpr uint32_t QUOTA_VERSION = 3;

struct QuotaBundle {
    uint32_t version;
    char plan[32];
    QuotaWindow windows[8];
    uint8_t window_count;
    ClaudeExtraUsage extra_usage;
    OpenAiResetCredits reset_credits;
    int64_t fetched_at;
};

struct CachedQuota {
    uint32_t generation;
    QuotaBundle quota;
};

bool terminated(const char *value, size_t cap) { return memchr(value, 0, cap) != nullptr; }

bool valid_quota(const QuotaBundle &quota)
{
    if (quota.version != QUOTA_VERSION || quota.window_count > 8 || !terminated(quota.plan, sizeof(quota.plan)) ||
        !terminated(quota.extra_usage.currency, sizeof(quota.extra_usage.currency))) return false;
    for (uint8_t i = 0; i < quota.window_count; ++i) {
        if (!terminated(quota.windows[i].label, sizeof(quota.windows[i].label))) return false;
    }
    return true;
}

esp_err_t read_record(AccountId id, AccountRecord *record)
{
    char key[16];
    account_key(id, 'a', key);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(ACCOUNTS_NS, NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    size_t length = sizeof(*record);
    result = nvs_get_blob(handle, key, record, &length);
    nvs_close(handle);
    if (result == ESP_OK && (!decode_account_record(record, length, record) || record->account.id != id))
        result = ESP_ERR_INVALID_VERSION;
    return result;
}

esp_err_t write_record(const AccountRecord &record)
{
    char key[16];
    account_key(record.account.id, 'a', key);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(ACCOUNTS_NS, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, key, &record, sizeof(record));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t list_accounts(std::vector<Account> *accounts)
{
    accounts->clear();
    RecordPtr record(new AccountRecord{});
    nvs_iterator_t iterator = nullptr;
    esp_err_t result = nvs_entry_find("nvs", ACCOUNTS_NS, NVS_TYPE_BLOB, &iterator);
    while (result == ESP_OK) {
        nvs_entry_info_t info{};
        nvs_entry_info(iterator, &info);
        if (info.key[0] == 'a') {
            char *end;
            const unsigned long id = strtoul(info.key + 1, &end, 16);
            if (strlen(info.key) != 9 || *end || !id || id > UINT32_MAX) {
                result = ESP_ERR_INVALID_VERSION;
                break;
            }
            result = read_record(static_cast<AccountId>(id), record.get());
            if (result != ESP_OK) break;
            accounts->push_back(record->account);
        }
        result = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    std::sort(accounts->begin(), accounts->end(), [](const Account &a, const Account &b) { return a.id < b.id; });
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}
}

esp_err_t token_store_clear()
{
    StoreLock lock;
    const char *namespaces[] = {"qm_openai", "qm_claude", ACCOUNTS_NS};
    for (const char *name : namespaces) {
        nvs_handle_t handle;
        esp_err_t result = nvs_open(name, NVS_READONLY, &handle);
        if (result == ESP_ERR_NVS_NOT_FOUND) continue;
        if (result != ESP_OK) return result;
        nvs_close(handle);
        result = nvs_open(name, NVS_READWRITE, &handle);
        if (result != ESP_OK) return result;
        result = nvs_erase_all(handle);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

esp_err_t token_store_accounts(std::vector<Account> *accounts)
{
    StoreLock lock;
    return list_accounts(accounts);
}

esp_err_t token_store_upload(Provider provider, const char *name, const TokenBundle &bundle)
{
    if (!valid_account_name(name) || !valid_token_bundle(provider, bundle)) return ESP_ERR_INVALID_ARG;
    StoreLock lock;
    std::vector<Account> accounts;
    esp_err_t result = list_accounts(&accounts);
    if (result != ESP_OK) return result;
    RecordPtr record(new AccountRecord{});
    AccountId next_id = 3;
    for (const auto &account : accounts) {
        if (same_account_target(account, provider, name)) record->account = account;
        if (account.id >= next_id) {
            if (account.id == UINT32_MAX) return ESP_ERR_NO_MEM;
            next_id = account.id + 1;
        }
    }
    if (!record->account.id) {
        record->account.id = next_id;
        record->account.provider = provider;
        snprintf(record->account.name, sizeof(record->account.name), "%s", name);
    }
    if (record->account.generation == UINT32_MAX) return ESP_ERR_INVALID_STATE;
    ++record->account.generation;
    record->bundle = bundle;
    return write_record(*record);
}

esp_err_t token_store_save(const Account &account, const TokenBundle &bundle)
{
    if (!valid_token_bundle(account.provider, bundle)) return ESP_ERR_INVALID_ARG;
    StoreLock lock;
    RecordPtr record(new AccountRecord{});
    esp_err_t result = read_record(account.id, record.get());
    if (result != ESP_OK) return result;
    if (record->account.generation != account.generation) return ESP_ERR_INVALID_STATE;
    record->bundle = bundle;
    return write_record(*record);
}

esp_err_t token_store_load(const Account &account, TokenBundle *bundle)
{
    StoreLock lock;
    secure_clear(bundle, sizeof(*bundle));
    RecordPtr record(new AccountRecord{});
    esp_err_t result = read_record(account.id, record.get());
    if (result == ESP_OK && record->account.generation != account.generation) return ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) *bundle = record->bundle;
    return result;
}

esp_err_t quota_store_save(const Account &account, const ProviderStatus &status)
{
    if (status.window_count > 8 || !terminated(status.plan, sizeof(status.plan)) ||
        !terminated(status.extra_usage.currency, sizeof(status.extra_usage.currency))) return ESP_ERR_INVALID_ARG;
    StoreLock lock;
    RecordPtr record(new AccountRecord{});
    esp_err_t current = read_record(account.id, record.get());
    if (current != ESP_OK) return current;
    if (record->account.generation != account.generation) return ESP_ERR_INVALID_STATE;
    CachedQuota cached{};
    cached.generation = account.generation;
    QuotaBundle &quota = cached.quota;
    quota.version = QUOTA_VERSION;
    memcpy(quota.plan, status.plan, sizeof(quota.plan));
    quota.window_count = status.window_count;
    for (uint8_t i = 0; i < status.window_count; ++i) {
        if (!terminated(status.windows[i].label, sizeof(status.windows[i].label))) return ESP_ERR_INVALID_ARG;
        quota.windows[i] = status.windows[i];
    }
    quota.extra_usage = status.extra_usage;
    quota.reset_credits = status.reset_credits;
    quota.fetched_at = status.fetched_at;
    char key[16];
    account_key(account.id, 'q', key);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(ACCOUNTS_NS, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, key, &cached, sizeof(cached));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t quota_store_load(const Account &account, ProviderStatus *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    StoreLock lock;
    RecordPtr record(new AccountRecord{});
    esp_err_t current = read_record(account.id, record.get());
    if (current != ESP_OK) return current;
    if (record->account.generation != account.generation) return ESP_ERR_INVALID_STATE;
    CachedQuota cached{};
    cached.generation = account.generation;
    QuotaBundle &quota = cached.quota;
    char key[16];
    account_key(account.id, 'q', key);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(ACCOUNTS_NS, NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    size_t length = sizeof(cached);
    result = nvs_get_blob(handle, key, &cached, &length);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    if (length != sizeof(cached) || cached.generation != account.generation || !valid_quota(quota)) return ESP_ERR_INVALID_VERSION;
    memcpy(status->plan, quota.plan, sizeof(status->plan));
    for (QuotaWindow &window : status->windows) window = {};
    memcpy(status->windows, quota.windows, sizeof(status->windows));
    status->window_count = quota.window_count;
    status->extra_usage = quota.extra_usage;
    status->reset_credits = quota.reset_credits;
    status->fetched_at = quota.fetched_at;
    return ESP_OK;
}


esp_err_t token_store_init()
{
    store_mutex = xSemaphoreCreateMutex();
    if (!store_mutex) return ESP_ERR_NO_MEM;
    const char *legacy_names[] = {"qm_openai", "qm_claude"};
    for (size_t index = 0; index < 2; ++index) {
        RecordPtr record(new AccountRecord{});
        const AccountId id = static_cast<AccountId>(index + 1);
        esp_err_t result = read_record(id, record.get());
        if (result == ESP_OK) continue;
        if (result != ESP_ERR_NVS_NOT_FOUND) return result;
        nvs_handle_t handle;
        result = nvs_open(legacy_names[index], NVS_READONLY, &handle);
        if (result == ESP_ERR_NVS_NOT_FOUND) continue;
        if (result != ESP_OK) return result;
        size_t length = sizeof(record->bundle);
        result = nvs_get_blob(handle, "bundle", &record->bundle, &length);
        record->account = {id, index == 0 ? Provider::OpenAI : Provider::Claude, "Default", 1};
        if (result != ESP_OK || length != sizeof(record->bundle) ||
            !valid_token_bundle(record->account.provider, record->bundle)) {
            nvs_close(handle);
            if (result == ESP_ERR_NVS_NOT_FOUND) continue;
            return result == ESP_OK ? ESP_ERR_INVALID_VERSION : result;
        }
        QuotaBundle quota{};
        length = sizeof(quota);
        result = nvs_get_blob(handle, "quota", &quota, &length);
        nvs_close(handle);
        if (result == ESP_OK && length == sizeof(quota) && valid_quota(quota)) {
            char key[16];
            account_key(id, 'q', key);
            result = nvs_open(ACCOUNTS_NS, NVS_READWRITE, &handle);
            if (result != ESP_OK) return result;
            const CachedQuota cached{record->account.generation, quota};
            result = nvs_set_blob(handle, key, &cached, sizeof(cached));
            if (result == ESP_OK) result = nvs_commit(handle);
            nvs_close(handle);
            if (result != ESP_OK) return result;
        }
        result = write_record(*record);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}
}
#endif
