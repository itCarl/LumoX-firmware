#pragma once

// ── Debug / verbose-print toggle ───────────────────────────────────────────
// Set to 1 in `[env:debug]` (platformio.ini) → enables periodic status dumps
// + verbose runtime logs. Set to 0 in production → those calls compile out
// to ((void)0), saving flash and keeping the serial port quiet during shows.
//
// Use DEBUG_PRINT/PRINTLN/PRINTF for *periodic* or *high-volume* logs only.
// Boot-time init prints (Serial.printf inside begin*() functions) and rare
// events (sender-swap, OpAddress, factory reset) stay as plain Serial.printf
// so the user can still diagnose problems on a misbehaving production node.
#ifndef LUMOX_DEBUG
#define LUMOX_DEBUG 0
#endif

#if LUMOX_DEBUG
  #define DEBUG_PRINT(x)     Serial.print(x)
  #define DEBUG_PRINTLN(x)   Serial.println(x)
  #define DEBUG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)     ((void)0)
  #define DEBUG_PRINTLN(x)   ((void)0)
  #define DEBUG_PRINTF(...)  ((void)0)
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
