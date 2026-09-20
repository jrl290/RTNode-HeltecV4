// Copyright (C) 2026, Firewall Mode Extension
// Based on microReticulum_Firmware by Mark Qvist
//
// FirewallConfig.h — Captive-portal web configuration for the legacy
// "Firewall Mode" path.
// together with the rest of the boundary-mode terminology. In this fork,
// Firewall Mode is the only intended mode of operation in this fork.
// When triggered (first boot with no config, or button hold >5s),
// the device starts a WiFi AP with a web form for all settings:
//   WiFi STA credentials, TCP backbone params, LoRa radio params,
//   and optional AP-mode TCP server.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef FIREWALL_CONFIG_H
#define FIREWALL_CONFIG_H

#ifdef FIREWALL_MODE

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "MdnsService.h"

// ─── Node hash (cached in RTC by normal boot, read here without starting RNS) ─
#define NODE_HASH_RTC_MAGIC  0x504B4841UL
extern uint32_t rtc_node_hash_magic;
extern char     rtc_node_hash_hex[33];
extern char     rtc_probe_hash_hex[21];

#define BOUNDARY_RESET_REPORT_MAGIC    0x42525054UL
#define BOUNDARY_RESET_REPORT_VERSION  1

enum BoundaryResetCause : uint8_t {
    BOUNDARY_RESET_CAUSE_NONE = 0,
    BOUNDARY_RESET_CAUSE_HEAP_WATCHDOG = 1,
    BOUNDARY_RESET_CAUSE_WIFI_WATCHDOG = 2,
};

enum BoundaryHeapPressureStage : uint8_t {
    BOUNDARY_HEAP_STAGE_NONE = 0,
    BOUNDARY_HEAP_STAGE_SHED = 1,
    BOUNDARY_HEAP_STAGE_TRIM = 2,
};

struct BoundaryResetReport {
    uint32_t magic;
    uint8_t version;
    uint8_t cause;
    uint8_t heap_stage;
    uint8_t observed_reset_reason;
    uint32_t uptime_ms;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t max_alloc_heap;
    int32_t wifi_status;
    uint16_t path_table_maxsize;
    uint16_t path_table_maxpersist;
    uint32_t bridged_lora_to_tcp;
    uint32_t bridged_tcp_to_lora;
};

extern BoundaryResetReport boundary_reset_report;
bool boundary_reset_report_available();
const char* boundary_reset_cause_label(uint8_t cause);
const char* boundary_heap_stage_label(uint8_t stage);
const char* boundary_reset_reason_label(uint8_t reason);

// ─── Config Portal State ─────────────────────────────────────────────────────
static bool config_portal_active = false;
static WebServer* config_server = nullptr;
static DNSServer* config_dns    = nullptr;

static const char CONFIG_AP_SSID[] = "RTNode-Setup";
static const uint16_t DNS_PORT = 53;
static const uint16_t HTTP_PORT = 80;

// Forward declarations
void config_portal_start();
void config_portal_stop();
void config_portal_loop();
bool config_portal_is_active();
bool boundary_needs_config();

// ─── Common bandwidth values (Hz) ───────────────────────────────────────────
// These match Reticulum standard channel plans
// Stored in flash (PROGMEM) to save ~200 bytes of RAM
static const uint32_t BW_OPTIONS_HZ[] PROGMEM = {
    7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000,
};
static const char BW_LABEL_0[]  PROGMEM = "7.8 kHz";
static const char BW_LABEL_1[]  PROGMEM = "10.4 kHz";
static const char BW_LABEL_2[]  PROGMEM = "15.6 kHz";
static const char BW_LABEL_3[]  PROGMEM = "20.8 kHz";
static const char BW_LABEL_4[]  PROGMEM = "31.25 kHz";
static const char BW_LABEL_5[]  PROGMEM = "41.7 kHz";
static const char BW_LABEL_6[]  PROGMEM = "62.5 kHz";
static const char BW_LABEL_7[]  PROGMEM = "125 kHz";
static const char BW_LABEL_8[]  PROGMEM = "250 kHz";
static const char BW_LABEL_9[]  PROGMEM = "500 kHz";
static const char* const BW_OPTIONS_LABELS[] PROGMEM = {
    BW_LABEL_0, BW_LABEL_1, BW_LABEL_2, BW_LABEL_3, BW_LABEL_4,
    BW_LABEL_5, BW_LABEL_6, BW_LABEL_7, BW_LABEL_8, BW_LABEL_9,
};
static const int BW_OPTIONS_COUNT = sizeof(BW_OPTIONS_HZ) / sizeof(BW_OPTIONS_HZ[0]);

static uint8_t config_default_display_rotation() {
    #if BOARD_MODEL == BOARD_LORA32_V2_1 || BOARD_MODEL == BOARD_TBEAM || BOARD_MODEL == BOARD_RAK4631
    return 0;
    #elif BOARD_MODEL == BOARD_HELTEC32_V2 || BOARD_MODEL == BOARD_HELTEC32_V3 || BOARD_MODEL == BOARD_HELTEC32_V4 || BOARD_MODEL == BOARD_HELTEC_T114 || BOARD_MODEL == BOARD_TBEAM_S_V1
    return 1;
    #else
    return 3;
    #endif
}

// ─── HTML Page Generation ────────────────────────────────────────────────────

static void config_send_html() {
    // Read current values from EEPROM/globals for pre-population
    char cur_ssid[33] = "";
    char cur_psk[33]  = "";

    for (int i = 0; i < 32; i++) {
        cur_ssid[i] = EEPROM.read(config_addr(ADDR_CONF_SSID + i));
        if (cur_ssid[i] == (char)0xFF) cur_ssid[i] = '\0';
    }
    cur_ssid[32] = '\0';

    for (int i = 0; i < 32; i++) {
        cur_psk[i] = EEPROM.read(config_addr(ADDR_CONF_PSK + i));
        if (cur_psk[i] == (char)0xFF) cur_psk[i] = '\0';
    }
    cur_psk[32] = '\0';

    // Current LoRa values (from globals, which were loaded from EEPROM)
    uint32_t cur_freq = lora_freq;
    uint32_t cur_bw   = lora_bw;
    int cur_sf        = lora_sf;
    int cur_cr        = lora_cr;
    int cur_txp       = lora_txp;
    if (cur_txp == 0xFF) cur_txp = PA_MAX_OUTPUT;  // Default to board max

    // Default frequency if not set
    if (cur_freq == 0) cur_freq = 914875000;  // 914.875 MHz default
    if (cur_bw == 0)   cur_bw   = 125000;     // 125 kHz default
    if (cur_sf == 0)   cur_sf   = 10;         // SF10 default
    if (cur_cr < 5 || cur_cr > 8) cur_cr = 5; // CR 4/5 default

    // Build the HTML page
    String html = F(
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>RTNode Setup</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;margin:0;padding:16px;}"
        "h1{color:#e94560;font-size:1.4em;margin:0 0 8px;}"
        "h2{color:#0f3460;background:#e0e0e0;padding:6px 10px;margin:18px -10px 10px;font-size:1em;border-radius:4px;}"
        "form{max-width:480px;margin:0 auto;}"
        "label{display:block;margin:8px 0 2px;font-size:0.9em;color:#aaa;}"
        "input,select{width:100%;padding:8px;margin:2px 0 6px;box-sizing:border-box;"
        "background:#16213e;border:1px solid #0f3460;color:#e0e0e0;border-radius:4px;font-size:0.95em;}"
        "input:focus,select:focus{border-color:#e94560;outline:none;}"
        ".row{display:flex;gap:10px;}.row>div{flex:1;}"
        ".note{font-size:0.8em;color:#666;margin:2px 0 8px;}"
        "button{width:100%;padding:12px;margin:20px 0;background:#e94560;color:#fff;"
        "border:none;border-radius:4px;font-size:1.1em;cursor:pointer;}"
        "button:hover{background:#c73e54;}"
        ".ok{background:#16213e;padding:20px;border-radius:8px;text-align:center;}"
        ".ok h1{color:#0f0;}"
        ".node-hash{background:#0f1a30;border:1px solid #0f3460;border-radius:6px;"
        "padding:10px 14px;margin:0 0 16px;}"
        ".node-hash .nh-label{display:block;font-size:0.75em;color:#888;margin-bottom:4px;}"
        ".node-hash code{font-family:monospace;font-size:0.95em;color:#7ecfff;"
        "word-break:break-all;letter-spacing:0.05em;}"
        ".reset-report{background:#241d12;border:1px solid #6b4f1d;border-radius:6px;"
        "padding:12px 14px;margin:0 0 16px;}"
        ".reset-report .rr-title{display:block;font-size:0.8em;color:#f4c87a;margin-bottom:8px;}"
        ".reset-report .rr-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px 12px;}"
        ".reset-report .rr-key{font-size:0.78em;color:#b8a98b;}"
        ".reset-report .rr-val{font-family:monospace;font-size:0.9em;color:#f8e7c2;word-break:break-word;}"
        ".reset-report .rr-note{font-size:0.78em;color:#b8a98b;margin-top:10px;}"
        "</style></head><body>"
        "<h1>&#x1f4e1; RTNode</h1>"
    );

    // ── Node public hash ──
    html += F("<div class='node-hash'><span class='nh-label'>&#x1f511; Node Hash (Reticulum destination)</span><code>");
    if (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0') {
        html += String(rtc_node_hash_hex);
    } else {
        html += F("<span style='color:#888;font-style:italic;'>Not yet assigned &mdash; will be set on first normal boot</span>");
    }
    html += F("</code></div>");

    if (boundary_reset_report_available()) {
        html += F("<div class='reset-report'><span class='rr-title'>&#x26a0; Last automatic reset report</span><div class='rr-grid'>");

        html += F("<div><div class='rr-key'>Trigger</div><div class='rr-val'>");
        html += String(boundary_reset_cause_label(boundary_reset_report.cause));
        html += F("</div></div>");

        html += F("<div><div class='rr-key'>Observed reset</div><div class='rr-val'>");
        html += String(boundary_reset_reason_label(boundary_reset_report.observed_reset_reason));
        if (boundary_reset_report.observed_reset_reason != 0) {
            html += F(" (");
            html += String((unsigned)boundary_reset_report.observed_reset_reason);
            html += F(")");
        }
        html += F("</div></div>");

        html += F("<div><div class='rr-key'>Heap stage</div><div class='rr-val'>");
        html += String(boundary_heap_stage_label(boundary_reset_report.heap_stage));
        html += F("</div></div>");

        html += F("<div><div class='rr-key'>Uptime at reset</div><div class='rr-val'>");
        html += String(boundary_reset_report.uptime_ms / 1000UL);
        html += F(" s</div></div>");

        html += F("<div><div class='rr-key'>Free heap</div><div class='rr-val'>");
        html += String(boundary_reset_report.free_heap);
        html += F(" B</div></div>");

        html += F("<div><div class='rr-key'>Min free heap</div><div class='rr-val'>");
        html += String(boundary_reset_report.min_free_heap);
        html += F(" B</div></div>");

        html += F("<div><div class='rr-key'>Max alloc heap</div><div class='rr-val'>");
        html += String(boundary_reset_report.max_alloc_heap);
        html += F(" B</div></div>");

        html += F("<div><div class='rr-key'>WiFi status</div><div class='rr-val'>");
        html += String(boundary_reset_report.wifi_status);
        html += F("</div></div>");

        html += F("<div><div class='rr-key'>Path caps</div><div class='rr-val'>");
        html += String(boundary_reset_report.path_table_maxsize);
        html += F("/");
        html += String(boundary_reset_report.path_table_maxpersist);
        html += F("</div></div>");

        html += F("<div><div class='rr-key'>Bridge counters</div><div class='rr-val'>L-&gt;T ");
        html += String(boundary_reset_report.bridged_lora_to_tcp);
        html += F(" / T-&gt;L ");
        html += String(boundary_reset_report.bridged_tcp_to_lora);
        html += F("</div></div>");

        html += F("</div><div class='rr-note'>Captured before an automatic reboot. Clears on power cycle.</div></div>");
    }

    html += F("<form method='POST' action='/save'>");

    // ── Node Name Section ──
    html += F(
        "<h2>&#x1f3f7; Node Name</h2>"
        "<p class='note'>A human-readable name for this node, shown in advertisements and on maps "
        "such as <a href='https://rmap.world' target='_blank' style='color:#7ecfff'>rmap.world</a>. "
        "Leave blank to auto-generate a name from the node hash.</p>"
        "<label>Name</label>"
        "<input name='node_name' maxlength='32' placeholder='e.g. My RNode' value='"
    );
    html += String(firewall_state.node_name);
    html += F("'>");

    html += F(
        "<h2>&#x1f4f6; WiFi Network</h2>"
        "<label>WiFi</label>"
        "<select name='wifi_en'>"
    );
    html += F("<option value='1'");
    if (firewall_state.wifi_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("<option value='0'");
    if (!firewall_state.wifi_enabled) html += F(" selected");
    html += F(">Disabled (LoRa-only repeater)</option>");
    html += F("</select>");

    html += F(
        "<label>SSID</label>"
        "<input name='ssid' maxlength='32' placeholder='Your WiFi network' value='"
    );
    html += String(cur_ssid);
    html += F(
        "'>"
        "<label>Password</label>"
        "<input name='psk' type='password' maxlength='32' placeholder='WiFi password' value='"
    );
    html += String(cur_psk);
    html += F("'>");

    // ── TCP Backbone Section ──
    html += F(
        "<h2>&#x1f310; TCP Backbones</h2>"
        "<p class='note'>Configure up to four independent TCP backbone uplinks. "
        "Each enabled slot becomes its own boundary interface. Leave host blank to disable a slot.</p>"
    );
    for (size_t slot = 0; slot < FIREWALL_BACKBONE_SLOTS; slot++) {
        html += F("<div class='card'>");
        html += F("<label>");
        html += String("Backbone ") + String((int)slot + 1);
        html += F("</label>");

        html += F("<select name='");
        html += String("bb") + String((int)slot) + F("_en'>");
        html += F("<option value='0'");
        if (!firewall_state.backbones[slot].enabled) html += F(" selected");
        html += F(">Disabled</option>");
        html += F("<option value='1'");
        if (firewall_state.backbones[slot].enabled) html += F(" selected");
        html += F(">Enabled</option></select>");

        html += F("<label>Host</label><input name='");
        html += String("bb") + String((int)slot) + F("_host' maxlength='63' placeholder='e.g. rns.example.net' value='");
        html += String(firewall_state.backbones[slot].host);
        html += F("'>");

        html += F("<label>Port</label><input name='");
        html += String("bb") + String((int)slot) + F("_port' type='number' min='1' max='65535' value='");
        html += String(firewall_state.backbones[slot].port);
        html += F("'>");
        html += F("</div>");
    }

    // ── Local TCP Server Section ──
    html += F(
        "<h2>&#x1f4e1; Local TCP Server (optional)</h2>"
        "<p class='note'>Run a TCP server on the same WiFi network so local devices can connect. "
        "Uses Gateway mode (forwards announces to and from local TCP clients). "
        "Connect endpoint clients only; disable Reticulum transport mode on Meshchat or other LAN clients.</p>"
        "<label>Local TCP Server</label>"
        "<select name='ap_tcp_en'>"
    );
    html += F("<option value='0'");
    if (!firewall_state.ap_tcp_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (firewall_state.ap_tcp_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("</select>");

    html += F("<label>TCP Port</label>");
    html += F("<input name='ap_tcp_port' type='number' min='1' max='65535' value='");
    html += String(firewall_state.ap_tcp_port);
    html += F("'>");

    html += F(
        "<label>Local Network Name (mDNS)</label>"
        "<p class='note'>Publishes the device on the local network so you can reach it as "
        "<code>&lt;name&gt;.local</code> from any computer in your LAN without knowing its IP. "
        "This applies to the node itself and local TCP access, even if the TCP server is disabled.</p>"
        "<select name='mdns_en'>"
    );
    html += F("<option value='1'");
    if (firewall_state.mdns_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("<option value='0'");
    if (!firewall_state.mdns_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("</select>");

    html += F(
        "<label>mDNS Hostname</label>"
        "<p class='note'>Leave blank for the default <code>rtnode&lt;XXXX&gt;.local</code> "
        "(last 4 hex chars of the device MAC). "
        "Allowed: lowercase letters, digits and hyphens; first/last char must be alphanumeric.</p>"
        "<input name='mdns_name' maxlength='32' placeholder='rtnode' value='"
    );
    html += String(firewall_state.mdns_hostname);
    html += F("'>");

    // ── Diagnostics / rnprobe Section ──
    html += F(
        "<h2>&#x1f50d; Diagnostics (rnprobe)</h2>"
        "<p class='note'>When enabled, this node responds to <code>rnprobe</code> utility pings "
        "from other Reticulum nodes.  rnprobe measures round-trip time and packet loss over "
        "the network path.  The probe destination is identity-backed and requires no link setup.</p>"
        "<label>rnprobe Responder</label>"
        "<select name='probe_en'>"
    );
    html += F("<option value='1'");
    if (firewall_state.probe_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("<option value='0'");
    if (!firewall_state.probe_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("</select>");

    html += F("<div class='node-hash' style='margin-top:8px;'><span class='nh-label'>&#x1f4e8; Probe Destination</span><code>");
    if (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_probe_hash_hex[0] != '\0') {
        html += String(rtc_probe_hash_hex);
    } else {
        html += F("<span style='color:#888;font-style:italic;'>Available after first normal boot</span>");
    }
    html += F("</code></div>");
    html += F("<p class='note'>Run: <code>rnprobe rnstransport.probe &lt;hash&gt;</code> "
              "from any reachable Reticulum node.  The hash is also printed in the serial log "
              "as <code>PROBE-DST</code> at boot.</p>");

#ifdef REMOTE_CONFIG
    // ── Remote management Section ──
    html += F(
        "<h2>&#x1f510; Remote Management</h2>"
        "<p class='note'>Reticulum identities allowed to read and change this node's settings over an "
        "encrypted Link (destination <code>rtnode.config</code>).  One 32-hex-character identity hash per "
        "line, up to 4.  The <b>first line is the super_admin</b>: it can never be removed over the network; "
        "other admins can add or remove admins but not the super_admin.  Leave empty to disable remote "
        "management.  Only identity hashes of people you trust: an admin can rewrite every setting of this node.</p>"
        "<label>Admin identity hashes</label>"
        "<textarea name='admin_ids' rows='4' spellcheck='false' autocomplete='off' "
        "style='width:100%;font-family:monospace;'>"
    );
    for (uint8_t a = 0; a < firewall_state.admin_count && a < REMOTE_CONFIG_MAX_ADMINS; a++) {
        char hx[33];
        for (uint8_t i = 0; i < 16; i++) snprintf(hx + i * 2, 3, "%02x", firewall_state.admin_hashes[a][i]);
        html += String(hx);
        html += F("\n");
    }
    html += F("</textarea>");
#endif

    // ── LoRa Radio Section ──
    html += F(
        "<h2>&#x1f4fb; LoRa Radio</h2>"
    );

    // Frequency — show in MHz for human-friendliness
    char freq_str[16];
    dtostrf((double)cur_freq / 1000000.0, 1, 3, freq_str);
    html += F("<label>Frequency (MHz)</label>");
    html += F("<input name='freq' type='text' placeholder='914.875' value='");
    html += String(freq_str);
    html += F("'>");
    html += F("<p class='note'>e.g. 914.875, 868.000, 433.000</p>");

    // Bandwidth — dropdown
    html += F("<label>Bandwidth</label><select name='bw'>");
    for (int i = 0; i < BW_OPTIONS_COUNT; i++) {
        uint32_t bw_hz = pgm_read_dword(&BW_OPTIONS_HZ[i]);
        char label_buf[16];
        strncpy_P(label_buf, (const char*)pgm_read_ptr(&BW_OPTIONS_LABELS[i]), sizeof(label_buf)-1);
        label_buf[sizeof(label_buf)-1] = '\0';
        html += F("<option value='");
        html += String(bw_hz);
        html += "'";
        if (bw_hz == cur_bw) html += F(" selected");
        html += ">";
        html += label_buf;
        html += F("</option>");
    }
    html += F("</select>");

    // Spreading Factor — dropdown 5-12
    html += F("<label>Spreading Factor</label><select name='sf'>");
    for (int sf = 5; sf <= 12; sf++) {
        html += F("<option value='");
        html += String(sf);
        html += "'";
        if (sf == cur_sf) html += F(" selected");
        html += ">SF";
        html += String(sf);
        html += F("</option>");
    }
    html += F("</select>");

    // Coding Rate — dropdown 5-8 (maps to 4/5 through 4/8)
    html += F("<label>Coding Rate</label><select name='cr'>");
    for (int cr = 5; cr <= 8; cr++) {
        html += F("<option value='");
        html += String(cr);
        html += "'";
        if (cr == cur_cr) html += F(" selected");
        html += ">4/";
        html += String(cr);
        html += F("</option>");
    }
    html += F("</select>");

    // TX Power
    html += F("<label>TX Power (dBm)</label>");
    html += F("<input name='txp' type='number' min='2' max='");
    #ifdef PA_MAX_OUTPUT
    html += String(PA_MAX_OUTPUT);
    #else
    html += "22";
    #endif
    html += F("' value='");
    html += String(cur_txp);
    html += F("'>");

    #ifdef PA_MAX_OUTPUT
    html += F("<p class='note'>Max output for this board: ");
    html += String(PA_MAX_OUTPUT);
    html += F(" dBm (with PA)</p>");
    #endif

    // Airtime / duty-cycle limits — pause TX when measured airtime
    // exceeds these thresholds. 0 = disabled. Range 0–25% with 0.1%
    // resolution. Common preset: EU868 = 1% on the 1hr limit.
    char st_al_str[16];
    char lt_al_str[16];
    dtostrf(firewall_state.st_airtime_limit * 100.0f, 1, 1, st_al_str);
    dtostrf(firewall_state.lt_airtime_limit * 100.0f, 1, 1, lt_al_str);
    html += F("<div class='row'>");
    html += F("<div><label>15s limit (%)</label>"
              "<input name='stal' type='number' step='0.1' min='0' max='25' value='");
    html += String(st_al_str);
    html += F("'></div>");
    html += F("<div><label>1hr limit (%)</label>"
              "<input name='ltal' type='number' step='0.1' min='0' max='25' value='");
    html += String(lt_al_str);
    html += F("'></div>");
    html += F("</div>");
    html += F("<p class='note'>Pause TX when measured LoRa airtime exceeds these limits. "
              "0 = disabled. EU868 regulations suggest 1% on the 1hr limit.</p>");

    // ── IFAC (Interface Access Code) Section ──
    html += F(
        "<h2>&#x1f512; Network Access (IFAC)</h2>"
        "<p class='note'>Set a network name and/or passphrase to restrict LoRa interface access. "
        "Only nodes with matching settings can communicate. Both fields are optional.</p>"
        "<label>IFAC</label>"
        "<select name='ifac_en'>"
    );
    html += F("<option value='0'");
    if (!firewall_state.ifac_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (firewall_state.ifac_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("</select>");

    html += F("<label>Network Name</label>");
    html += F("<input name='ifac_name' maxlength='32' placeholder='e.g. MyNetwork' value='");
    html += String(firewall_state.ifac_netname);
    html += F("'>");

    html += F("<label>Passphrase</label>");
    html += F("<input name='ifac_pass' type='password' maxlength='32' placeholder='Shared secret' value='");
    html += String(firewall_state.ifac_passphrase);
    html += F("'>");

    // ── Device Advertisement Section ──
    html += F(
        "<h2>&#x1f4cd; Device Advertisement</h2>"
        "<p class='note'>Advertise this node and its parameters on the Reticulum network. "
        "Maps such as <a href='https://rmap.world' target='_blank' style='color:#7ecfff'>rmap.world</a> "
        "use these announcements to automatically place a pin for the node. "
        "Disabled by default; enable only if you want this node to be publicly listed.</p>"
        "<label>Advertise Device</label>"
        "<select name='advert_en'>"
    );
    html += F("<option value='0'");
    if (!firewall_state.advert_enabled) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (firewall_state.advert_enabled) html += F(" selected");
    html += F(">Enabled</option>");
    html += F("</select>");

    // Latitude / Longitude — pre-populate with current values, but keep
    // the inputs blank when the user has not yet set coordinates so the
    // browser placeholder hint is visible.
    char lat_str[32];
    char lon_str[32];
    lat_str[0] = '\0';
    lon_str[0] = '\0';
    if (firewall_state.advert_enabled ||
        firewall_state.advert_lat != 0.0 ||
        firewall_state.advert_lon != 0.0) {
        dtostrf(firewall_state.advert_lat, 1, 6, lat_str);
        dtostrf(firewall_state.advert_lon, 1, 6, lon_str);
    }

    html += F("<div class='row'>");
    html += F("<div><label>Latitude (&deg;)</label>");
    html += F("<input id='advert_lat' name='advert_lat' type='text' inputmode='decimal' "
              "placeholder='e.g. 37.774929' value='");
    html += String(lat_str);
    html += F("'></div>");
    html += F("<div><label>Longitude (&deg;)</label>");
    html += F("<input id='advert_lon' name='advert_lon' type='text' inputmode='decimal' "
              "placeholder='e.g. -122.419416' value='");
    html += String(lon_str);
    html += F("'></div>");
    html += F("</div>");
    html += F("<p class='note'>Decimal degrees, signed. North/East positive, South/West negative. "
              "Leave both blank to omit coordinates.</p>");

    html += F("<label>Randomize Offset</label>"
              "<select name='advert_jitter'>");
    html += F("<option value='0'");
    if (!firewall_state.advert_jitter) html += F(" selected");
    html += F(">Disabled</option>");
    html += F("<option value='1'");
    if (firewall_state.advert_jitter) html += F(" selected");
    html += F(">Enabled (~0.5 km / 0.5 mi)</option>");
    html += F("</select>");
    html += F("<p class='note'>When enabled, the advertised coordinates are shifted by a "
              "random offset of approximately half a kilometre (about half a mile) for "
              "privacy. The exact stored coordinates are not changed.</p>");

    // ── Options Section ──
    html += F(
        "<h2>&#x2699; Options</h2>"
        "<label>Display Blanking</label>"
        "<select name='disp_blank'>"
    );

    // Read current blanking timeout from EEPROM (stored as minutes, 0 = never)
    uint8_t cur_blank = 5;
    if (EEPROM.read(eeprom_addr(ADDR_CONF_BSET)) == CONF_OK_BYTE) {
        cur_blank = EEPROM.read(eeprom_addr(ADDR_CONF_DBLK));
    }

    uint8_t cur_rotation = EEPROM.read(eeprom_addr(ADDR_CONF_DROT));
    if (cur_rotation > 3) {
        cur_rotation = config_default_display_rotation();
    }

    static const uint8_t blank_vals[]   = { 0, 1, 5, 10, 30, 60 };
    static const char* blank_labels[]   = { "Never", "1 minute", "5 minutes", "10 minutes", "30 minutes", "60 minutes" };
    static const int blank_count = 6;

    for (int i = 0; i < blank_count; i++) {
        html += F("<option value='");
        html += String(blank_vals[i]);
        html += "'";
        if (blank_vals[i] == cur_blank) html += F(" selected");
        html += ">";
        html += blank_labels[i];
        html += F("</option>");
    }
    html += F("</select>");
    html += F("<p class='note'>Turn off display after inactivity to save power</p>");

    html += F("<label>Display Orientation</label><select name='disp_rot'>");
    html += F("<option value='0'");
    if (cur_rotation == 0) html += F(" selected");
    html += F(">Landscape</option>");
    html += F("<option value='1'");
    if (cur_rotation == 1) html += F(" selected");
    html += F(">Portrait</option>");
    html += F("<option value='2'");
    if (cur_rotation == 2) html += F(" selected");
    html += F(">Landscape Flipped</option>");
    html += F("<option value='3'");
    if (cur_rotation == 3) html += F(" selected");
    html += F(">Portrait Flipped</option>");
    html += F("</select>");
    html += F("<p class='note'>Choose the orientation that matches your OLED mounting. "
              "Landscape modes place the two status panes side by side; portrait modes stack them.</p>");

    // ── Submit ──
    html += F(
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form></body></html>"
    );

    config_server->send(200, "text/html", html);
}

// ─── Handle POST /save ──────────────────────────────────────────────────────

static void config_handle_save() {
    // ── WiFi STA credentials ──
    String ssid = config_server->arg("ssid");
    String psk  = config_server->arg("psk");

    // Write SSID to config EEPROM area
    for (int i = 0; i < 32; i++) {
        uint8_t c = (i < (int)ssid.length()) ? ssid[i] : 0x00;
        EEPROM.write(config_addr(ADDR_CONF_SSID + i), c);
    }
    EEPROM.write(config_addr(ADDR_CONF_SSID + 32), 0x00);

    // Write PSK
    for (int i = 0; i < 32; i++) {
        uint8_t c = (i < (int)psk.length()) ? psk[i] : 0x00;
        EEPROM.write(config_addr(ADDR_CONF_PSK + i), c);
    }
    EEPROM.write(config_addr(ADDR_CONF_PSK + 32), 0x00);

    // Set WiFi mode to STA
    EEPROM.write(eeprom_addr(ADDR_CONF_WIFI), WR_WIFI_STA);

    // Firewall mode always uses DHCP on the STA interface. Clear the legacy
    // static IP and netmask slots so stale values from older firmware or tools
    // cannot force a persistent static address.
    for (int i = 0; i < 4; i++) {
        EEPROM.write(config_addr(ADDR_CONF_IP + i), 0x00);
        EEPROM.write(config_addr(ADDR_CONF_NM + i), 0x00);
    }

    // ── WiFi enable setting ──
    firewall_state.wifi_enabled = (config_server->arg("wifi_en").toInt() == 1);

    // ── Display blanking (EEPROM stores minutes, 0 = disabled) ──
    int blank_minutes = config_server->arg("disp_blank").toInt();
    if (blank_minutes <= 0) {
        display_blanking_enabled = false;
        eeprom_update(eeprom_addr(ADDR_CONF_BSET), CONF_OK_BYTE);
        eeprom_update(eeprom_addr(ADDR_CONF_DBLK), 0);
    } else {
        uint8_t blank_val = (uint8_t)(blank_minutes > 255 ? 255 : blank_minutes);
        display_blanking_enabled = true;
        display_blanking_timeout = (uint32_t)blank_val * 60UL * 1000UL;
        eeprom_update(eeprom_addr(ADDR_CONF_BSET), CONF_OK_BYTE);
        eeprom_update(eeprom_addr(ADDR_CONF_DBLK), blank_val);
    }

    int display_rotation = config_server->arg("disp_rot").toInt();
    if (display_rotation < 0 || display_rotation > 3) {
        display_rotation = config_default_display_rotation();
    }
    eeprom_update(eeprom_addr(ADDR_CONF_DROT), (uint8_t)display_rotation);

    // ── TCP backbone settings ──
    for (size_t slot = 0; slot < FIREWALL_BACKBONE_SLOTS; slot++) {
        String en_arg = String("bb") + String((int)slot) + String("_en");
        String host_arg_name = String("bb") + String((int)slot) + String("_host");
        String port_arg_name = String("bb") + String((int)slot) + String("_port");

        firewall_state.backbones[slot].enabled = (config_server->arg(en_arg).toInt() == 1);

        String bb_host = config_server->arg(host_arg_name);
        bb_host.trim();
        memset(firewall_state.backbones[slot].host, 0, sizeof(firewall_state.backbones[slot].host));
        strncpy(firewall_state.backbones[slot].host, bb_host.c_str(), sizeof(firewall_state.backbones[slot].host) - 1);

        firewall_state.backbones[slot].port = (uint16_t)config_server->arg(port_arg_name).toInt();
        if (firewall_state.backbones[slot].port == 0) firewall_state.backbones[slot].port = 4242;

        if (firewall_state.backbones[slot].host[0] == '\0') {
            firewall_state.backbones[slot].enabled = false;
        }
    }

    // ── Local TCP server settings ──
    firewall_state.ap_tcp_enabled = (config_server->arg("ap_tcp_en").toInt() == 1);
    firewall_state.ap_tcp_port = (uint16_t)config_server->arg("ap_tcp_port").toInt();
    if (firewall_state.ap_tcp_port == 0) firewall_state.ap_tcp_port = 4242;

    // ── IFAC settings ──
    firewall_state.ifac_enabled = (config_server->arg("ifac_en").toInt() == 1);

    String ifac_name = config_server->arg("ifac_name");
    memset(firewall_state.ifac_netname, 0, sizeof(firewall_state.ifac_netname));
    strncpy(firewall_state.ifac_netname, ifac_name.c_str(), sizeof(firewall_state.ifac_netname) - 1);

    String ifac_pass = config_server->arg("ifac_pass");
    memset(firewall_state.ifac_passphrase, 0, sizeof(firewall_state.ifac_passphrase));
    strncpy(firewall_state.ifac_passphrase, ifac_pass.c_str(), sizeof(firewall_state.ifac_passphrase) - 1);

    // If IFAC is enabled but both fields are empty, disable it
    if (firewall_state.ifac_enabled &&
        firewall_state.ifac_netname[0] == '\0' &&
        firewall_state.ifac_passphrase[0] == '\0') {
        firewall_state.ifac_enabled = false;
    }

    // ── Device advertisement settings ──
    firewall_state.advert_enabled = (config_server->arg("advert_en").toInt() == 1);

    // Empty lat/lon strings are treated as "not set" → 0.0. Otherwise parse
    // and clamp to valid ranges; out-of-range values are silently coerced
    // to 0.0 rather than rejecting the whole save.
    String lat_arg = config_server->arg("advert_lat");
    String lon_arg = config_server->arg("advert_lon");
    lat_arg.trim();
    lon_arg.trim();
    if (lat_arg.length() == 0) {
        firewall_state.advert_lat = 0.0;
    } else {
        double lat_val = lat_arg.toDouble();
        if (lat_val < -90.0 || lat_val > 90.0 || isnan(lat_val)) {
            lat_val = 0.0;
        }
        firewall_state.advert_lat = lat_val;
    }
    if (lon_arg.length() == 0) {
        firewall_state.advert_lon = 0.0;
    } else {
        double lon_val = lon_arg.toDouble();
        if (lon_val < -180.0 || lon_val > 180.0 || isnan(lon_val)) {
            lon_val = 0.0;
        }
        firewall_state.advert_lon = lon_val;
    }

    firewall_state.advert_jitter = (config_server->arg("advert_jitter").toInt() == 1);

    // ── Node name ──
    String node_name_arg = config_server->arg("node_name");
    node_name_arg.trim();
    memset(firewall_state.node_name, 0, sizeof(firewall_state.node_name));
    strncpy(firewall_state.node_name, node_name_arg.c_str(), sizeof(firewall_state.node_name) - 1);

    // ── mDNS enable + hostname ──
    firewall_state.mdns_enabled = (config_server->arg("mdns_en").toInt() != 0);
    // Lowercase, strip whitespace, allow only [a-z0-9-]; reject leading/trailing
    // hyphens.  Empty input falls back to the auto-generated `rtnode<XXXX>` at
    // mDNS start time.
    {
        String mdns_arg = config_server->arg("mdns_name");
        mdns_arg.trim();
        mdns_arg.toLowerCase();
        char clean[33];
        size_t j = 0;
        for (size_t i = 0; i < (size_t)mdns_arg.length() && j < sizeof(clean) - 1; i++) {
            char c = mdns_arg.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
                clean[j++] = c;
            }
        }
        clean[j] = '\0';
        // Strip leading/trailing hyphens to keep RFC-952-ish compliance.
        while (j > 0 && clean[j - 1] == '-') clean[--j] = '\0';
        size_t start = 0;
        while (clean[start] == '-') start++;
        memset(firewall_state.mdns_hostname, 0, sizeof(firewall_state.mdns_hostname));
        if (clean[start] != '\0') {
            strncpy(firewall_state.mdns_hostname, clean + start,
                    sizeof(firewall_state.mdns_hostname) - 1);
        }
    }

    // ── rnprobe responder ──
    firewall_state.probe_enabled = (config_server->arg("probe_en").toInt() == 1);

#ifdef REMOTE_CONFIG
    // ── Remote management admin identities ──
    // Accepts 32-hex-character tokens separated by whitespace or commas; anything else is ignored.
    if (config_server->hasArg("admin_ids")) {
        String ids = config_server->arg("admin_ids");
        uint8_t count = 0;
        memset(firewall_state.admin_hashes, 0, sizeof(firewall_state.admin_hashes));
        int pos = 0, len = (int)ids.length();
        while (pos < len && count < REMOTE_CONFIG_MAX_ADMINS) {
            while (pos < len && !isxdigit((unsigned char)ids[pos])) pos++;
            int start = pos;
            while (pos < len && isxdigit((unsigned char)ids[pos])) pos++;
            if (pos - start == 32) {
                for (uint8_t i = 0; i < 16; i++) {
                    char two[3] = { ids[start + i * 2], ids[start + i * 2 + 1], 0 };
                    firewall_state.admin_hashes[count][i] = (uint8_t)strtoul(two, nullptr, 16);
                }
                count++;
            }
        }
        firewall_state.admin_count = count;
    }
#endif

    // Save boundary config to EEPROM
    firewall_save_config();

    // ── LoRa radio settings ──
    // Parse frequency as integer Hz from a MHz decimal string (e.g. "869.4625"
    // or "869,4625" → 869462500) without floating-point loss.
    String freq_str = config_server->arg("freq");
    if (freq_str.length() > 0) {
        // Accept both '.' and ',' as decimal separator (locale-independent)
        freq_str.replace(',', '.');
        uint32_t freq_hz = 0;
        int dot = freq_str.indexOf('.');
        if (dot >= 0) {
            // MHz integer part
            freq_hz = (uint32_t)freq_str.substring(0, dot).toInt() * 1000000UL;
            // Fractional part: pad/truncate to exactly 6 digits (Hz resolution)
            String frac = freq_str.substring(dot + 1);
            while (frac.length() < 6) frac += '0';
            if (frac.length() > 6) frac = frac.substring(0, 6);
            freq_hz += (uint32_t)frac.toInt();
        } else {
            // No decimal point — treat as integer Hz
            freq_hz = (uint32_t)freq_str.toInt();
        }
        if (freq_hz > 0) lora_freq = freq_hz;
    }

    String bw_str = config_server->arg("bw");
    uint32_t bw_val = (uint32_t)bw_str.toInt();
    if (bw_val > 0) lora_bw = bw_val;

    int sf_val = config_server->arg("sf").toInt();
    if (sf_val >= 5 && sf_val <= 12) lora_sf = sf_val;

    int cr_val = config_server->arg("cr").toInt();
    if (cr_val >= 5 && cr_val <= 8) lora_cr = cr_val;

    int txp_val = config_server->arg("txp").toInt();
    if (txp_val >= 2 && txp_val <= 30) lora_txp = txp_val;

    // Airtime / duty-cycle limits. Empty / out-of-range / 0 = disabled.
    // firewall_save_config() has already run above, so write the bytes
    // directly here alongside the other LoRa parameters.
    {
        String stal_arg = config_server->arg("stal");
        String ltal_arg = config_server->arg("ltal");
        stal_arg.trim();
        ltal_arg.trim();
        float st_pct = stal_arg.length() ? (float)stal_arg.toFloat() : 0.0f;
        float lt_pct = ltal_arg.length() ? (float)ltal_arg.toFloat() : 0.0f;
        if (st_pct < 0.0f || isnan(st_pct)) st_pct = 0.0f;
        if (lt_pct < 0.0f || isnan(lt_pct)) lt_pct = 0.0f;
        if (st_pct > 25.0f) st_pct = 25.0f;
        if (lt_pct > 25.0f) lt_pct = 25.0f;
        firewall_state.st_airtime_limit = st_pct / 100.0f;
        firewall_state.lt_airtime_limit = lt_pct / 100.0f;
        uint8_t st_byte = (uint8_t)(st_pct * 10.0f + 0.5f);
        uint8_t lt_byte = (uint8_t)(lt_pct * 10.0f + 0.5f);
        EEPROM.write(config_addr(ADDR_CONF_ST_AL), st_byte);
        EEPROM.write(config_addr(ADDR_CONF_LT_AL), lt_byte);
    }

    // Save LoRa config to EEPROM (reuse existing eeprom_conf functions)
    // Write directly since hw_ready may not be set yet
    eeprom_update(eeprom_addr(ADDR_CONF_SF), lora_sf);
    eeprom_update(eeprom_addr(ADDR_CONF_CR), lora_cr);
    eeprom_update(eeprom_addr(ADDR_CONF_TXP), lora_txp);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 0, lora_bw >> 24);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 1, lora_bw >> 16);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 2, lora_bw >> 8);
    eeprom_update(eeprom_addr(ADDR_CONF_BW) + 3, lora_bw);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 0, lora_freq >> 24);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 1, lora_freq >> 16);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 2, lora_freq >> 8);
    eeprom_update(eeprom_addr(ADDR_CONF_FREQ) + 3, lora_freq);
    eeprom_update(eeprom_addr(ADDR_CONF_OK), CONF_OK_BYTE);

    EEPROM.commit();

    // ── Send confirmation page ──
    String ok = F(
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;padding:40px;"
        "display:flex;align-items:center;justify-content:center;min-height:80vh;}"
        ".ok{background:#16213e;padding:30px;border-radius:12px;text-align:center;max-width:400px;}"
        "h1{color:#4caf50;margin-bottom:16px;}"
        "p{color:#aaa;}"
        "</style></head><body>"
        "<div class='ok'>"
        "<h1>&#x2705; Configuration Saved</h1>"
        "<p>Device will reboot in 3 seconds and connect to your WiFi network.</p>"
        "<p style='color:#666;font-size:0.85em;'>If the device cannot connect, hold the button for 5+ seconds to re-enter setup.</p>"
        "</div></body></html>"
    );
    config_server->send(200, "text/html", ok);

    // Give the response time to send
    delay(3000);

    // Reboot
    ESP.restart();
}

// ─── Captive Portal redirect ─────────────────────────────────────────────────
static void config_handle_redirect() {
    config_server->sendHeader("Location", "http://10.0.0.1/", true);
    config_server->send(302, "text/plain", "Redirecting to setup...");
}

// ─── Check if config is needed ───────────────────────────────────────────────
bool boundary_needs_config() {
    // If the RTNode app marker is missing, this node was either never
    // configured by RTNode or was flashed from a different firmware family
    // such as stock RNode. Force the portal so RTNode can claim and rewrite
    // its persisted settings explicitly.
    if (!firewall_app_marker_valid()) {
        return true;
    }

    // Check if WiFi SSID is configured
    char ssid[33];
    for (int i = 0; i < 32; i++) {
        ssid[i] = EEPROM.read(config_addr(ADDR_CONF_SSID + i));
        if (ssid[i] == (char)0xFF) ssid[i] = '\0';
    }
    ssid[32] = '\0';

    // Also check firewall mode enable flag
    uint8_t bmode = EEPROM.read(config_addr(ADDR_CONF_BMODE));

    // Need config if no SSID set and boundary not yet configured
    if (ssid[0] == '\0' && bmode != FIREWALL_ENABLE_BYTE) {
        return true;
    }
    return false;
}

// ─── Start Config Portal ─────────────────────────────────────────────────────
void config_portal_start() {
    if (config_portal_active) return;

    Serial.println("[Config] Starting configuration portal...");

    // Tear down any STA-mode mDNS before flipping the radio to AP — the
    // ESPmDNS state must not survive a WiFi mode change.
    mdns_service::stop();

    // Stop any existing WiFi
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_MODE_NULL);
    delay(100);

    // Start AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CONFIG_AP_SSID, NULL);  // Open AP for easy setup
    delay(150);

    IPAddress ap_addr(10, 0, 0, 1);
    IPAddress ap_mask(255, 255, 255, 0);
    WiFi.softAPConfig(ap_addr, ap_addr, ap_mask);

    Serial.print("[Config] AP started: ");
    Serial.println(CONFIG_AP_SSID);
    Serial.print("[Config] IP: ");
    Serial.println(WiFi.softAPIP());

    // Start DNS server for captive portal (redirect all domains to us)
    config_dns = new DNSServer();
    config_dns->start(DNS_PORT, "*", ap_addr);

    // Start web server
    config_server = new WebServer(HTTP_PORT);
    config_server->on("/", HTTP_GET, config_send_html);
    config_server->on("/save", HTTP_POST, config_handle_save);
    config_server->onNotFound(config_handle_redirect);  // Captive portal catch-all
    config_server->begin();

    // Publish a stable .local name so users don't need to remember the AP IP.
    // Captive portal still works via DNSServer for clients without mDNS.
    if (firewall_state.mdns_enabled) {
        mdns_service::start_ap_config("rtnode");
    }

    config_portal_active = true;

    Serial.println("[Config] Portal ready — connect to WiFi: " + String(CONFIG_AP_SSID));

    #if HAS_DISPLAY
    if (disp_ready) {
        // Show config mode on display
        stat_area.fillScreen(SSD1306_BLACK);
        stat_area.setTextColor(SSD1306_WHITE);
        stat_area.setTextSize(1);
        stat_area.setTextWrap(false);

        stat_area.setCursor((64 - (6 * 6)) / 2, 0);
        stat_area.print("CONFIG");
        stat_area.setCursor((64 - (4 * 6)) / 2, 8);
        stat_area.print("MODE");

        stat_area.setCursor((64 - (10 * 6)) / 2, 24);
        stat_area.print("Connect to");
        stat_area.setCursor((64 - (10 * 6)) / 2, 32);
        stat_area.print("Wifi SSID:");
        stat_area.setCursor((64 - (7 * 6)) / 2, 40);
        stat_area.print("RTNode-");
        stat_area.setCursor((64 - (5 * 6)) / 2, 48);
        stat_area.print("Setup");

        display.clearDisplay();
        display.drawBitmap(0, 0, stat_area.getBuffer(), stat_area.width(), stat_area.height(), SSD1306_WHITE, SSD1306_BLACK);
        display.display();
    }
    #endif
    // Headless: LED ramp will be driven from the WCC portal loop
    if (headless_mode) {
        Serial.println("[Config] Headless mode — LED will breathe during config portal");
    }
}

// ─── Stop Config Portal ──────────────────────────────────────────────────────
void config_portal_stop() {
    if (!config_portal_active) return;

    Serial.println("[Config] Stopping configuration portal");

    if (config_server) {
        config_server->stop();
        delete config_server;
        config_server = nullptr;
    }
    if (config_dns) {
        config_dns->stop();
        delete config_dns;
        config_dns = nullptr;
    }

    mdns_service::stop();

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
    config_portal_active = false;
}

// ─── Portal Loop — call from main loop() ─────────────────────────────────────
void config_portal_loop() {
    if (!config_portal_active) return;
    if (config_dns)    config_dns->processNextRequest();
    if (config_server) config_server->handleClient();
}

// ─── Is portal active? ──────────────────────────────────────────────────────
bool config_portal_is_active() {
    return config_portal_active;
}

#endif // FIREWALL_MODE
#endif // FIREWALL_CONFIG_H
