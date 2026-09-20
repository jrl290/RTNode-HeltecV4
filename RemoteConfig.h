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

// KISS command carrying one JSON request/response over USB (provisioning; physical access = trust).
#define CMD_RT_CONFIG               0xB0
#define REMOTE_CONFIG_USB_MAX       1400

static uint32_t remote_config_reboot_at = 0;   // millis() deadline of a deferred restart, 0 = none

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
    else if (section == "admins") {
        JsonArray arr = doc["admins"].to<JsonArray>();
        for (uint8_t a = 0; a < firewall_state.admin_count && a < REMOTE_CONFIG_MAX_ADMINS; a++) {
            char hx[33];
            for (uint8_t i = 0; i < 16; i++) snprintf(hx + i * 2, 3, "%02x", firewall_state.admin_hashes[a][i]);
            arr.add(hx);
        }
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

// ── Partial configuration apply ──────────────────────────────────────────────────────────────
// Applies only the keys present in `cfg`, with the same validation and persistence as the
// captive portal, then saves. `allow_admins` (USB only) also allows replacing the admin list.
static void remote_config_write_string(int addr, const char* value, int maxlen) {
    for (int i = 0; i < maxlen; i++) {
        EEPROM.write(config_addr(addr + i), (value[i] != '\0' && i < (int)strlen(value)) ? (uint8_t)value[i] : 0x00);
    }
    EEPROM.write(config_addr(addr + maxlen), 0x00);
}

static bool remote_config_fits(JsonVariantConst v, size_t maxlen) {
    return v.is<const char*>() && strlen(v.as<const char*>()) <= maxlen;
}

static bool remote_config_hex16(const char* hex, uint8_t out[16]) {
    if (strlen(hex) != 32) return false;
    for (int i = 0; i < 32; i++) if (!isxdigit((unsigned char)hex[i])) return false;
    for (int i = 0; i < 16; i++) { char two[3] = { hex[i * 2], hex[i * 2 + 1], 0 }; out[i] = (uint8_t)strtoul(two, nullptr, 16); }
    return true;
}

static void remote_config_apply(JsonObjectConst cfg, bool allow_admins, JsonDocument& report) {
    JsonArray applied = report["applied"].to<JsonArray>();
    JsonArray errors  = report["errors"].to<JsonArray>();
    bool any = false;

    if (!cfg["name"].isNull()) {
        if (remote_config_fits(cfg["name"], 32)) {
            memset(firewall_state.node_name, 0, sizeof(firewall_state.node_name));
            strncpy(firewall_state.node_name, cfg["name"].as<const char*>(), sizeof(firewall_state.node_name) - 1);
            applied.add("name"); any = true;
        } else errors.add("name");
    }

    JsonObjectConst wifi = cfg["wifi"];
    if (!wifi.isNull()) {
        bool ok = true;
        if (!wifi["enabled"].isNull()) firewall_state.wifi_enabled = wifi["enabled"].as<bool>();
        if (!wifi["ssid"].isNull()) {
            if (remote_config_fits(wifi["ssid"], 32)) {
                remote_config_write_string(ADDR_CONF_SSID, wifi["ssid"].as<const char*>(), 32);
                EEPROM.write(eeprom_addr(ADDR_CONF_WIFI), WR_WIFI_STA);
            } else { errors.add("wifi.ssid"); ok = false; }
        }
        if (!wifi["psk"].isNull()) {
            if (remote_config_fits(wifi["psk"], 32)) remote_config_write_string(ADDR_CONF_PSK, wifi["psk"].as<const char*>(), 32);
            else { errors.add("wifi.psk"); ok = false; }
        }
        if (ok) { applied.add("wifi"); any = true; }
    }

    JsonArrayConst bbs = cfg["backbones"];
    if (!bbs.isNull()) {
        size_t slot = 0;
        for (JsonVariantConst b : bbs) {
            if (slot >= FIREWALL_BACKBONE_SLOTS) { errors.add("backbones.count"); break; }
            if (!b.isNull()) {
                FirewallBackboneSlot& s = firewall_state.backbones[slot];
                if (!b["host"].isNull()) {
                    if (remote_config_fits(b["host"], FIREWALL_BACKBONE_HOST_LEN - 1)) {
                        memset(s.host, 0, sizeof(s.host));
                        strncpy(s.host, b["host"].as<const char*>(), sizeof(s.host) - 1);
                    } else errors.add("backbones.host");
                }
                if (!b["port"].isNull()) {
                    long port = b["port"].as<long>();
                    if (port >= 1 && port <= 65535) s.port = (uint16_t)port; else errors.add("backbones.port");
                }
                if (!b["enabled"].isNull()) s.enabled = b["enabled"].as<bool>();
                if (s.port == 0) s.port = 4242;
                if (s.host[0] == '\0') s.enabled = false;   // same rule as the portal
                any = true;
            }
            slot++;
        }
        applied.add("backbones");
    }

    JsonObjectConst server = cfg["server"];
    if (!server.isNull()) {
        if (!server["tcp_enabled"].isNull()) firewall_state.ap_tcp_enabled = server["tcp_enabled"].as<bool>();
        if (!server["tcp_port"].isNull()) {
            long port = server["tcp_port"].as<long>();
            if (port >= 1 && port <= 65535) firewall_state.ap_tcp_port = (uint16_t)port; else errors.add("server.tcp_port");
        }
        if (!server["mdns_enabled"].isNull()) firewall_state.mdns_enabled = server["mdns_enabled"].as<bool>();
        if (!server["probe_enabled"].isNull()) firewall_state.probe_enabled = server["probe_enabled"].as<bool>();
        if (!server["mdns_hostname"].isNull()) {
            if (remote_config_fits(server["mdns_hostname"], 32)) {
                const char* in = server["mdns_hostname"].as<const char*>();
                memset(firewall_state.mdns_hostname, 0, sizeof(firewall_state.mdns_hostname));
                size_t j = 0;
                for (size_t i = 0; in[i] && j < sizeof(firewall_state.mdns_hostname) - 1; i++) {
                    char c = in[i];
                    if (c >= 'A' && c <= 'Z') c += 32;
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') firewall_state.mdns_hostname[j++] = c;
                }
            } else errors.add("server.mdns_hostname");
        }
        applied.add("server"); any = true;
    }

    JsonObjectConst lora = cfg["lora"];
    if (!lora.isNull()) {
        if (!lora["freq_hz"].isNull()) { uint32_t v = lora["freq_hz"].as<uint32_t>(); if (v >= 137000000UL && v <= 1020000000UL) lora_freq = v; else errors.add("lora.freq_hz"); }
        if (!lora["bw_hz"].isNull())   { uint32_t v = lora["bw_hz"].as<uint32_t>();   if (v >= 7800 && v <= 500000) lora_bw = v; else errors.add("lora.bw_hz"); }
        if (!lora["sf"].isNull())      { int v = lora["sf"].as<int>();  if (v >= 5 && v <= 12) lora_sf = v;  else errors.add("lora.sf"); }
        if (!lora["cr"].isNull())      { int v = lora["cr"].as<int>();  if (v >= 5 && v <= 8)  lora_cr = v;  else errors.add("lora.cr"); }
        if (!lora["txp_dbm"].isNull()) { int v = lora["txp_dbm"].as<int>(); if (v >= 2 && v <= 30) lora_txp = v; else errors.add("lora.txp_dbm"); }
        if (!lora["airtime_short_pct"].isNull() || !lora["airtime_long_pct"].isNull()) {
            float st = lora["airtime_short_pct"] | (firewall_state.st_airtime_limit * 100.0f);
            float lt = lora["airtime_long_pct"]  | (firewall_state.lt_airtime_limit * 100.0f);
            if (isnan(st) || st < 0.0f) st = 0.0f; if (st > 25.0f) st = 25.0f;
            if (isnan(lt) || lt < 0.0f) lt = 0.0f; if (lt > 25.0f) lt = 25.0f;
            firewall_state.st_airtime_limit = st / 100.0f;
            firewall_state.lt_airtime_limit = lt / 100.0f;
            EEPROM.write(config_addr(ADDR_CONF_ST_AL), (uint8_t)(st * 10.0f + 0.5f));
            EEPROM.write(config_addr(ADDR_CONF_LT_AL), (uint8_t)(lt * 10.0f + 0.5f));
        }
        // Persist exactly what the portal persists
        eeprom_update(eeprom_addr(ADDR_CONF_SF), lora_sf);
        eeprom_update(eeprom_addr(ADDR_CONF_CR), lora_cr);
        eeprom_update(eeprom_addr(ADDR_CONF_TXP), lora_txp);
        for (int i = 0; i < 4; i++) eeprom_update(eeprom_addr(ADDR_CONF_BW) + i, lora_bw >> (24 - 8 * i));
        for (int i = 0; i < 4; i++) eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + i, lora_freq >> (24 - 8 * i));
        eeprom_update(eeprom_addr(ADDR_CONF_OK), CONF_OK_BYTE);
        applied.add("lora"); any = true;
    }

    JsonObjectConst ifac = cfg["ifac"];
    if (!ifac.isNull()) {
        if (!ifac["enabled"].isNull()) firewall_state.ifac_enabled = ifac["enabled"].as<bool>();
        if (!ifac["netname"].isNull()) {
            if (remote_config_fits(ifac["netname"], 32)) {
                memset(firewall_state.ifac_netname, 0, sizeof(firewall_state.ifac_netname));
                strncpy(firewall_state.ifac_netname, ifac["netname"].as<const char*>(), sizeof(firewall_state.ifac_netname) - 1);
            } else errors.add("ifac.netname");
        }
        if (!ifac["passphrase"].isNull()) {
            if (remote_config_fits(ifac["passphrase"], 32)) {
                memset(firewall_state.ifac_passphrase, 0, sizeof(firewall_state.ifac_passphrase));
                strncpy(firewall_state.ifac_passphrase, ifac["passphrase"].as<const char*>(), sizeof(firewall_state.ifac_passphrase) - 1);
            } else errors.add("ifac.passphrase");
        }
        if (firewall_state.ifac_enabled && firewall_state.ifac_netname[0] == '\0' && firewall_state.ifac_passphrase[0] == '\0') {
            firewall_state.ifac_enabled = false;   // same rule as the portal
        }
        applied.add("ifac"); any = true;
    }

    JsonObjectConst advert = cfg["advert"];
    if (!advert.isNull()) {
        if (!advert["enabled"].isNull()) firewall_state.advert_enabled = advert["enabled"].as<bool>();
        if (!advert["jitter"].isNull())  firewall_state.advert_jitter  = advert["jitter"].as<bool>();
        if (!advert["lat"].isNull()) { double v = advert["lat"].as<double>(); if (v >= -90.0 && v <= 90.0)   firewall_state.advert_lat = v; else errors.add("advert.lat"); }
        if (!advert["lon"].isNull()) { double v = advert["lon"].as<double>(); if (v >= -180.0 && v <= 180.0) firewall_state.advert_lon = v; else errors.add("advert.lon"); }
        applied.add("advert"); any = true;
    }

    if (!cfg["admins"].isNull()) {
        if (!allow_admins) {
            errors.add("admins: USB only");
        } else {
            uint8_t n = 0;
            uint8_t hashes[REMOTE_CONFIG_MAX_ADMINS][16];
            bool ok = true;
            for (JsonVariantConst a : cfg["admins"].as<JsonArrayConst>()) {
                if (n >= REMOTE_CONFIG_MAX_ADMINS || !a.is<const char*>() || !remote_config_hex16(a.as<const char*>(), hashes[n])) { ok = false; break; }
                n++;
            }
            if (ok) {
                memset(firewall_state.admin_hashes, 0, sizeof(firewall_state.admin_hashes));
                memcpy(firewall_state.admin_hashes, hashes, sizeof(hashes[0]) * n);
                firewall_state.admin_count = n;
                applied.add("admins"); any = true;
            } else errors.add("admins");
        }
    }

    if (any) firewall_save_config();   // writes the firewall state and EEPROM.commit()
    else EEPROM.commit();
    report["ok"] = errors.size() == 0;
}

// Request handler for "/config/set". Same core as USB, but administrators can not be changed here.
static RNS::Bytes remote_config_set(const RNS::Bytes& path, const RNS::Bytes& data, const RNS::Bytes& request_id,
                                    const RNS::Bytes& link_id, const RNS::Identity& remote_identity, double requested_at) {
    if (ESP.getFreeHeap() < REMOTE_CONFIG_MIN_FREE_HEAP) return remote_config_error("low memory");
    JsonDocument req;
    if (data.size() == 0 || deserializeJson(req, data.data(), data.size())) return remote_config_error("bad request");
    JsonDocument report;
    remote_config_apply(req["config"].as<JsonObjectConst>(), false, report);
    bool reboot = req["reboot"] | true;
    if (reboot && report["ok"].as<bool>()) remote_config_reboot_at = millis() + 3000;
    report["reboot"] = reboot && report["ok"].as<bool>();
    return remote_config_json(report);
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
    remote_config_destination.register_request_handler("/config/set", remote_config_set,
                                                       RNS::Type::Destination::ALLOW_LIST, allowed);

    // Let the boundary firewall admit backbone traffic addressed to this destination.
    RNS::Transport::firewall_pin_local_destination(remote_config_destination.hash());
    remote_config_ready = true;
    NOTICE("Remote management enabled, destination " + remote_config_destination.hash().toHex()
           + ", " + std::to_string(firewall_state.admin_count) + " admin(s)");
}

// Housekeeping: drop an idle session so that a forgotten client cannot lock out the others.
inline void remote_config_loop() {
    if (remote_config_reboot_at != 0 && (int32_t)(millis() - remote_config_reboot_at) >= 0) {
        NOTICE("Remote management: restarting to apply configuration");
        ESP.restart();
    }
    if (!remote_config_ready) return;
    if (remote_config_link && remote_config_link.status() == RNS::Type::Link::ACTIVE
        && remote_config_link.inactive_for() > REMOTE_CONFIG_IDLE_TIMEOUT) {
        NOTICE("Remote management: idle session torn down");
        remote_config_link.teardown();
    }
}

// ── USB provisioning channel ─────────────────────────────────────────────────────────────────
// One KISS frame  FEND CMD_RT_CONFIG <escaped JSON> FEND  carries a request; the answer uses the same framing.
//   {"op":"get","section":"wifi","secrets":true}
//   {"op":"apply","config":{...},"reboot":true}     (same keys as /config/set, plus "admins": [hex32, ...])
//   {"op":"reboot"}
// Only honoured when the bytes really came from the USB port (never WiFi or Bluetooth): physical access is the
// trust anchor, exactly like the captive portal's button. It works even before any admin identity exists.
static char   remote_config_usb_buf[REMOTE_CONFIG_USB_MAX + 1];
static size_t remote_config_usb_len = 0;
static bool   remote_config_usb_esc = false;
static bool   remote_config_usb_overflow = false;

inline void remote_config_usb_begin() {
    remote_config_usb_len = 0;
    remote_config_usb_esc = false;
    remote_config_usb_overflow = false;
}

inline void remote_config_usb_feed(uint8_t b) {
    if (b == FESC) { remote_config_usb_esc = true; return; }
    if (remote_config_usb_esc) {
        if (b == TFEND) b = FEND; else if (b == TFESC) b = FESC;
        remote_config_usb_esc = false;
    }
    if (remote_config_usb_len < REMOTE_CONFIG_USB_MAX) remote_config_usb_buf[remote_config_usb_len++] = (char)b;
    else remote_config_usb_overflow = true;
}

static void remote_config_usb_reply(const JsonDocument& doc) {
    static char out[720];
    size_t n = measureJson(doc) < sizeof(out) ? serializeJson(doc, out, sizeof(out)) : 0;
    if (n == 0) { static const char e[] = "{\"error\":\"response too large\"}"; memcpy(out, e, sizeof(e) - 1); n = sizeof(e) - 1; }
    Serial.write((uint8_t)FEND);
    Serial.write((uint8_t)CMD_RT_CONFIG);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)out[i];
        if (b == FEND)      { Serial.write((uint8_t)FESC); Serial.write((uint8_t)TFEND); }
        else if (b == FESC) { Serial.write((uint8_t)FESC); Serial.write((uint8_t)TFESC); }
        else                { Serial.write(b); }
    }
    Serial.write((uint8_t)FEND);
    Serial.flush();
}

// Called on the closing FEND of a CMD_RT_CONFIG frame. `from_usb` must come from the caller's source check.
inline void remote_config_usb_finish(bool from_usb) {
    if (!from_usb) { NOTICE("Remote management: configuration command ignored (not received over USB)"); return; }
    JsonDocument reply;
    JsonDocument req;
    if (remote_config_usb_overflow || remote_config_usb_len == 0) {
        reply["error"] = "bad frame";
    } else if (deserializeJson(req, remote_config_usb_buf, remote_config_usb_len)) {
        reply["error"] = "bad json";
    } else {
        const char* op = req["op"] | "";
        if (strcmp(op, "get") == 0) {
            std::string section = req["section"] | "info";
            if (!remote_config_section(section, req["secrets"] | false, reply)) { reply.clear(); reply["error"] = "unknown section"; }
        } else if (strcmp(op, "apply") == 0) {
            remote_config_apply(req["config"].as<JsonObjectConst>(), true, reply);
            bool reboot = (req["reboot"] | true) && reply["ok"].as<bool>();
            if (reboot) remote_config_reboot_at = millis() + 2000;
            reply["reboot"] = reboot;
        } else if (strcmp(op, "reboot") == 0) {
            remote_config_reboot_at = millis() + 1500;
            reply["ok"] = true;
        } else {
            reply["error"] = "unknown op";
        }
    }
    remote_config_usb_reply(reply);
}

#else  // !REMOTE_CONFIG

inline void remote_config_init() {}
inline void remote_config_loop() {}

#endif // FIREWALL_MODE && REMOTE_CONFIG
#endif // REMOTE_CONFIG_H
