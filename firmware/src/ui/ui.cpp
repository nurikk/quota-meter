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
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

extern "C" {
LV_IMAGE_DECLARE(CLAUDE_LOGO);
LV_IMAGE_DECLARE(CODEX_LOGO);
}

namespace qm {
namespace {

constexpr uint32_t COLOR_BACKGROUND = 0x080c10;
constexpr uint32_t COLOR_SURFACE = 0x121820;
constexpr uint32_t COLOR_BORDER = 0x26313a;
constexpr uint32_t COLOR_PRIMARY = 0xf2f4f6;
constexpr uint32_t COLOR_SECONDARY = 0x8d98a3;
constexpr uint32_t COLOR_ACCENT = 0x14b8a6;
constexpr uint32_t COLOR_ACCENT_PRESSED = 0x0f766e;
constexpr uint32_t COLOR_CODEX = 0x2684ff;
constexpr uint32_t COLOR_CLAUDE = 0xf28c28;
constexpr uint32_t COLOR_WARNING = 0xf2c94c;
constexpr uint32_t COLOR_HIGH = 0xef4444;
constexpr uint32_t COLOR_TRACK = 0x182129;
constexpr uint32_t COLOR_TIME = 0x66737e;

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 320;
constexpr int FULL_PERCENT = 100;
constexpr float PERCENT_ROUNDING = 0.5f;
constexpr int CONTENT_PERCENT = 96;
constexpr int CONTENT_LEFT = 8;
constexpr int CONTENT_RIGHT = SCREEN_WIDTH - CONTENT_LEFT;
constexpr int CONTENT_WIDTH = CONTENT_RIGHT - CONTENT_LEFT;
constexpr int DASHBOARD_TEXT_RIGHT = SCREEN_WIDTH - 4;
constexpr int DASHBOARD_PERCENT_RIGHT = SCREEN_WIDTH - 6;
constexpr int COLUMN_PADDING = 12;
constexpr int COLUMN_GAP = 8;
constexpr int CARD_PADDING = 10;
constexpr int CARD_GAP = 5;
constexpr int SURFACE_BORDER_WIDTH = 1;
constexpr int SURFACE_RADIUS = 12;
constexpr int BUTTON_HEIGHT = 44;
constexpr int LOGIN_QR_SIZE = 210;

constexpr size_t MAX_CONNECTION_PAGES = 3;
constexpr size_t MAX_QUOTA_BINDINGS = 6;
constexpr size_t PAGE_NUMBER_TEXT_CAPACITY = 40;
constexpr size_t COUNTDOWN_TEXT_CAPACITY = 20;
constexpr size_t PERCENT_TEXT_CAPACITY = 16;
constexpr size_t BAR_PERCENT_TEXT_CAPACITY = 12;
constexpr size_t TIMEZONE_TEXT_CAPACITY = 8;
constexpr size_t FOOTER_TEXT_CAPACITY = 64;
constexpr size_t WIFI_PAYLOAD_CAPACITY = 128;
constexpr size_t WIFI_CREDENTIALS_CAPACITY = 96;
constexpr size_t LOGIN_INSTRUCTIONS_CAPACITY = 128;
constexpr size_t PORTAL_TEXT_CAPACITY = 160;
constexpr size_t UTILIZATION_TEXT_CAPACITY = 20;
constexpr size_t SPEND_TEXT_CAPACITY = 40;
constexpr size_t LIMIT_TEXT_CAPACITY = 48;
constexpr size_t AMOUNT_TEXT_CAPACITY = 28;

constexpr int DIVIDER_HEIGHT = 1;
constexpr int DASHBOARD_BAR_LABEL_X = CONTENT_LEFT;
constexpr int DASHBOARD_BAR_LABEL_Y_OFFSET = -2;
constexpr int DASHBOARD_BAR_X = 60;
constexpr int DASHBOARD_BAR_LABEL_WIDTH = DASHBOARD_BAR_X - DASHBOARD_BAR_LABEL_X;
constexpr int DASHBOARD_BAR_WIDTH = 371;
constexpr int DASHBOARD_BAR_HEIGHT = 10;
constexpr int DASHBOARD_BAR_CAPTION_X = 440;
constexpr int DASHBOARD_BAR_CAPTION_Y_OFFSET = -3;
constexpr int DASHBOARD_BAR_CAPTION_WIDTH = DASHBOARD_PERCENT_RIGHT - DASHBOARD_BAR_CAPTION_X;

constexpr int QUOTA_PERIOD_X = 64;
constexpr int QUOTA_PERIOD_WIDTH = 40;
constexpr int QUOTA_USED_CAPTION_Y_OFFSET = 30;
constexpr int QUOTA_USED_X = 112;
constexpr int QUOTA_USED_WIDTH = 100;
constexpr int QUOTA_USED_CAPTION_X = 113;
constexpr int QUOTA_VALUE_CAPTION_WIDTH = 50;
constexpr int QUOTA_COUNTDOWN_X = 276;
constexpr int QUOTA_COUNTDOWN_Y_OFFSET = 2;
constexpr int QUOTA_COUNTDOWN_WIDTH = 144;
constexpr int QUOTA_RESET_CAPTION_X = 424;
constexpr int QUOTA_RESET_CAPTION_Y_OFFSET = 7;
constexpr int QUOTA_RESET_CAPTION_WIDTH = DASHBOARD_TEXT_RIGHT - QUOTA_RESET_CAPTION_X;
constexpr int QUOTA_USAGE_BAR_Y_OFFSET = 50;
constexpr int QUOTA_ELAPSED_BAR_Y_OFFSET = 78;
constexpr int COUNTDOWN_LETTER_SPACE = 2;

constexpr int PROVIDER_LOGO_X = CONTENT_LEFT;
constexpr int PROVIDER_LOGO_Y = 4;
constexpr int PROVIDER_LOGO_SIZE = 48;
constexpr int DASHBOARD_FOOTER_Y = SCREEN_HEIGHT - 14;
constexpr int DASHBOARD_ZONE_WIDTH = 50;
constexpr int DASHBOARD_FOOTER_X = 250;
constexpr int DASHBOARD_FOOTER_WIDTH = CONTENT_RIGHT - DASHBOARD_FOOTER_X;
constexpr int FIVE_HOUR_WINDOW_MINUTES = 300;
constexpr int SEVEN_DAY_WINDOW_MINUTES = 10080;

constexpr int CODEX_PRIMARY_Y = 30;
constexpr int CODEX_MIDDLE_DIVIDER_Y = 149;
constexpr int CODEX_SECONDARY_Y = 184;
constexpr int CODEX_SINGLE_Y = 107;
constexpr int CLAUDE_PRIMARY_Y = 4;
constexpr int CLAUDE_FIRST_DIVIDER_Y = 101;
constexpr int CLAUDE_SECONDARY_Y = 105;
constexpr int CLAUDE_SECOND_DIVIDER_Y = 202;
constexpr int FABLE_LABEL_Y = 211;
constexpr int FABLE_LABEL_WIDTH = 78;
constexpr int FABLE_VALUE_X = 91;
constexpr int FABLE_VALUE_Y = 214;
constexpr int FABLE_VALUE_WIDTH = 65;
constexpr int FABLE_BAR_X = 162;
constexpr int FABLE_BAR_Y = 223;
constexpr int FABLE_BAR_WIDTH = 120;
constexpr int FABLE_COUNTDOWN_X = 248;
constexpr int FABLE_COUNTDOWN_Y = 212;
constexpr int FABLE_COUNTDOWN_WIDTH = QUOTA_RESET_CAPTION_X - FABLE_COUNTDOWN_X;
constexpr int FABLE_RESET_CAPTION_X = 428;
constexpr int FABLE_RESET_CAPTION_Y = 215;
constexpr int FABLE_RESET_CAPTION_WIDTH = DASHBOARD_TEXT_RIGHT - FABLE_RESET_CAPTION_X;
constexpr int FABLE_DIVIDER_Y = 245;
constexpr int EXTRA_HEADING_Y = 254;
constexpr int EXTRA_HEADING_WIDTH = 60;
constexpr int EXTRA_VALUE_X = 91;
constexpr int EXTRA_STATE_X = 64;
constexpr int EXTRA_STATE_WIDTH = EXTRA_VALUE_X - EXTRA_STATE_X;
constexpr int EXTRA_VALUE_Y = 251;
constexpr int EXTRA_VALUE_WIDTH = 85;
constexpr int EXTRA_CAPTION_Y = 281;
constexpr int EXTRA_CAPTION_WIDTH = 50;
constexpr int EXTRA_SPEND_X = 330;
constexpr int EXTRA_SPEND_WIDTH = CONTENT_RIGHT - EXTRA_SPEND_X;
constexpr int EXTRA_LIMIT_X = 275;
constexpr int EXTRA_LIMIT_WIDTH = CONTENT_RIGHT - EXTRA_LIMIT_X;

constexpr uint32_t UI_CREATE_LOCK_TIMEOUT_MS = 1000;
constexpr uint32_t UI_UPDATE_LOCK_TIMEOUT_MS = 500;
constexpr uint32_t UI_REFRESH_INTERVAL_MS = 250;

struct TileEntry {
    ConnectionPage page;
    lv_obj_t *tile;
};

lv_obj_t *root;
TileEntry tiles[MAX_CONNECTION_PAGES];
size_t tile_count;
ConnectionPage active_page = ConnectionPage::Codex;
bool pending_provider_valid;
Provider pending_provider;

struct QuotaBinding {
    QuotaWindow window;
    int window_minutes;
    lv_obj_t *countdown;
    lv_obj_t *elapsed_bar;
    lv_obj_t *elapsed_caption;
};

QuotaBinding quota_bindings[MAX_QUOTA_BINDINGS];
size_t quota_binding_count;

const char *page_name(ConnectionPage page)
{
    switch (page) {
    case ConnectionPage::Codex: return "Codex";
    case ConnectionPage::Claude: return "Claude";
    case ConnectionPage::Add: return "Add";
    }
    return "Unknown";
}

bool is_connected(const ProviderStatus &status)
{
    return status.auth == AuthState::Authenticated || status.auth == AuthState::Refreshing;
}

void style_surface(lv_obj_t *object)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(object, SURFACE_BORDER_WIDTH, 0);
    lv_obj_set_style_radius(object, SURFACE_RADIUS, 0);
}

lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *value = lv_obj_create(parent);
    lv_obj_set_size(value, lv_pct(FULL_PERCENT), lv_pct(FULL_PERCENT));
    lv_obj_set_flex_flow(value, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(value, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(value, LV_DIR_VER);
    lv_obj_add_flag(value, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_style_bg_opa(value, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(value, 0, 0);
    lv_obj_set_style_pad_all(value, COLUMN_PADDING, 0);
    lv_obj_set_style_pad_gap(value, COLUMN_GAP, 0);
    return value;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font = &lv_font_montserrat_14,
                uint32_t color = COLOR_PRIMARY)
{
    lv_obj_t *value = lv_label_create(parent);
    lv_label_set_text(value, text);
    lv_obj_set_width(value, lv_pct(CONTENT_PERCENT));
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(value, font, 0);
    return value;
}

lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *data = nullptr)
{
    lv_obj_t *value = lv_button_create(parent);
    lv_obj_set_height(value, BUTTON_HEIGHT);
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
    char text[PAGE_NUMBER_TEXT_CAPACITY];
    snprintf(text, sizeof(text), "%u / %u  Swipe", static_cast<unsigned>(index + 1),
             static_cast<unsigned>(count));
    lv_obj_t *value = label(parent, text, &lv_font_montserrat_12, COLOR_SECONDARY);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
}

lv_obj_t *dashboard_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                          const lv_font_t *font = &lv_font_montserrat_12, uint32_t color = COLOR_PRIMARY)
{
    lv_obj_t *value = lv_label_create(parent);
    lv_label_set_text(value, text);
    lv_obj_set_pos(value, x, y);
    lv_obj_set_width(value, width);
    lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(value, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(value, font, 0);
    return value;
}

void divider(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, CONTENT_LEFT, y);
    lv_obj_set_size(line, CONTENT_WIDTH, DIVIDER_HEIGHT);
    lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
}


lv_obj_t *dashboard_bar(lv_obj_t *parent, int y, const char *prefix, float percent, uint32_t color,
                        lv_obj_t **caption_output = nullptr)
{
    dashboard_label(parent, prefix, DASHBOARD_BAR_LABEL_X, y + DASHBOARD_BAR_LABEL_Y_OFFSET,
                    DASHBOARD_BAR_LABEL_WIDTH, &lv_font_montserrat_10);
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, DASHBOARD_BAR_X, y);
    lv_obj_set_size(bar, DASHBOARD_BAR_WIDTH, DASHBOARD_BAR_HEIGHT);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    const int value = percent < 0 ? 0 : percent > FULL_PERCENT
                                             ? FULL_PERCENT
                                             : static_cast<int>(percent + PERCENT_ROUNDING);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    char text[BAR_PERCENT_TEXT_CAPACITY];
    snprintf(text, sizeof(text), "%d%%", value);
    lv_obj_t *caption = dashboard_label(parent, text, DASHBOARD_BAR_CAPTION_X,
                                         y + DASHBOARD_BAR_CAPTION_Y_OFFSET,
                                         DASHBOARD_BAR_CAPTION_WIDTH, &lv_font_montserrat_12);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_RIGHT, 0);
    if (caption_output) *caption_output = caption;
    return bar;
}

void add_quota_binding(const QuotaWindow *window, int window_minutes, lv_obj_t *countdown,
                       lv_obj_t *elapsed_bar = nullptr, lv_obj_t *elapsed_caption = nullptr)
{
    if (!window || quota_binding_count >= MAX_QUOTA_BINDINGS) return;
    quota_bindings[quota_binding_count++] = {*window, window_minutes, countdown, elapsed_bar,
                                              elapsed_caption};
}

void update_quota_bindings(int64_t now)
{
    for (size_t index = 0; index < quota_binding_count; ++index) {
        QuotaBinding &binding = quota_bindings[index];
        char countdown[COUNTDOWN_TEXT_CAPACITY];
        format_countdown(binding.window.resets_at, now, countdown, sizeof(countdown));
        lv_label_set_text(binding.countdown, countdown);
        if (!binding.elapsed_bar) continue;
        const float elapsed = elapsed_percent(binding.window, now, binding.window_minutes);
        const int value = elapsed < 0 ? 0 : elapsed > FULL_PERCENT
                                                ? FULL_PERCENT
                                                : static_cast<int>(elapsed + PERCENT_ROUNDING);
        lv_bar_set_value(binding.elapsed_bar, value, LV_ANIM_OFF);
        char text[BAR_PERCENT_TEXT_CAPACITY];
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(binding.elapsed_caption, text);
    }
}

void quota_block(lv_obj_t *parent, const QuotaWindow *window, const char *fallback_period,
                 int y, int window_minutes, uint32_t accent)
{
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int effective_minutes = window && window->window_minutes > 0 ? window->window_minutes : window_minutes;
    const float used = window ? window->used_percent : 0;
    const float elapsed = window ? elapsed_percent(*window, now, effective_minutes) : 0;
    char period[12];
    format_window_period(window, fallback_period, period, sizeof(period));
    char used_text[PERCENT_TEXT_CAPACITY] = "--%";
    char countdown[COUNTDOWN_TEXT_CAPACITY];
    if (window) {
        snprintf(used_text, sizeof(used_text), "%.0f%%", used);
        format_countdown(window->resets_at, now, countdown, sizeof(countdown));
    } else {
        snprintf(countdown, sizeof(countdown), "--:--:--");
    }
    dashboard_label(parent, period, QUOTA_PERIOD_X, y, QUOTA_PERIOD_WIDTH, &lv_font_montserrat_24);
    dashboard_label(parent, used_text, QUOTA_USED_X, y, QUOTA_USED_WIDTH, &lv_font_montserrat_24, accent);
    dashboard_label(parent, "USED", QUOTA_USED_CAPTION_X, y + QUOTA_USED_CAPTION_Y_OFFSET,
                    QUOTA_VALUE_CAPTION_WIDTH, &lv_font_montserrat_12, COLOR_SECONDARY);
    lv_obj_t *count = dashboard_label(parent, countdown, QUOTA_COUNTDOWN_X,
                                      y + QUOTA_COUNTDOWN_Y_OFFSET, QUOTA_COUNTDOWN_WIDTH,
                                      &lv_font_unscii_16);
    lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(count, COUNTDOWN_LETTER_SPACE, 0);
    dashboard_label(parent, "TO RESET", QUOTA_RESET_CAPTION_X, y + QUOTA_RESET_CAPTION_Y_OFFSET,
                    QUOTA_RESET_CAPTION_WIDTH, &lv_font_montserrat_10, COLOR_SECONDARY);
    dashboard_bar(parent, y + QUOTA_USAGE_BAR_Y_OFFSET, "USAGE", used, accent);
    lv_obj_t *elapsed_caption = nullptr;
    lv_obj_t *elapsed_bar = dashboard_bar(parent, y + QUOTA_ELAPSED_BAR_Y_OFFSET, "ELAPSED", elapsed,
                                           COLOR_TIME, &elapsed_caption);
    add_quota_binding(window, effective_minutes, count, elapsed_bar, elapsed_caption);
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

void provider_marker(lv_obj_t *parent, const lv_image_dsc_t *logo)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, logo);
    lv_obj_set_pos(image, PROVIDER_LOGO_X, PROVIDER_LOGO_Y);
    lv_obj_set_size(image, PROVIDER_LOGO_SIZE, PROVIDER_LOGO_SIZE);
}

void dashboard_footer(lv_obj_t *parent, const ProviderStatus &status)
{
    time_t now = time(nullptr);
    struct tm local{};
    char zone[TIMEZONE_TEXT_CAPACITY] = "UK";
    localtime_r(&now, &local);
    strftime(zone, sizeof(zone), "%Z", &local);
    dashboard_label(parent, zone, CONTENT_LEFT, DASHBOARD_FOOTER_Y, DASHBOARD_ZONE_WIDTH,
                    &lv_font_montserrat_12, COLOR_SECONDARY);
    char text[FOOTER_TEXT_CAPACITY];
    FooterTone tone = format_dashboard_footer(status, error_message(status.error), text, sizeof(text));
    uint32_t color = tone == FooterTone::Warning ? COLOR_WARNING : tone == FooterTone::Error ? COLOR_HIGH : COLOR_SECONDARY;
    lv_obj_t *footer = dashboard_label(parent, text, DASHBOARD_FOOTER_X, DASHBOARD_FOOTER_Y,
                                        DASHBOARD_FOOTER_WIDTH, &lv_font_montserrat_12, color);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_RIGHT, 0);
}

void render_codex_dashboard(lv_obj_t *tile, const ProviderStatus &status)
{
    provider_marker(tile, &CODEX_LOGO);
    const QuotaWindow *primary = find_quota_window(status, "Primary");
    const QuotaWindow *secondary = find_quota_window(status, "Secondary");
    const QuotaWindow *top = find_quota_window_by_duration(status, FIVE_HOUR_WINDOW_MINUTES);
    const QuotaWindow *bottom = find_quota_window_by_duration(status, SEVEN_DAY_WINDOW_MINUTES);
    if (!top && primary != bottom) top = primary;
    if (!bottom && secondary != top) bottom = secondary;

    if (top && bottom) {
        quota_block(tile, top, "5H", CODEX_PRIMARY_Y,
                    FIVE_HOUR_WINDOW_MINUTES, COLOR_CODEX);
        divider(tile, CODEX_MIDDLE_DIVIDER_Y);
        quota_block(tile, bottom, "7D", CODEX_SECONDARY_Y,
                    SEVEN_DAY_WINDOW_MINUTES, COLOR_CODEX);
    } else if (top) {
        quota_block(tile, top, "5H", CODEX_SINGLE_Y,
                    FIVE_HOUR_WINDOW_MINUTES, COLOR_CODEX);
    } else if (bottom) {
        quota_block(tile, bottom, "7D", CODEX_SINGLE_Y,
                    SEVEN_DAY_WINDOW_MINUTES, COLOR_CODEX);
    }
    dashboard_footer(tile, status);
}

void render_claude_dashboard(lv_obj_t *tile, const ProviderStatus &status)
{
    provider_marker(tile, &CLAUDE_LOGO);
    quota_block(tile, find_quota_window(status, "5 hour"), "5H", CLAUDE_PRIMARY_Y,
                FIVE_HOUR_WINDOW_MINUTES, COLOR_CLAUDE);
    divider(tile, CLAUDE_FIRST_DIVIDER_Y);
    quota_block(tile, find_quota_window(status, "7 day"), "7D", CLAUDE_SECONDARY_Y,
                SEVEN_DAY_WINDOW_MINUTES, COLOR_CLAUDE);
    divider(tile, CLAUDE_SECOND_DIVIDER_Y);

    const QuotaWindow *fable = find_model_limit(status, "Fable");
    char fable_used[PERCENT_TEXT_CAPACITY] = "--%";
    char fable_countdown[COUNTDOWN_TEXT_CAPACITY] = "--:--:--";
    float used = 0;
    if (fable) {
        used = fable->used_percent;
        snprintf(fable_used, sizeof(fable_used), "%.0f%%", used);
        format_countdown(fable->resets_at, static_cast<int64_t>(time(nullptr)), fable_countdown, sizeof(fable_countdown));
    }
    dashboard_label(tile, fable ? fable->label : "MODEL", CONTENT_LEFT, FABLE_LABEL_Y,
                    FABLE_LABEL_WIDTH, &lv_font_montserrat_16);
    dashboard_label(tile, fable_used, FABLE_VALUE_X, FABLE_VALUE_Y, FABLE_VALUE_WIDTH,
                    &lv_font_montserrat_24, COLOR_CLAUDE);
    lv_obj_t *bar = lv_bar_create(tile);
    lv_obj_set_pos(bar, FABLE_BAR_X, FABLE_BAR_Y);
    lv_obj_set_size(bar, FABLE_BAR_WIDTH, DASHBOARD_BAR_HEIGHT);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_CLAUDE), LV_PART_INDICATOR);
    lv_bar_set_value(bar, used < 0 ? 0 : used > FULL_PERCENT ? FULL_PERCENT : static_cast<int>(used),
                     LV_ANIM_OFF);
    lv_obj_t *fable_reset = dashboard_label(tile, fable_countdown, FABLE_COUNTDOWN_X,
                                             FABLE_COUNTDOWN_Y, FABLE_COUNTDOWN_WIDTH,
                                             &lv_font_unscii_16);
    lv_obj_set_style_text_align(fable_reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(fable_reset, COUNTDOWN_LETTER_SPACE, 0);
    dashboard_label(tile, "TO RESET", FABLE_RESET_CAPTION_X, FABLE_RESET_CAPTION_Y,
                    FABLE_RESET_CAPTION_WIDTH, &lv_font_montserrat_10, COLOR_SECONDARY);
    add_quota_binding(fable, SEVEN_DAY_WINDOW_MINUTES, fable_reset);
    divider(tile, FABLE_DIVIDER_Y);

    const ClaudeExtraUsage &extra = status.extra_usage;
    dashboard_label(tile, "EXTRA", CONTENT_LEFT, EXTRA_HEADING_Y, EXTRA_HEADING_WIDTH,
                    &lv_font_montserrat_16);
    dashboard_label(tile, extra.present && extra.is_enabled ? "ON" : "OFF", EXTRA_STATE_X,
                    EXTRA_HEADING_Y, EXTRA_STATE_WIDTH, &lv_font_montserrat_12,
                    extra.is_enabled ? COLOR_CLAUDE : COLOR_SECONDARY);
    char utilization[UTILIZATION_TEXT_CAPACITY] = "--%";
    if (extra.has_utilization) {
        snprintf(utilization, sizeof(utilization), "%.3f", extra.utilization);
        char *end = utilization + strlen(utilization) - 1;
        while (end > utilization && *end == '0') *end-- = 0;
        if (*end == '.') *end = 0;
        strncat(utilization, "%", sizeof(utilization) - strlen(utilization) - 1);
    }
    dashboard_label(tile, utilization, EXTRA_VALUE_X, EXTRA_VALUE_Y, EXTRA_VALUE_WIDTH,
                    &lv_font_montserrat_24, COLOR_CLAUDE);
    dashboard_label(tile, "used", EXTRA_VALUE_X, EXTRA_CAPTION_Y, EXTRA_CAPTION_WIDTH,
                    &lv_font_montserrat_12, COLOR_SECONDARY);
    const bool usd = strcmp(extra.currency, "usd") == 0 || strcmp(extra.currency, "USD") == 0;
    const char *currency = usd ? "$" : extra.currency;
    char spend[SPEND_TEXT_CAPACITY] = "--";
    const bool has_used = extra.has_spend || extra.has_used_credits;
    const double used_amount = extra.has_spend ? extra.spend : extra.used_credits;
    if (has_used) {
        snprintf(spend, sizeof(spend), usd ? "%s%.*f" : "%s %.*f", currency,
                 extra.spend_decimal_places, used_amount);
    }
    lv_obj_t *spend_label = dashboard_label(tile, spend, EXTRA_SPEND_X, EXTRA_VALUE_Y,
                                             EXTRA_SPEND_WIDTH, &lv_font_montserrat_20);
    lv_obj_set_style_text_align(spend_label, LV_TEXT_ALIGN_RIGHT, 0);
    char limit[LIMIT_TEXT_CAPACITY] = "monthly limit unavailable";
    if (extra.has_monthly_limit) {
        char amount[AMOUNT_TEXT_CAPACITY];
        snprintf(amount, sizeof(amount), usd ? "%s%.*f" : "%s %.*f", currency,
                 extra.limit_decimal_places, extra.monthly_limit);
        if (extra.has_spend_percent) {
            snprintf(limit, sizeof(limit), "of %s / %.0f%%", amount, extra.spend_percent);
        } else {
            snprintf(limit, sizeof(limit), "of %s", amount);
        }
    }
    lv_obj_t *limit_label = dashboard_label(tile, limit, EXTRA_LIMIT_X, EXTRA_CAPTION_Y,
                                             EXTRA_LIMIT_WIDTH, &lv_font_montserrat_12,
                                             COLOR_SECONDARY);
    lv_obj_set_style_text_align(limit_label, LV_TEXT_ALIGN_RIGHT, 0);
    dashboard_footer(tile, status);
}

void render_add_option(lv_obj_t *parent, const char *name, const char *description, Provider provider,
                       const ProviderStatus &status)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(CONTENT_PERCENT));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, CARD_PADDING, 0);
    lv_obj_set_style_pad_gap(card, CARD_GAP, 0);
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

    ConnectionPage pages[MAX_CONNECTION_PAGES];
    tile_count = connection_pages(snapshot, pages, MAX_CONNECTION_PAGES);
    lv_obj_t *tileview = lv_tileview_create(root);
    lv_obj_set_size(tileview, lv_pct(FULL_PERCENT), lv_pct(FULL_PERCENT));
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
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        tiles[index] = {pages[index], tile};
        if (pages[index] == active_page) selected = index;

        switch (pages[index]) {
        case ConnectionPage::Codex:
            render_codex_dashboard(tile, snapshot.openai);
            break;
        case ConnectionPage::Claude:
            render_claude_dashboard(tile, snapshot.claude);
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
    char payload[WIFI_PAYLOAD_CAPACITY];
    if (wifi_qr_payload(snapshot.ap_ssid, snapshot.ap_password, payload, sizeof(payload))) {
        qr(view, payload, LOGIN_QR_SIZE);
    }
    char credentials[WIFI_CREDENTIALS_CAPACITY];
    snprintf(credentials, sizeof(credentials), "Network: %s\nPassword: %s", snapshot.ap_ssid, snapshot.ap_password);
    label(view, credentials);
    button(view, "Clear credentials & restart", reset_wifi);
}

void render_openai(const AppSnapshot &snapshot)
{
    lv_obj_t *view = column(root);
    label(view, "OpenAI device login", &lv_font_montserrat_20);
    qr(view, snapshot.openai.login_url, LOGIN_QR_SIZE);
    char text[LOGIN_INSTRUCTIONS_CAPACITY];
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
    qr(view, snapshot.claude.login_url, LOGIN_QR_SIZE);
    label(view, "Complete authorization, then paste the callback URL or code#state into the authenticated portal.",
          &lv_font_montserrat_12, COLOR_SECONDARY);
    char portal[PORTAL_TEXT_CAPACITY];
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
    quota_binding_count = 0;
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
    if (!bsp_display_lock(UI_CREATE_LOCK_TIMEOUT_MS)) return;
    root = lv_screen_active();
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, lv_color_hex(COLOR_PRIMARY), 0);
    render(app_state_get());
    bsp_display_unlock();
}

void ui_task(void *)
{
    AppSnapshot previous = app_state_get();
    int64_t rendered_second = static_cast<int64_t>(time(nullptr));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_INTERVAL_MS));
        AppSnapshot current = app_state_get();
        const int64_t current_second = static_cast<int64_t>(time(nullptr));
        const bool state_changed = memcmp(&current, &previous, sizeof(current)) != 0;
        if (!state_changed && current_second == rendered_second) continue;
        if (bsp_display_lock(UI_UPDATE_LOCK_TIMEOUT_MS)) {
            if (state_changed) {
                const ConnectionPage previous_page = active_page;
                active_page = focus_after_usage_change(active_page, previous, current);
                if (active_page != previous_page) {
                    ESP_LOGI("quota_ui", "Active page changed %s -> %s", page_name(previous_page),
                             page_name(active_page));
                }
                render(current);
            }
            else update_quota_bindings(current_second);
            bsp_display_unlock();
            previous = current;
            rendered_second = current_second;
        }
    }
}

} // namespace qm
#endif
