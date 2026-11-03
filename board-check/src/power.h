// power.h — see power.cpp.
#pragma once
#include <Wire.h>
#include "board_types.h"

void power_on(const BoardDef &b);
void power_off(const BoardDef &b);
uint8_t pmu_identify(TwoWire &w);                      // PMU_NONE when nothing answers at 0x34
bool pmu_enable_rails(TwoWire &w, const BoardDef &b);  // false when the board isn't known to need it
