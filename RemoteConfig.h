// ───────────────────────────────────────
//  RemoteConfig.h — authenticated remote management of an RTNode over a Reticulum Link
//
//  Optional (build flag -DREMOTE_CONFIG, requires FIREWALL_MODE). When at least one
//  admin identity hash is configured in the captive portal, the node exposes the
//  destination "rtnode.config". A peer opens a Link to it, calls link.identify() with
//  an identity whose hash is in the admin list, then issues requests:
//
//      /config/get   {"section": "<name>", "secrets": false}   -> JSON text (bytes)
//
//  Sections: info, wifi, backbones, server, lora, ifac, advert.
//
//  Design constraints (see CORE_PRINCIPLES.md):
//   - Nothing is exposed unless an admin identity is configured (inert otherwise).
//   - The Link is end-to-end encrypted and the request handler is ALLOW_LIST: the
//     library only runs a handler when the peer proved ownership of a listed identity.
//   - One session at a time; idle sessions are torn down.
//   - Every response is a single Link packet (bounded size, no Resource transfer),
//     hence one small section per request.
//   - Secrets (WiFi PSK, IFAC passphrase) are only returned when explicitly asked for.
//   - The destination is pinned in the boundary whitelist so the WAN filter admits it.
// ───────────────────────────────────────
#ifndef REMOTE_CONFIG_H
#define REMOTE_CONFIG_H

#if defined(FIREWALL_MODE) && defined(REMOTE_CONFIG)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Bytes.h>
#include <Identity.h>
#include <Destination.h>
#include <Link.h>
#include <Transport.h>
#include <Log.h>
#include <set>
#include <string>

#include "FirewallMode.h"

// Externally-defined LoRa parameters (see Config.h / RNode_Firmware.ino)
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int      lora_sf;
extern int      lora_cr;
extern int      lora_txp;
extern float    st_airtime_limit;
extern float    lt_airtime_limit;

#ifndef NODE_HASH_RTC_MAGIC
#define NODE_HASH_RTC_MAGIC  0x504B4841UL
#endif
extern uint32_t rtc_node_hash_magic;
extern char     rtc_node_hash_hex[33];

// A response must fit in one Link packet: MDU is 431 bytes at the default MTU, minus msgpack framing.
#define REMOTE_CONFIG_MAX_RESPONSE  360
// Refuse to serve when the heap is nearly exhausted (the ESP32-S3 has ~200 KB free at rest).
#define REMOTE_CONFIG_MIN_FREE_HEAP 40000
// Tear down a session that has been idle this long (seconds).
#define REMOTE_CONFIG_IDLE_TIMEOUT  120.0

static RNS::Destination remote_config_destination({RNS::Type::NONE});
static RNS::Link        remote_config_link({RNS::Type::NONE});
static bool             remote_config_ready = false;

static RNS::Bytes remote_config_json(const JsonDocument& doc) {
    if (measureJson(doc) > REMOTE_CONFIG_MAX_RESPONSE) {
        static const char err[] = "{\"error\":\"response too large\"}";
        return RNS::Bytes((const uint8_t*)err, sizeof(err) - 1);
    }
    char buf[REMOTE_CONFIG_MAX_RESPONSE + 1];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    return RNS::Bytes((const uint8_t*)buf, n);
}

static RNS::Bytes remote_config_error(const char* message) {
    JsonDocument doc;
    doc["error"] = message;
    return remote_config_json(doc);
}

static std::string remote_config_eeprom_string(int addr, int maxlen) {
    std::string s;
    for (int i = 0; i < maxlen; i++) {
        uint8_t c = EEPROM.read(config_addr(addr + i));
        if (c == 0x00 || c == 0xFF) break;
        s.push_back((char)c);
    }
    return s;
}

// Fill `doc` with one configuration section. Returns false for an unknown section.
static bool remote_config_section(const std::string& section, bool secrets, JsonDocument& doc) {
    doc["section"] = section;
    if (section == "info") {
        doc["firmware"] = FW_RELEASE_TAG;
        doc["name"] = firewall_state.node_name;
        if (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC) doc["node_hash"] = rtc_node_hash_hex;
        doc["uptime_s"] = (uint32_t)(millis() / 1000UL);
        doc["free_heap"] = (uint32_t)ESP.getFreeHeap();
        doc["wifi_connected"] = firewall_state.wifi_connected;
        doc["backbones_connected"] = (uint32_t)firewall_backbone_connected_count();
        doc["admins"] = firewall_state.admin_count;
    }
    else if (section == "wifi") {
        doc["enabled"] = firewall_state.wifi_enabled;
        std::string ssid = remote_config_eeprom_string(ADDR_CONF_SSID, 32);
        std::string psk  = remote_config_eeprom_string(ADDR_CONF_PSK, 32);
        doc["ssid"] = ssid;
        doc["psk_set"] = !psk.empty();
        if (secrets) doc["psk"] = psk;
    }
    else if (section == "backbones") {
        JsonArray arr = doc["backbones"].to<JsonArray>();
        for (size_t i = 0; i < FIREWALL_BACKBONE_SLOTS; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["enabled"] = firewall_state.backbones[i].enabled;
            o["host"] = firewall_state.backbones[i].host;
            o["port"] = firewall_state.backbones[i].port;
            o["connected"] = firewall_state.backbones[i].connected;
        }
    }
    else if (section == "server") {
        doc["tcp_enabled"] = firewall_state.ap_tcp_enabled;
        doc["tcp_port"] = firewall_state.ap_tcp_port;
        doc["mdns_enabled"] = firewall_state.mdns_enabled;
        doc["mdns_hostname"] = firewall_state.mdns_hostname;
        doc["probe_enabled"] = firewall_state.probe_enabled;
    }
    else if (section == "lora") {
        doc["freq_hz"] = lora_freq;
        doc["bw_hz"] = lora_bw;
        doc["sf"] = lora_sf;
        doc["cr"] = lora_cr;
        doc["txp_dbm"] = lora_txp;
        doc["airtime_short"] = st_airtime_limit;
        doc["airtime_long"] = lt_airtime_limit;
    }
    else if (section == "ifac") {
        doc["enabled"] = firewall_state.ifac_enabled;
        doc["netname"] = firewall_state.ifac_netname;
        doc["passphrase_set"] = (firewall_state.ifac_passphrase[0] != '\0');
        if (secrets) doc["passphrase"] = firewall_state.ifac_passphrase;
    }
    else if (section == "advert") {
        doc["enabled"] = firewall_state.advert_enabled;
        doc["lat"] = firewall_state.advert_lat;
        doc["lon"] = firewall_state.advert_lon;
        doc["jitter"] = firewall_state.advert_jitter;
    }
    else {
        return false;
    }
    return true;
}

// Request handler for "/config/get". Only reached for peers proven to own an admin identity.
static RNS::Bytes remote_config_get(const RNS::Bytes& path, const RNS::Bytes& data, const RNS::Bytes& request_id,
                                    const RNS::Bytes& link_id, const RNS::Identity& remote_identity, double requested_at) {
    if (ESP.getFreeHeap() < REMOTE_CONFIG_MIN_FREE_HEAP) return remote_config_error("low memory");
    std::string section = "info";
    bool secrets = false;
    if (data.size() > 0) {
        JsonDocument req;
        if (deserializeJson(req, data.data(), data.size())) return remote_config_error("bad request");
        section = req["section"] | "info";
        secrets = req["secrets"] | false;
    }
    JsonDocument doc;
    if (!remote_config_section(section, secrets, doc)) return remote_config_error("unknown section");
    return remote_config_json(doc);
}

static void remote_config_link_closed(RNS::Link& link) {
    NOTICE("Remote management: session closed");
    remote_config_link = RNS::Link({RNS::Type::NONE});
}

// Called by the library when a peer has opened a Link to the management destination.
static void remote_config_link_established(RNS::Link& link) {
    if (remote_config_link && remote_config_link.status() == RNS::Type::Link::ACTIVE) {
        NOTICE("Remote management: session already open, refusing another");
        link.teardown();
        return;
    }
    remote_config_link = link;
    link.set_link_closed_callback(remote_config_link_closed);
    NOTICE("Remote management: session opened");
}

// Register the management destination. Inert unless at least one admin identity is configured.
inline void remote_config_init() {
    if (firewall_state.admin_count == 0) {
        NOTICE("Remote management: no admin identity configured, disabled");
        return;
    }
    remote_config_destination = RNS::Destination(RNS::Transport::identity(), RNS::Type::Destination::IN,
                                                 RNS::Type::Destination::SINGLE, "rtnode", "config");
    remote_config_destination.set_link_established_callback(remote_config_link_established);

    std::set<RNS::Bytes> allowed;
    for (uint8_t a = 0; a < firewall_state.admin_count && a < REMOTE_CONFIG_MAX_ADMINS; a++) {
        allowed.insert(RNS::Bytes(firewall_state.admin_hashes[a], 16));
    }
    remote_config_destination.register_request_handler("/config/get", remote_config_get,
                                                       RNS::Type::Destination::ALLOW_LIST, allowed);

    // Let the boundary firewall admit backbone traffic addressed to this destination.
    RNS::Transport::firewall_pin_local_destination(remote_config_destination.hash());
    remote_config_ready = true;
    NOTICE("Remote management enabled, destination " + remote_config_destination.hash().toHex()
           + ", " + std::to_string(firewall_state.admin_count) + " admin(s)");
}

// Housekeeping: drop an idle session so that a forgotten client cannot lock out the others.
inline void remote_config_loop() {
    if (!remote_config_ready) return;
    if (remote_config_link && remote_config_link.status() == RNS::Type::Link::ACTIVE
        && remote_config_link.inactive_for() > REMOTE_CONFIG_IDLE_TIMEOUT) {
        NOTICE("Remote management: idle session torn down");
        remote_config_link.teardown();
    }
}

#else  // !REMOTE_CONFIG

inline void remote_config_init() {}
inline void remote_config_loop() {}

#endif // FIREWALL_MODE && REMOTE_CONFIG
#endif // REMOTE_CONFIG_H
