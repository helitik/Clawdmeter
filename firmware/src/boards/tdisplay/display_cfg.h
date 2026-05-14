#pragma once

#include <Arduino_GFX_Library.h>

// ---- LilyGO T-Display S3 (standard, non-touch) ----
// ST7789 1.9" 170×320 via 8-bit i80 parallel bus.
// Reference: https://github.com/Xinyuan-LilyGO/T-Display-S3

// Native panel orientation is portrait 170×320. We rotate to landscape in
// software via Arduino_GFX::setRotation(1) → 320×170 effective.
#define LCD_NATIVE_W   170
#define LCD_NATIVE_H   320
#define LCD_WIDTH      320   // after rotation 1
#define LCD_HEIGHT     170

// ST7789 control pins
#define LCD_CS         6
#define LCD_DC         7
#define LCD_WR         8
#define LCD_RD         9
#define LCD_RST        5

// 8-bit parallel data bus (D0..D7)
#define LCD_D0         39
#define LCD_D1         40
#define LCD_D2         41
#define LCD_D3         42
#define LCD_D4         45
#define LCD_D5         46
#define LCD_D6         47
#define LCD_D7         48

// Backlight (PWM via LEDC) and LCD power enable
#define LCD_BL         38
#define LCD_POWER_ON   15   // must drive HIGH to enable LCD 3v3 rail

// Physical buttons
#define BTN_LEFT       0    // BOOT/KEY0 — left of the screen in landscape
#define BTN_RIGHT      14   // KEY1 — right of the screen in landscape

// Battery sense: GPIO 4 (ADC1_CH3), 100k/100k divider on board
#define BAT_ADC_PIN    4
#define BAT_ADC_DIVIDER  2.0f   // upstream resistor divider ratio

// Panel ST7789 has a 35-pixel column offset in the controller's frame buffer
// for the 170-wide window. Arduino_GFX exposes this via the col/row offset
// constructor args.
#define LCD_COL_OFFSET 35
#define LCD_ROW_OFFSET 0

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_GFX     *gfx;
