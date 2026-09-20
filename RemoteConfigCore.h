// ───────────────────────────────────────
//  RemoteConfigCore.h — platform-independent core of the remote configuration feature
//
//  The parts of RemoteConfig.h that do not need Reticulum: the partial configuration apply (same validation and
//  persistence as the captive portal), the admin-list rules (super_admin, admins_add / admins_remove) and the
//  receive buffer of the USB provisioning frame. Kept apart so that it can be compiled and unit-tested on a PC
//  (see tests/native/).
//
//  Expected from the including translation unit: FirewallMode.h (firewall_state, ADDR_CONF_*), ROM.h/Config.h
//  constants, EEPROM, config_addr()/eeprom_addr()/eeprom_update(), firewall_save_config(), the LoRa globals below,
//  the NOTICE(std::string) logging macro, and remote_config_admins_changed().
// ───────────────────────────────────────
#ifndef REMOTE_CONFIG_CORE_H
#define REMOTE_CONFIG_CORE_H

#if defined(FIREWALL_MODE) && defined(REMOTE_CONFIG)

#include <ArduinoJson.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define REMOTE_CONFIG_USB_MAX       1400

// KISS framing bytes (same values as Framing.h); defined here only if not already available.
#ifndef FEND
#define FEND  0xC0
#define FESC  0xDB
#define TFEND 0xDC
#define TFESC 0xDD
#endif

// Externally-defined LoRa parameters (see Config.h / RNode_Firmware.ino)
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int      lora_sf;
extern int      lora_cr;
extern int      lora_txp;
extern float    st_airtime_limit;
extern float    lt_airtime_limit;


// ── Partial configuration apply ──────────────────────────────────────────────────────────────
// Applies only the keys present in `cfg`, with the same validation and persistence as the
// captive portal, then saves. `from_usb` (physical access) also allows replacing the whole admin list; over the
// Reticulum network administrators are managed with "admins_add" / "admins_remove" only. `caller` is the 16-byte
// hash of the requesting identity (nullptr for USB), used to refuse self-removal.
static void remote_config_admins_changed();   // hook: called after the admin list changed (defined by the caller)

// Index of `hash` in the admin list, or -1. Slot 0 is the super_admin: installed over USB or in the portal, and
// never removable over the Reticulum network.
static int remote_config_admin_index(const uint8_t hash[16]) {
    for (uint8_t a = 0; a < firewall_state.admin_count && a < REMOTE_CONFIG_MAX_ADMINS; a++) {
        if (memcmp(firewall_state.admin_hashes[a], hash, 16) == 0) return a;
    }
    return -1;
}

static std::string remote_config_hex_short(const uint8_t hash[16]) {
    char hx[9];
    for (int i = 0; i < 4; i++) snprintf(hx + i * 2, 3, "%02x", hash[i]);
    return std::string(hx);
}

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

static void remote_config_apply(JsonObjectConst cfg, bool from_usb, const uint8_t* caller, JsonDocument& report) {
    bool admins_changed = false;
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
            if (isnan(st) || st < 0.0f) st = 0.0f;
            if (st > 25.0f) st = 25.0f;
            if (isnan(lt) || lt < 0.0f) lt = 0.0f;
            if (lt > 25.0f) lt = 25.0f;
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

    // Full replacement of the admin list: USB only. The first entry becomes the super_admin.
    if (!cfg["admins"].isNull()) {
        if (!from_usb) {
            errors.add("admins: whole-list replacement is USB only (use admins_add / admins_remove)");
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
                admins_changed = true;
            } else errors.add("admins");
        }
    }

    // Adding administrators (idempotent). Allowed to any administrator; the list is capped.
    if (!cfg["admins_add"].isNull()) {
        for (JsonVariantConst a : cfg["admins_add"].as<JsonArrayConst>()) {
            uint8_t h[16];
            if (!a.is<const char*>() || !remote_config_hex16(a.as<const char*>(), h)) { errors.add("admins_add: invalid hash"); continue; }
            if (remote_config_admin_index(h) >= 0) continue;   // already an administrator
            if (firewall_state.admin_count >= REMOTE_CONFIG_MAX_ADMINS) { errors.add("admins_add: list full"); continue; }
            memcpy(firewall_state.admin_hashes[firewall_state.admin_count], h, 16);
            firewall_state.admin_count++;
            admins_changed = true;
            NOTICE("Remote management: admin added " + remote_config_hex_short(h));
        }
    }

    // Removing administrators (idempotent). Never the super_admin (slot 0), never yourself.
    if (!cfg["admins_remove"].isNull()) {
        for (JsonVariantConst a : cfg["admins_remove"].as<JsonArrayConst>()) {
            uint8_t h[16];
            if (!a.is<const char*>() || !remote_config_hex16(a.as<const char*>(), h)) { errors.add("admins_remove: invalid hash"); continue; }
            int idx = remote_config_admin_index(h);
            if (idx < 0) continue;   // not an administrator
            if (idx == 0) { errors.add("admins_remove: super_admin can not be removed remotely"); continue; }
            if (caller != nullptr && memcmp(caller, h, 16) == 0) { errors.add("admins_remove: can not remove yourself"); continue; }
            for (int i = idx; i < (int)firewall_state.admin_count - 1; i++) memcpy(firewall_state.admin_hashes[i], firewall_state.admin_hashes[i + 1], 16);
            memset(firewall_state.admin_hashes[firewall_state.admin_count - 1], 0, 16);
            firewall_state.admin_count--;
            admins_changed = true;
            NOTICE("Remote management: admin removed " + remote_config_hex_short(h));
        }
    }
    if (admins_changed) { applied.add("admins"); any = true; }

    if (any) firewall_save_config();   // writes the firewall state and EEPROM.commit()
    else EEPROM.commit();
    if (admins_changed) remote_config_admins_changed();   // effective immediately, no reboot
    report["ok"] = errors.size() == 0;
}


// ── USB provisioning channel: receive buffer ────────────────────────────────────────────────────
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


#endif // FIREWALL_MODE && REMOTE_CONFIG
#endif // REMOTE_CONFIG_CORE_H
