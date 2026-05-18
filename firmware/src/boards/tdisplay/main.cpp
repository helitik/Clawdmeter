#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "splash.h"
#include "usage_rate.h"

#define LONG_PRESS_MS  500

// ---- Hardware objects ----
Arduino_DataBus *bus = new Arduino_ESP32LCD8(
    LCD_DC, LCD_CS, LCD_WR, LCD_RD,
    LCD_D0, LCD_D1, LCD_D2, LCD_D3,
    LCD_D4, LCD_D5, LCD_D6, LCD_D7);

Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 0 /* rotation: 0 = portrait 170×320, USB connector at bottom (180° flipped) */, true /* IPS */,
    LCD_NATIVE_W, LCD_NATIVE_H,
    LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);

// ---- LVGL partial-render buffers (1/4 screen each, PSRAM) ----
#define LVGL_BUF_LINES   42
#define LVGL_BUF_PIXELS  (LCD_WIDTH * LVGL_BUF_LINES)
static uint16_t *lvgl_buf1 = nullptr;
static uint16_t *lvgl_buf2 = nullptr;

static UsageData usage = {};
static ble_state_t last_ble_state = BLE_STATE_INIT;

static void backlight_set(uint8_t pct) {
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledcWrite(LCD_BL, duty);
}

static uint32_t my_tick(void) {
    return millis();
}

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    lv_display_flush_ready(disp);
}

// Dispatch an inbound BLE payload. Two shapes are accepted:
//   - usage:  {"s":..,"sr":..,"w":..,"wr":..,"st":"...","ok":true}
//   - event:  {"e":"done"}   ← Claude Code Stop-hook signal
// Events fire side-effects (e.g. celebration animation) without touching
// the cached UsageData. ACK is sent either way for a parseable payload.
static void on_ble_data(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        ble_send_nack();
        return;
    }

    // Optional context-token count piggy-backed on any payload. `cm` is the
    // model's context-window max (e.g. 200000) — passed alongside `c` so the
    // firmware can render the bar without baking the limit into the build.
    uint32_t ctx_tokens = doc["c"]  | (uint32_t)0;
    uint32_t ctx_max    = doc["cm"] | (uint32_t)0;
    if (ctx_tokens > 0) ui_set_context_tokens(ctx_tokens, ctx_max);

    // Optional project + branch label ("Clawdmeter / tdisplay-portrait").
    const char* prj = doc["p"] | (const char*)nullptr;
    if (prj && *prj) ui_set_project_info(prj);

    const char* event = doc["e"] | (const char*)nullptr;
    if (event && *event) {
        Serial.printf("event: %s c=%lu\n", event, (unsigned long)ctx_tokens);
        if (strcmp(event, "done") == 0) {
            ui_note_activity();   // Stop hook fired → user is actively coding
            ui_celebrate();
        }
        ble_send_ack();
        return;
    }

    usage.session_pct = doc["s"] | 0.0f;
    usage.session_reset_mins = doc["sr"] | -1;
    usage.weekly_pct = doc["w"] | 0.0f;
    usage.weekly_reset_mins = doc["wr"] | -1;
    strlcpy(usage.status, doc["st"] | "unknown", sizeof(usage.status));
    usage.ok = doc["ok"] | false;
    usage.valid = true;

    // Clock sync: optional "t" (epoch seconds) + "tz" (offset minutes east
    // of UTC). The clock screen needs this to display wall-clock time
    // without pulling NTP onto the firmware.
    uint32_t t  = doc["t"]  | (uint32_t)0;
    int      tz = doc["tz"] | 0;
    if (t > 0) ui_set_clock_time(t, tz);

    int g_before = usage_rate_group();
    usage_rate_sample(usage.session_pct);
    int g_after = usage_rate_group();
    if (g_after != g_before && splash_is_active()) {
        splash_pick_for_current_rate();
    }
    // Active rate group (anything above idle) also counts as user activity
    // for the idle-switch timer, in case the Stop hook isn't configured.
    if (g_after >= 1) ui_note_activity();
    Serial.printf("usage: s=%.1f%%, w=%.1f%%\n", usage.session_pct, usage.weekly_pct);
    ui_update(&usage);
    ble_send_ack();
}

// ---- Serial screenshot command (mirrors AMOLED build) ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) { Serial.println("SCREENSHOT_ERR"); return; }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);
    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }
    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "cycle") == 0) ui_cycle_screen();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"boot\":\"tdisplay-s3\"}");

    pinMode(LCD_POWER_ON, OUTPUT);
    digitalWrite(LCD_POWER_ON, HIGH);
    delay(50);

    if (!gfx->begin()) Serial.println("gfx->begin() failed");
    gfx->fillScreen(0x0000);  // black (Arduino_GFX named constants are RGB565_*)

    ledcAttach(LCD_BL, 5000, 8);
    backlight_set(80);

    Serial.printf("Display ready: %dx%d\n", gfx->width(), gfx->height());

    // ---- LVGL ----
    lv_init();
    lv_tick_set_cb(my_tick);

    lvgl_buf1 = (uint16_t*)heap_caps_malloc(LVGL_BUF_PIXELS * 2, MALLOC_CAP_SPIRAM);
    lvgl_buf2 = (uint16_t*)heap_caps_malloc(LVGL_BUF_PIXELS * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, lvgl_buf1, lvgl_buf2, LVGL_BUF_PIXELS * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // ---- BLE ----
    ble_init();

    // ---- Power (ADC battery) ----
    power_init();

    // ---- Buttons ----
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    // ---- UI ----
    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_battery_pct(), power_is_charging());
    // Boot to the Clock — Usage only makes sense once a Claude session is
    // running (context bar would be empty, session/weekly bars at 0%). The
    // auto-switch in ui_tick_anim flips to Usage on the first ui_note_activity()
    // call (Stop hook or rate-group rise) and back to Clock after 5 min idle.
    ui_show_screen(SCREEN_CLOCK);

    Serial.println("Ready, advertising as 'Claude Controller'");
}

// Per-button state. Short press fires HID on release. Long press fires
// screen-cycle action mid-hold and inhibits the HID release event so the
// same gesture doesn't both inject a key and switch screens.
struct ButtonState {
    bool     was_pressed;
    uint32_t press_start_ms;
    bool     long_handled;
};
static ButtonState btn_left  = {false, 0, false};
static ButtonState btn_right = {false, 0, false};

// Poll a single button. `hid_key`/`hid_mod` are sent on short press release.
// On long-press while held, ui_cycle_screen() is invoked once.
static void poll_button(ButtonState* st, int pin, uint8_t hid_key, uint8_t hid_mod) {
    bool now_pressed = (digitalRead(pin) == LOW);
    uint32_t now = millis();

    if (now_pressed && !st->was_pressed) {
        // Edge: just pressed
        st->press_start_ms = now;
        st->long_handled = false;
    } else if (now_pressed && st->was_pressed) {
        // Still held — fire long-press once threshold crossed
        if (!st->long_handled && (now - st->press_start_ms) >= LONG_PRESS_MS) {
            st->long_handled = true;
            ui_cycle_screen();
        }
    } else if (!now_pressed && st->was_pressed) {
        // Edge: just released
        if (!st->long_handled) {
            // Short press → HID press + immediate release
            ble_keyboard_press(hid_key, hid_mod);
            delay(15);  // typical key-event spacing
            ble_keyboard_release();
        }
    }
    st->was_pressed = now_pressed;
}

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_tick();
    splash_tick();

    // Buttons: short = HID, long = screen cycle.
    //   LEFT (GPIO 0)  → Space (Claude Code voice mode)
    //   RIGHT (GPIO 14) → Shift+Tab (Claude Code mode toggle)
    poll_button(&btn_left,  BTN_LEFT,  0x2C, 0);     // Space, no mods
    poll_button(&btn_right, BTN_RIGHT, 0x2B, 0x02);  // Tab + LEFT_SHIFT

    // BLE state change → refresh UI
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Battery change → refresh UI
    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Incoming JSON from daemon (usage payload or event)
    if (ble_has_data()) {
        on_ble_data(ble_get_data());
    }

    check_serial_cmd();
    delay(5);
}
