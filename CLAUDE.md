# Project context

ESP32-S3 firmware for a desk-side Claude Code usage monitor. Two boards are supported, each as its own PlatformIO env:

- **`waveshare_amoled_216`** — Waveshare ESP32-S3-Touch-AMOLED-2.16 (480×480 square AMOLED, touch, IMU, AXP2101 PMU). Original target.
- **`tdisplay_s3`** — LilyGO T-Display S3 (170×320 ST7789 i80, 2 buttons, ADC battery, no touch, no IMU, no PMU). Added on branch `tdisplay-s3`.

Both connect to the same host daemon over BLE; daemon polls Anthropic API for usage data.

This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Repo layout

```
firmware/src/
├── ble.{h,cpp}                    SHARED — NimBLE peripheral + HID keyboard
├── data.h                         SHARED — UsageData struct
├── ui.h                           SHARED — UI interface (each board implements)
├── theme.h                        SHARED — design tokens (colors)
├── usage_rate.{h,cpp}             SHARED — ring buffer + rate-group classifier
├── splash.{h,cpp}                 SHARED — 20×20 pixel-art animation engine,
│                                  CELL upscale factor overridable via -DSPLASH_CELL
├── splash_animations.h            SHARED — generated, do not hand-edit
├── power.h                        SHARED — interface (battery %, charging, pwr button)
├── boards/amoled/                 Waveshare-specific implementations + assets
│   ├── main.cpp, display_cfg.h, ui.cpp, power.cpp, imu.{h,cpp}
│   ├── icons.h (48×48), logo.h (80×80)
│   └── font_*.c (Tiempos 56, Styrene 48/28/24/20, Mono 32, …)
└── boards/tdisplay/               T-Display S3-specific implementations
    ├── main.cpp, display_cfg.h, ui.cpp, power.cpp
    └── (uses LVGL built-in Montserrat 12/14/20 + FontAwesome symbols, no custom fonts)
```

PlatformIO `build_src_filter` excludes the other board's dir per env. The shared `power.h` interface keeps `power.cpp` board-specific but its API stable.

## Hardware

### Waveshare AMOLED 2.16" (env `waveshare_amoled_216`)

- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → cycle screens; on splash → cycle animations)

### LilyGO T-Display S3 standard (env `tdisplay_s3`)

- Display: **ST7789** 170×320 IPS via 8-bit i80 parallel
  - CS=6, DC=7, WR=8, RD=9, RST=5, D0..D7 = 39,40,41,42,45,46,47,48
  - Backlight PWM on GPIO 38, LCD power enable on GPIO 15 (drive HIGH!)
  - Col offset 35 in controller frame buffer
  - Rotation 1 → 320×170 landscape
- Battery: GPIO 4 (ADC1_CH3) via 100k/100k divider → V_bat = ADC × 2 × (3.3/4095)
- Buttons: GPIO 0 (BTN_LEFT → short=Space, long=cycle screen), GPIO 14 (BTN_RIGHT → short=Shift+Tab, long=cycle screen)
- No PMU, no IMU, no touch, no charging detection (no VBUS GPIO exposed)

## Build / flash

Default env is `tdisplay_s3`. Specify explicitly with `-e <env>`:

```bash
pio run -d firmware -e tdisplay_s3                                       # build
pio run -d firmware -e tdisplay_s3 -t upload --upload-port /dev/ttyACM0  # flash
pio run -d firmware -e waveshare_amoled_216 -t upload                    # other board
```

Or via the helper scripts:

```bash
./flash.sh                                # uses BOARD env var (default: tdisplay_s3)
BOARD=waveshare_amoled_216 ./flash.sh
./screenshot.sh                           # auto-detects panel dims from device
```

`/home/hermann/.platformio/penv/bin/pio` if `pio` isn't on PATH.

Device shows up as `/dev/ttyACM0` (Espressif USB JTAG/serial debug unit). No boot-mode gymnastics needed — direct flash works.

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer over `/dev/ttyACM0`. `./screenshot.sh out.png /dev/ttyACM0` captures a 480×480 PNG. **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

### AMOLED-specific (env `waveshare_amoled_216`)

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping in `my_flush_cb`** in main.cpp. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw.
2. **CO5300 needs even-aligned flush regions.** `rounder_cb` enforces this.
3. **Touch reading must be centralized.** CST9220's `getPoint()` does a full I2C transaction. Calling it from multiple places consumed each other's data and broke input. `touch_read()` is called once per loop in main.cpp; both LVGL `my_touch_cb` and `touch.cpp` read from shared `touch_pressed/touch_x/touch_y` state.
4. **Touch `setSwapXY(true)` and `setMirrorXY(true, false)`** are the empirically-correct values for default rotation 0. IMU rotation logic doesn't change touch mapping (it does CPU-side rotation of the rendered pixels, so LVGL still thinks the display is portrait at 0°).

### T-Display S3-specific (env `tdisplay_s3`)

1. **GPIO 15 LCD power.** Must `pinMode(15, OUTPUT); digitalWrite(15, HIGH);` BEFORE `gfx->begin()` or the panel stays dark. Easy to miss because `gfx->begin()` doesn't error out — it just sends commands into the void.
2. **Column offset 35.** The ST7789 controller has a 240-wide frame buffer but the panel exposes only 170 columns starting at index 35. Pass `(col_offset1=35, row_offset1=0, col_offset2=35, row_offset2=0)` to the `Arduino_ST7789` constructor.
3. **No charging detection.** No VBUS GPIO is exposed; `power_is_charging()` always returns false. The battery icon shows a level only, never the charging bolt.
4. **No HID middle button.** With only 2 buttons (vs. 3 on AMOLED), short-press = HID key (Space / Shift+Tab), long-press (≥500 ms) on either button = cycle screen. Don't try to send HID on long-press — the short-press handler is gated on `!st->long_handled`.

### Shared

5. **OPI PSRAM** required on both boards: `board_build.arduino.memory_type = qio_opi`. Without this, `MALLOC_CAP_SPIRAM` returns NULL and LVGL buffers fail to allocate.
6. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
7. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible. (Only relevant when adding new custom fonts to `boards/amoled/`. The `tdisplay_s3` env uses LVGL's built-in Montserrat fonts to avoid this entirely.)
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **`splash.cpp` upscale is per-board.** Default `SPLASH_CELL=24` gives a 480×480 canvas; override with `-DSPLASH_CELL=8` for 160×160 (T-Display S3). The 20×20 source data is identical between boards — only the cell size changes.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

13 × 20×20 pixel-art creature animations sourced from
[claudepix.vercel.app](https://claudepix.vercel.app). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

Each animation has a per-animation 10-color RGB565 palette. Cell values 0..9 index it. Default boot screen.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Recent session highlights

- Migrated from Panlee SC01 Plus (480×320 IPS) to Waveshare 2.16" AMOLED (480×480 square). Full hardware/library swap.
- Added IMU auto-rotation, battery indicator, USB-state-aware screen switching.
- Added splash screen with scraped pixel-art animations and 3-button physical input layout.
- Fonts and icons re-scaled ~1.9× for the higher-DPI panel.
- All UI margins widened to 20px to clear the rounded display corners.
- Battery icons converted to RGB565A8 alpha so they blend cleanly over the splash animations.

## Daemon / host side

Bash daemon (`daemon/claude-usage-daemon.sh`) reads OAuth token, polls Anthropic API, sends JSON over BLE GATT. Run with `systemctl --user start claude-usage-daemon`. The unit file's `ExecStart` is the absolute path to the script — repoint it when switching between the worktree and the main checkout.

**Discovery & resilience:**

- Connects by name (`"Claude Controller"`) on first run, caches resolved MAC at `~/.config/claude-usage-monitor/ble-address`. ESP32 BLE addresses are factory-burned per-chip, so swapping any board invalidates the cache.
- On connect failure: cache is dropped AND device is removed from bluez (`bluetoothctl remove`) so the next scan won't re-pick a dead MAC. Multi-candidate scans pick `head -1` and let the failure cycle converge.
- `POLL_INTERVAL=60`, `TICK=5`. Inner loop wakes every 5s to detect disconnects fast; polls Anthropic when 60s elapsed OR when ESP fires a refresh request.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. Daemon subscribes via `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"`; awk drops a flag file the inner loop picks up. See the `feedback_dbus_monitor_pipe` memory for the three subtle gotchas (pipe buffering, busctl-exits race, `wait` blocking on pipeline jobs).
