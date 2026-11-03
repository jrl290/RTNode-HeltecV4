// buttons.cpp — see buttons.h.
#include <Arduino.h>
#include "buttons.h"

static bool watching[GPIO_COUNT];
static bool busy[GPIO_COUNT];
static uint8_t rest[GPIO_COUNT];     // the level with nothing pressed
static uint8_t stable[GPIO_COUNT];   // the last settled level
static uint32_t changed_at[GPIO_COUNT];
static uint32_t last_scan = 0;
static const uint32_t SETTLE_MS = 30;

void buttons_begin(const bool allowed[GPIO_COUNT]) {
  for (int p = 0; p < GPIO_COUNT; p++) {
    watching[p] = false;
    busy[p] = false;
    if (allowed[p]) pinMode(p, INPUT_PULLUP);
  }
  delay(5);
  for (int p = 0; p < GPIO_COUNT; p++)
    if (allowed[p]) rest[p] = digitalRead(p);
  uint32_t t0 = millis();
  while (millis() - t0 < 200) {
    for (int p = 0; p < GPIO_COUNT; p++)
      if (allowed[p] && digitalRead(p) != rest[p]) busy[p] = true;
    delay(2);
  }
  for (int p = 0; p < GPIO_COUNT; p++) {
    if (!allowed[p]) continue;
    if (busy[p]) {
      pinMode(p, INPUT);
      continue;
    }
    watching[p] = true;
    stable[p] = rest[p];
    changed_at[p] = 0;
  }
}

bool buttons_watching(int p) { return p >= 0 && p < GPIO_COUNT && watching[p]; }
bool buttons_busy(int p) { return p >= 0 && p < GPIO_COUNT && busy[p]; }

void buttons_poll(void (*changed)(int pin, bool down, int level)) {
  uint32_t now = millis();
  if (now - last_scan < 5) return;
  last_scan = now;
  for (int p = 0; p < GPIO_COUNT; p++) {
    if (!watching[p]) continue;
    uint8_t v = digitalRead(p);
    if (v == stable[p]) {
      changed_at[p] = 0;
      continue;
    }
    if (changed_at[p] == 0) {
      changed_at[p] = now;
      continue;
    }
    if (now - changed_at[p] >= SETTLE_MS) {
      stable[p] = v;
      changed_at[p] = 0;
      changed(p, v != rest[p], v);
    }
  }
}
