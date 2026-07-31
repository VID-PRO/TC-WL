#pragma once
#include "config.h"
#define TCWL_LTC 1
#define TCWL_CLAP 1

// TC-WL-CLAP: WiFi + BLE client + LED matrix + OLED
#undef MATRIX_ENABLED_DEFAULT
#define MATRIX_ENABLED_DEFAULT 1

// OLED on shared I2C bus (SDA=GPIO4, SCL=GPIO5 — free on CLAP)
#define OLED_ENABLE 1
#define OLED_I2C_SDA_PIN   4
#define OLED_I2C_SCL_PIN   5

// OLED menu push-buttons (momentary to GND, internal pull-up enabled)
// CLAP uses free GPIOs that don't conflict with the MAX7219 matrix (2/3/10),
// OLED I2C (4/5), battery ADC (0), or LTC out (6).
#undef BTN_UP_PIN
#undef BTN_DOWN_PIN
#undef BTN_OK_PIN
#undef BTN_CANCEL_PIN
#define BTN_UP_PIN       1
#define BTN_DOWN_PIN     7
#define BTN_OK_PIN       8
#define BTN_CANCEL_PIN   9

// Battery ADC on GPIO 0 (ADC1_CH0), 200k:200k divider, 2000mAh LiPo
#undef BAT_ADC_PIN
#define BAT_ADC_PIN           0
#undef BAT_FULL_RUNTIME_MIN
#define BAT_FULL_RUNTIME_MIN  600   // 2000mAh / ~200mA ≈ 10h
