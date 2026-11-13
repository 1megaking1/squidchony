// Board wiring and fixed constants. See docs/hardware.md for the schematic notes.
#pragma once
#include <stdint.h>

#define FIRMWARE_VERSION "0.1.0"

// ---- Pins (Raspberry Pi Pico 2 W) -------------------------------------------------------
// Avoid GP23/24/25/29: the Pico 2 W uses them for the wireless chip.
constexpr int PIN_RADAR_ADC = 26;  // ADC0: amplifier output through the 10k/15k divider
constexpr int PIN_BATT_ADC = 27;   // ADC1: battery through 470k/100k + 100 nF
constexpr int ADC_CH_RADAR = 0;
constexpr int ADC_CH_BATT = 1;

constexpr int PIN_SPI_MISO = 16;   // SPI0 (only the display's microSD would use MISO)
constexpr int PIN_TFT_CS = 17;
constexpr int PIN_SPI_SCK = 18;
constexpr int PIN_SPI_MOSI = 19;
constexpr int PIN_TFT_DC = 20;
constexpr int PIN_TFT_RST = 21;
constexpr int PIN_TFT_BL = 22;     // backlight: on/off only, never PWM (PWM is radar noise)
constexpr int PIN_SD_CS = 13;      // display board microSD, unused for now (held high)

constexpr int PIN_BTN_READ = 15;   // to GND when pressed, internal pull-up
constexpr int PIN_BTN_MODE = 14;
constexpr int PIN_LED_READ = 11;   // high = LED on (2N3904 low-side switch from the 5 V rail)
constexpr int PIN_LED_MODE = 10;

// The Pico 2 W's 3.3 V buck-boost (RT6154) skips pulses at light load ("PFM"), which puts
// bursts of noise in the audio band where the Doppler tones live. Its MODE pin is wired to
// the wireless chip's GPIO1; high forces steady PWM. arduino-pico numbers those GPIOs 64+.
constexpr int PIN_SMPS_MODE = 64 + 1;

// ---- Analog front end -------------------------------------------------------------------
constexpr float ADC_VREF = 3.3f;
constexpr float BATT_DIVIDER = 5.7f;      // (470k + 100k) / 100k
constexpr uint16_t CLIP_LO = 40;          // op-amp output rails seen through the divider
constexpr uint16_t CLIP_HI = 3680;

// ---- Radar ------------------------------------------------------------------------------
constexpr float RADAR_F0_HZ = 24.125e9f;  // CDM324 nominal; per-unit error goes in "cal"

// ---- Wi-Fi ------------------------------------------------------------------------------
constexpr const char* AP_PREFIX = "CHRONO-";  // + 4 hex digits from the board ID
constexpr uint8_t AP_IP[4] = {192, 168, 4, 1};
constexpr const char* MDNS_NAME = "chrono";   // http://chrono.local
