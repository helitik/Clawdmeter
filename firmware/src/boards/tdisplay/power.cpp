#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// LilyGO T-Display S3: battery sense on GPIO 4 (ADC1_CH3) through a 100k/100k
// resistor divider. The board has no PMU and no direct VBUS GPIO, so we
// cannot detect charging state — power_is_charging() always returns false
// for this variant. power_pwr_pressed() is unused (long-press in main.cpp
// drives screen cycling instead of a dedicated button).

#define BATTERY_POLL_MS  2000
#define SAMPLES          8

// LiPo discharge curve approximated linear over 3.30 V (0%) → 4.20 V (100%).
// Below 3.30 V we clamp at 0; above 4.20 V we clamp at 100.
#define V_EMPTY  3.30f
#define V_FULL   4.20f

static int      cached_pct      = -1;
static uint32_t last_battery_ms = 0;

static float read_battery_voltage(void) {
    uint32_t sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sum += analogRead(BAT_ADC_PIN);
    }
    float raw = (float)sum / SAMPLES;
    // ESP32-S3 ADC is 12-bit by default, ref ~3.3V (with calibration the
    // mapping is approximately linear in the 0–3.0V range; the divider keeps
    // us well within that).
    float v_pin = raw * 3.3f / 4095.0f;
    return v_pin * BAT_ADC_DIVIDER;
}

void power_init(void) {
    analogReadResolution(12);
    pinMode(BAT_ADC_PIN, INPUT);

    float v = read_battery_voltage();
    Serial.printf("Battery voltage at boot: %.2f V\n", v);
    float frac = (v - V_EMPTY) / (V_FULL - V_EMPTY);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    cached_pct = (int)(frac * 100.0f + 0.5f);
}

void power_tick(void) {
    uint32_t now = millis();
    if (now - last_battery_ms < BATTERY_POLL_MS) return;
    last_battery_ms = now;

    float v = read_battery_voltage();
    float frac = (v - V_EMPTY) / (V_FULL - V_EMPTY);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    cached_pct = (int)(frac * 100.0f + 0.5f);
}

int power_battery_pct(void) {
    return cached_pct;
}

bool power_is_charging(void) {
    // T-Display S3 has no VBUS sense pin or charge-status pin exposed. We
    // could read USBSerial state as a proxy for VBUS but it conflates "USB
    // host attached" with "charging", which is misleading. Leave it false.
    return false;
}

bool power_pwr_pressed(void) {
    return false;  // not used on T-Display S3
}
