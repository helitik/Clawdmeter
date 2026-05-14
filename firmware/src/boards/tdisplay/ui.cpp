#include "ui.h"
#include "splash.h"
#include <lvgl.h>
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

// ---- Title-bar Clawd logo (animated 20×20 pixel-art) ----
// Buffer must outlive the canvas, so it's static. The canvas widget is kept
// so we can invalidate it whenever splash_mini_tick advances a frame.
#define LOGO_SIZE 20
static uint16_t logo_buf[LOGO_SIZE * LOGO_SIZE];
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

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_usage_screen(scr);
    init_bluetooth_screen(scr);
    splash_init(scr);

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
    // Auto-dismiss celebration splash. Done first so the usage-screen
    // animations resume immediately after the return.
    if (celebration_end_ms && lv_tick_get() >= celebration_end_ms) {
        celebration_end_ms = 0;
        ui_show_screen(pre_celebration_screen);
    }

    if (current_screen != SCREEN_USAGE) return;
    uint32_t now = lv_tick_get();
    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
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
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:    splash_show(); break;
    case SCREEN_USAGE:     lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH: lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    // Usage → Bluetooth → Splash → Usage
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:     next = SCREEN_BLUETOOTH; break;
    case SCREEN_BLUETOOTH: next = SCREEN_SPLASH;    break;
    case SCREEN_SPLASH:    next = SCREEN_USAGE;     break;
    default:               next = SCREEN_USAGE;     break;
    }
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
