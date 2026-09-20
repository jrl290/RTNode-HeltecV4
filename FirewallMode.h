// Copyright (C) 2026, Firewall Mode Extension
// Based on microReticulum_Firmware by Mark Qvist
//
// FirewallMode.h — Configuration and runtime state for Firewall Mode.
// This is the only intended operating mode for this firmware fork.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef FIREWALL_MODE_H
#define FIREWALL_MODE_H

#ifdef FIREWALL_MODE

// ─── Firewall Mode Configuration ────────────────────────────────────────────
//
// The firmware acts as a LoRa↔TCP bridge/firewall node:
//   - LAN→LAN and LAN→WAN traffic flows freely.
//   - WAN→LAN traffic is filtered unless a LAN device has "punched a hole"
//     by referencing the source or a LAN device has been referenced first.
//
// The boundary node operates with a LoRa mesh interface plus up to four
// TCP backbone client interfaces:
//
//   1. LoRaInterface (MODE_GATEWAY) — radio side, handles LoRa mesh
//   2. BackboneInterface[1-4] (MODE_BOUNDARY) — WiFi side TCP uplinks
//
// RNS Transport is ALWAYS enabled in firewall mode.
// Packets received on either interface are routed through Transport
// to the other interface based on path table lookups and announce rules.

// ─── WiFi Backbone Connection ────────────────────────────────────────────────
// These can be overridden via build flags or EEPROM at runtime.

// Default backbone server to connect to (client mode)
// Set to empty string "" if operating in server mode
#ifndef FIREWALL_BACKBONE_HOST
#define FIREWALL_BACKBONE_HOST ""
#endif

#ifndef FIREWALL_BACKBONE_PORT
#define FIREWALL_BACKBONE_PORT 4242
#endif

// TCP interface mode: 0 = disabled, 1 = client (connect out)
#ifndef FIREWALL_TCP_MODE
#define FIREWALL_TCP_MODE 1
#endif

// TCP server listen port (when in server mode)
#ifndef FIREWALL_TCP_PORT
#define FIREWALL_TCP_PORT 4242
#endif

#define FIREWALL_BACKBONE_SLOTS 4
#define FIREWALL_BACKBONE_HOST_LEN 64
#define FIREWALL_BACKBONE_SLOT_BYTES (1 + FIREWALL_BACKBONE_HOST_LEN + 2)

// ─── EEPROM Extension Addresses ──────────────────────────────────────────────
// We use the CONFIG area (config_addr) for additional firewall mode settings.
// These are after the existing WiFi SSID/PSK/IP/NM fields.
// Existing layout:
//   0x00-0x20: SSID (33 bytes)
//   0x21-0x41: PSK (33 bytes)
//   0x42-0x45: IP (4 bytes)
//   0x46-0x49: NM (4 bytes)
// Our additions (config_addr space, 0x4A onwards):
#define ADDR_CONF_BMODE      0x4A  // Firewall mode enabled flag (1 byte, 0x73 = enabled)
#define ADDR_CONF_BTCP_MODE  0x4B  // TCP mode: 0=server, 1=client (1 byte)
#define ADDR_CONF_BTCP_PORT  0x4C  // TCP port (2 bytes, big-endian)
#define ADDR_CONF_BHOST      0x4E  // Backbone host (64 bytes, null-terminated)
#define ADDR_CONF_BHPORT     0x8E  // Backbone target port (2 bytes, big-endian)
#define ADDR_CONF_AP_TCP_EN  0x90  // AP TCP server enable (1 byte, 0x73 = enabled)
#define ADDR_CONF_AP_TCP_PORT 0x91 // AP TCP server port (2 bytes, big-endian)
#define ADDR_CONF_AP_SSID    0x93  // AP SSID (33 bytes, null-terminated)
#define ADDR_CONF_AP_PSK     0xB4  // AP PSK (33 bytes, null-terminated)
#define ADDR_CONF_WIFI_EN   0xD5  // WiFi enable flag (1 byte, 0x73 = enabled)
// IFAC (Interface Access Code) settings for LoRa interface
#define ADDR_CONF_IFAC_EN   0xD6  // IFAC enable flag (1 byte, 0x73 = enabled)
#define ADDR_CONF_IFAC_NAME 0xD7  // Network name (33 bytes, null-terminated)
#define ADDR_CONF_IFAC_PASS 0xF8  // Passphrase (33 bytes, null-terminated)
#define ADDR_CONF_APP_MARKER0 0x119 // RTNode app marker byte 0
#define ADDR_CONF_APP_MARKER1 0x11A // RTNode app marker byte 1
#define ADDR_CONF_APP_VERSION 0x11B // RTNode app config version
// Device advertisement settings (advertise this node's parameters and
// optional GPS coordinates to the Reticulum network so external maps such
// as rmap.world can pin it). Stored after the app version markers so the
// existing layout is preserved and old saves keep working (uninitialised
// 0xFF bytes are interpreted as "advertisement disabled, no coordinates").
#define ADDR_CONF_ADVERT_EN     0x11C // Advertise enable flag (1 byte, 0x73 = enabled)
#define ADDR_CONF_ADVERT_LAT    0x11D // Latitude as IEEE-754 double (8 bytes, host byte order)
#define ADDR_CONF_ADVERT_LON    0x125 // Longitude as IEEE-754 double (8 bytes, host byte order)
#define ADDR_CONF_ADVERT_JITTER 0x12D // Randomize ~0.5 km offset flag (1 byte, 0x73 = enabled)
#define ADDR_CONF_NODE_NAME     0x12E // Node display name (33 bytes, null-terminated)
// Airtime (duty-cycle) limits. Stored as 1 byte each, in units of 0.1%
// (so byte value 10 = 1.0%, byte value 100 = 10.0%; max 25.0% per byte).
// 0xFF (uninitialised) or 0 = disabled. When the measured short-term
// airtime exceeds st_alock, or the long-term airtime exceeds lt_alock,
// LoRa TX is paused (airtime_lock) until the rolling window drops below
// the threshold again. Useful for self-imposed regional duty-cycle
// budgets (e.g. EU868 = 1.0% long-term).
#define ADDR_CONF_ST_AL         0x14F // Short-term airtime limit (1 byte, percent * 10)
#define ADDR_CONF_LT_AL         0x150 // Long-term  airtime limit (1 byte, percent * 10)
#define ADDR_CONF_MDNS_EN       0x151 // mDNS enable flag (1 byte; 0x73 = enabled, 0xFF = unset/default-enabled)
#define ADDR_CONF_MDNS_NAME     0x152 // Custom mDNS hostname (33 bytes, null-terminated; empty = auto)
#define ADDR_CONF_PROBE_EN      0x23C // rnprobe responder enable (1 byte; 0x73 = enabled, 0xFF = unset/disabled)
#ifdef REMOTE_CONFIG
// Remote management (see RemoteConfig.h): identity hashes allowed to manage this node over a
// Reticulum Link. Zero admins (or 0xFF = unset) leaves remote management inert.
#define REMOTE_CONFIG_MAX_ADMINS 4
#define ADDR_CONF_ADMIN_COUNT   0x240 // number of admin identity hashes (1 byte; 0xFF = unset = none)
#define ADDR_CONF_ADMIN_HASHES  0x241 // REMOTE_CONFIG_MAX_ADMINS x 16 bytes (0x241..0x280)
#endif
// Extra backbone slots 1-3 (slot 0 remains in the legacy BTCP/BHOST/BHPORT
// fields for backward compatibility with existing devices).
#define ADDR_CONF_BSLOT_BASE    0x173
#define ADDR_CONF_BSLOT_EN(slot)   (ADDR_CONF_BSLOT_BASE + ((slot) - 1) * FIREWALL_BACKBONE_SLOT_BYTES)
#define ADDR_CONF_BSLOT_HOST(slot) (ADDR_CONF_BSLOT_EN(slot) + 1)
#define ADDR_CONF_BSLOT_PORT(slot) (ADDR_CONF_BSLOT_HOST(slot) + FIREWALL_BACKBONE_HOST_LEN)
// Total: 0x23C (572 bytes — still within the extended CONFIG area used on ESP32)

#define FIREWALL_ENABLE_BYTE 0x73
#define FIREWALL_APP_MARKER0 0x52
#define FIREWALL_APP_MARKER1 0x54
#define FIREWALL_APP_VERSION 0x01

// ─── Firewall Mode Runtime State ─────────────────────────────────────────────
#ifndef FIREWALL_STATE_DEFINED
#define FIREWALL_STATE_DEFINED
struct FirewallBackboneSlot {
    bool     enabled;
    char     host[FIREWALL_BACKBONE_HOST_LEN];
    uint16_t port;
    bool     connected;
};

struct FirewallState {
    bool     enabled;
    bool     wifi_enabled;    // false = LoRa-only repeater (no WiFi)
    FirewallBackboneSlot backbones[FIREWALL_BACKBONE_SLOTS];

    // AP TCP server settings
    bool     ap_tcp_enabled;  // Whether to run a WiFi AP with TCP server
    uint16_t ap_tcp_port;     // Port for the AP TCP server
    char     ap_ssid[33];     // AP SSID
    char     ap_psk[33];      // AP PSK (empty = open)

    // IFAC settings for LoRa interface
    bool     ifac_enabled;    // Whether IFAC is configured
    char     ifac_netname[33]; // Network name (empty = not set)
    char     ifac_passphrase[33]; // Passphrase (empty = not set)

    // Device advertisement settings (used to advertise this node's
    // parameters and optional GPS location so external maps such as
    // rmap.world can pin this node). Defaults to disabled.
    bool     advert_enabled;  // Whether to advertise this device
    double   advert_lat;      // Latitude in decimal degrees (-90..90)
    double   advert_lon;      // Longitude in decimal degrees (-180..180)
    bool     advert_jitter;   // Randomize ~0.5 km offset for advertised coords
    char     node_name[33];   // Human-readable name (empty = auto from node hash)

    // Airtime / duty-cycle limits, in fraction (0.0 = disabled, 0.01 = 1%).
    // Mirrored into the global st_airtime_limit / lt_airtime_limit at boot.
    float    st_airtime_limit; // ~15 second rolling window
    float    lt_airtime_limit; // ~1 hour rolling window

    // mDNS / Bonjour service.  When disabled the device publishes nothing on
    // multicast and is only reachable by IP.  Enabled by default.
    bool     mdns_enabled;
    // Custom mDNS hostname.  Empty string = auto-generated `rtnode<XXXX>` from
    // the last 4 hex chars of the device MAC.  Validated to RFC-952 plus
    // hyphen/digit at save time.
    char     mdns_hostname[33];

    // rnprobe responder — when enabled, the Transport instance registers a
    // well-known destination that responds to rnprobe utility probes.
    // The destination hash is deterministic: hash(transport_identity_hash +
    // name_hash("rnstransport", "probe")).  Default: disabled.
    bool     probe_enabled;

#ifdef REMOTE_CONFIG
    // Remote management: identity hashes (16 bytes) allowed to manage this node (0 = inert)
    uint8_t  admin_count;
    uint8_t  admin_hashes[REMOTE_CONFIG_MAX_ADMINS][16];
#endif

    // Runtime state
    bool     wifi_connected;
    bool     ap_tcp_connected;    // Local TCP server (LAN) has client
    bool     ap_active;
    uint32_t packets_bridged_lora_to_tcp;
    uint32_t packets_bridged_tcp_to_lora;
    uint32_t last_bridge_activity;
};
#endif // FIREWALL_STATE_DEFINED

// Global boundary state instance (defined in RNode_Firmware.ino)
extern FirewallState firewall_state;

inline size_t firewall_backbone_enabled_count() {
    size_t count = 0;
    for (size_t i = 0; i < FIREWALL_BACKBONE_SLOTS; i++) {
        if (firewall_state.backbones[i].enabled) count++;
    }
    return count;
}

inline size_t firewall_backbone_connected_count() {
    size_t count = 0;
    for (size_t i = 0; i < FIREWALL_BACKBONE_SLOTS; i++) {
        if (firewall_state.backbones[i].enabled && firewall_state.backbones[i].connected) count++;
    }
    return count;
}

inline bool firewall_any_backbone_enabled() {
    return firewall_backbone_enabled_count() > 0;
}

// ─── Firewall Mode EEPROM Load/Save ─────────────────────────────────────────

inline bool firewall_app_marker_valid() {
    return EEPROM.read(config_addr(ADDR_CONF_APP_MARKER0)) == FIREWALL_APP_MARKER0 &&
           EEPROM.read(config_addr(ADDR_CONF_APP_MARKER1)) == FIREWALL_APP_MARKER1;
}

inline bool firewall_app_version_matches() {
    return firewall_app_marker_valid() &&
           EEPROM.read(config_addr(ADDR_CONF_APP_VERSION)) == FIREWALL_APP_VERSION;
}

inline void firewall_clear_app_marker() {
    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER0), 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER1), 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_APP_VERSION), 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_BMODE), 0xFF);
    EEPROM.commit();
}

// ─── Helpers for serialising IEEE-754 doubles to EEPROM ──────────────────────
// Stored as 8 raw bytes in EEPROM order. If all 8 bytes read back as 0xFF
// (uninitialised), the value is treated as "not set" and a default is used.
inline void firewall_write_double(int addr, double value) {
    uint8_t buf[8];
    memcpy(buf, &value, sizeof(buf));
    for (int i = 0; i < 8; i++) {
        EEPROM.write(config_addr(addr + i), buf[i]);
    }
}

inline bool firewall_read_double(int addr, double& out) {
    uint8_t buf[8];
    bool all_ff = true;
    for (int i = 0; i < 8; i++) {
        buf[i] = EEPROM.read(config_addr(addr + i));
        if (buf[i] != 0xFF) all_ff = false;
    }
    if (all_ff) return false;
    memcpy(&out, buf, sizeof(out));
    // Reject NaN / inf values that may slip in from corrupted EEPROM. Range
    // clamping for valid-but-out-of-range coordinates happens at the call site.
    if (isnan(out) || isinf(out)) return false;
    return true;
}

inline void firewall_load_config() {
    // Check if firewall mode is configured
    uint8_t bmode = EEPROM.read(config_addr(ADDR_CONF_BMODE));
    firewall_state.enabled = (bmode == FIREWALL_ENABLE_BYTE);

    if (!firewall_state.enabled) {
        // Use compile-time defaults
        firewall_state.wifi_enabled = true;
        for (size_t i = 0; i < FIREWALL_BACKBONE_SLOTS; i++) {
            firewall_state.backbones[i].enabled = false;
            firewall_state.backbones[i].host[0] = '\0';
            firewall_state.backbones[i].port = FIREWALL_BACKBONE_PORT;
            firewall_state.backbones[i].connected = false;
        }
        firewall_state.backbones[0].enabled = (FIREWALL_TCP_MODE == 1);
        strncpy(firewall_state.backbones[0].host, FIREWALL_BACKBONE_HOST,
            sizeof(firewall_state.backbones[0].host) - 1);
        firewall_state.backbones[0].host[sizeof(firewall_state.backbones[0].host) - 1] = '\0';
        firewall_state.backbones[0].port = FIREWALL_BACKBONE_PORT;
        firewall_state.ap_tcp_enabled = false;
        firewall_state.ap_tcp_port = 4242;
        firewall_state.ap_ssid[0] = '\0';
        firewall_state.ap_psk[0] = '\0';
        firewall_state.ifac_enabled = false;
        firewall_state.ifac_netname[0] = '\0';
        firewall_state.ifac_passphrase[0] = '\0';
        firewall_state.advert_enabled = false;
        firewall_state.advert_lat = 0.0;
        firewall_state.advert_lon = 0.0;
        firewall_state.advert_jitter = false;
        firewall_state.node_name[0] = '\0';
        firewall_state.st_airtime_limit = 0.0f;
        firewall_state.lt_airtime_limit = 0.0f;
        st_airtime_limit = 0.0f;
        lt_airtime_limit = 0.0f;
        firewall_state.mdns_enabled = true;
        firewall_state.mdns_hostname[0] = '\0';
        // Mark as enabled since we're compiled with FIREWALL_MODE
        firewall_state.enabled = true;
        return;
    }

    // Load wifi enable flag (default to enabled if unprogrammed 0xFF)
    uint8_t wifi_en_byte = EEPROM.read(config_addr(ADDR_CONF_WIFI_EN));
    firewall_state.wifi_enabled = (wifi_en_byte == FIREWALL_ENABLE_BYTE || wifi_en_byte == 0xFF);

    // Load from EEPROM
    firewall_state.backbones[0].enabled =
        (EEPROM.read(config_addr(ADDR_CONF_BTCP_MODE)) == 1);
    for (int i = 0; i < (int)sizeof(firewall_state.backbones[0].host) - 1; i++) {
        firewall_state.backbones[0].host[i] = EEPROM.read(config_addr(ADDR_CONF_BHOST + i));
        if (firewall_state.backbones[0].host[i] == (char)0xFF) {
            firewall_state.backbones[0].host[i] = '\0';
        }
    }
    firewall_state.backbones[0].host[sizeof(firewall_state.backbones[0].host) - 1] = '\0';

    firewall_state.backbones[0].port =
        ((uint16_t)EEPROM.read(config_addr(ADDR_CONF_BHPORT)) << 8) |
        (uint16_t)EEPROM.read(config_addr(ADDR_CONF_BHPORT + 1));
    if (firewall_state.backbones[0].port == 0 || firewall_state.backbones[0].port == 0xFFFF) {
        firewall_state.backbones[0].port = FIREWALL_BACKBONE_PORT;
    }
    firewall_state.backbones[0].connected = false;

    for (size_t slot = 1; slot < FIREWALL_BACKBONE_SLOTS; slot++) {
        firewall_state.backbones[slot].enabled =
            (EEPROM.read(config_addr(ADDR_CONF_BSLOT_EN(slot))) == FIREWALL_ENABLE_BYTE);
        for (int i = 0; i < (int)sizeof(firewall_state.backbones[slot].host) - 1; i++) {
            firewall_state.backbones[slot].host[i] = EEPROM.read(config_addr(ADDR_CONF_BSLOT_HOST(slot) + i));
            if (firewall_state.backbones[slot].host[i] == (char)0xFF) {
                firewall_state.backbones[slot].host[i] = '\0';
            }
        }
        firewall_state.backbones[slot].host[sizeof(firewall_state.backbones[slot].host) - 1] = '\0';

        firewall_state.backbones[slot].port =
            ((uint16_t)EEPROM.read(config_addr(ADDR_CONF_BSLOT_PORT(slot))) << 8) |
            (uint16_t)EEPROM.read(config_addr(ADDR_CONF_BSLOT_PORT(slot) + 1));
        if (firewall_state.backbones[slot].port == 0 || firewall_state.backbones[slot].port == 0xFFFF) {
            firewall_state.backbones[slot].port = FIREWALL_BACKBONE_PORT;
        }
        firewall_state.backbones[slot].connected = false;
    }

    // Load AP TCP server settings
    firewall_state.ap_tcp_enabled =
        (EEPROM.read(config_addr(ADDR_CONF_AP_TCP_EN)) == FIREWALL_ENABLE_BYTE);

    firewall_state.ap_tcp_port =
        ((uint16_t)EEPROM.read(config_addr(ADDR_CONF_AP_TCP_PORT)) << 8) |
        (uint16_t)EEPROM.read(config_addr(ADDR_CONF_AP_TCP_PORT + 1));
    if (firewall_state.ap_tcp_port == 0 || firewall_state.ap_tcp_port == 0xFFFF) {
        firewall_state.ap_tcp_port = 4242;
    }

    for (int i = 0; i < 32; i++) {
        firewall_state.ap_ssid[i] = EEPROM.read(config_addr(ADDR_CONF_AP_SSID + i));
        if (firewall_state.ap_ssid[i] == (char)0xFF) firewall_state.ap_ssid[i] = '\0';
    }
    firewall_state.ap_ssid[32] = '\0';

    for (int i = 0; i < 32; i++) {
        firewall_state.ap_psk[i] = EEPROM.read(config_addr(ADDR_CONF_AP_PSK + i));
        if (firewall_state.ap_psk[i] == (char)0xFF) firewall_state.ap_psk[i] = '\0';
    }
    firewall_state.ap_psk[32] = '\0';

    // Load IFAC settings
    firewall_state.ifac_enabled =
        (EEPROM.read(config_addr(ADDR_CONF_IFAC_EN)) == FIREWALL_ENABLE_BYTE);

    for (int i = 0; i < 32; i++) {
        firewall_state.ifac_netname[i] = EEPROM.read(config_addr(ADDR_CONF_IFAC_NAME + i));
        if (firewall_state.ifac_netname[i] == (char)0xFF) firewall_state.ifac_netname[i] = '\0';
    }
    firewall_state.ifac_netname[32] = '\0';

    for (int i = 0; i < 32; i++) {
        firewall_state.ifac_passphrase[i] = EEPROM.read(config_addr(ADDR_CONF_IFAC_PASS + i));
        if (firewall_state.ifac_passphrase[i] == (char)0xFF) firewall_state.ifac_passphrase[i] = '\0';
    }
    firewall_state.ifac_passphrase[32] = '\0';

    // Load device advertisement settings. Defaults to disabled for both
    // the master toggle and the location jitter, so previously saved
    // configurations (which have 0xFF in these slots) keep advertising
    // off until the user explicitly enables it from the portal.
    {
        uint8_t advert_en_byte = EEPROM.read(config_addr(ADDR_CONF_ADVERT_EN));
        firewall_state.advert_enabled = (advert_en_byte == FIREWALL_ENABLE_BYTE);

        if (!firewall_read_double(ADDR_CONF_ADVERT_LAT, firewall_state.advert_lat)) {
            firewall_state.advert_lat = 0.0;
        }
        if (!firewall_read_double(ADDR_CONF_ADVERT_LON, firewall_state.advert_lon)) {
            firewall_state.advert_lon = 0.0;
        }

        // Clamp to valid ranges in case of corrupted EEPROM data.
        if (firewall_state.advert_lat < -90.0 || firewall_state.advert_lat > 90.0) {
            firewall_state.advert_lat = 0.0;
        }
        if (firewall_state.advert_lon < -180.0 || firewall_state.advert_lon > 180.0) {
            firewall_state.advert_lon = 0.0;
        }

        uint8_t advert_jitter_byte = EEPROM.read(config_addr(ADDR_CONF_ADVERT_JITTER));
        firewall_state.advert_jitter = (advert_jitter_byte == FIREWALL_ENABLE_BYTE);

        for (int i = 0; i < 32; i++) {
            firewall_state.node_name[i] = EEPROM.read(config_addr(ADDR_CONF_NODE_NAME + i));
            if (firewall_state.node_name[i] == (char)0xFF) firewall_state.node_name[i] = '\0';
        }
        firewall_state.node_name[32] = '\0';
    }

    // Airtime limits (1 byte each, percent * 10; 0xFF = unset = disabled).
    {
        uint8_t st_byte = EEPROM.read(config_addr(ADDR_CONF_ST_AL));
        uint8_t lt_byte = EEPROM.read(config_addr(ADDR_CONF_LT_AL));
        if (st_byte == 0xFF) st_byte = 0;
        if (lt_byte == 0xFF) lt_byte = 0;
        // Convert to fraction (percent/100) with 0.1% resolution.
        firewall_state.st_airtime_limit = (float)st_byte / 1000.0f;
        firewall_state.lt_airtime_limit = (float)lt_byte / 1000.0f;
        // Apply to globals consumed by the airtime_lock check.
        st_airtime_limit = firewall_state.st_airtime_limit;
        lt_airtime_limit = firewall_state.lt_airtime_limit;
    }

    // mDNS enable flag — enabled unless explicitly disabled (0xFF = unset = enabled).
    {
        uint8_t mdns_en_byte = EEPROM.read(config_addr(ADDR_CONF_MDNS_EN));
        firewall_state.mdns_enabled =
            (mdns_en_byte == FIREWALL_ENABLE_BYTE || mdns_en_byte == 0xFF);
    }

    // Custom mDNS hostname (33 bytes, null-terminated; 0xFF = unset = empty).
    for (int i = 0; i < 32; i++) {
        firewall_state.mdns_hostname[i] = EEPROM.read(config_addr(ADDR_CONF_MDNS_NAME + i));
        if (firewall_state.mdns_hostname[i] == (char)0xFF) firewall_state.mdns_hostname[i] = '\0';
    }
    firewall_state.mdns_hostname[32] = '\0';

    // rnprobe responder — disabled by default (0xFF = unset)
    {
        uint8_t probe_byte = EEPROM.read(config_addr(ADDR_CONF_PROBE_EN));
        firewall_state.probe_enabled = (probe_byte == FIREWALL_ENABLE_BYTE);
    }

#ifdef REMOTE_CONFIG
    // Remote management admin identities (0xFF / out of range = none)
    {
        uint8_t n = EEPROM.read(config_addr(ADDR_CONF_ADMIN_COUNT));
        if (n > REMOTE_CONFIG_MAX_ADMINS) n = 0;
        firewall_state.admin_count = n;
        for (uint8_t a = 0; a < REMOTE_CONFIG_MAX_ADMINS; a++) {
            for (uint8_t i = 0; i < 16; i++) {
                firewall_state.admin_hashes[a][i] = (a < n) ? EEPROM.read(config_addr(ADDR_CONF_ADMIN_HASHES + a * 16 + i)) : 0;
            }
        }
    }
#endif

    // Reset runtime state
    firewall_state.packets_bridged_lora_to_tcp = 0;
    firewall_state.packets_bridged_tcp_to_lora = 0;
    firewall_state.last_bridge_activity = 0;
    firewall_state.wifi_connected = false;
    for (size_t i = 0; i < FIREWALL_BACKBONE_SLOTS; i++) {
        firewall_state.backbones[i].connected = false;
    }
    firewall_state.ap_active = false;
}

inline void firewall_save_config() {
    EEPROM.write(config_addr(ADDR_CONF_BMODE), FIREWALL_ENABLE_BYTE);
    EEPROM.write(config_addr(ADDR_CONF_WIFI_EN),
                 firewall_state.wifi_enabled ? FIREWALL_ENABLE_BYTE : 0x00);
    EEPROM.write(config_addr(ADDR_CONF_BTCP_MODE), firewall_state.backbones[0].enabled ? 1 : 0);
    // Keep legacy BTCP_PORT in sync with backbone slot 1 for compatibility
    // with older tooling that still inspects this field.
    EEPROM.write(config_addr(ADDR_CONF_BTCP_PORT), (firewall_state.backbones[0].port >> 8) & 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_BTCP_PORT + 1), firewall_state.backbones[0].port & 0xFF);
    for (int i = 0; i < (int)sizeof(firewall_state.backbones[0].host) - 1; i++) {
        EEPROM.write(config_addr(ADDR_CONF_BHOST + i), firewall_state.backbones[0].host[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_BHOST + 63), 0x00);
    EEPROM.write(config_addr(ADDR_CONF_BHPORT), (firewall_state.backbones[0].port >> 8) & 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_BHPORT + 1), firewall_state.backbones[0].port & 0xFF);

    for (size_t slot = 1; slot < FIREWALL_BACKBONE_SLOTS; slot++) {
        EEPROM.write(config_addr(ADDR_CONF_BSLOT_EN(slot)),
                     firewall_state.backbones[slot].enabled ? FIREWALL_ENABLE_BYTE : 0x00);
        for (int i = 0; i < (int)sizeof(firewall_state.backbones[slot].host) - 1; i++) {
            EEPROM.write(config_addr(ADDR_CONF_BSLOT_HOST(slot) + i), firewall_state.backbones[slot].host[i]);
        }
        EEPROM.write(config_addr(ADDR_CONF_BSLOT_HOST(slot) + sizeof(firewall_state.backbones[slot].host) - 1), 0x00);
        EEPROM.write(config_addr(ADDR_CONF_BSLOT_PORT(slot)), (firewall_state.backbones[slot].port >> 8) & 0xFF);
        EEPROM.write(config_addr(ADDR_CONF_BSLOT_PORT(slot) + 1), firewall_state.backbones[slot].port & 0xFF);
    }

    // AP TCP server settings
    EEPROM.write(config_addr(ADDR_CONF_AP_TCP_EN),
                 firewall_state.ap_tcp_enabled ? FIREWALL_ENABLE_BYTE : 0x00);
    EEPROM.write(config_addr(ADDR_CONF_AP_TCP_PORT), (firewall_state.ap_tcp_port >> 8) & 0xFF);
    EEPROM.write(config_addr(ADDR_CONF_AP_TCP_PORT + 1), firewall_state.ap_tcp_port & 0xFF);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_AP_SSID + i), firewall_state.ap_ssid[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_AP_SSID + 32), 0x00);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_AP_PSK + i), firewall_state.ap_psk[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_AP_PSK + 32), 0x00);

    // IFAC settings
    EEPROM.write(config_addr(ADDR_CONF_IFAC_EN),
                 firewall_state.ifac_enabled ? FIREWALL_ENABLE_BYTE : 0x00);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_IFAC_NAME + i), firewall_state.ifac_netname[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_IFAC_NAME + 32), 0x00);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_IFAC_PASS + i), firewall_state.ifac_passphrase[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_IFAC_PASS + 32), 0x00);

    // Device advertisement settings
    EEPROM.write(config_addr(ADDR_CONF_ADVERT_EN),
                 firewall_state.advert_enabled ? FIREWALL_ENABLE_BYTE : 0x00);
    firewall_write_double(ADDR_CONF_ADVERT_LAT, firewall_state.advert_lat);
    firewall_write_double(ADDR_CONF_ADVERT_LON, firewall_state.advert_lon);
    EEPROM.write(config_addr(ADDR_CONF_ADVERT_JITTER),
                 firewall_state.advert_jitter ? FIREWALL_ENABLE_BYTE : 0x00);
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_NODE_NAME + i), firewall_state.node_name[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_NODE_NAME + 32), 0x00);

    // Airtime limits — clamp to 0.0–25.5% then encode as percent * 10.
    {
        float st_pct = firewall_state.st_airtime_limit * 100.0f;
        float lt_pct = firewall_state.lt_airtime_limit * 100.0f;
        if (st_pct < 0.0f) st_pct = 0.0f;
        if (lt_pct < 0.0f) lt_pct = 0.0f;
        if (st_pct > 25.5f) st_pct = 25.5f;
        if (lt_pct > 25.5f) lt_pct = 25.5f;
        uint8_t st_byte = (uint8_t)(st_pct * 10.0f + 0.5f);
        uint8_t lt_byte = (uint8_t)(lt_pct * 10.0f + 0.5f);
        EEPROM.write(config_addr(ADDR_CONF_ST_AL), st_byte);
        EEPROM.write(config_addr(ADDR_CONF_LT_AL), lt_byte);
    }

    // mDNS enable flag
    EEPROM.write(config_addr(ADDR_CONF_MDNS_EN),
                 firewall_state.mdns_enabled ? FIREWALL_ENABLE_BYTE : 0x00);

    // Custom mDNS hostname
    for (int i = 0; i < 32; i++) {
        EEPROM.write(config_addr(ADDR_CONF_MDNS_NAME + i), firewall_state.mdns_hostname[i]);
    }
    EEPROM.write(config_addr(ADDR_CONF_MDNS_NAME + 32), 0x00);

    // rnprobe responder
    EEPROM.write(config_addr(ADDR_CONF_PROBE_EN),
                 firewall_state.probe_enabled ? FIREWALL_ENABLE_BYTE : 0x00);

#ifdef REMOTE_CONFIG
    // Remote management admin identities
    EEPROM.write(config_addr(ADDR_CONF_ADMIN_COUNT), firewall_state.admin_count);
    for (uint8_t a = 0; a < REMOTE_CONFIG_MAX_ADMINS; a++) {
        for (uint8_t i = 0; i < 16; i++) {
            EEPROM.write(config_addr(ADDR_CONF_ADMIN_HASHES + a * 16 + i), firewall_state.admin_hashes[a][i]);
        }
    }
#endif

    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER0), FIREWALL_APP_MARKER0);
    EEPROM.write(config_addr(ADDR_CONF_APP_MARKER1), FIREWALL_APP_MARKER1);
    EEPROM.write(config_addr(ADDR_CONF_APP_VERSION), FIREWALL_APP_VERSION);

    EEPROM.commit();
}

#endif // FIREWALL_MODE
#endif // FIREWALL_MODE_H
