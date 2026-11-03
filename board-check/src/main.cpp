// main.cpp — the board check image. It finds which board it is running on
// from the board library (Meshtastic's board definitions), then shows the
// pictures the web page asks for and reports every button press and touch.
// It talks to the page in JSON, one object per line, at 115200 baud, over
// the chip's own USB port or its UART0 (boards with a USB bridge chip).
//
// The page does the recording: nothing here needs to survive a reset.
// The plan, and the reasons for each step, are in BOARD_CHECK_PLAN.md.
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include "board_library.h"
#include "radio_probe.h"
#include "power.h"
#include "screen.h"
#include "touch.h"
#include "buttons.h"

static const char *FW_VERSION = "board-check 0.1";

// ------------------------------------------------------------------ output

static bool uart_console = true;  // until the page speaks over native USB

static void emit(JsonDocument &doc) {
  String s;
  serializeJson(doc, s);
  if (Serial) Serial.println(s);
  if (uart_console) Serial0.println(s);
}

static void emit_step(const char *step, const char *detail) {
  JsonDocument d;
  d["event"] = "step";
  d["step"] = step;
  if (detail) d["detail"] = detail;
  emit(d);
}

static String hex2(uint8_t v) {
  char b[5];
  snprintf(b, sizeof b, "0x%02x", v);
  return String(b);
}

// ------------------------------------------------------------- pin safety

// What the page read from the chip while writing this image.
static uint8_t hint_psram = PSRAM_ANY, hint_usb = USB_ANY, hint_flash_mb = 0;

// Never touch the flash or USB pins, nor the PSRAM's on chips with 8 MB.
static bool pin_safe(int p) {
  if (p < 0 || p >= GPIO_COUNT) return false;
  if (p == 19 || p == 20 || (p >= 22 && p <= 32)) return false;
  if (hint_psram == PSRAM_OCTAL && p >= 33 && p <= 37) return false;
  return true;
}

static bool radio_safe(const RadioDef &r) {
  const int8_t pins[] = {r.cs, r.rst, r.sck, r.miso, r.mosi};
  for (int8_t p : pins)
    if (!pin_safe(p)) return false;
  const int8_t optional[] = {r.irq, r.busy, r.txen, r.rxen};
  for (int8_t p : optional)
    if (p >= 0 && !pin_safe(p)) return false;
  return true;
}

static bool power_safe(const BoardDef &b) {
  for (const PowerPin &p : b.power)
    if (p.pin >= 0 && !pin_safe(p.pin)) return false;
  return true;
}

// ------------------------------------------------------------------ probe

struct BusScan {
  int8_t sda, scl;
  bool stuck;
  uint8_t n;
  uint8_t addrs[24];
};

static BusScan scans[24];
static int scan_count = 0;
static bool probed = false;
static bool radio_found = false;
static RadioDef radio_def;
static RadioResult radio;
static int radio_tries = 0;
static int powered = -1;          // the board whose power pins are on
static uint8_t pmu_found = PMU_NONE;
static int8_t pmu_sda = -1, pmu_scl = -1;
static int8_t score[BOARD_LIBRARY_COUNT];
static int winners[8];
static int winner_count = 0;
static int lead = -1;             // the board the screen and touch are driven as
static uint8_t screen_type = SCREEN_NONE, screen_addr = 0;
static uint8_t touch_addr = 0;
static int32_t battery_mv = -1;
static uint32_t ms_pmu, ms_radio, ms_i2c;

static BusScan *scan_bus(int8_t sda, int8_t scl) {
  for (int i = 0; i < scan_count; i++)
    if (scans[i].sda == sda && scans[i].scl == scl) return &scans[i];
  if (!pin_safe(sda) || !pin_safe(scl) || sda == scl || scan_count >= 24) return nullptr;
  BusScan &s = scans[scan_count++];
  s.sda = sda;
  s.scl = scl;
  s.n = 0;
  s.stuck = false;
  Wire.begin(sda, scl, 100000);
  Wire.setTimeOut(10);
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    uint8_t r = Wire.endTransmission();
    if (r == 0 && s.n < 24) s.addrs[s.n++] = a;
    if (r >= 4) {  // a line held low: nothing on this pair can answer
      s.stuck = true;
      break;
    }
  }
  Wire.end();
  pinMode(sda, INPUT);
  pinMode(scl, INPUT);
  return &s;
}

static bool bus_has(int8_t sda, int8_t scl, uint8_t a) {
  for (int i = 0; i < scan_count; i++) {
    if (scans[i].sda != sda || scans[i].scl != scl) continue;
    for (uint8_t k = 0; k < scans[i].n; k++)
      if (scans[i].addrs[k] == a) return true;
  }
  return false;
}

static uint8_t oled_addr(int8_t sda, int8_t scl) {
  if (bus_has(sda, scl, 0x3C)) return 0x3C;
  if (bus_has(sda, scl, 0x3D)) return 0x3D;
  return 0;
}

static bool same_radio(const RadioDef &a, const RadioDef &b) {
  return a.type == b.type && a.cs == b.cs && a.rst == b.rst && a.irq == b.irq && a.busy == b.busy &&
         a.sck == b.sck && a.miso == b.miso && a.mosi == b.mosi;
}

static bool has_radio(const BoardDef &b, const RadioDef &r) {
  for (const RadioDef &x : b.radio)
    if (same_radio(x, r)) return true;
  return false;
}

static bool oled_type(uint8_t t) { return t == SCREEN_SSD1306 || t == SCREEN_SH1106 || t == SCREEN_SH1107; }

static int prior(const BoardDef &b) {
  int s = 0;
  if (hint_psram != PSRAM_ANY && b.psram != PSRAM_ANY) s += b.psram == hint_psram ? 2 : -6;
  if (hint_usb != USB_ANY && b.usb != USB_ANY) s += b.usb == hint_usb ? 1 : -3;
  if (hint_flash_mb && b.flash_mb) s += b.flash_mb > hint_flash_mb ? -3 : b.flash_mb == hint_flash_mb ? 1 : 0;
  return s;
}

static void switch_power(int board) {
  if (powered == board) return;
  if (powered >= 0) power_off(BOARD_LIBRARY[powered]);
  powered = -1;
  if (board >= 0 && power_safe(BOARD_LIBRARY[board])) {
    power_on(BOARD_LIBRARY[board]);
    powered = board;
  }
}

static void find_pmu(const int *order) {
  for (int k = 0; k < BOARD_LIBRARY_COUNT; k++) {
    const BoardDef &b = BOARD_LIBRARY[order[k]];
    if (b.pmu == PMU_NONE) continue;
    int8_t sda = b.i2c[b.pmu_bus * 2], scl = b.i2c[b.pmu_bus * 2 + 1];
    if (!pin_safe(sda) || !pin_safe(scl) || (sda == pmu_sda && scl == pmu_scl)) continue;
    Wire.begin(sda, scl, 100000);
    Wire.setTimeOut(10);
    uint8_t t = pmu_identify(Wire);
    if (t != PMU_NONE) {
      pmu_found = t;
      pmu_sda = sda;
      pmu_scl = scl;
      if (t == PMU_AXP2101 && pmu_enable_rails(Wire, b)) emit_step("power chip", b.id);
    }
    Wire.end();
    pinMode(sda, INPUT);
    pinMode(scl, INPUT);
    if (pmu_found != PMU_NONE) return;
  }
}

static void find_radio(const int *order) {
  struct Tried { RadioDef r; PowerPin p[4]; };
  static Tried tried[96];
  int nt = 0;
  for (int k = 0; k < BOARD_LIBRARY_COUNT && !radio_found; k++) {
    int i = order[k];
    const BoardDef &b = BOARD_LIBRARY[i];
    for (const RadioDef &r : b.radio) {
      if (r.type == RADIO_NONE || !radio_safe(r)) continue;
      bool seen = false;
      for (int t = 0; t < nt && !seen; t++)
        seen = same_radio(tried[t].r, r) && memcmp(tried[t].p, b.power, sizeof b.power) == 0;
      if (seen || nt >= 96) continue;
      tried[nt].r = r;
      memcpy(tried[nt].p, b.power, sizeof b.power);
      nt++;
      switch_power(i);
      radio_tries++;
      RadioResult res;
      if (probe_radio(r, res, true)) {
        radio = res;
        radio_def = r;
        radio_found = true;
        emit_step("radio found", b.id);
        break;
      }
    }
  }
  if (!radio_found) switch_power(-1);
}

// A reset pulse, then the line held high for the part to run.
static void release_reset(int8_t p) {
  if (!pin_safe(p)) return;
  pinMode(p, OUTPUT);
  digitalWrite(p, LOW);
  delay(5);
  digitalWrite(p, HIGH);
  delay(50);  // touch chips need the longest after reset
}

static void score_boards(const int *order) {
  // Scan the I2C buses of the boards still in the running. With no radio
  // found, every board is, so only the likeliest dozen are scanned.
  int scanned = 0;
  for (int k = 0; k < BOARD_LIBRARY_COUNT; k++) {
    int i = order[k];
    const BoardDef &b = BOARD_LIBRARY[i];
    if (radio_found ? !has_radio(b, radio_def) : scanned >= 12) continue;
    scanned++;
    if (!radio_found) switch_power(i);
    // An OLED or touch chip held in reset doesn't answer: let both run.
    int8_t oled_rst = (oled_type(b.screen) || b.screen == SCREEN_OLED_MAYBE) ? b.tft[TFT_RST] : -1;
    int8_t touch_rst = b.touch ? b.touch_rst : -1;
    release_reset(oled_rst);
    release_reset(touch_rst);
    scan_bus(b.i2c[0], b.i2c[1]);
    scan_bus(b.i2c[2], b.i2c[3]);
    if (pin_safe(oled_rst)) pinMode(oled_rst, INPUT);
    if (pin_safe(touch_rst)) pinMode(touch_rst, INPUT);
  }
  if (!radio_found) switch_power(-1);

  int best = -100;
  for (int i = 0; i < BOARD_LIBRARY_COUNT; i++) {
    const BoardDef &b = BOARD_LIBRARY[i];
    int s = prior(b);
    bool match = radio_found && has_radio(b, radio_def);
    if (radio_found && !match) { score[i] = -100; continue; }
    if (match) s += 10;
    bool oled = oled_addr(b.i2c[0], b.i2c[1]) != 0;
    if (oled_type(b.screen)) s += oled ? 3 : -3;
    else if (b.screen == SCREEN_OLED_MAYBE) s += oled ? 1 : 0;
    else if (oled) s -= 2;  // an OLED this board doesn't have
    if (b.pmu != PMU_NONE) s += (pmu_found == b.pmu && pmu_sda == b.i2c[b.pmu_bus * 2]) ? 3 : -3;
    if (b.touch && b.touch_addr)
      s += bus_has(b.i2c[b.touch_bus * 2], b.i2c[b.touch_bus * 2 + 1], b.touch_addr) ? 3 : -1;
    score[i] = s;
    if (s > best) best = s;
  }
  winner_count = 0;
  for (int k = 0; k < BOARD_LIBRARY_COUNT && winner_count < 8; k++)
    if (score[order[k]] == best && best > -100) winners[winner_count++] = order[k];
  // Without a radio, the scores rest on the I2C devices alone: too little to
  // name a board, so the page asks for its name.
  if (!radio_found && best < 3) winner_count = 0;
}

static void bring_up(int i) {
  const BoardDef &b = BOARD_LIBRARY[i];
  lead = i;
  switch_power(i);

  screen_type = b.screen;
  TwoWire *bus = nullptr;
  if (oled_type(b.screen) || b.screen == SCREEN_OLED_MAYBE) {
    screen_addr = oled_addr(b.i2c[0], b.i2c[1]);
    if (screen_addr) {
      if (b.screen == SCREEN_OLED_MAYBE) screen_type = SCREEN_SSD1306;
      Wire.begin(b.i2c[0], b.i2c[1], 400000);
      bus = &Wire;
    }
  }
  bool drawable = (screen_type == SCREEN_ST7789 || screen_type == SCREEN_ST7735)
                    ? (pin_safe(b.tft[TFT_SCK]) && pin_safe(b.tft[TFT_MOSI]) && pin_safe(b.tft[TFT_CS]) &&
                       pin_safe(b.tft[TFT_DC]))
                    : bus != nullptr;
  if (drawable && screen_begin(b, screen_type, bus, screen_addr)) screen_status(b.name, "Follow the web page");

  if (b.touch) {
    int8_t sda = b.i2c[b.touch_bus * 2], scl = b.i2c[b.touch_bus * 2 + 1];
    TwoWire *tw = nullptr;
    if (pin_safe(sda) && pin_safe(scl)) {
      if (bus && sda == b.i2c[0] && scl == b.i2c[1]) tw = bus;
      else {
        Wire1.begin(sda, scl, 400000);
        tw = &Wire1;
      }
    }
    if (b.touch_rst >= 0 && pin_safe(b.touch_rst)) {
      pinMode(b.touch_rst, OUTPUT);
      digitalWrite(b.touch_rst, HIGH);
    }
    static const uint8_t known[] = {0x38, 0x15, 0x5D, 0x14, 0x2E, 0x1A, 0x5A};
    touch_addr = (tw && bus_has(sda, scl, b.touch_addr)) ? b.touch_addr : 0;
    for (uint8_t a : known)
      if (!touch_addr && tw && bus_has(sda, scl, a)) touch_addr = a;
    touch_begin(touch_addr ? tw : nullptr, touch_addr, pin_safe(b.touch_int) ? b.touch_int : -1);
  }

  if (pin_safe(b.battery)) {
    if (pin_safe(b.adc_ctrl)) {
      pinMode(b.adc_ctrl, OUTPUT);
      digitalWrite(b.adc_ctrl, b.adc_ctrl_on ? HIGH : LOW);
      delay(10);
    }
    battery_mv = (int32_t)analogReadMilliVolts(b.battery) * b.adc_mult_milli / 1000;
    if (pin_safe(b.adc_ctrl)) pinMode(b.adc_ctrl, INPUT);
  }
}

// Every pin the probe found a use for, or that the board's definition gives
// to another part, is left out of the button watch.
static void start_buttons() {
  bool allowed[GPIO_COUNT];
  for (int p = 0; p < GPIO_COUNT; p++)
    allowed[p] = pin_safe(p) && !(uart_console && (p == 43 || p == 44));
  auto drop = [&](int p) { if (p >= 0 && p < GPIO_COUNT) allowed[p] = false; };
  if (radio_found) {
    const int8_t r[] = {radio_def.cs, radio_def.rst, radio_def.irq, radio_def.busy, radio_def.txen,
                        radio_def.rxen, radio_def.sck, radio_def.miso, radio_def.mosi};
    for (int8_t p : r) drop(p);
  }
  for (int i = 0; i < scan_count; i++)
    if (scans[i].n) { drop(scans[i].sda); drop(scans[i].scl); }
  drop(pmu_sda);
  drop(pmu_scl);
  if (lead >= 0) {
    const BoardDef &b = BOARD_LIBRARY[lead];
    for (const PowerPin &p : b.power) drop(p.pin);
    for (int8_t p : b.tft) drop(p);
    for (int8_t p : b.i2c) drop(p);
    for (int8_t p : b.other_pins) drop(p);
    drop(b.touch_int);
    drop(b.touch_rst);
    drop(b.led);
    drop(b.battery);
    drop(b.adc_ctrl);
    for (int8_t p : b.buttons)
      if (pin_safe(p) && !(uart_console && (p == 43 || p == 44))) allowed[p] = true;
  }
  buttons_begin(allowed);
  JsonDocument d;
  d["event"] = "buttons";
  JsonArray w = d["watching"].to<JsonArray>();
  JsonArray busy = d["busy"].to<JsonArray>();
  for (int p = 0; p < GPIO_COUNT; p++) {
    if (buttons_watching(p)) w.add(p);
    if (buttons_busy(p)) busy.add(p);
  }
  emit(d);
}

static const char *screen_name(uint8_t t) {
  static const char *n[] = {"none", "oled", "ssd1306", "sh1106", "sh1107", "st7789", "st7735", "e-paper", "other tft"};
  return t < sizeof n / sizeof n[0] ? n[t] : "unknown";
}

static void emit_found() {
  JsonDocument d;
  d["event"] = "found";
  d["match"] = winner_count == 1 ? "one" : winner_count > 1 ? "several" : "none";
  if (winner_count == 1) {
    d["board"]["id"] = BOARD_LIBRARY[winners[0]].id;
    d["board"]["name"] = BOARD_LIBRARY[winners[0]].name;
  }
  JsonArray c = d["candidates"].to<JsonArray>();
  for (int k = 0; k < winner_count; k++) {
    JsonObject o = c.add<JsonObject>();
    o["id"] = BOARD_LIBRARY[winners[k]].id;
    o["name"] = BOARD_LIBRARY[winners[k]].name;
  }
  d["radio_tries"] = radio_tries;
  if (radio_found) {
    JsonObject r = d["radio"].to<JsonObject>();
    r["type"] = radio_type_name(radio.type);
    r["chip"] = radio.chip;
    JsonObject p = r["pins"].to<JsonObject>();
    p["sck"] = radio_def.sck; p["miso"] = radio_def.miso; p["mosi"] = radio_def.mosi;
    p["cs"] = radio_def.cs; p["rst"] = radio_def.rst; p["irq"] = radio_def.irq; p["busy"] = radio_def.busy;
    if (radio_def.txen >= 0) p["txen"] = radio_def.txen;
    if (radio_def.rxen >= 0) p["rxen"] = radio_def.rxen;
    if (radio_def.tcxo_dv) r["tcxo_v"] = radio_def.tcxo_dv / 10.0;
    r["rf_switch_dio2"] = radio_def.rf_switch;
    r["busy_ms"] = radio.busy_ms;
    char errs[8];
    snprintf(errs, sizeof errs, "0x%04x", radio.errors);
    r["errors"] = errs;
    r["oscillator"] = radio.osc == 1 ? "ok" : radio.osc == 0 ? "failed" : "not checked";
    r["irq"] = radio.irq == 1 ? "ok" : radio.irq == 0 ? "line silent" : radio.irq == 2 ? "no answer" : "not checked";
    if (radio.irq == 1) r["irq_ms"] = radio.irq_ms;
  } else {
    d["radio"] = nullptr;
  }
  JsonObject pw = d["power"].to<JsonObject>();
  pw["chip"] = pmu_found == PMU_AXP2101 ? "AXP2101" : pmu_found == PMU_AXP192 ? "AXP192" : "none";
  JsonArray pins = pw["pins"].to<JsonArray>();
  if (lead >= 0)
    for (const PowerPin &p : BOARD_LIBRARY[lead].power)
      if (p.pin >= 0) { JsonObject o = pins.add<JsonObject>(); o["pin"] = p.pin; o["on"] = p.on; }
  JsonArray buses = d["i2c"].to<JsonArray>();
  for (int i = 0; i < scan_count; i++) {
    JsonObject o = buses.add<JsonObject>();
    o["sda"] = scans[i].sda;
    o["scl"] = scans[i].scl;
    if (scans[i].stuck) o["stuck"] = true;
    JsonArray a = o["devices"].to<JsonArray>();
    for (uint8_t k = 0; k < scans[i].n; k++) a.add(hex2(scans[i].addrs[k]));
  }
  JsonObject s = d["screen"].to<JsonObject>();
  s["type"] = screen_name(screen_type);
  s["drawn"] = screen_ready();
  if (screen_addr) s["addr"] = hex2(screen_addr);
  if (screen_ready()) {
    s["w"] = screen_width();
    s["h"] = screen_height();
    s["colour"] = screen_is_colour();
  }
  if (lead >= 0 && BOARD_LIBRARY[lead].touch) {
    JsonObject t = d["touch"].to<JsonObject>();
    t["chip"] = touch_addr ? touch_chip_name(touch_addr) : "not found";
    if (touch_addr) t["addr"] = hex2(touch_addr);
    t["int"] = BOARD_LIBRARY[lead].touch_int;
  }
  if (lead >= 0) {
    JsonArray bt = d["buttons_expected"].to<JsonArray>();
    for (int8_t p : BOARD_LIBRARY[lead].buttons)
      if (p >= 0) bt.add(p);
    d["led"] = BOARD_LIBRARY[lead].led;
  }
  if (battery_mv >= 0) d["battery_mv"] = battery_mv;
  d["ms"]["power_chip"] = ms_pmu;
  d["ms"]["radio"] = ms_radio;
  d["ms"]["i2c"] = ms_i2c;
  emit(d);
}

static void probe_all() {
  if (probed) { emit_found(); return; }  // the probe runs once per boot
  static int order[BOARD_LIBRARY_COUNT];
  for (int i = 0; i < BOARD_LIBRARY_COUNT; i++) order[i] = i;
  for (int i = 1; i < BOARD_LIBRARY_COUNT; i++)   // stable sort, likeliest first
    for (int j = i; j > 0 && prior(BOARD_LIBRARY[order[j]]) > prior(BOARD_LIBRARY[order[j - 1]]); j--)
      std::swap(order[j], order[j - 1]);
  uint32_t t0 = millis();
  emit_step("power chip", nullptr);
  find_pmu(order);
  ms_pmu = millis() - t0;
  emit_step("radio", nullptr);
  find_radio(order);
  ms_radio = millis() - t0 - ms_pmu;
  emit_step("i2c", nullptr);
  score_boards(order);
  ms_i2c = millis() - t0 - ms_pmu - ms_radio;
  if (winner_count > 0) bring_up(winners[0]);
  probed = true;
  start_buttons();
  emit_found();
}

static void scan_wireless() {
  JsonDocument d;
  d["event"] = "wireless";
  WiFi.persistent(false);  // write nothing to the board's saved settings
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, true);
  int best = -200;
  for (int i = 0; i < n; i++) best = max(best, (int)WiFi.RSSI(i));
  d["wifi_networks"] = n < 0 ? 0 : n;
  if (n > 0) d["wifi_best_rssi"] = best;
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(false);
  NimBLEScanResults res = scan->start(3, false);
  d["ble_devices"] = res.getCount();
  scan->clearResults();
  NimBLEDevice::deinit(true);
  emit(d);
}

// ------------------------------------------------------------- the pictures

enum Mode { MODE_STATUS, MODE_X, MODE_ARROW };
static Mode mode = MODE_STATUS;
static uint8_t orientation = 0;

static void emit_orientation() {
  JsonDocument d;
  d["event"] = "orientation";
  d["turns"] = orientation & 3;
  d["mirrored"] = orientation >= 4;
  emit(d);
}

static void next_orientation() {
  orientation = (orientation + 1) % 8;
  screen_show_arrow(orientation);
  emit_orientation();
}

static void on_button(int pin, bool down, int level) {
  JsonDocument d;
  d["event"] = "button";
  d["pin"] = pin;
  d["down"] = down;
  d["level"] = level;
  emit(d);
  if (down && mode == MODE_ARROW) next_orientation();
}

// ---------------------------------------------------------------- commands

static void handle(const String &line) {
  JsonDocument in;
  if (deserializeJson(in, line) != DeserializationError::Ok) return;
  const char *cmd = in["cmd"] | "";
  if (!strcmp(cmd, "hello")) {
    JsonDocument d;
    d["event"] = "hello";
    d["fw"] = FW_VERSION;
    d["reset"] = (int)esp_reset_reason();
    d["probed"] = probed;
    d["boards"] = BOARD_LIBRARY_COUNT;
    emit(d);
  } else if (!strcmp(cmd, "probe")) {
    const char *ps = in["psram"] | "";
    hint_psram = !strcmp(ps, "none") ? PSRAM_NONE : !strcmp(ps, "2MB") ? PSRAM_QUAD
               : !strcmp(ps, "8MB") ? PSRAM_OCTAL : PSRAM_ANY;
    const char *usb = in["usb"] | "";
    hint_usb = !strcmp(usb, "native") ? USB_NATIVE : !strcmp(usb, "bridge") ? USB_BRIDGE : USB_ANY;
    hint_flash_mb = in["flash_mb"] | 0;
    probe_all();
  } else if (!strcmp(cmd, "wireless")) {
    scan_wireless();
  } else if (!strcmp(cmd, "show")) {
    const char *what = in["what"] | "";
    if (!strcmp(what, "x")) { mode = MODE_X; screen_show_x(); }
    else if (!strcmp(what, "arrow")) { mode = MODE_ARROW; orientation = 0; screen_show_arrow(0); emit_orientation(); }
    else { mode = MODE_STATUS; if (lead >= 0) screen_status(BOARD_LIBRARY[lead].name, "Follow the web page"); }
  } else if (!strcmp(cmd, "rotate")) {
    if (mode == MODE_ARROW) next_orientation();
  } else if (!strcmp(cmd, "dump")) {
    const uint8_t *buf;
    size_t n = screen_dump(&buf);
    String hex;
    hex.reserve(2 * n);
    for (size_t i = 0; i < n; i++) {
      char h[3];
      snprintf(h, sizeof h, "%02x", buf[i]);
      hex += h;
    }
    JsonDocument d;
    d["event"] = "dump";
    d["w"] = screen_width();
    d["h"] = screen_height();
    d["bits"] = hex;
    emit(d);
  }
}

static String usb_line, uart_line;

static void read_port(Stream &s, String &buf, bool native) {
  while (s.available()) {
    char c = s.read();
    if (c == '\n') {
      if (native && uart_console) {  // the page is on native USB: free UART0's pins
        uart_console = false;
        Serial0.end();
        pinMode(43, INPUT);
        pinMode(44, INPUT);
      }
      handle(buf);
      buf = "";
    } else if (c != '\r' && buf.length() < 512) {
      buf += c;
    }
  }
}

void setup() {
  Serial.setTxBufferSize(4096);  // the report is one long line
  Serial.setTxTimeoutMs(200);    // only waited on while the page is connected
  Serial.begin(115200);
  Serial0.begin(115200);
  JsonDocument d;
  d["event"] = "boot";
  d["fw"] = FW_VERSION;
  d["reset"] = (int)esp_reset_reason();
  emit(d);
}

void loop() {
  read_port(Serial, usb_line, true);
  if (uart_console) read_port(Serial0, uart_line, false);
  if (!probed) { delay(2); return; }
  buttons_poll(on_button);
  bool down;
  int16_t x, y;
  uint8_t raw[8];
  if (touch_poll(down, x, y, raw)) {
    JsonDocument t;
    t["event"] = "touch";
    t["down"] = down;
    if (x >= 0) { t["x"] = x; t["y"] = y; }
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", raw[i]);
    t["raw"] = hex;
    emit(t);
    if (down && mode == MODE_ARROW) next_orientation();
  }
  delay(1);
}
