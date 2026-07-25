#pragma once

// ---------------------------------------------------------------------------
// Pin & hardware configuration — Seeed Studio XIAO ESP32-C3
// ---------------------------------------------------------------------------

// I2C pins to the TC358743 HDMI receiver board (single bus, shared with OLED & RTC)
#define TC_I2C_SDA_PIN   4
#define TC_I2C_SCL_PIN   5
#define TC_I2C_ADDR      0x0F   // default 7-bit address on most TC358743 breakout boards

// TC358743 hardware reset pin (-1 = skip, rely on internal power-on reset)
#define TC_RESET_PIN     4

// LTC audio output pin (goes to your RC filter -> 3.5mm TRS -> LTC input)
#define LTC_OUT_PIN      6

// LTC audio input pin (LTC signal from external source, TC-WL-LTC master mode only)
#define LTC_IN_PIN       7

// OLED menu push-buttons (momentary to GND, internal pull-up enabled)
// TC-WL-LTC has no LED matrix, so GPIO 2 and 3 (normally MAX7219 DIN/CS) are
// free for button use.  GPIO0/1 avoided (strapping function).
#define BTN_UP_PIN       8
#define BTN_DOWN_PIN     9
#define BTN_OK_PIN       4
#define BTN_CANCEL_PIN   5

// TC358743 RESETN has internal pull-up — no GPIO needed

// ---------------------------------------------------------------------------
// Frame rate auto-detection
// ---------------------------------------------------------------------------
#ifndef FPS_AUTO_DETECT
#define FPS_AUTO_DETECT     1
#endif

// ---------------------------------------------------------------------------
// OLED display configuration
// ---------------------------------------------------------------------------
// 128x64 SSD1306 OLED on the shared I2C bus.
#define OLED_ENABLE         1
#define OLED_I2C_ADDR       0x3C

// ---------------------------------------------------------------------------
// RTC (Real-Time Clock) configuration
// ---------------------------------------------------------------------------
// DS3231 RTC on the shared I2C bus (same SDA/SCL as TC358743 + OLED — all at
// different addresses: 0x0F, 0x3C, 0x68 — no conflict).
#define RTC_ENABLE          1
#define RTC_I2C_SDA_PIN     4   // same bus as TC358743 / OLED (only one I2C on ESP32-C3)
#define RTC_I2C_SCL_PIN     5
#define RTC_I2C_ADDR        0x68

// ---------------------------------------------------------------------------
// LTC frame rate fallback defaults
// ---------------------------------------------------------------------------
#ifndef LTC_FPS
#define LTC_FPS 25
#endif

#ifndef LTC_DROP_FRAME
#define LTC_DROP_FRAME 0
#endif

// ---------------------------------------------------------------------------
#ifndef MATRIX_ENABLED_DEFAULT
#define MATRIX_ENABLED_DEFAULT 0
#endif

// MAX7219 8x8 LED matrix display configuration (optional — set to 0 to skip)
// 8 modules daisy-chained = 64x8 pixels. Software SPI (no hardware SPI on these pins).
// Pins: see schematic for wiring on XIAO ESP32-C3.
#define MAX7219_ENABLE       1
#define MAX7219_DIN_PIN      2
#define MAX7219_CS_PIN       3
#define MAX7219_CLK_PIN      10
#define MAX7219_NUM_MODULES  8

// ---------------------------------------------------------------------------
// Battery voltage monitoring (ADC, 1/2 resistor divider on A0)
// BAT_ADC_PIN = -1 to disable; set to ADC-capable GPIO otherwise.
// BAT_DIVIDER = voltage divider ratio (e.g. 2.0 for 200k:200k).
// ---------------------------------------------------------------------------
#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN           -1
#endif
#define BAT_DIVIDER           2.0f
#ifndef BAT_FULL_RUNTIME_MIN
#define BAT_FULL_RUNTIME_MIN  600           // estimated runtime (min) at 100%
#endif

// ---------------------------------------------------------------------------
// Web UI (WiFi AP + optional STA) configuration
// ---------------------------------------------------------------------------
#define WEBUI_ENABLE        1
#define WEBUI_AP_SSID       "TC-WL-AP"
#define WEBUI_AP_PASSWORD   nullptr         // nullptr = open network

// Optional: connect to an existing WiFi network (STA mode) so the web UI
// is also reachable on your LAN.  Leave SSID empty ("") to skip STA.
#define WEBUI_STA_SSID      ""
#define WEBUI_STA_PASSWORD  ""

// ---------------------------------------------------------------------------
// Operating mode (HDMI only — see config_tcwl_hdmi.h)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Firmware version
// ---------------------------------------------------------------------------
#define FW_VERSION "v1.5.1"
