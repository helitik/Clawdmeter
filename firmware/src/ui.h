#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_CLOCK,
    SCREEN_BLUETOOTH,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);

// Show a brief celebration animation (random pick from splash module's
// celebration pool) then return to whichever screen was visible before.
// Re-triggering while a celebration is already on screen extends the timer.
void ui_celebrate(void);

// Sync the clock display from a daemon-supplied epoch + timezone offset.
// `tz_offset_min` is local minutes ahead of UTC (e.g. 60 for CEST, -300
// for EST). Calling this resets the internal millis() drift correction.
void ui_set_clock_time(uint32_t epoch_seconds, int tz_offset_min);

// Mark that the user is actively coding so the idle-switch timer resets.
// Called on Stop hook events and when rate-group climbs above idle.
void ui_note_activity(void);

// Update the on-screen indicator showing how many tokens are currently in
// Claude Code's context window. Driven by the Stop hook, which reads the
// session transcript and sums the latest turn's input + cache_creation +
// cache_read counters.
void ui_set_context_tokens(uint32_t tokens);
