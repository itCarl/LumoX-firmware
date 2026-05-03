#pragma once

// ── Defaults ───────────────────────────────────────────────────────────────
// Written to NVS on first boot and editable via the web interface afterwards.
// "DEFAULT_*" is only the factory value.

// STA (join an existing WiFi). Leave SSID empty to force AP mode on first boot.
#define DEFAULT_STA_SSID        "SPRITZ!Box 1337"
#define DEFAULT_STA_PASSWORD    "51473579835369041337"

// AP fallback (started when STA is unavailable). SSID gets a "-XXYY" MAC suffix.
#define DEFAULT_AP_SSID         "Lumox"
#define DEFAULT_AP_PASSWORD     "lumox123"

// Art-Net
#define DEFAULT_ARTNET_UNIVERSE 0

// mDNS hostname + ArtPollReply short name (MAC suffix appended automatically).
#define DEFAULT_DEVICE_NAME     "Lumox"

// ── Ethernet (W5500) defaults ─────────────────────────────────────────────
// DHCP on by default — most networks have a server. Static fields below are
// pre-populated on the /config form so users get sane Art-Net-friendly values
// when they switch to static mode (Art-Net II convention is 2.x.x.x / 8).
#define DEFAULT_ETH_DHCP        true
#define DEFAULT_ETH_IP          "2.0.0.10"
#define DEFAULT_ETH_GW          "2.0.0.1"
#define DEFAULT_ETH_SUB         "255.0.0.0"
#define DEFAULT_ETH_DNS         "8.8.8.8"

// "Disable WiFi when Ethernet is up" — off by default so the web UI stays
// reachable on WiFi while ETH carries the show traffic.
#define DEFAULT_WIFI_OFF_ON_ETH false

// W5500 INT pin connected with pullup? Compile-time switch — must match
// hardware wiring. 0 = polling mode (irq=-1), bulletproof. 1 = use ETH_INT_PIN
// for IRQ-driven link events. Default 0 because GPIO34 is input-only with no
// internal pullup; a floating INT line blocks link-state events. Set to 1
// only if you've wired an external pullup on ETH_INT_PIN, or moved INT to a
// GPIO with a built-in pullup.
#define ETH_USE_IRQ             0

// ── Fixed settings (not editable via the web UI) ───────────────────────────
#define ARTNET_UDP_PORT         6454
#define WEB_SERVER_PORT         80
#define SERIAL_BAUD             115200

// ── Firmware version (reported in ArtPollReply) ───────────────────────────
// Generated from package.json at build time by tools/cdata.js.
// Fallback values let the source compile if the pre-build hook hasn't run yet
// (e.g. fresh checkout opened in an IDE before `pio run`).
#if __has_include("version.h")
  #include "version.h"
#else
  #define FW_VERSION_STRING   "dev"
  #define FW_VERSION_MAJOR    0
  #define FW_VERSION_MINOR    0
  #define FW_VERSION_PATCH    0
  #define FW_BUILD_TIME       0
  #define FW_BUILD_DATE       "unknown"
  #define FW_GIT_HASH         ""
#endif

// ── OEM / ESTA codes (controller-side filters) ────────────────────────────
// 0x00FF = Art-Net generic OEM. ESTA 0x0000 = no ESTA-assigned manufacturer.
#define ARTNET_OEM_CODE         0x00FF
#define ARTNET_ESTA_CODE        0x0000

// ── STA fallback ──────────────────────────────────────────────────────────
// Maximum time to wait for the configured WiFi to connect before falling
// back to AP mode + captive portal.
#define STA_CONNECT_TIMEOUT_MS  15000

// ── W5500 Ethernet ────────────────────────────────────────────────────────
// PHY address (1 for W5500 — fixed by chip), SPI clock.
// After STA fails, wait this long for Ethernet DHCP before starting AP fallback.
// LUMOX_ prefix avoids collision with arduino-esp32's <ETH.h> which defines
// its own ETH_PHY_ADDR macro for built-in RMII PHYs.
#define LUMOX_ETH_PHY_ADDR      1
#define ETH_SPI_CLK_MHZ         20
#define ETH_LINK_WAIT_MS        5000

// ── Art-Net link-stale threshold ──────────────────────────────────────────
// If no ArtDMX packet is received within this window, the status LED switches
// to slow pulse. DMX output holds the last value regardless.
#define DMX_LINK_STALE_MS       2000
