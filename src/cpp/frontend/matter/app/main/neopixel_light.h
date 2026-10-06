/*
 * ESPectre - NeoPixel light driver for the Matter frontend
 *
 * Drives a WS2812 strip attached to one GPIO and renders the Matter
 * extended color light state (on/off, level, hue/saturation, xy,
 * color temperature) onto it. Also renders the Identify animation.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Commercial licensing available under separate agreement; see LICENSING.md.
 */
#pragma once

#include <cstdint>

#include <esp_err.h>

namespace espectre {
namespace neopixel {

// Initializes the WS2812 strip. Safe to call once from app_main.
// Returns ESP_ERR_INVALID_STATE when already initialized.
esp_err_t init(int gpio_num, int led_count);

// True once init() has succeeded.
bool initialized();

// Matter state setters. Each one stores the value and re-renders the
// strip; all are safe to call from any task (they serialize on an
// internal lock). Values follow the Matter cluster encodings:
//   level          0-254 (LevelControl::CurrentLevel)
//   hue            0-254 (ColorControl::CurrentHue)
//   saturation     0-254 (ColorControl::CurrentSaturation)
//   x, y           0-65535 normalized CIE 1931 (ColorControl::CurrentX/Y)
//   mireds         color temperature in mireds (ColorControl::ColorTemperatureMireds)
//   color_mode     0 = hue/saturation, 1 = xy, 2 = color temperature
void set_on(bool on);
void set_level(uint8_t level);
void set_hue_saturation(uint8_t hue, uint8_t saturation);
void set_xy(uint16_t x, uint16_t y);
void set_color_temperature(uint16_t mireds);
void set_color_mode(uint8_t color_mode);

// Re-renders the strip from the current stored state.
void apply();

// Last-written values, for combining split hue/saturation and xy updates.
uint8_t current_hue();
uint8_t current_saturation();
uint16_t current_x();
uint16_t current_y();

// Identify animation (Identify cluster). start() spawns a blinking task
// that owns the strip until stop() is called, which restores the state.
void identify_start(uint8_t effect_id);
void identify_stop();

}  // namespace neopixel
}  // namespace espectre