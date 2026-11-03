// radio_probe.h — looks for a LoRa radio on one set of pins: reads its ID
// over SPI, checks its oscillator, and proves its IRQ line with a short
// listen that ends in a timeout. Nothing is ever transmitted.
//
// Written from the Semtech datasheets (SX126x, SX127x, SX128x, LR11xx); the
// command and register numbers were checked against RadioLib's (MIT).
// Every wait has a fixed bound from the datasheet, and nothing is retried:
// a radio that doesn't answer once isn't on those pins.
#pragma once
#include "board_types.h"

struct RadioResult {
  bool answered;     // the chip's ID read back
  uint8_t type;
  char chip[24];     // the chip's version string, or its name
  int16_t busy_ms;   // how long BUSY took to fall after reset; -1 if it never did
  uint16_t errors;   // the chip's error flags after calibration (SX126x, LR11xx)
  int8_t osc;        // 1 the oscillator runs, 0 it failed, -1 not checked
  int8_t irq;        // 1 the IRQ line rose, 0 the radio finished but the line
                     // stayed low, 2 the radio never finished, -1 not checked
  int16_t irq_ms;
};

// Pins are set up, used and handed back to input before this returns.
// keep_selected: leave CS and reset pulled up afterwards (the board's radio,
// once found), so a floating CS can't wake it.
bool probe_radio(const RadioDef &r, RadioResult &out, bool keep_selected);

const char *radio_type_name(uint8_t type);
