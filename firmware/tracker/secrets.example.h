// Copy this file to secrets.h (same folder) and fill it in. secrets.h is
// git-ignored: real credentials never go into the repository (contract 7.3, 9).
//
// These are only DEFAULTS for a freshly flashed board. Anything set later with
// the serial CLI (`wifi ...`, `keys ...`) is stored in NVS and wins.
#pragma once

#define WIFI_SSID "my-phone-hotspot"      // 2.4 GHz hotspot. No eduroam / university login here.
#define WIFI_PASS "hotspot-password"
#define TS_CH1 "0000000"                  // ThingSpeak channel 1 "AST raw" id
#define TS_WRITE_KEY "XXXXXXXXXXXXXXXX"   // channel 1 WRITE key (stays on the device only)
#define TS_READ_KEY ""                    // optional channel 1 READ key: enables the read-back after a lost response

// Optional: pin ONE root certificate instead of the built-in CA bundle.
// Paste the PEM of the root CA of api.thingspeak.com, obtained on a trusted
// network (NOT through a TLS-inspecting proxy). See firmware/README.md.
// #define TS_ROOT_CA_PEM "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
