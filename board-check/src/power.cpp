// power.cpp — switches on the supplies a board's radio and screen sit
// behind: plain switch pins, and the AXP2101 power chip on LilyGo boards.
#include <Arduino.h>
#include <Wire.h>
#include "power.h"

void power_on(const BoardDef &b) {
  for (const PowerPin &p : b.power) {
    if (p.pin < 0) continue;
    pinMode(p.pin, OUTPUT);
    digitalWrite(p.pin, p.on ? HIGH : LOW);
  }
  delay(20);  // supplies settle well within this on every board in the library
}

void power_off(const BoardDef &b) {
  for (const PowerPin &p : b.power)
    if (p.pin >= 0) pinMode(p.pin, INPUT);
}

// Which AXP2101 outputs each board's own firmware switches on, all at 3.3 V,
// from Meshtastic's Power.cpp. A board not listed keeps the chip's defaults:
// switching on an output at the wrong voltage could harm what it feeds (the
// T-Watch Ultra runs a 1.8 V sensor from ALDO4).
struct Axp2101Rails {
  const char *board;
  uint8_t ldo_mask;  // register 0x90: bit 0-3 ALDO1-4, bit 4-5 BLDO1-2
};
static const Axp2101Rails AXP2101_RAILS[] = {
  {"tbeam-s3-core", 0x3F},
  {"t-watch-s3", 0x3F},
  {"t-watch-ultra", 0x06},  // ALDO2 screen, ALDO3 radio
};

static bool reg_read(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t &v) {
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, (uint8_t)1) != 1) return false;
  v = w.read();
  return true;
}

static bool reg_write(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t v) {
  w.beginTransmission(addr);
  w.write(reg);
  w.write(v);
  return w.endTransmission() == 0;
}

uint8_t pmu_identify(TwoWire &w) {
  uint8_t id;
  if (!reg_read(w, 0x34, 0x03, id)) return PMU_NONE;
  if (id == 0x4A) return PMU_AXP2101;
  if (id == 0x03) return PMU_AXP192;
  return PMU_NONE;
}

bool pmu_enable_rails(TwoWire &w, const BoardDef &b) {
  for (const Axp2101Rails &r : AXP2101_RAILS) {
    if (strcmp(r.board, b.id) != 0) continue;
    for (uint8_t i = 0; i < 6; i++) {
      uint8_t v;  // bits 0-4: (mV - 500) / 100; the top bits are kept
      if ((r.ldo_mask & (1 << i)) && reg_read(w, 0x34, 0x92 + i, v))
        reg_write(w, 0x34, 0x92 + i, (v & 0xE0) | ((3300 - 500) / 100));
    }
    uint8_t en;
    if (!reg_read(w, 0x34, 0x90, en)) return false;
    bool ok = reg_write(w, 0x34, 0x90, en | r.ldo_mask);
    delay(20);
    return ok;
  }
  return false;
}
