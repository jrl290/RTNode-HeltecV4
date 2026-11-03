// touch.cpp — see touch.h.
#include <Arduino.h>
#include "touch.h"

static TwoWire *bus = nullptr;
static uint8_t addr = 0;
static int8_t int_pin = -1;
static bool was_down = false;
static uint32_t last_poll = 0;

const char *touch_chip_name(uint8_t a) {
  switch (a) {
    case 0x38: return "FT6x36";
    case 0x15: return "CST816";
    case 0x5D: case 0x14: return "GT911";
    case 0x2E: return "CHSC6x";
    case 0x1A: return "CST328";
    case 0x5A: return "CST226";
    default: return "unknown";
  }
}

void touch_begin(TwoWire *b, uint8_t a, int8_t ip) {
  bus = b;
  addr = a;
  int_pin = ip;
  if (int_pin >= 0) pinMode(int_pin, INPUT_PULLUP);
}

bool touch_active() { return bus != nullptr || int_pin >= 0; }

static bool read_regs(uint16_t reg, bool wide, uint8_t *buf, uint8_t n) {
  bus->beginTransmission(addr);
  if (wide) bus->write(reg >> 8);
  bus->write((uint8_t)reg);
  if (bus->endTransmission(false) != 0) return false;
  if (bus->requestFrom(addr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = bus->read();
  return true;
}

static bool read_chip(bool &down, int16_t &x, int16_t &y, uint8_t raw[8]) {
  memset(raw, 0, 8);
  switch (addr) {
    case 0x38:
    case 0x15:  // FT6x36 and CST816 share this layout; a sleeping CST816 doesn't answer
      if (!read_regs(0x00, false, raw, 7)) { down = false; return true; }
      down = (raw[2] & 0x0F) > 0;
      x = ((raw[3] & 0x0F) << 8) | raw[4];
      y = ((raw[5] & 0x0F) << 8) | raw[6];
      return true;
    case 0x5D:
    case 0x14: {
      uint8_t st;
      if (!read_regs(0x814E, true, &st, 1)) return false;
      raw[0] = st;
      if (!(st & 0x80)) { down = was_down; return true; }  // no new data
      down = (st & 0x0F) > 0;
      if (down && read_regs(0x8150, true, raw + 1, 4)) {
        x = raw[1] | (raw[2] << 8);
        y = raw[3] | (raw[4] << 8);
      }
      bus->beginTransmission(addr);
      bus->write(0x81);
      bus->write(0x4E);
      bus->write(0x00);
      bus->endTransmission();
      return true;
    }
    case 0x2E:  // CHSC6x: read straight away, no register address
      if (bus->requestFrom(addr, (uint8_t)5) != 5) { down = false; return true; }
      for (uint8_t i = 0; i < 5; i++) raw[i] = bus->read();
      down = raw[0] == 0x01;
      x = raw[2];
      y = raw[4];
      return true;
    default:
      return false;
  }
}

bool touch_poll(bool &down, int16_t &x, int16_t &y, uint8_t raw[8]) {
  if (!touch_active() || millis() - last_poll < 20) return false;
  last_poll = millis();
  x = y = -1;
  bool chip = bus && read_chip(down, x, y, raw);
  if (!chip) {
    if (int_pin < 0) return false;
    memset(raw, 0, 8);
    down = digitalRead(int_pin) == LOW;
  }
  if (down == was_down) return false;
  was_down = down;
  return true;
}
