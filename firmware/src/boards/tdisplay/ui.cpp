#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>
#include "theme.h"

// 320×170 landscape layout. Uses LVGL's built-in Montserrat fonts and
// FontAwesome symbol subset — no custom font_*.c files to regenerate.

#define SCR_W      320
#define SCR_H      170
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

// ---- Title-bar Clawd logo (animated 20×20 pixel-art) ----
// Buffer must outlive the canvas, so it's static. The canvas widget is kept
// so we can invalidate it whenever splash_mini_tick advances a frame.
#define LOGO_SIZE 20
#define CLOCK_LOGO_SCALE 4
#define CLOCK_LOGO_SIZE  (LOGO_SIZE * CLOCK_LOGO_SCALE)   // 80 px — Clawd dominates
                                                          // the clock screen as a
                                                          // visual focal point
static uint16_t logo_buf[LOGO_SIZE * LOGO_SIZE];
static uint16_t clock_logo_buf[CLOCK_LOGO_SIZE * CLOCK_LOGO_SIZE];
static lv_obj_t* logo_canvas = NULL;
static splash_mini_state_t logo_state;

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
    lv_obj_set_style_text_font(*out_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(*out_label, COL_DIM, 0);
    lv_obj_set_pos(*out_label, MARGIN, y);

    *out_pct = lv_label_create(parent);
    lv_label_set_text(*out_pct, "--%");
    lv_obj_set_style_text_font(*out_pct, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_align(*out_pct, LV_ALIGN_TOP_RIGHT, -MARGIN, y - 2);

    *out_bar = lv_bar_create(parent);
    lv_obj_set_pos(*out_bar, MARGIN, y + 22);
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
    lv_obj_set_style_text_font(*out_reset, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, MARGIN, y + 34);
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
    // frame data via splash_mini_*. Animation index 0 is the first entry in
    // splash_anims[] (typically a calm "idle breathe"), which is what we
    // want for a title-bar accent.
    logo_canvas = lv_canvas_create(usage_container);
    lv_canvas_set_buffer(logo_canvas, logo_buf, LOGO_SIZE, LOGO_SIZE, LV_COLOR_FORMAT_RGB565);
    splash_mini_init(&logo_state, 0, logo_buf);
    lv_obj_set_pos(logo_canvas, MARGIN, 1);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_set_pos(lbl_title, MARGIN + LOGO_SIZE + 6, 4);

    // Session @ y=26 (label/pct/bar/reset total height ~50 px)
    make_metric_row(usage_container, 26, "Session",
                    &lbl_session_label, &lbl_session_pct,
                    &bar_session, &lbl_session_reset);

    // Weekly @ y=84
    make_metric_row(usage_container, 84, "Weekly",
                    &lbl_weekly_label, &lbl_weekly_pct,
                    &bar_weekly, &lbl_weekly_reset);

    // Bottom message ribbon
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_LEFT, MARGIN, -2);
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
    lv_obj_set_pos(lbl_ble_status, MARGIN, 30);

    lbl_ble_device = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, MARGIN, 64);

    lbl_ble_mac = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_mac, "MAC: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, MARGIN, 82);

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

    // Animated Clawd at 4× upscale (80×80), centered vertically. Positioned
    // so that Clawd + the time digits form a roughly-centered group on the
    // 320 px wide screen (≈42 px outer margin on either side).
    clock_logo_canvas = lv_canvas_create(clock_container);
    lv_canvas_set_buffer(clock_logo_canvas, clock_logo_buf,
                         CLOCK_LOGO_SIZE, CLOCK_LOGO_SIZE, LV_COLOR_FORMAT_RGB565);
    splash_mini_init_scaled(&clock_logo_state, 0, clock_logo_buf, CLOCK_LOGO_SCALE);
    lv_obj_align(clock_logo_canvas, LV_ALIGN_LEFT_MID, 28, 0);

    // Big HH:MM next to Clawd. Y-offset shifts the time down slightly so
    // its visual centre aligns with Clawd's visible body (Clawd's head sits
    // near the top of its 80×80 canvas, so the body's optical centre is
    // below the canvas midpoint).
    lbl_clock_time = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_time, "--:--");
    lv_obj_set_style_text_font(lbl_clock_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_clock_time, COL_TEXT, 0);
    lv_obj_align(lbl_clock_time, LV_ALIGN_RIGHT_MID, -28, -10);

    // Date underneath the time, right-aligned to match.
    lbl_clock_date = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_date, "");
    lv_obj_set_style_text_font(lbl_clock_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_clock_date, COL_DIM, 0);
    lv_obj_align(lbl_clock_date, LV_ALIGN_RIGHT_MID, -28, 32);

    // Playful "Musing..." / "Vibing..." word under the Clawd, cycling in
    // sync with the Usage screen's spinner message. The label centre is
    // pinned to the canvas centre (x = 28 offset + half of 80 = 68), one
    // line below the canvas bottom.
    lbl_clock_msg = lv_label_create(clock_container);
    {
        char seed[40];
        snprintf(seed, sizeof(seed), "%s ...", anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_clock_msg, seed);
    }
    lv_obj_set_style_text_font(lbl_clock_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_clock_msg, COL_ACCENT, 0);
    lv_obj_set_style_text_align(lbl_clock_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_clock_msg, LV_ALIGN_LEFT_MID, 68, 54);
    lv_obj_set_style_translate_x(lbl_clock_msg, LV_PCT(-50), 0);

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
        } else if (!idle && current_screen == SCREEN_CLOCK) {
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
    // rotating word beneath the Clawd when it ticks.
    if (current_screen == SCREEN_CLOCK) {
        clock_render(false);
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
    if (logo_canvas && splash_mini_tick(&logo_state, logo_buf)) {
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
        pre_celebration_screen = (current_screen == SCREEN_SPLASH)
                                     ? prev_non_splash_screen
                                     : current_screen;
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
