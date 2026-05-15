#pragma once
#include <stdint.h>
#include <lvgl.h>

// Initialize splash module. Creates the canvas widget inside `parent` and
// allocates the 480x480 pixel buffer (PSRAM).
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// Return the next animation index for the current usage-rate group, without
// touching the fullscreen splash state. Round-robins through the group's
// animations and bumps its internal rotation counter, so calling this from
// e.g. the clock screen's mini-canvas advances the same rotation the
// fullscreen splash uses. Returns -1 if no animations are loaded.
int splash_pick_index_for_rate(void);

// Switch to a random energetic animation from the celebration pool and
// show the splash overlay. ui_celebrate() owns the timing — splash is
// purely the animation pick + canvas reveal.
void splash_play_celebration(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Render the first frame of animation `idx` (clamped) into `out` as a
// 20×20 RGB565 grid. Used by the title bar to display Clawd as a static
// pictogram without duplicating splash_animations.h across .cpp files.
// Returns false if no animations are loaded; on false, `out` is unchanged.
bool splash_render_static(uint16_t idx, uint16_t *out);

// Tiny independent animation state — lets a 20×20 canvas (e.g. title-bar
// logo) cycle through frames of a single animation while the fullscreen
// splash module manages its own playback separately. Callers own the
// pixel buffer and the state struct.
typedef struct {
    uint16_t anim_idx;
    uint16_t cur_frame;
    uint32_t frame_started_ms;
} splash_mini_state_t;

// Reset state to frame 0 of `anim_idx` (clamped to the catalog) and
// render that frame into out_buf.
void splash_mini_init(splash_mini_state_t *s, uint16_t anim_idx, uint16_t *out_buf);

// Like splash_mini_init but renders frame 0 at an N× upscale into a
// (20·scale)² buffer. Pair with splash_mini_tick_scaled() at the same
// scale on subsequent frames.
void splash_mini_init_scaled(splash_mini_state_t *s, uint16_t anim_idx,
                             uint16_t *out_buf, uint8_t scale);

// Advance to the next frame if the current frame's hold time has elapsed,
// re-rendering into out_buf. Returns true when a new frame was drawn — the
// caller should invalidate the canvas backed by out_buf in that case.
bool splash_mini_tick(splash_mini_state_t *s, uint16_t *out_buf);

// Same as splash_mini_tick but renders at an N× pixel upscale into a
// (20·scale) × (20·scale) RGB565 buffer. Used for larger logo placements
// like the clock screen's prominently-displayed Clawd next to the time.
bool splash_mini_tick_scaled(splash_mini_state_t *s, uint16_t *out_buf, uint8_t scale);
