// Native unit tests for RemoteConfigCore.h (partial configuration apply, admin-list rules, USB receive buffer).
// Build and run: make -C tests/native test
//
// The real ROM.h and FirewallMode.h are used (real address map, real firewall_save_config()), with a fake EEPROM, so
// the tests also check what is actually persisted.
#include <string>
#include <vector>

#include "Arduino.h"
#include "EEPROM.h"

// ---- platform pieces normally provided by Config.h / Utilities.h / Log.h -------------------------------------
#define CONFIG_OFFSET 0
#define EEPROM_OFFSET 824
#define config_addr(a) ((a) + CONFIG_OFFSET)
#define eeprom_addr(a) ((a) + EEPROM_OFFSET)
#define WR_WIFI_STA 0x01
FakeEEPROM EEPROM;
static void eeprom_update(int addr, uint8_t value) { if (EEPROM.read(addr) != value) EEPROM.write(addr, value); }

#include "ROM.h"

static std::vector<std::string> g_log;
#define NOTICE(msg) (g_log.push_back(std::string(msg)))

uint32_t lora_freq = 0;
uint32_t lora_bw = 0;
int      lora_sf = 0;
int      lora_cr = 0;
int      lora_txp = 0;
float    st_airtime_limit = 0.0f;
float    lt_airtime_limit = 0.0f;

#include "FirewallMode.h"
FirewallState firewall_state;

static int g_admins_changed = 0;
static void remote_config_admins_changed();

#include "RemoteConfigCore.h"
static void remote_config_admins_changed() { g_admins_changed++; }

// ---- tiny test framework ---------------------------------------------------------------------------------------
static int g_checks = 0, g_failures = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define RUN(fn) do { reset(); printf("  %s\n", #fn); fn(); } while (0)

static void reset() {
    memset(&firewall_state, 0, sizeof(firewall_state));
    EEPROM.erase();
    g_log.clear();
    g_admins_changed = 0;
    lora_freq = 867200000; lora_bw = 125000; lora_sf = 7; lora_cr = 5; lora_txp = 10;
}

static const char* A = "00000000000000000000000000000001";   // super_admin
static const char* B = "0000000000000000000000000000000b";
static const char* C = "0000000000000000000000000000000c";
static const char* D = "0000000000000000000000000000000d";

static void hex(const char* s, uint8_t out[16]) { remote_config_hex16(s, out); }
static void seed_admins(std::initializer_list<const char*> l) {
    firewall_state.admin_count = 0;
    for (const char* h : l) hex(h, firewall_state.admin_hashes[firewall_state.admin_count++]);
}
static bool admin_is(int i, const char* h) { uint8_t x[16]; hex(h, x); return memcmp(firewall_state.admin_hashes[i], x, 16) == 0; }

struct Result { bool ok; std::vector<std::string> errors; std::vector<std::string> applied; };
static Result apply(const char* json, bool from_usb = false, const char* caller = nullptr) {
    JsonDocument cfg, report;
    if (deserializeJson(cfg, json)) { printf("    bad test json: %s\n", json); g_failures++; }
    uint8_t c[16];
    if (caller) hex(caller, c);
    remote_config_apply(cfg.as<JsonObjectConst>(), from_usb, caller ? c : nullptr, report);
    Result r; r.ok = report["ok"].as<bool>();
    for (JsonVariantConst e : report["errors"].as<JsonArrayConst>()) r.errors.push_back(e.as<const char*>());
    for (JsonVariantConst e : report["applied"].as<JsonArrayConst>()) r.applied.push_back(e.as<const char*>());
    return r;
}
static bool has_error(const Result& r, const char* fragment) {
    for (auto& e : r.errors) if (e.find(fragment) != std::string::npos) return true;
    return false;
}

// ---- hex parsing -----------------------------------------------------------------------------------------------
static void test_hex16() {
    uint8_t out[16];
    CHECK(remote_config_hex16("0123456789abcdef0123456789ABCDEF", out));
    CHECK(out[0] == 0x01 && out[7] == 0xef && out[15] == 0xef);
    CHECK(!remote_config_hex16("0123456789abcdef0123456789abcde", out));    // 31 chars
    CHECK(!remote_config_hex16("0123456789abcdef0123456789abcdef0", out));  // 33 chars
    CHECK(!remote_config_hex16("0123456789abcdef0123456789abcdeg", out));   // non-hex
    CHECK(!remote_config_hex16("", out));
}

// ---- admin list ------------------------------------------------------------------------------------------------
static void test_admins_add() {
    seed_admins({A});
    Result r = apply("{\"admins_add\":[\"0000000000000000000000000000000b\"]}");
    CHECK(r.ok);
    CHECK(firewall_state.admin_count == 2 && admin_is(0, A) && admin_is(1, B));
    CHECK(g_admins_changed == 1);
    CHECK(EEPROM.read(config_addr(ADDR_CONF_ADMIN_COUNT)) == 2);          // persisted
    CHECK(EEPROM.read(config_addr(ADDR_CONF_ADMIN_HASHES + 16 + 15)) == 0x0b);
    bool logged = false; for (auto& l : g_log) if (l.find("admin added") != std::string::npos) logged = true;
    CHECK(logged);
}
static void test_admins_add_is_idempotent() {
    seed_admins({A, B});
    Result r = apply("{\"admins_add\":[\"0000000000000000000000000000000b\"]}");
    CHECK(r.ok && firewall_state.admin_count == 2 && g_admins_changed == 0);
}
static void test_admins_add_invalid_and_full() {
    seed_admins({A});
    Result r = apply("{\"admins_add\":[\"nothex\",\"abcd\"]}");
    CHECK(!r.ok && has_error(r, "invalid hash") && firewall_state.admin_count == 1);
    seed_admins({A, B, C, D});
    r = apply("{\"admins_add\":[\"0000000000000000000000000000000e\"]}");
    CHECK(!r.ok && has_error(r, "list full") && firewall_state.admin_count == 4);
}
static void test_admins_remove_shifts_list() {
    seed_admins({A, B, C, D});
    Result r = apply("{\"admins_remove\":[\"0000000000000000000000000000000b\"]}");
    CHECK(r.ok && firewall_state.admin_count == 3);
    CHECK(admin_is(0, A) && admin_is(1, C) && admin_is(2, D));
    uint8_t zero[16] = {0};
    CHECK(memcmp(firewall_state.admin_hashes[3], zero, 16) == 0);         // vacated slot is cleared
    CHECK(g_admins_changed == 1);
}
static void test_super_admin_can_not_be_removed_over_the_network() {
    seed_admins({A, B});
    Result r = apply("{\"admins_remove\":[\"00000000000000000000000000000001\"]}", false, B);
    CHECK(!r.ok && has_error(r, "super_admin"));
    CHECK(firewall_state.admin_count == 2 && admin_is(0, A));
    // not even by the super_admin itself, and not even over USB (whole-list replacement is the USB way)
    r = apply("{\"admins_remove\":[\"00000000000000000000000000000001\"]}", false, A);
    CHECK(!r.ok && has_error(r, "super_admin") && firewall_state.admin_count == 2);
    r = apply("{\"admins_remove\":[\"00000000000000000000000000000001\"]}", true, nullptr);
    CHECK(!r.ok && has_error(r, "super_admin") && firewall_state.admin_count == 2);
    CHECK(g_admins_changed == 0);
}
static void test_admin_can_not_remove_itself() {
    seed_admins({A, B, C});
    Result r = apply("{\"admins_remove\":[\"0000000000000000000000000000000b\"]}", false, B);
    CHECK(!r.ok && has_error(r, "yourself") && firewall_state.admin_count == 3);
    // ...but can remove another admin, and the super_admin can remove any regular admin
    r = apply("{\"admins_remove\":[\"0000000000000000000000000000000c\"]}", false, B);
    CHECK(r.ok && firewall_state.admin_count == 2 && admin_is(1, B));
    r = apply("{\"admins_remove\":[\"0000000000000000000000000000000b\"]}", false, A);
    CHECK(r.ok && firewall_state.admin_count == 1);
}
static void test_admins_remove_unknown_is_a_noop() {
    seed_admins({A});
    Result r = apply("{\"admins_remove\":[\"0000000000000000000000000000000b\"]}");
    CHECK(r.ok && firewall_state.admin_count == 1 && g_admins_changed == 0);
}
static void test_whole_list_replacement_is_usb_only() {
    seed_admins({A});
    const char* req = "{\"admins\":[\"0000000000000000000000000000000b\",\"0000000000000000000000000000000c\"]}";
    Result r = apply(req, false, A);
    CHECK(!r.ok && has_error(r, "USB only") && firewall_state.admin_count == 1 && admin_is(0, A));
    r = apply(req, true, nullptr);
    CHECK(r.ok && firewall_state.admin_count == 2 && admin_is(0, B) && admin_is(1, C));   // first entry = new super_admin
    CHECK(g_admins_changed == 1);
}
static void test_whole_list_replacement_rejects_bad_input_atomically() {
    seed_admins({A, B});
    Result r = apply("{\"admins\":[\"0000000000000000000000000000000c\",\"zz\"]}", true);
    CHECK(!r.ok && firewall_state.admin_count == 2 && admin_is(0, A) && admin_is(1, B));  // unchanged
    r = apply("{\"admins\":[\"00000000000000000000000000000001\",\"0000000000000000000000000000000b\","
              "\"0000000000000000000000000000000c\",\"0000000000000000000000000000000d\","
              "\"0000000000000000000000000000000e\"]}", true);
    CHECK(!r.ok && firewall_state.admin_count == 2);                                        // 5 > 4
}

// ---- field validation ------------------------------------------------------------------------------------------
static void test_lora_ranges() {
    Result r = apply("{\"lora\":{\"sf\":4}}");   CHECK(!r.ok && has_error(r, "lora.sf") && lora_sf == 7);
    r = apply("{\"lora\":{\"sf\":13}}");         CHECK(!r.ok && lora_sf == 7);
    r = apply("{\"lora\":{\"sf\":5}}");          CHECK(r.ok && lora_sf == 5);
    r = apply("{\"lora\":{\"sf\":12}}");         CHECK(r.ok && lora_sf == 12);
    r = apply("{\"lora\":{\"cr\":4}}");          CHECK(!r.ok && lora_cr == 5);
    r = apply("{\"lora\":{\"cr\":9}}");          CHECK(!r.ok && lora_cr == 5);
    r = apply("{\"lora\":{\"cr\":8}}");          CHECK(r.ok && lora_cr == 8);
    r = apply("{\"lora\":{\"bw_hz\":7000}}");    CHECK(!r.ok && lora_bw == 125000);
    r = apply("{\"lora\":{\"bw_hz\":500001}}");  CHECK(!r.ok && lora_bw == 125000);
    r = apply("{\"lora\":{\"bw_hz\":500000}}");  CHECK(r.ok && lora_bw == 500000);
    r = apply("{\"lora\":{\"freq_hz\":100000000}}");  CHECK(!r.ok && lora_freq == 867200000);
    r = apply("{\"lora\":{\"freq_hz\":1100000000}}"); CHECK(!r.ok && lora_freq == 867200000);
    r = apply("{\"lora\":{\"txp_dbm\":1}}");     CHECK(!r.ok && lora_txp == 10);
    r = apply("{\"lora\":{\"txp_dbm\":31}}");    CHECK(!r.ok && lora_txp == 10);
    r = apply("{\"lora\":{\"txp_dbm\":30}}");    CHECK(r.ok && lora_txp == 30);
}
static void test_lora_is_persisted_like_the_portal() {
    Result r = apply("{\"lora\":{\"freq_hz\":867200000,\"bw_hz\":125000,\"sf\":9,\"cr\":6,\"txp_dbm\":14}}");
    CHECK(r.ok);
    CHECK(EEPROM.read(eeprom_addr(ADDR_CONF_SF)) == 9);
    CHECK(EEPROM.read(eeprom_addr(ADDR_CONF_CR)) == 6);
    CHECK(EEPROM.read(eeprom_addr(ADDR_CONF_TXP)) == 14);
    CHECK(EEPROM.read(eeprom_addr(ADDR_CONF_FREQ) + 0) == 0x33 && EEPROM.read(eeprom_addr(ADDR_CONF_FREQ) + 1) == 0xB0 &&
          EEPROM.read(eeprom_addr(ADDR_CONF_FREQ) + 2) == 0x6C && EEPROM.read(eeprom_addr(ADDR_CONF_FREQ) + 3) == 0x00);   // 867200000, big-endian
    CHECK(EEPROM.read(eeprom_addr(ADDR_CONF_OK)) == CONF_OK_BYTE);
    CHECK(EEPROM.commits >= 1);
}
static void test_airtime_limits() {
    Result r = apply("{\"lora\":{\"airtime_short_pct\":10,\"airtime_long_pct\":1.5}}");
    CHECK(r.ok);
    CHECK(fabsf(firewall_state.st_airtime_limit - 0.10f) < 1e-6f && fabsf(firewall_state.lt_airtime_limit - 0.015f) < 1e-6f);
    CHECK(EEPROM.read(config_addr(ADDR_CONF_ST_AL)) == 100 && EEPROM.read(config_addr(ADDR_CONF_LT_AL)) == 15);
    r = apply("{\"lora\":{\"airtime_short_pct\":80,\"airtime_long_pct\":-5}}");             // clamped, not rejected
    CHECK(r.ok && fabsf(firewall_state.st_airtime_limit - 0.25f) < 1e-6f && firewall_state.lt_airtime_limit == 0.0f);
    CHECK(EEPROM.read(config_addr(ADDR_CONF_ST_AL)) == 250);
    r = apply("{\"lora\":{\"airtime_long_pct\":2}}");                                       // the other one is kept
    CHECK(r.ok && fabsf(firewall_state.st_airtime_limit - 0.25f) < 1e-6f && fabsf(firewall_state.lt_airtime_limit - 0.02f) < 1e-6f);
}
static void test_backbones() {
    Result r = apply("{\"backbones\":[{\"enabled\":true,\"host\":\"192.168.90.163\",\"port\":4243}]}");
    CHECK(r.ok && firewall_state.backbones[0].enabled && firewall_state.backbones[0].port == 4243);
    CHECK(strcmp(firewall_state.backbones[0].host, "192.168.90.163") == 0);
    r = apply("{\"backbones\":[{\"port\":0}]}");   CHECK(!r.ok && has_error(r, "port") && firewall_state.backbones[0].port == 4243);
    r = apply("{\"backbones\":[{\"port\":65536}]}"); CHECK(!r.ok && firewall_state.backbones[0].port == 4243);
    std::string longhost(FIREWALL_BACKBONE_HOST_LEN, 'a');
    r = apply(("{\"backbones\":[{\"host\":\"" + longhost + "\"}]}").c_str());
    CHECK(!r.ok && has_error(r, "host") && strcmp(firewall_state.backbones[0].host, "192.168.90.163") == 0);
    r = apply("{\"backbones\":[{\"host\":\"\"}]}");   // an empty host disables the slot (same rule as the portal)
    CHECK(r.ok && !firewall_state.backbones[0].enabled);
    r = apply("{\"backbones\":[null,{\"enabled\":true,\"host\":\"x.example\",\"port\":4242}]}");   // null skips a slot
    CHECK(r.ok && firewall_state.backbones[1].enabled && strcmp(firewall_state.backbones[1].host, "x.example") == 0);
    r = apply("{\"backbones\":[{},{},{},{},{}]}");   CHECK(!r.ok && has_error(r, "backbones.count"));
}
static void test_wifi() {
    Result r = apply("{\"wifi\":{\"enabled\":true,\"ssid\":\"Skyloz\",\"psk\":\"secret\"}}");
    CHECK(r.ok && firewall_state.wifi_enabled);
    CHECK(EEPROM.read(config_addr(ADDR_CONF_SSID)) == 'S' && EEPROM.read(config_addr(ADDR_CONF_SSID + 5)) == 'z');
    CHECK(EEPROM.read(config_addr(ADDR_CONF_SSID + 6)) == 0 && EEPROM.read(config_addr(ADDR_CONF_SSID + 32)) == 0);   // padded, terminated
    CHECK(EEPROM.read(config_addr(ADDR_CONF_PSK)) == 's');
    CHECK(EEPROM.read(eeprom_addr(ADDR_CONF_WIFI)) == WR_WIFI_STA);
    std::string s33(33, 'x');
    r = apply(("{\"wifi\":{\"ssid\":\"" + s33 + "\"}}").c_str());  CHECK(!r.ok && has_error(r, "wifi.ssid"));
    r = apply(("{\"wifi\":{\"psk\":\"" + s33 + "\"}}").c_str());   CHECK(!r.ok && has_error(r, "wifi.psk"));
    CHECK(EEPROM.read(config_addr(ADDR_CONF_SSID)) == 'S');                                                            // untouched
}
static void test_ifac() {
    Result r = apply("{\"ifac\":{\"enabled\":true,\"netname\":\"lab\",\"passphrase\":\"pw\"}}");
    CHECK(r.ok && firewall_state.ifac_enabled && strcmp(firewall_state.ifac_netname, "lab") == 0);
    r = apply("{\"ifac\":{\"enabled\":true,\"netname\":\"\",\"passphrase\":\"\"}}");   // enabled but empty => disabled (portal rule)
    CHECK(r.ok && !firewall_state.ifac_enabled);
    std::string s33(33, 'k');
    r = apply(("{\"ifac\":{\"netname\":\"" + s33 + "\"}}").c_str());     CHECK(!r.ok && has_error(r, "ifac.netname"));
    r = apply(("{\"ifac\":{\"passphrase\":\"" + s33 + "\"}}").c_str());  CHECK(!r.ok && has_error(r, "ifac.passphrase"));
}
static void test_server_and_mdns_hostname() {
    Result r = apply("{\"server\":{\"tcp_enabled\":true,\"tcp_port\":5000,\"probe_enabled\":true,\"mdns_hostname\":\"My_Host.Name-1\"}}");
    CHECK(r.ok && firewall_state.ap_tcp_enabled && firewall_state.ap_tcp_port == 5000 && firewall_state.probe_enabled);
    CHECK(strcmp(firewall_state.mdns_hostname, "myhostname-1") == 0);        // lower-cased, only [a-z0-9-] kept
    r = apply("{\"server\":{\"tcp_port\":0}}");  CHECK(!r.ok && has_error(r, "tcp_port") && firewall_state.ap_tcp_port == 5000);
}
static void test_name_and_advert() {
    Result r = apply("{\"name\":\"rtnode-par1\"}");
    CHECK(r.ok && strcmp(firewall_state.node_name, "rtnode-par1") == 0);
    r = apply(("{\"name\":\"" + std::string(33, 'n') + "\"}").c_str());
    CHECK(!r.ok && has_error(r, "name") && strcmp(firewall_state.node_name, "rtnode-par1") == 0);
    r = apply("{\"advert\":{\"enabled\":true,\"lat\":48.85,\"lon\":2.35,\"jitter\":true}}");
    CHECK(r.ok && firewall_state.advert_enabled && firewall_state.advert_jitter);
    r = apply("{\"advert\":{\"lat\":91}}");
    CHECK(!r.ok);
    CHECK(has_error(r, "advert.lat"));
    CHECK(fabs(firewall_state.advert_lat - 48.85) < 1e-5);   // ArduinoJson keeps 48.85 in single precision (~17 cm)
    r = apply("{\"advert\":{\"lon\":-181}}");  CHECK(!r.ok && has_error(r, "advert.lon"));
    r = apply("{\"advert\":{\"lat\":-90,\"lon\":180}}");  CHECK(r.ok);       // bounds are inclusive
}
static void test_partial_apply_leaves_other_fields_alone() {
    apply("{\"name\":\"keep\",\"lora\":{\"sf\":9},\"ifac\":{\"enabled\":true,\"netname\":\"lab\",\"passphrase\":\"pw\"}}");
    Result r = apply("{\"lora\":{\"cr\":7}}");
    CHECK(r.ok && lora_cr == 7 && lora_sf == 9);
    CHECK(strcmp(firewall_state.node_name, "keep") == 0 && firewall_state.ifac_enabled);
}
static void test_unknown_and_empty_requests() {
    Result r = apply("{}");                        CHECK(r.ok && r.applied.empty());
    r = apply("{\"nonsense\":{\"a\":1}}");         CHECK(r.ok && r.applied.empty());
    int commits = EEPROM.commits;
    r = apply("{\"lora\":{\"sf\":99}}");           CHECK(!r.ok);
    CHECK(EEPROM.commits >= commits);              // never leaves an uncommitted half-write behind
}
static void test_admins_are_loaded_back_from_eeprom() {
    seed_admins({A, B});
    apply("{\"admins_add\":[\"0000000000000000000000000000000c\"]}");
    memset(&firewall_state, 0, sizeof(firewall_state));
    firewall_load_config();
    CHECK(firewall_state.admin_count == 3 && admin_is(0, A) && admin_is(1, B) && admin_is(2, C));
    EEPROM.write(config_addr(ADDR_CONF_ADMIN_COUNT), 0xFF);          // erased flash: no admin
    firewall_load_config();
    CHECK(firewall_state.admin_count == 0);
    EEPROM.write(config_addr(ADDR_CONF_ADMIN_COUNT), 9);             // out of range: treated as none
    firewall_load_config();
    CHECK(firewall_state.admin_count == 0);
}

// ---- USB frame receive buffer ----------------------------------------------------------------------------------
static void feed(const std::initializer_list<uint8_t>& bytes) { for (uint8_t b : bytes) remote_config_usb_feed(b); }
static void test_usb_unescaping() {
    remote_config_usb_begin();
    feed({'a', 0xDB, 0xDC, 'b', 0xDB, 0xDD, 'c'});   // FESC TFEND -> FEND, FESC TFESC -> FESC
    CHECK(remote_config_usb_len == 5 && !remote_config_usb_overflow);
    CHECK((uint8_t)remote_config_usb_buf[1] == 0xC0 && (uint8_t)remote_config_usb_buf[3] == 0xDB && remote_config_usb_buf[4] == 'c');
    remote_config_usb_begin();
    CHECK(remote_config_usb_len == 0 && !remote_config_usb_esc);
}
static void test_usb_overflow() {
    remote_config_usb_begin();
    for (int i = 0; i < REMOTE_CONFIG_USB_MAX + 10; i++) remote_config_usb_feed('x');
    CHECK(remote_config_usb_len == REMOTE_CONFIG_USB_MAX && remote_config_usb_overflow);
    remote_config_usb_begin();                                        // a new frame starts clean
    CHECK(remote_config_usb_len == 0 && !remote_config_usb_overflow);
}

int main() {
    printf("RemoteConfigCore native tests\n");
    RUN(test_hex16);
    RUN(test_admins_add);
    RUN(test_admins_add_is_idempotent);
    RUN(test_admins_add_invalid_and_full);
    RUN(test_admins_remove_shifts_list);
    RUN(test_super_admin_can_not_be_removed_over_the_network);
    RUN(test_admin_can_not_remove_itself);
    RUN(test_admins_remove_unknown_is_a_noop);
    RUN(test_whole_list_replacement_is_usb_only);
    RUN(test_whole_list_replacement_rejects_bad_input_atomically);
    RUN(test_lora_ranges);
    RUN(test_lora_is_persisted_like_the_portal);
    RUN(test_airtime_limits);
    RUN(test_backbones);
    RUN(test_wifi);
    RUN(test_ifac);
    RUN(test_server_and_mdns_hostname);
    RUN(test_name_and_advert);
    RUN(test_partial_apply_leaves_other_fields_alone);
    RUN(test_unknown_and_empty_requests);
    RUN(test_admins_are_loaded_back_from_eeprom);
    RUN(test_usb_unescaping);
    RUN(test_usb_overflow);
    printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
