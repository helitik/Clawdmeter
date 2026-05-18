#include "ui.h"
#include "splash.h"
#include "usage_rate.h"
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>
#include "theme.h"

// How often the clock screen swaps to a new animation within the current
// rate group, matching the fullscreen splash's auto-rotate cadence.
#define CLOCK_ANIM_ROTATE_MS 20000

// 170×320 portrait layout (rotation 2, USB connector at top). Uses LVGL's
// built-in Montserrat fonts and FontAwesome symbol subset — no custom
// font_*.c files to regenerate.

#define SCR_W      170
#define SCR_H      320
#define MARGIN     10

#define COL_BG       THEME_BG
#define COL_PANEL    THEME_PANEL
#define COL_TEXT     THEME_TEXT
#define COL_DIM      THEME_DIM
#define COL_ACCENT   THEME_ACCENT
#define COL_GREEN    THEME_GREEN
#define COL_AMBER    THEME_AMBER
#define COL_RED      THEME_RED
#define COL_BAR_BG   THEME_BAR_BG

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;
// Context-window metric row (third bar under Session/Weekly). Populated by
// the Stop hook → daemon → BLE pipeline; widgets stay at "--%" / blank until
// the first event arrives.
static lv_obj_t* lbl_ctx_label = NULL;
static lv_obj_t* lbl_ctx_pct   = NULL;
static lv_obj_t* bar_ctx       = NULL;
static lv_obj_t* lbl_ctx_abs   = NULL;
static uint32_t  last_ctx_max  = 200000;  // sane default until the daemon supplies one

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ---- Clock screen widgets (declared after LOGO_SIZE further down) ----
static lv_obj_t* clock_container = NULL;
static lv_obj_t* clock_logo_canvas = NULL;
static lv_obj_t* lbl_clock_time = NULL;
static lv_obj_t* lbl_clock_date = NULL;
static lv_obj_t* lbl_clock_msg = NULL;
static splash_mini_state_t clock_logo_state;
static int      clock_last_rate_group = -1;
static uint32_t clock_anim_rotated_ms = 0;

// Clock-sync state. epoch_at_sync is *already* timezone-shifted to local
// seconds-since-epoch so gmtime_r() yields local wall-clock values without
// pulling in newlib's full tz machinery.
static uint32_t clock_local_at_sync = 0;
static uint32_t clock_millis_at_sync = 0;
static bool     clock_synced = false;
static uint32_t clock_last_render_min = UINT32_MAX;

// Activity tracking for the idle auto-switch to the Clock screen.
#define IDLE_SWITCH_MS         (5UL * 60UL * 1000UL)  // 5 minutes
#define MANUAL_OVERRIDE_MS     (2UL * 60UL * 1000UL)  // 2 minutes
static uint32_t last_activity_ms = 0;
static uint32_t manual_override_until_ms = 0;
// Gates the Clock→Usage auto-switch so a fresh boot stays on the Clock
// until something has actually happened (Stop hook fire, rate-group rise).
// Once flipped to true it never resets — the timer-based USAGE→Clock idle
// switch handles "session ended" the same way as before.
static bool     activity_seen = false;

// ---- Title-bar Clawd logo (animated 20×20 pixel-art, upscaled) ----
// Buffer must outlive the canvas, so it's static. The canvas widget is kept
// so we can invalidate it whenever splash_mini_tick advances a frame.
#define LOGO_SIZE         20
#define HEADER_LOGO_SCALE 2
#define HEADER_LOGO_SIZE  (LOGO_SIZE * HEADER_LOGO_SCALE)  // 40 px — give Clawd
                                                           // more presence in the
                                                           // portrait header band
#define CLOCK_LOGO_SCALE 4
#define CLOCK_LOGO_SIZE  (LOGO_SIZE * CLOCK_LOGO_SCALE)   // 80 px — Clawd dominates
                                                          // the clock screen as a
                                                          // visual focal point
static uint16_t logo_buf[HEADER_LOGO_SIZE * HEADER_LOGO_SIZE];
static uint16_t clock_logo_buf[CLOCK_LOGO_SIZE * CLOCK_LOGO_SIZE];
static lv_obj_t* logo_canvas = NULL;
static splash_mini_state_t logo_state;
// Rate-group tracking for the title-bar Clawd so the pictogram swaps
// animation as the user's session intensity climbs — same cadence as the
// fullscreen splash and the clock-screen Clawd.
static int      logo_last_rate_group = -1;
static uint32_t logo_anim_rotated_ms = 0;

// ---- Battery symbol (top-right) ----
// Hidden by default on T-Display S3 (no charge-status pin, often USB-powered,
// reading misleading). Compile with -DSHOW_BATTERY_ICON=1 to opt in.
static lv_obj_t* battery_lbl = NULL;

static screen_t current_screen = SCREEN_USAGE;
static screen_t prev_non_splash_screen = SCREEN_USAGE;

// Celebration state — set non-zero while a Stop-hook-triggered splash is on
// screen; checked from ui_tick_anim() to auto-return to the prior screen.
#define CELEBRATION_DURATION_MS 6000
static screen_t pre_celebration_screen = SCREEN_USAGE;
static uint32_t celebration_end_ms = 0;

// ---- Animated message (Claude Code style — message rotates every 4 s) ----
static uint32_t anim_msg_start = 0;
static uint8_t  anim_msg_idx = 0;
#define ANIM_MSG_MS  4000

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Brewing", "Forging", "Pondering",
    "Cogitating", "Generating", "Ruminating",
    "Conjuring", "Hatching", "Scheming",
    "Cooking", "Ideating", "Simmering",
    "Crafting", "Imagining", "Synthesizing",
    "Creating", "Marinating", "Thinking",
    "Crunching", "Mulling", "Tinkering",
    "Deciphering", "Musing", "Vibing",
    "Doing", "Noodling", "Wandering",
    "Working", "Wrangling", "Whirring",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Build a Session/Weekly row at the given y position. Returns nothing — the
// out_* widgets are stored in the caller's slots for later updates.
static void make_metric_row(lv_obj_t* parent, int y, const char* label_text,
                            lv_obj_t** out_label, lv_obj_t** out_pct,
                            lv_obj_t** out_bar, lv_obj_t** out_reset) {
    *out_label = lv_label_create(parent);
    lv_label_set_text(*out_label, label_text);
    lv_obj_set_style_text_font(*out_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(*out_label, COL_DIM, 0);
    lv_obj_set_pos(*out_label, MARGIN, y);

    *out_pct = lv_label_create(parent);
    lv_label_set_text(*out_pct, "--%");
    lv_obj_set_style_text_font(*out_pct, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_align(*out_pct, LV_ALIGN_TOP_RIGHT, -MARGIN, y - 4);

    *out_bar = lv_bar_create(parent);
    lv_obj_set_pos(*out_bar, MARGIN, y + 30);
    lv_obj_set_size(*out_bar, SCR_W - 2 * MARGIN, 8);
    lv_bar_set_range(*out_bar, 0, 100);
    lv_bar_set_value(*out_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(*out_bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(*out_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(*out_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(*out_bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(*out_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(*out_bar, 4, LV_PART_INDICATOR);

    *out_reset = lv_label_create(parent);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, MARGIN, y + 42);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, SCR_W, SCR_H);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);

    // Animated Clawd pictogram next to the title — reuses splash animation
    // frame data via splash_mini_*. Upscaled to 40×40 so it carries more
    // weight in the portrait header band.
    logo_canvas = lv_canvas_create(usage_container);
    lv_canvas_set_buffer(logo_canvas, logo_buf, HEADER_LOGO_SIZE, HEADER_LOGO_SIZE,
                         LV_COLOR_FORMAT_RGB565);
    splash_mini_init_scaled(&logo_state, 0, logo_buf, HEADER_LOGO_SCALE);
    lv_obj_set_pos(logo_canvas, MARGIN, 8);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // Vertically centered against the 40 px logo (y=8..48 → mid≈28). The 20pt
    // font is ~22 px tall, so a top offset of 18 lines up the text's optical
    // center with the logo's.
    lv_obj_set_pos(lbl_title, MARGIN + HEADER_LOGO_SIZE + 10, 18);

    // Three stacked metric rows. Bigger header pushes the first row down to
    // y=68 (clearing the 48 px logo zone with a small gap), and the spacing
    // is widened to 72 px for a more breathable layout.
    make_metric_row(usage_container, 68, "Session",
                    &lbl_session_label, &lbl_session_pct,
                    &bar_session, &lbl_session_reset);

    make_metric_row(usage_container, 140, "Weekly",
                    &lbl_weekly_label, &lbl_weekly_pct,
                    &bar_weekly, &lbl_weekly_reset);

    // Context row: same layout as Session/Weekly but the footer slot shows
    // "150k / 200k" instead of a reset countdown. Hidden until the first
    // ui_set_context_tokens() call — pre-Stop-hook the figure would be a
    // misleading "--%" on a fresh boot with no active Claude session.
    make_metric_row(usage_container, 212, "Context",
                    &lbl_ctx_label, &lbl_ctx_pct,
                    &bar_ctx, &lbl_ctx_abs);
    lv_obj_add_flag(lbl_ctx_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_ctx_pct,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar_ctx,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_ctx_abs,   LV_OBJ_FLAG_HIDDEN);

    // Animated word ribbon at the bottom of the screen.
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_set_style_text_align(lbl_anim, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -16);
}

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, SCR_W, SCR_H);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, LV_SYMBOL_BLUETOOTH " Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_set_pos(lbl_ble_title, MARGIN, 4);

    lbl_ble_status = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, MARGIN, 50);

    // Device + MAC labels wrap their values to a second line — at 170 px wide
    // a full MAC string "AA:BB:CC:DD:EE:FF" in Montserrat 12 won't fit on the
    // same row as the "MAC: " prefix, so we let it wrap naturally.
    lbl_ble_device = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, MARGIN, 120);
    lv_obj_set_width(lbl_ble_device, SCR_W - 2 * MARGIN);
    lv_label_set_long_mode(lbl_ble_device, LV_LABEL_LONG_WRAP);

    lbl_ble_mac = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_mac, "MAC: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, MARGIN, 170);
    lv_obj_set_width(lbl_ble_mac, SCR_W - 2 * MARGIN);
    lv_label_set_long_mode(lbl_ble_mac, LV_LABEL_LONG_WRAP);

    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

static void init_clock_screen(lv_obj_t* scr) {
    clock_container = lv_obj_create(scr);
    lv_obj_set_size(clock_container, SCR_W, SCR_H);
    lv_obj_set_pos(clock_container, 0, 0);
    lv_obj_set_style_bg_opa(clock_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clock_container, 0, 0);
    lv_obj_set_style_pad_all(clock_container, 0, 0);
    lv_obj_clear_flag(clock_container, LV_OBJ_FLAG_SCROLLABLE);

    // Portrait layout: Clawd up top, big time below, date, then animated
    // word at the bottom — vertically stacked and horizontally centered.
    clock_logo_canvas = lv_canvas_create(clock_container);
    lv_canvas_set_buffer(clock_logo_canvas, clock_logo_buf,
                         CLOCK_LOGO_SIZE, CLOCK_LOGO_SIZE, LV_COLOR_FORMAT_RGB565);
    splash_mini_init_scaled(&clock_logo_state, 0, clock_logo_buf, CLOCK_LOGO_SCALE);
    lv_obj_align(clock_logo_canvas, LV_ALIGN_TOP_MID, 0, 24);

    // Big HH:MM directly under Clawd. The 48 px font is ~50 px tall, so the
    // baseline ends near y=180 — leaves room for the date strip below.
    lbl_clock_time = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_time, "--:--");
    lv_obj_set_style_text_font(lbl_clock_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_clock_time, COL_TEXT, 0);
    lv_obj_set_style_text_align(lbl_clock_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_clock_time, LV_ALIGN_TOP_MID, 0, 130);

    // Date strip under the time.
    lbl_clock_date = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_date, "");
    lv_obj_set_style_text_font(lbl_clock_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_clock_date, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_clock_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_clock_date, LV_ALIGN_TOP_MID, 0, 198);

    // Playful "Musing..." / "Vibing..." word at the bottom, cycling in sync
    // with the Usage screen's spinner message.
    lbl_clock_msg = lv_label_create(clock_container);
    {
        char seed[40];
        snprintf(seed, sizeof(seed), "%s ...", anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_clock_msg, seed);
    }
    lv_obj_set_style_text_font(lbl_clock_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_clock_msg, COL_ACCENT, 0);
    lv_obj_set_style_text_align(lbl_clock_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_clock_msg, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_obj_add_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
}

// Compute the device's current local epoch (already timezone-shifted) using
// millis() drift since the last sync. Falls back to 0 before the first sync.
static uint32_t clock_local_now(void) {
    if (!clock_synced) return 0;
    return clock_local_at_sync + (millis() - clock_millis_at_sync) / 1000;
}

// Render HH:MM + "Day DD Mon YYYY" into the clock labels. Cheap to call;
// gated by clock_last_render_min so we only repaint when the minute flips.
static void clock_render(bool force) {
    if (!clock_synced) {
        lv_label_set_text(lbl_clock_time, "--:--");
        lv_label_set_text(lbl_clock_date, "no sync");
        return;
    }
    time_t t = (time_t)clock_local_now();
    struct tm tm;
    gmtime_r(&t, &tm);  // already-local epoch → don't double-apply tz

    uint32_t this_min = (uint32_t)t / 60;
    if (!force && this_min == clock_last_render_min) return;
    clock_last_render_min = this_min;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(lbl_clock_time, buf);

    static const char* const days[]   = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char* const months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                          "Jul","Aug","Sep","Oct","Nov","Dec" };
    char date_buf[32];
    snprintf(date_buf, sizeof(date_buf), "%s %d %s %d",
             days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], 1900 + tm.tm_year);
    lv_label_set_text(lbl_clock_date, date_buf);
}

void ui_set_clock_time(uint32_t epoch_seconds, int tz_offset_min) {
    clock_local_at_sync = epoch_seconds + (uint32_t)(tz_offset_min * 60);
    clock_millis_at_sync = millis();
    clock_synced = true;
    clock_last_render_min = UINT32_MAX;  // force redraw on next tick
    if (current_screen == SCREEN_CLOCK) clock_render(true);
}

void ui_note_activity(void) {
    last_activity_ms = millis();
    activity_seen = true;
}

void ui_set_project_info(const char* text) {
    (void)text;  // no-op — the project/branch label was too busy in the layout
                 // and rolled back; signature kept so main.cpp still compiles.
}

void ui_set_context_tokens(uint32_t tokens, uint32_t max_tokens) {
    if (!bar_ctx) return;
    if (max_tokens > 0) last_ctx_max = max_tokens;

    // First valid payload reveals the row (hidden at boot to avoid a
    // misleading "--%" before any Claude session has run).
    if (tokens > 0) {
        lv_obj_clear_flag(lbl_ctx_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_ctx_pct,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar_ctx,       LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_ctx_abs,   LV_OBJ_FLAG_HIDDEN);
    }

    // Compute % of context used. Clamp to [0..100] so the bar never
    // overflows even if a future model reports more than the cached max.
    uint32_t pct_u = (last_ctx_max > 0)
        ? (uint32_t)((uint64_t)tokens * 100 / last_ctx_max) : 0;
    if (pct_u > 100) pct_u = 100;
    int pct = (int)pct_u;

    lv_label_set_text_fmt(lbl_ctx_pct, "%d%%", pct);
    lv_bar_set_value(bar_ctx, pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_ctx, pct_color((float)pct), LV_PART_INDICATOR);

    // Footer: "150k / 200k" — readable at a glance, model-agnostic. Round
    // to the nearest thousand so the figure stays compact in the 150-px row.
    char buf[24];
    unsigned long cur_k = ((unsigned long)tokens + 500) / 1000;
    unsigned long max_k = ((unsigned long)last_ctx_max + 500) / 1000;
    snprintf(buf, sizeof(buf), "%luk / %luk", cur_k, max_k);
    lv_label_set_text(lbl_ctx_abs, buf);
}

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_usage_screen(scr);
    init_bluetooth_screen(scr);
    init_clock_screen(scr);
    splash_init(scr);

    last_activity_ms = millis();

#if SHOW_BATTERY_ICON
    // Battery symbol on top of all containers, upper-right. Opt-in: no
    // charge-status pin on T-Display S3 means the reading is often
    // misleading when the board is USB-powered without a battery attached.
    battery_lbl = lv_label_create(scr);
    lv_label_set_text(battery_lbl, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_font(battery_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(battery_lbl, COL_DIM, 0);
    lv_obj_align(battery_lbl, LV_ALIGN_TOP_RIGHT, -MARGIN, 4);
#endif
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    int s_pct = (int)(data->session_pct + 0.5f);
    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);
    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_tick_anim(void) {
    uint32_t now = millis();

    // Auto-dismiss celebration splash. Done first so the usage-screen
    // animations resume immediately after the return.
    if (celebration_end_ms && lv_tick_get() >= celebration_end_ms) {
        celebration_end_ms = 0;
        ui_show_screen(pre_celebration_screen);
    }

    // Idle auto-switch between Usage and Clock. Skipped during celebration
    // (the splash is taking the screen) and during a manual-override window.
    if (!celebration_end_ms && now >= manual_override_until_ms) {
        bool idle = (now - last_activity_ms) >= IDLE_SWITCH_MS;
        if (idle && current_screen == SCREEN_USAGE && clock_synced) {
            ui_show_screen(SCREEN_CLOCK);
        } else if (!idle && activity_seen && current_screen == SCREEN_CLOCK) {
            // Only auto-flip Clock→Usage once we've seen at least one
            // activity event — otherwise a fresh boot with no Claude session
            // running would jump straight to a useless Usage screen.
            ui_show_screen(SCREEN_USAGE);
        }
    }

    // Advance the rotating "Musing..."/"Vibing..."/etc. message every 4 s
    // regardless of which screen is up — Usage and Clock both display it,
    // and we want them to stay in sync if the user cycles between them.
    uint32_t lvt = lv_tick_get();
    bool msg_changed = false;
    if (lvt - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = lvt;
        msg_changed = true;
    }

    // Clock screen tick: re-render time/date once per minute (gated by
    // clock_last_render_min), advance the animated Clawd, refresh the
    // rotating word beneath the Clawd when it ticks. Also swap the Clawd
    // animation to track the current usage rate group (idle/normal/active/
    // heavy) — same rotation cadence as the fullscreen splash.
    if (current_screen == SCREEN_CLOCK) {
        clock_render(false);

        int rg = usage_rate_group();
        bool rate_changed = (rg != clock_last_rate_group);
        bool rotate_due   = (now - clock_anim_rotated_ms) >= CLOCK_ANIM_ROTATE_MS;
        if ((rate_changed || rotate_due) && clock_logo_canvas) {
            int idx = splash_pick_index_for_rate();
            if (idx >= 0) {
                splash_mini_init_scaled(&clock_logo_state, (uint16_t)idx,
                                        clock_logo_buf, CLOCK_LOGO_SCALE);
                lv_obj_invalidate(clock_logo_canvas);
            }
            clock_last_rate_group = rg;
            clock_anim_rotated_ms = now;
        }

        if (clock_logo_canvas &&
            splash_mini_tick_scaled(&clock_logo_state, clock_logo_buf, CLOCK_LOGO_SCALE)) {
            lv_obj_invalidate(clock_logo_canvas);
        }
        if (msg_changed && lbl_clock_msg) {
            static char buf[40];
            snprintf(buf, sizeof(buf), "%s ...", anim_messages[anim_msg_idx]);
            lv_label_set_text(lbl_clock_msg, buf);
        }
        return;
    }

    if (current_screen != SCREEN_USAGE) return;
    if (msg_changed) {
        static char buf[40];
        snprintf(buf, sizeof(buf), "%s ...", anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }

    // Track the same rate group as the fullscreen splash + clock Clawd.
    // splash_pick_index_for_rate() returns the next anim index inside the
    // current group (with the existing 20s rotation cadence).
    int rg = usage_rate_group();
    bool rate_changed = (rg != logo_last_rate_group);
    bool rotate_due   = (now - logo_anim_rotated_ms) >= CLOCK_ANIM_ROTATE_MS;
    if ((rate_changed || rotate_due) && logo_canvas) {
        int idx = splash_pick_index_for_rate();
        if (idx >= 0) {
            splash_mini_init_scaled(&logo_state, (uint16_t)idx,
                                    logo_buf, HEADER_LOGO_SCALE);
            lv_obj_invalidate(logo_canvas);
        }
        logo_last_rate_group = rg;
        logo_anim_rotated_ms = now;
    }

    if (logo_canvas &&
        splash_mini_tick_scaled(&logo_state, logo_buf, HEADER_LOGO_SCALE)) {
        lv_obj_invalidate(logo_canvas);
    }
}

static void apply_battery_visibility(void) {
    if (!battery_lbl) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_lbl, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_lbl, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    if (clock_container) lv_obj_add_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:    splash_show(); break;
    case SCREEN_USAGE:     lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_CLOCK:
        if (clock_container) lv_obj_clear_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
        clock_render(true);
        break;
    case SCREEN_BLUETOOTH: lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    // Usage → Clock → Bluetooth → Splash → Usage
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:     next = SCREEN_CLOCK;     break;
    case SCREEN_CLOCK:     next = SCREEN_BLUETOOTH; break;
    case SCREEN_BLUETOOTH: next = SCREEN_SPLASH;    break;
    case SCREEN_SPLASH:    next = SCREEN_USAGE;     break;
    default:               next = SCREEN_USAGE;     break;
    }
    // Manual cycling suppresses the idle auto-switch for a couple of minutes
    // so the user can dwell on whichever screen they picked.
    manual_override_until_ms = millis() + MANUAL_OVERRIDE_MS;
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_celebrate(void) {
    // If a celebration is already on screen, extend its lifetime. Otherwise
    // capture the screen we'll restore to when the timer fires.
    if (celebration_end_ms == 0) {
        // Capture wherever the user actually is. The re-trigger case (a
        // celebration firing while another is on screen) is already filtered
        // by the celebration_end_ms == 0 guard above, so we never overwrite
        // the saved screen with SCREEN_SPLASH mid-celebration.
        pre_celebration_screen = current_screen;
        lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
        if (clock_container) lv_obj_add_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
        current_screen = SCREEN_SPLASH;
        apply_battery_visibility();
    }
    splash_play_celebration();
    celebration_end_ms = lv_tick_get() + CELEBRATION_DURATION_MS;
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }
    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "MAC: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_lbl) return;
    const char* sym;
    if (charging)              sym = LV_SYMBOL_CHARGE;
    else if (percent < 0)      sym = LV_SYMBOL_BATTERY_EMPTY;
    else if (percent <= 10)    sym = LV_SYMBOL_BATTERY_EMPTY;
    else if (percent <= 35)    sym = LV_SYMBOL_BATTERY_1;
    else if (percent <= 65)    sym = LV_SYMBOL_BATTERY_2;
    else if (percent <= 90)    sym = LV_SYMBOL_BATTERY_3;
    else                       sym = LV_SYMBOL_BATTERY_FULL;
    lv_label_set_text(battery_lbl, sym);
    apply_battery_visibility();
}
