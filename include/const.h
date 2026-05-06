#pragma once

// ── Serial-log toggle ──────────────────────────────────────────────────────
// `[env:debug]` (platformio.ini) sets LUMOX_DEBUG=1 → all log macros expand
// to Serial.* and `Serial.begin()` runs. Production `[env:esp32dev]` sets
// LUMOX_DEBUG=0 → every log macro compiles to ((void)0) and the UART driver
// is never started, so the serial port is genuinely silent during shows
// (no half-formatted strings going to a closed port).
//
// All firmware logging — boot init, lifecycle events, error paths, periodic
// dumps — goes through the LOG_* macros. There are no raw Serial.* calls in
// the codebase (any newly added one will compile in prod and break silence).
#ifndef LUMOX_DEBUG
#define LUMOX_DEBUG 0
#endif

#if LUMOX_DEBUG
  #define LOG_BEGIN(b)       Serial.begin(b)
  #define LOG_PRINT(x)       Serial.print(x)
  #define LOG_PRINTLN(...)   Serial.println(__VA_ARGS__)
  #define LOG_PRINTF(...)    Serial.printf(__VA_ARGS__)
#else
  #define LOG_BEGIN(b)       ((void)0)
  #define LOG_PRINT(x)       ((void)0)
  #define LOG_PRINTLN(...)   ((void)0)
  #define LOG_PRINTF(...)    ((void)0)
#endif

// ── DMX UART & MAX485 ──────────────────────────────────────────────────────
// Wiring:
//   TX_PIN  → MAX485 DI  (Data In)
//   RX_PIN  → MAX485 RO  (Receiver Out, not used)
//   DE_PIN  → MAX485 DE + RE (tied together, HIGH = transmit)
//
#define DMX_UART_PORT   1       // UART1 (esp_dmx 4.1 crashes on UART2 with Arduino-ESP32 core 2.x)
#define DMX_TX_PIN      17
#define DMX_RX_PIN      16
#define DMX_DE_PIN      4

// ── Status LED ─────────────────────────────────────────────────────────────
#define LED_PIN         2       // onboard LED on most ESP32 dev boards

// ── Wiznet W5500 (wired Ethernet backup) ───────────────────────────────────
// SPI (VSPI / SPI3_HOST) wiring:
//   SCK  → W5500 SCLK
//   MISO → W5500 MISO
//   MOSI → W5500 MOSI
//   CS   → W5500 SCS  (chip-select, low-active)
//   INT  → W5500 INT  (input-only pin OK; W5500 uses it for link / RX events)
//   RST  → W5500 RST  (low-active, held high after reset)
//
// If no W5500 is present, ETH.begin() simply fails — firmware continues on WiFi.
// ⚠ GPIO 4 is used for DMX DE, GPIO 2 for LED — do NOT reassign to those.
#define ETH_CS_PIN      5
#define ETH_INT_PIN     34      // GPIO34 is input-only — fine for IRQ
#define ETH_RST_PIN     33
#define ETH_SCK_PIN     18
#define ETH_MISO_PIN    19
#define ETH_MOSI_PIN    23
