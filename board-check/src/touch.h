// touch.h — notices a finger on the screen. Reads the touch chip's
// coordinates where its registers are known (FT6x36, CST816, GT911, CHSC6x),
// and otherwise watches its interrupt line.
#pragma once
#include <Wire.h>
#include "board_types.h"

void touch_begin(TwoWire *bus, uint8_t addr, int8_t int_pin);
bool touch_active();
// True when a finger came down or lifted since the last call.
bool touch_poll(bool &down, int16_t &x, int16_t &y, uint8_t raw[8]);
const char *touch_chip_name(uint8_t addr);
