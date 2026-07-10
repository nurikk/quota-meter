#include "ui.h"
#ifdef ESP_PLATFORM
#include "view_model.h"
#include "../app/app_state.h"
#include "../auth/auth_manager.h"
#include "../domain/state_reducer.h"
#include "../portal/portal_logic.h"
#include "../portal/wifi_portal.h"
#include "bsp/esp-bsp.h"
#include "esp_system.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace qm {
namespace {

constexpr uint32_t COLOR_BACKGROUND = 0x10141d;
constexpr uint32_t COLOR_SURFACE = 0x1a2230;
constexpr uint32_t COLOR_BORDER = 0x334155;
constexpr uint32_t COLOR_PRIMARY = 0xf2f4f8;
constexpr uint32_t COLOR_SECONDARY = 0xaab4c3;
constexpr uint32_t COLOR_ACCENT = 0x14b8a6;
constexpr uint32_t COLOR_ACCENT_PRESSED = 0x0f766e;

struct TileEntry {
    ConnectionPage page;
    lv_obj_t *tile;
};

lv_obj_t *root;
TileEntry tiles[3];
size_t tile_count;
ConnectionPage active_page = ConnectionPage::Codex;
bool pending_provider_valid;
Provider pending_provider;

bool is_connected(const ProviderStatus &status)
{
    return status.auth == AuthState::Authenticated || status.auth == AuthState::Refreshing;
}

void style_surface(lv_obj_t *object)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_radius(object, 12, 0);
}

lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *value = lv_obj_create(parent);
    lv_obj_set_size(value, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(value, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(value, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(value, LV_DIR_VER);
    lv_obj_add_flag(value, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_style_bg_opa(value, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(value, 0, 0);
    lv_obj_set_style_pad_all(value, 12, 0);
    lv_obj_set_style_pad_gap(value, 8, 0);
    return value;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font = &lv_font_montserrat_14,
                uint32_t color = COLOR_PRIMARY)
{
    lv_obj_t *value = lv_label_create(parent);
    lv_label_set_text(value, text);
    lv_obj_set_width(value, lv_pct(96));
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(value, font, 0);
    return value;
}

lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *data = nullptr)
{
    lv_obj_t *value = lv_button_create(parent);
    lv_obj_set_height(value, 44);
    lv_obj_set_style_bg_color(value, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(value, lv_color_hex(COLOR_ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(value, callback, LV_EVENT_CLICKED, data);
    lv_obj_t *caption = lv_label_create(value);
    lv_label_set_text(caption, text);
    lv_obj_set_style_text_color(caption, lv_color_hex(COLOR_PRIMARY), 0);
    lv_obj_center(caption);
    return value;
}

lv_obj_t *qr(lv_obj_t *parent, const char *text, int size)
{
    lv_obj_t *value = lv_qrcode_create(parent);
    lv_qrcode_set_size(value, size);
    lv_qrcode_set_dark_color(value, lv_color_black());
    lv_qrcode_set_light_color(value, lv_color_white());
    lv_qrcode_update(value, text, strlen(text));
    return value;
}

void start_provider(lv_event_t *event)
{
    pending_provider = static_cast<Provider>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    pending_provider_valid = true;
    active_page = ConnectionPage::Add;
    auth_enqueue_start(pending_provider);
}

void refresh_provider(lv_event_t *event)
{
    auth_enqueue_refresh(static_cast<Provider>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void logout_provider(lv_event_t *event)
{
    pending_provider_valid = false;
    active_page = ConnectionPage::Add;
    auth_enqueue_logout(static_cast<Provider>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void reset_wifi(lv_event_t *)
{
    clear_wifi_credentials();
    esp_restart();
}

void render_page_number(lv_obj_t *parent, size_t index, size_t count)
{
    char text[40];
    snprintf(text, sizeof(text), "%u / %u  Swipe", static_cast<unsigned>(index + 1),
             static_cast<unsigned>(count));
    lv_obj_t *value = label(parent, text, &lv_font_montserrat_12, COLOR_SECONDARY);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
}

void render_quota_windows(lv_obj_t *parent, const ProviderStatus &status)
{
    if (status.window_count == 0) {
        label(parent, "Quota data will appear after the next refresh.", &lv_font_montserrat_14, COLOR_SECONDARY);
        return;
    }

    for (uint8_t index = 0; index < status.window_count; ++index) {
        const QuotaWindow &window = status.windows[index];
        char reset[40]{};
        if (window.resets_at > 0) {
            time_t value = static_cast<time_t>(window.resets_at);
            struct tm utc{};
            gmtime_r(&value, &utc);
            strftime(reset, sizeof(reset), " - resets %b %d %H:%M UTC", &utc);
        }

        char detail[128];
        snprintf(detail, sizeof(detail), "%s: %.1f%% used%s%s", window.label, window.used_percent, reset,
                 status.quota == QuotaState::Stale ? " (stale)" : "");
        label(parent, detail, &lv_font_montserrat_12, COLOR_SECONDARY);

        lv_obj_t *bar = lv_bar_create(parent);
        lv_obj_set_size(bar, lv_pct(100), 12);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x2a3342), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
        const int percent = window.used_percent < 0
                                ? 0
                                : window.used_percent > 100 ? 100 : static_cast<int>(window.used_percent);
        lv_bar_set_value(bar, percent, LV_ANIM_OFF);
    }
}

const char *error_message(ErrorCode error)
{
    switch (error) {
    case ErrorCode::Network:
        return "Network request failed";
    case ErrorCode::InvalidResponse:
        return "Provider response was invalid";
    case ErrorCode::Unauthorized:
        return "Login expired";
    case ErrorCode::Forbidden:
        return "Account access was rejected";
    case ErrorCode::Throttled:
        return "Provider is rate limiting requests";
    case ErrorCode::Storage:
        return "Credential storage failed";
    case ErrorCode::StateMismatch:
        return "Login state did not match";
    case ErrorCode::Timeout:
        return "Provider request timed out";
    case ErrorCode::None:
        return nullptr;
    }
    return "Provider request failed";
}

void render_provider_page(lv_obj_t *tile, const char *name, Provider provider, const ProviderStatus &status,
                          size_t index, size_t count)
{
    lv_obj_t *view = column(tile);
    label(view, name, &lv_font_montserrat_20);

    char summary[128];
    format_provider_summary(status, summary, sizeof(summary));
    label(view, summary, &lv_font_montserrat_14, COLOR_SECONDARY);
    if (const char *message = error_message(status.error)) {
        label(view, message, &lv_font_montserrat_12, 0xf87171);
    }

    lv_obj_t *card = lv_obj_create(view);
    lv_obj_set_width(card, lv_pct(96));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_gap(card, 6, 0);
    style_surface(card);
    render_quota_windows(card, status);

    lv_obj_t *actions = lv_obj_create(card);
    lv_obj_set_size(actions, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_gap(actions, 8, 0);
    button(actions, "Refresh", refresh_provider, reinterpret_cast<void *>(static_cast<uintptr_t>(provider)));
    button(actions, "Logout", logout_provider, reinterpret_cast<void *>(static_cast<uintptr_t>(provider)));

    render_page_number(view, index, count);
}

void render_add_option(lv_obj_t *parent, const char *name, const char *description, Provider provider,
                       const ProviderStatus &status)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(96));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_gap(card, 5, 0);
    style_surface(card);
    label(card, name, &lv_font_montserrat_16);
    label(card, description, &lv_font_montserrat_12, COLOR_SECONDARY);

    if (status.auth == AuthState::Starting || status.auth == AuthState::Exchanging) {
        label(card, "Starting login...", &lv_font_montserrat_14, COLOR_SECONDARY);
    } else {
        button(card, "Login", start_provider, reinterpret_cast<void *>(static_cast<uintptr_t>(provider)));
    }
}

void render_add_page(lv_obj_t *tile, const AppSnapshot &snapshot, size_t index, size_t count)
{
    lv_obj_t *view = column(tile);
    label(view, "Add connection", &lv_font_montserrat_20);
    label(view, "Choose a quota provider. These private integrations may change without notice.",
          &lv_font_montserrat_12, COLOR_SECONDARY);

    if (!is_connected(snapshot.openai)) {
        render_add_option(view, "Codex", "Connect an OpenAI Codex account.", Provider::OpenAI, snapshot.openai);
    }
    if (!is_connected(snapshot.claude)) {
        render_add_option(view, "Claude", "Connect a Claude subscription.", Provider::Claude, snapshot.claude);
    }
    if (is_connected(snapshot.openai) && is_connected(snapshot.claude)) {
        label(view, "All supported connections are already added.", &lv_font_montserrat_14, COLOR_SECONDARY);
    }

    render_page_number(view, index, count);
}

void tile_changed(lv_event_t *event)
{
    lv_obj_t *tileview = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    lv_obj_t *active = lv_tileview_get_tile_active(tileview);
    for (size_t index = 0; index < tile_count; ++index) {
        if (tiles[index].tile == active) {
            active_page = tiles[index].page;
            return;
        }
    }
}

void update_pending_provider(const AppSnapshot &snapshot)
{
    if (!pending_provider_valid) return;
    const ProviderStatus &status = pending_provider == Provider::OpenAI ? snapshot.openai : snapshot.claude;
    if (is_connected(status)) {
        active_page = pending_provider == Provider::OpenAI ? ConnectionPage::Codex : ConnectionPage::Claude;
        pending_provider_valid = false;
    } else if (status.auth == AuthState::Error) {
        active_page = ConnectionPage::Add;
        pending_provider_valid = false;
    }
}

void render_carousel(const AppSnapshot &snapshot)
{
    update_pending_provider(snapshot);

    ConnectionPage pages[3];
    tile_count = connection_pages(snapshot, pages, 3);
    lv_obj_t *tileview = lv_tileview_create(root);
    lv_obj_set_size(tileview, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tileview, 0, 0);
    lv_obj_set_style_pad_all(tileview, 0, 0);

    size_t selected = 0;
    for (size_t index = 0; index < tile_count; ++index) {
        lv_dir_t direction = index == 0
                                 ? LV_DIR_RIGHT
                                 : index + 1 == tile_count
                                       ? LV_DIR_LEFT
                                       : static_cast<lv_dir_t>(LV_DIR_LEFT | LV_DIR_RIGHT);
        lv_obj_t *tile = lv_tileview_add_tile(tileview, static_cast<uint8_t>(index), 0, direction);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        tiles[index] = {pages[index], tile};
        if (pages[index] == active_page) selected = index;

        switch (pages[index]) {
        case ConnectionPage::Codex:
            render_provider_page(tile, "Codex", Provider::OpenAI, snapshot.openai, index, tile_count);
            break;
        case ConnectionPage::Claude:
            render_provider_page(tile, "Claude", Provider::Claude, snapshot.claude, index, tile_count);
            break;
        case ConnectionPage::Add:
            render_add_page(tile, snapshot, index, tile_count);
            break;
        }
    }

    active_page = pages[selected];
    lv_tileview_set_tile(tileview, tiles[selected].tile, LV_ANIM_OFF);
    lv_obj_add_event_cb(tileview, tile_changed, LV_EVENT_VALUE_CHANGED, nullptr);
}

void render_wifi(const AppSnapshot &snapshot)
{
    lv_obj_t *view = column(root);
    label(view, "Connect Quota Meter", &lv_font_montserrat_20);
    label(view, "Scan the Wi-Fi QR, then open http://192.168.4.1", &lv_font_montserrat_12, COLOR_SECONDARY);
    char payload[128];
    if (wifi_qr_payload(snapshot.ap_ssid, snapshot.ap_password, payload, sizeof(payload))) qr(view, payload, 210);
    char credentials[96];
    snprintf(credentials, sizeof(credentials), "Network: %s\nPassword: %s", snapshot.ap_ssid, snapshot.ap_password);
    label(view, credentials);
    button(view, "Clear credentials & restart", reset_wifi);
}

void render_openai(const AppSnapshot &snapshot)
{
    lv_obj_t *view = column(root);
    label(view, "OpenAI device login", &lv_font_montserrat_20);
    qr(view, snapshot.openai.login_url, 210);
    char text[128];
    snprintf(text, sizeof(text), "Open %s\nand enter code: %s\nExpires after 15 minutes.",
             snapshot.openai.login_url, snapshot.openai.user_code);
    label(view, text, &lv_font_montserrat_12, COLOR_SECONDARY);
    button(view, "Cancel", logout_provider,
           reinterpret_cast<void *>(static_cast<uintptr_t>(Provider::OpenAI)));
}

void render_claude(const AppSnapshot &snapshot)
{
    lv_obj_t *view = column(root);
    label(view, "Claude login", &lv_font_montserrat_20);
    qr(view, snapshot.claude.login_url, 210);
    label(view, "Complete authorization, then paste the callback URL or code#state into the authenticated portal.",
          &lv_font_montserrat_12, COLOR_SECONDARY);
    char portal[160];
    const char *host = snapshot.sta_ip[0] && strcmp(snapshot.sta_ip, "0.0.0.0") != 0
                           ? snapshot.sta_ip
                           : "192.168.4.1";
    snprintf(portal, sizeof(portal), "Portal: http://%s/?p=%s", host, snapshot.ap_password);
    label(view, portal, &lv_font_montserrat_12, COLOR_SECONDARY);
    button(view, "Cancel", logout_provider,
           reinterpret_cast<void *>(static_cast<uintptr_t>(Provider::Claude)));
}

void render(const AppSnapshot &snapshot)
{
    lv_obj_clean(root);
    switch (select_screen(snapshot)) {
    case Screen::WifiSetup:
        render_wifi(snapshot);
        break;
    case Screen::OpenAiCode:
        render_openai(snapshot);
        break;
    case Screen::ClaudeManualCode:
        render_claude(snapshot);
        break;
    case Screen::Dashboard:
    case Screen::ProviderLogin:
        render_carousel(snapshot);
        break;
    default:
        label(root, "Display initialization failed");
        break;
    }
}

} // namespace

void ui_create()
{
    if (!bsp_display_lock(1000)) return;
    root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, lv_color_hex(COLOR_PRIMARY), 0);
    render(app_state_get());
    bsp_display_unlock();
}

void ui_task(void *)
{
    AppSnapshot previous = app_state_get();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        AppSnapshot current = app_state_get();
        if (memcmp(&current, &previous, sizeof(current)) == 0) continue;
        if (bsp_display_lock(500)) {
            render(current);
            bsp_display_unlock();
            previous = current;
        }
    }
}

} // namespace qm
#endif
