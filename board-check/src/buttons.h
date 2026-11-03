// buttons.h — finds buttons by watching pins, never driving them: each
// allowed pin is an input with its pull-up on, and a pin that changes level
// for a while is reported as a press. Pins that keep changing by themselves
// (a GPS's serial line, say) are set aside as busy before watching starts.
#pragma once
#include <stdint.h>

static const int GPIO_COUNT = 49;

// allowed[p]: the pin may be watched. Takes 200 ms to find busy pins.
void buttons_begin(const bool allowed[GPIO_COUNT]);
bool buttons_watching(int pin);
bool buttons_busy(int pin);
// Calls back once per settled change: down is true when the pin left its resting level.
void buttons_poll(void (*changed)(int pin, bool down, int level));
