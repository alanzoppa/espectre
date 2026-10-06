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
#include "neopixel_light.h"

#include <driver/rmt_types.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <led_strip.h>
#include <led_strip_rmt.h>

#include <algorithm>
#include <cmath>

namespace espectre {
namespace neopixel {
namespace {

constexpr char kTag[] = "espectre.neopixel";

SemaphoreHandle_t s_lock = nullptr;      // guards state below
led_strip_handle_t s_strip = nullptr;
int s_led_count = 0;
int s_gpio = -1;

// Matter light state
bool s_on = false;
uint8_t s_level = 254;                    // CurrentLevel: 0-254
uint8_t s_hue = 0;                        // CurrentHue: 0-254
uint8_t s_saturation = 0;                // CurrentSaturation: 0-254
uint16_t s_x = 0;                         // CurrentX/Y: 0-65535
uint16_t s_y = 0;
uint16_t s_mireds = 250;                 // ColorTemperatureMireds
uint8_t s_color_mode = 0;                // 0 hue/sat, 1 xy, 2 color temp

// Identify animation
volatile bool s_identify_active = false;

float clamp01(float v) { return std::clamp(v, 0.0F, 1.0F); }

uint8_t u8(float v) { return static_cast<uint8_t>(std::lround(clamp01(v) * 255.0F)); }

// Matter hue/saturation (0-254) -> RGB. Standard HSV with hue mapped over
// 360 degrees and level as value.
void hsv_to_rgb(uint8_t hue, uint8_t sat, uint8_t value, uint8_t *r, uint8_t *g, uint8_t *b) {
  const float h = (static_cast<float>(hue) / 254.0F) * 360.0F;
  const float s = static_cast<float>(sat) / 254.0F;
  const float v = static_cast<float>(value) / 254.0F;

  const int region = static_cast<int>(h / 60.0F) % 6;
  const float remainder = std::fmod(h, 60.0F);
  const float c = v * s;
  const float x = c * (1.0F - std::fabs(std::fmod(remainder / 60.0F, 2.0F) - 1.0F));
  const float m = v - c;

  float rf = 0.0F, gf = 0.0F, bf = 0.0F;
  switch (region) {
    case 0: rf = c; gf = x; break;
    case 1: rf = x; gf = c; break;
    case 2: gf = c; bf = x; break;
    case 3: gf = x; bf = c; break;
    case 4: rf = x; bf = c; break;
    default: rf = c; bf = x; break;
  }
  *r = u8((rf + m));
  *g = u8((gf + m));
  *b = u8((bf + m));
}

// CIE 1931 xy (Matter 0-65535 encoding) -> RGB, brightness 1.0, sRGB gamma.
void xy_to_rgb(uint16_t x16, uint16_t y16, uint8_t *r, uint8_t *g, uint8_t *b) {
  const float x = std::clamp(static_cast<float>(x16) / 65535.0F, 0.0F, 1.0F);
  const float y = std::clamp(static_cast<float>(y16) / 65535.0F, 0.0F, 1.0F);
  if (y <= 0.0001F) {
    *r = 0;
    *g = 0;
    *b = 0;
    return;
  }
  // CIE xyY -> XYZ with Y = 1
  const float X = (x / y);
  const float Y = 1.0F;
  const float Z = ((1.0F - x - y) / y);
  // XYZ -> linear sRGB (D65)
  const float lr = 3.2406F * X - 1.5372F * Y - 0.4986F * Z;
  const float lg = -0.9689F * X + 1.8758F * Y + 0.0415F * Z;
  const float lb = 0.0557F * X - 0.2040F * Y + 1.0570F * Z;
  const float gamma = 1.0F / 2.2F;
  *r = u8(std::pow(clamp01(lr), gamma));
  *g = u8(std::pow(clamp01(lg), gamma));
  *b = u8(std::pow(clamp01(lb), gamma));
}

// Color temperature (mireds) -> RGB. Tanner Helland's approximation,
// clamped to the Matter bulb range 153 (6500K) .. 500 (2000K).
void mireds_to_rgb(uint16_t mireds, uint8_t *r, uint8_t *g, uint8_t *b) {
  const float kelvin = 1000000.0F / std::clamp(static_cast<float>(mireds), 153.0F, 500.0F);
  const float t = kelvin / 100.0F;

  float fr, fg, fb;
  if (t <= 66.0F) {
    fr = 255.0F;
    fg = 99.4708025861F * std::log(t) - 161.1195681661F;
  } else {
    fr = 329.698727449F * std::pow(t - 60.0F, -0.1332047592F);
    fg = 288.1221695283F * std::pow(t - 60.0F, -0.0755148492F);
  }
  if (t >= 66.0F) {
    fb = 255.0F;
  } else if (t <= 19.0F) {
    fb = 0.0F;
  } else {
    fb = 138.5177312231F * std::log(t - 10.0F) - 305.0447927307F;
  }
  *r = u8(fr / 255.0F);
  *g = u8(fg / 255.0F);
  *b = u8(fb / 255.0F);
}

// Renders the current state to the strip. Caller must hold s_lock.
// Skipped while the identify animation owns the strip.
esp_err_t render_locked() {
  if (s_strip == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_identify_active) {
    return ESP_OK;
  }
  uint8_t r = 0, g = 0, b = 0;
  if (s_on) {
    switch (s_color_mode) {
      case 1: xy_to_rgb(s_x, s_y, &r, &g, &b); break;
      case 2: mireds_to_rgb(s_mireds, &r, &g, &b); break;
      default: hsv_to_rgb(s_hue, s_saturation, s_level, &r, &g, &b); break;
    }
    // xy and color temperature carry only chromaticity; CurrentLevel
    // scales brightness. hsv already uses level as value.
    if (s_color_mode != 0) {
      const float scale = static_cast<float>(s_level) / 254.0F;
      const float gamma = 1.0F / 2.2F;  // perceptual brightness curve
      const float br = std::pow(scale, gamma);
      r = static_cast<uint8_t>(std::lround(r * br));
      g = static_cast<uint8_t>(std::lround(g * br));
      b = static_cast<uint8_t>(std::lround(b * br));
    }
  }
  for (int i = 0; i < s_led_count; ++i) {
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, i, r, g, b), kTag, "set pixel %d", i);
  }
  return led_strip_refresh(s_strip);
}

void identify_task(void *) {
  const TickType_t period = pdMS_TO_TICKS(500);
  bool phase = true;
  while (s_identify_active) {
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
      if (s_strip != nullptr) {
        const uint8_t level = phase ? 96 : 0;
        for (int i = 0; i < s_led_count; ++i) {
          (void) led_strip_set_pixel(s_strip, i, level, level, level);
        }
        (void) led_strip_refresh(s_strip);
      }
      xSemaphoreGive(s_lock);
    }
    phase = !phase;
    vTaskDelay(period);
  }
  // Restore the stored light state on the way out.
  if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    (void) render_locked();
    xSemaphoreGive(s_lock);
  }
  vTaskDelete(nullptr);
}

}  // namespace

esp_err_t init(int gpio_num, int led_count) {
  if (s_strip != nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (gpio_num < 0 || led_count <= 0) {
    return ESP_ERR_INVALID_ARG;
  }
  s_lock = xSemaphoreCreateMutex();
  if (s_lock == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = gpio_num;
  strip_config.max_leds = static_cast<uint32_t>(led_count);
  strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.flags.invert_out = false;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = 10 * 1000 * 1000;
  rmt_config.mem_block_symbols = 0;  // driver default
  rmt_config.flags.with_dma = false;

  esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Failed to create RMT strip (gpio %d, %d leds): %s", gpio_num, led_count,
             esp_err_to_name(err));
    vSemaphoreDelete(s_lock);
    s_lock = nullptr;
    return err;
  }
  s_gpio = gpio_num;
  s_led_count = led_count;

  // Start dark so the strip does not flash random colors at boot.
  (void) render_locked();
  ESP_LOGI(kTag, "WS2812 strip ready: gpio %d, %d leds", s_gpio, s_led_count);
  return ESP_OK;
}

bool initialized() { return s_strip != nullptr; }

void set_on(bool on) {
  if (s_lock == nullptr) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_on = on;
  (void) render_locked();
  xSemaphoreGive(s_lock);
}

void set_level(uint8_t level) {
  if (s_lock == nullptr) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_level = level;
  (void) render_locked();
  xSemaphoreGive(s_lock);
}

void set_hue_saturation(uint8_t hue, uint8_t saturation) {
  if (s_lock == nullptr) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_hue = hue;
  s_saturation = saturation;
  (void) render_locked();
  xSemaphoreGive(s_lock);
}

void set_xy(uint16_t x, uint16_t y) {
  if (s_lock == nullptr) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_x = x;
  s_y = y;
  (void) render_locked();
  xSemaphoreGive(s_lock);
}

void set_color_temperature(uint16_t mireds) {
  if (s_lock == nullptr) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_mireds = mireds;
  (void) render_locked();
  xSemaphoreGive(s_lock);
}

void set_color_mode(uint8_t color_mode) {
  if (s_lock == nullptr) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_color_mode = color_mode;
  (void) render_locked();
  xSemaphoreGive(s_lock);
}

void apply() {
  if (s_lock == nullptr) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  (void) render_locked();
  xSemaphoreGive(s_lock);
}

uint8_t current_hue() { return s_hue; }
uint8_t current_saturation() { return s_saturation; }
uint16_t current_x() { return s_x; }
uint16_t current_y() { return s_y; }

void identify_start(uint8_t) {
  if (s_lock == nullptr || s_strip == nullptr) return;
  if (s_identify_active) return;
  s_identify_active = true;
  if (xTaskCreate(identify_task, "np_identify", 3072, nullptr, 5, nullptr) != pdPASS) {
    s_identify_active = false;
    ESP_LOGE(kTag, "Failed to create identify task");
  }
}

void identify_stop() {
  if (s_lock == nullptr) return;
  // The task notices the flag and restores the stored state itself.
  s_identify_active = false;
}

}  // namespace neopixel
}  // namespace espectre