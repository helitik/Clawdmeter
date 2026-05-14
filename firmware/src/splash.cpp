#include "splash.h"
#include "splash_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// 20×20 pixel-art grid. CELL is the upscale factor — overridable per board
// via build flag (e.g. -DSPLASH_CELL=8 for a 160×160 canvas on smaller
// displays). Default 24× fills a 480×480 panel.
#define GRID         20
#ifndef SPLASH_CELL
#define SPLASH_CELL  24
#endif
#define CELL         SPLASH_CELL
#define CANVAS_W     (GRID * CELL)
#define CANVAS_H     (GRID * CELL)

// Background fallback when palette is missing
#define COL_EMPTY    0x0000  // true black (matches THEME_BG)

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;

// While splash is showing, auto-cycle to the next animation in the current
// rate-driven group every this many ms.
#define SPLASH_ROTATE_INTERVAL_MS 20000

// Usage-rate animation groups: 4 groups × up to 4 animations each.
// Filled at init by matching literal names from splash_anims[].
#define GROUP_COUNT 4
#define GROUP_MAX   4
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

static const char* GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    // Group 0 — idle / sleepy
    { "expression sleep", "idle breathe", "idle blink", "expression wink" },
    // Group 1 — normal pace
    { "idle look around", "work think", "work coding", NULL },
    // Group 2 — active
    { "dance sway", "expression surprise", "dance bounce", NULL },
    // Group 3 — heavy
    { "dance bounce dj", "dance sway dj", "dance djmix", NULL },
};

// Celebration pool — picked at random by splash_play_celebration() when the
// host signals that Claude finished responding. Energetic dance/surprise
// animations to make the device feel alive at the moment of attention.
#define CELEBRATION_MAX 5
static const char* CELEBRATION_NAMES[CELEBRATION_MAX] = {
    "dance bounce dj", "dance sway dj", "dance djmix",
    "expression surprise", "dance bounce",
};
static int8_t  celebration_pool[CELEBRATION_MAX];
static uint8_t celebration_pool_size = 0;

static void resolve_celebration_pool(void) {
    celebration_pool_size = 0;
    for (int s = 0; s < CELEBRATION_MAX; s++) {
        celebration_pool[s] = -1;
        const char* want = CELEBRATION_NAMES[s];
        if (!want) continue;
        for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
            if (strcmp(splash_anims[i].name, want) == 0) {
                celebration_pool[celebration_pool_size++] = (int8_t)i;
                break;
            }
        }
    }
}

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
            const char* want = GROUP_NAMES[g][s];
            if (!want) continue;
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].name, want) == 0) {
                    group_lists[g][group_size[g]++] = (int8_t)i;
                    break;
                }
            }
        }
    }
}

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    for (int gy = 0; gy < GRID; gy++) {
        uint16_t row[CANVAS_W];
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row[gx * CELL];
            for (int i = 0; i < CELL; i++) p[i] = color;
        }
        for (int dy = 0; dy < CELL; dy++) {
            memcpy(&canvas_buf[(gy * CELL + dy) * CANVAS_W], row, CANVAS_W * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}

static void show_placeholder() {
    // Solid dark background + centered status label.
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) canvas_buf[i] = COL_EMPTY;
    if (canvas) lv_obj_invalidate(canvas);
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    canvas_buf = (uint16_t*)heap_caps_malloc(CANVAS_W * CANVAS_H * 2, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    splash_container = lv_obj_create(parent);
    // Match the parent screen size so the splash overlay fully covers the
    // background regardless of panel resolution. Canvas inside is fixed at
    // GRID*CELL and centered by lv_obj_center() below.
    lv_obj_set_size(splash_container, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    // Center the canvas in its container. On AMOLED the canvas (480×480) fills
    // the screen so alignment is moot; on T-Display S3 portrait (170×320, canvas
    // 160×160) centering gives equal breathing room above the dj-headphone
    // rows and below the legs, vs. bottom-align which left the top half blank.
    lv_obj_center(canvas);

    // Placeholder label (visible only when no animations are loaded)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "run tools/scrape_claudepix.js\n"
        "then tools/convert_to_c.js");
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();
    resolve_celebration_pool();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        const splash_anim_def_t *a = &splash_anims[0];
        render_frame(a->frames[0], a->palette);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
    if (!active || SPLASH_ANIM_COUNT == 0) return;

    // Auto-rotate to the next animation in the current group.
    if (millis() - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        splash_pick_for_current_rate();
    }

    const splash_anim_def_t *a = &splash_anims[cur_anim];
    if (a->frame_count == 0) return;

    uint16_t hold = a->holds[cur_frame];
    if (millis() - frame_started_ms >= hold) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms = millis();
        render_frame(a->frames[cur_frame], a->palette);
    }
}

void splash_next(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    cur_anim = (cur_anim + 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
    Serial.printf("splash: -> %s\n", a->name);
}

// Bumps the current rate-group's rotation counter and returns the next
// animation index, or -1 if no animations are loaded. Shared by the
// fullscreen splash and the clock-screen mini canvas so they round-robin
// through the same group rotation.
static int8_t pick_next_for_rate(void) {
    if (SPLASH_ANIM_COUNT == 0) return -1;
    int g = usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return -1;
    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    return group_lists[g][slot];
}

void splash_pick_for_current_rate(void) {
    int8_t idx = pick_next_for_rate();
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
}

int splash_pick_index_for_rate(void) {
    int8_t idx = pick_next_for_rate();
    return (idx < 0) ? -1 : (int)idx;
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    splash_pick_for_current_rate();
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}

void splash_play_celebration(void) {
    if (SPLASH_ANIM_COUNT == 0 || celebration_pool_size == 0) return;
    uint32_t r = millis() ^ (millis() >> 16);
    uint8_t slot = (uint8_t)(r % celebration_pool_size);
    int8_t idx = celebration_pool[slot];
    if (idx < 0) return;
    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    // Push the rate-rotate deadline far enough that the celebration won't
    // be interrupted by the auto-cycle in splash_tick().
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
    Serial.printf("splash: celebration -> %s\n", a->name);
}

static void render_frame_into(const splash_anim_def_t *a, uint16_t frame, uint16_t *out) {
    const uint8_t *cells = a->frames[frame];
    for (int i = 0; i < GRID * GRID; i++) {
        uint8_t code = cells[i];
        out[i] = (code < SPLASH_PALETTE_SIZE) ? a->palette[code] : COL_EMPTY;
    }
}

bool splash_render_static(uint16_t idx, uint16_t *out) {
    if (SPLASH_ANIM_COUNT == 0) return false;
    if (idx >= SPLASH_ANIM_COUNT) idx = 0;
    const splash_anim_def_t *a = &splash_anims[idx];
    if (a->frame_count == 0 || a->palette == NULL) return false;
    render_frame_into(a, 0, out);
    return true;
}

void splash_mini_init(splash_mini_state_t *s, uint16_t anim_idx, uint16_t *out_buf) {
    if (SPLASH_ANIM_COUNT == 0) {
        s->anim_idx = 0;
        s->cur_frame = 0;
        s->frame_started_ms = 0;
        return;
    }
    if (anim_idx >= SPLASH_ANIM_COUNT) anim_idx = 0;
    s->anim_idx = anim_idx;
    s->cur_frame = 0;
    s->frame_started_ms = millis();
    splash_render_static(anim_idx, out_buf);
}

bool splash_mini_tick(splash_mini_state_t *s, uint16_t *out_buf) {
    if (SPLASH_ANIM_COUNT == 0) return false;
    const splash_anim_def_t *a = &splash_anims[s->anim_idx];
    if (a->frame_count == 0 || a->palette == NULL) return false;

    uint16_t hold = a->holds[s->cur_frame];
    if (millis() - s->frame_started_ms < hold) return false;

    s->cur_frame = (s->cur_frame + 1) % a->frame_count;
    s->frame_started_ms = millis();
    render_frame_into(a, s->cur_frame, out_buf);
    return true;
}

static void render_frame_scaled(const splash_anim_def_t *a, uint16_t frame,
                                uint16_t *out, uint8_t scale);

void splash_mini_init_scaled(splash_mini_state_t *s, uint16_t anim_idx,
                             uint16_t *out_buf, uint8_t scale) {
    if (SPLASH_ANIM_COUNT == 0 || scale == 0) {
        s->anim_idx = 0; s->cur_frame = 0; s->frame_started_ms = 0;
        return;
    }
    if (anim_idx >= SPLASH_ANIM_COUNT) anim_idx = 0;
    s->anim_idx = anim_idx;
    s->cur_frame = 0;
    s->frame_started_ms = millis();
    const splash_anim_def_t *a = &splash_anims[anim_idx];
    if (a->frame_count == 0 || a->palette == NULL) return;
    render_frame_scaled(a, 0, out_buf, scale);
}

static void render_frame_scaled(const splash_anim_def_t *a, uint16_t frame,
                                uint16_t *out, uint8_t scale) {
    const uint8_t *cells = a->frames[frame];
    int W = GRID * scale;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (code < SPLASH_PALETTE_SIZE) ? a->palette[code] : COL_EMPTY;
            // Write the scale×scale block for this source pixel.
            for (int dy = 0; dy < scale; dy++) {
                uint16_t *row = &out[(gy * scale + dy) * W + gx * scale];
                for (int dx = 0; dx < scale; dx++) row[dx] = color;
            }
        }
    }
}

bool splash_mini_tick_scaled(splash_mini_state_t *s, uint16_t *out_buf, uint8_t scale) {
    if (SPLASH_ANIM_COUNT == 0 || scale == 0) return false;
    const splash_anim_def_t *a = &splash_anims[s->anim_idx];
    if (a->frame_count == 0 || a->palette == NULL) return false;

    uint16_t hold = a->holds[s->cur_frame];
    if (millis() - s->frame_started_ms < hold) return false;

    s->cur_frame = (s->cur_frame + 1) % a->frame_count;
    s->frame_started_ms = millis();
    render_frame_scaled(a, s->cur_frame, out_buf, scale);
    return true;
}
