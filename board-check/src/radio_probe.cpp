// radio_probe.cpp — see radio_probe.h.
#include <Arduino.h>
#include <SPI.h>
#include "radio_probe.h"

static SPIClass radio_spi(FSPI);
static const SPISettings SPI_SET(2000000, MSBFIRST, SPI_MODE0);
static RadioDef cur;

// Sub-GHz chips listen at 433 MHz, inside every SX126x and SX127x band
// except the SX1272's (860-1020 MHz). Listening is legal everywhere.
static const uint32_t LISTEN_HZ = 433000000UL;
static const uint32_t LISTEN_HZ_SX1272 = 868000000UL;
static const uint32_t LISTEN_HZ_24G = 2440000000UL;

const char *radio_type_name(uint8_t type) {
  switch (type) {
    case RADIO_SX126X: return "sx126x";
    case RADIO_SX127X: return "sx127x";
    case RADIO_SX128X: return "sx128x";
    case RADIO_LR11XX: return "lr11xx";
    case RADIO_LR2021: return "lr2021";
    default: return "none";
  }
}

static int wait_level(int8_t pin, int level, uint32_t bound_ms) {
  if (pin < 0) return -1;
  uint32_t t0 = millis();
  while (digitalRead(pin) != level) {
    if (millis() - t0 > bound_ms) return -1;
    delayMicroseconds(50);
  }
  return (int)(millis() - t0);
}

static void xfer(const uint8_t *tx, uint8_t *rx, size_t n) {
  radio_spi.beginTransaction(SPI_SET);
  digitalWrite(cur.cs, LOW);
  for (size_t i = 0; i < n; i++) {
    uint8_t b = radio_spi.transfer(tx ? tx[i] : 0x00);
    if (rx) rx[i] = b;
  }
  digitalWrite(cur.cs, HIGH);
  radio_spi.endTransaction();
}

// SX126x, SX128x and LR11xx take a command only while BUSY is low.
static bool cmd(const uint8_t *tx, uint8_t *rx, size_t n, uint32_t bound_ms = 20) {
  if (wait_level(cur.busy, LOW, bound_ms) < 0) return false;
  xfer(tx, rx, n);
  return true;
}

static void reset_chip() {
  pinMode(cur.rst, OUTPUT);
  digitalWrite(cur.rst, LOW);
  delay(2);                       // every family needs at least 100 us
  digitalWrite(cur.rst, HIGH);
}

static void copy_printable(char *dst, const char *src, size_t n) {
  size_t j = 0;
  for (size_t i = 0; i < n && src[i] && j < 23; i++) {
    char c = src[i];
    dst[j++] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  dst[j] = 0;
}

// SetDIO3AsTCXOCtrl / SetTcxoMode voltage codes, 1.6 V to 3.3 V.
static uint8_t tcxo_code(uint8_t dv) {
  static const uint8_t volts[] = {16, 17, 18, 22, 24, 27, 30, 33};
  uint8_t best = 0;
  for (uint8_t i = 0; i < 8; i++)
    if (abs((int)volts[i] - (int)dv) < abs((int)volts[best] - (int)dv)) best = i;
  return best;
}

static void rx_switch(bool on) {
  if (cur.rxen >= 0) { pinMode(cur.rxen, OUTPUT); digitalWrite(cur.rxen, on ? HIGH : LOW); }
  if (cur.txen >= 0) { pinMode(cur.txen, OUTPUT); digitalWrite(cur.txen, LOW); }
}

static void finish_irq(RadioResult &o, bool finished) {
  if (o.irq_ms >= 0) o.irq = 1;
  else o.irq = finished ? 0 : 2;
}

// ------------------------------------------------------------------ SX126x

static bool sx126x_read_reg(uint16_t addr, uint8_t *buf, size_t n) {
  uint8_t tx[20] = {0x1D, (uint8_t)(addr >> 8), (uint8_t)addr, 0x00};
  uint8_t rx[20];
  if (!cmd(tx, rx, 4 + n)) return false;
  memcpy(buf, rx + 4, n);
  return true;
}

static bool probe_sx126x(RadioResult &o) {
  reset_chip();
  o.busy_ms = wait_level(cur.busy, LOW, 50);    // ready a few ms after reset
  if (o.busy_ms < 0) return false;
  char ver[17] = {0};
  if (!sx126x_read_reg(0x0320, (uint8_t *)ver, 16)) return false;
  if (strncmp(ver, "SX126", 5) != 0 && strncmp(ver, "LLCC68", 6) != 0) return false;
  copy_printable(o.chip, ver, 16);
  o.answered = true;

  const uint8_t stby[] = {0x80, 0x00};
  cmd(stby, nullptr, sizeof stby);
  if (cur.tcxo_dv) {
    const uint8_t tcxo[] = {0x97, tcxo_code(cur.tcxo_dv), 0x00, 0x01, 0x40};  // 5 ms start-up
    cmd(tcxo, nullptr, sizeof tcxo);
  }
  // The chip flags XOSC_START_ERR at power-up whenever a TCXO is fitted, as
  // it tries its crystal first: clear the flags, then calibrate and look.
  const uint8_t clr_err[] = {0x07, 0x00, 0x00};
  cmd(clr_err, nullptr, sizeof clr_err);
  const uint8_t cal[] = {0x89, 0x7F};
  cmd(cal, nullptr, sizeof cal);
  const uint8_t errs[] = {0x17, 0x00, 0x00, 0x00};
  uint8_t er[4] = {0};
  cmd(errs, er, sizeof errs, 50);               // calibration: 3.5 ms plus TCXO start-up
  o.errors = (er[2] << 8) | er[3];
  o.osc = (o.errors & 0x0020) ? 0 : 1;          // XOSC_START_ERR
  cmd(clr_err, nullptr, sizeof clr_err);

  // A 10 ms listen that ends in a timeout, signalled on DIO1.
  if (cur.rf_switch) {
    const uint8_t sw[] = {0x9D, 0x01};
    cmd(sw, nullptr, sizeof sw);
  }
  const uint8_t ptype[] = {0x8A, 0x01};
  cmd(ptype, nullptr, sizeof ptype);
  uint32_t frf = (uint32_t)(((uint64_t)LISTEN_HZ << 25) / 32000000ULL);
  const uint8_t freq[] = {0x86, (uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
  cmd(freq, nullptr, sizeof freq);
  const uint8_t irqp[] = {0x08, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00};  // Timeout | RxDone on DIO1
  cmd(irqp, nullptr, sizeof irqp);
  const uint8_t clr[] = {0x02, 0xFF, 0xFF};
  cmd(clr, nullptr, sizeof clr);
  rx_switch(true);
  const uint8_t rx[] = {0x82, 0x00, 0x02, 0x80};  // 640 x 15.625 us = 10 ms
  cmd(rx, nullptr, sizeof rx);
  o.irq_ms = wait_level(cur.irq, HIGH, 100);
  const uint8_t gi[] = {0x12, 0x00, 0x00, 0x00};
  uint8_t gr[4] = {0};
  cmd(gi, gr, sizeof gi);
  finish_irq(o, ((gr[2] << 8) | gr[3]) & 0x0202);
  cmd(clr, nullptr, sizeof clr);
  rx_switch(false);
  const uint8_t sleep[] = {0x84, 0x00};
  cmd(sleep, nullptr, sizeof sleep);
  return true;
}

// ------------------------------------------------------------------ SX127x

static uint8_t sx127x_read(uint8_t reg) {
  uint8_t tx[2] = {(uint8_t)(reg & 0x7F), 0x00}, rx[2];
  xfer(tx, rx, 2);
  return rx[1];
}

static void sx127x_write(uint8_t reg, uint8_t v) {
  uint8_t tx[2] = {(uint8_t)(reg | 0x80), v};
  xfer(tx, nullptr, 2);
}

static bool probe_sx127x(RadioResult &o) {
  reset_chip();
  delay(6);                                       // ready 5 ms after reset
  uint8_t v = sx127x_read(0x42);
  bool sx1272 = (v == 0x22);
  if (v != 0x12 && !sx1272) return false;
  strcpy(o.chip, sx1272 ? "SX1272/3" : "SX1276/7/8/9");
  o.answered = true;

  // A channel activity check: a few milliseconds of listening that ends
  // with CadDone on DIO0. Finishing at all also shows the oscillator runs.
  sx127x_write(0x01, 0x00);                      // sleep, FSK
  sx127x_write(0x01, 0x80);                      // sleep, LoRa
  sx127x_write(0x01, 0x81);                      // standby
  uint32_t hz = sx1272 ? LISTEN_HZ_SX1272 : LISTEN_HZ;
  uint32_t frf = (uint32_t)(((uint64_t)hz << 19) / 32000000ULL);
  sx127x_write(0x06, frf >> 16);
  sx127x_write(0x07, frf >> 8);
  sx127x_write(0x08, frf);
  sx127x_write(0x40, 0x80);                      // DIO0 = CadDone
  sx127x_write(0x11, 0x00);                      // no IRQ masked
  sx127x_write(0x12, 0xFF);                      // clear
  rx_switch(true);
  sx127x_write(0x01, 0x87);                      // CAD
  o.irq_ms = wait_level(cur.irq, HIGH, 100);
  bool done = sx127x_read(0x12) & 0x04;
  finish_irq(o, done);
  o.osc = done ? 1 : 0;
  sx127x_write(0x12, 0xFF);
  rx_switch(false);
  sx127x_write(0x01, 0x80);                      // sleep
  return true;
}

// ------------------------------------------------------------------ SX128x

static bool probe_sx128x(RadioResult &o) {
  reset_chip();
  o.busy_ms = wait_level(cur.busy, LOW, 50);
  if (o.busy_ms < 0) return false;
  uint8_t tx[20] = {0x19, 0x01, 0xF0, 0x00}, rx[20];
  if (!cmd(tx, rx, 20)) return false;
  if (strncmp((char *)rx + 4, "SX128", 5) != 0) return false;
  copy_printable(o.chip, (char *)rx + 4, 16);
  o.answered = true;

  const uint8_t stby[] = {0x80, 0x00};
  cmd(stby, nullptr, sizeof stby);
  const uint8_t ptype[] = {0x8A, 0x01};
  cmd(ptype, nullptr, sizeof ptype);
  uint32_t frf = (uint32_t)(((uint64_t)LISTEN_HZ_24G << 18) / 52000000ULL);
  const uint8_t freq[] = {0x86, (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
  cmd(freq, nullptr, sizeof freq);
  const uint8_t irqp[] = {0x8D, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};  // RxTxTimeout on DIO1
  cmd(irqp, nullptr, sizeof irqp);
  const uint8_t clr[] = {0x97, 0xFF, 0xFF};
  cmd(clr, nullptr, sizeof clr);
  rx_switch(true);
  const uint8_t setrx[] = {0x82, 0x02, 0x00, 0x0A};  // 10 x 1 ms
  cmd(setrx, nullptr, sizeof setrx);
  o.irq_ms = wait_level(cur.irq, HIGH, 100);
  const uint8_t gi[] = {0x15, 0x00, 0x00, 0x00};
  uint8_t gr[4] = {0};
  cmd(gi, gr, sizeof gi);
  bool done = ((gr[2] << 8) | gr[3]) & 0x4000;
  finish_irq(o, done);
  o.osc = done ? 1 : -1;
  cmd(clr, nullptr, sizeof clr);
  rx_switch(false);
  const uint8_t sleep[] = {0x84, 0x00};
  cmd(sleep, nullptr, sizeof sleep);
  return true;
}

// ------------------------------------------------------------------ LR11xx

static bool lr11xx_read(uint16_t op, uint8_t *out, size_t n, uint32_t bound_ms) {
  const uint8_t tx[] = {(uint8_t)(op >> 8), (uint8_t)op};
  if (!cmd(tx, nullptr, sizeof tx)) return false;
  if (wait_level(cur.busy, LOW, bound_ms) < 0) return false;
  xfer(nullptr, out, n);
  return true;
}

static bool probe_lr11xx(RadioResult &o) {
  reset_chip();
  o.busy_ms = wait_level(cur.busy, LOW, 300);   // the LR11xx boots for longer
  if (o.busy_ms < 0) return false;
  uint8_t v[5] = {0};
  if (!lr11xx_read(0x0101, v, sizeof v, 20)) return false;  // GetVersion
  const char *name = v[2] == 0x01 ? "LR1110" : v[2] == 0x02 ? "LR1120" : v[2] == 0x03 ? "LR1121"
                   : v[2] == 0xDF ? "LR11xx bootloader" : nullptr;
  if (!name) return false;
  snprintf(o.chip, sizeof o.chip, "%s fw %u.%u", name, v[3], v[4]);
  o.answered = true;
  if (cur.tcxo_dv) {
    const uint8_t tcxo[] = {0x01, 0x17, tcxo_code(cur.tcxo_dv), 0x00, 0x00, 0xA4};  // 5 ms
    cmd(tcxo, nullptr, sizeof tcxo);
  }
  const uint8_t cal[] = {0x01, 0x0F, 0x3F};
  cmd(cal, nullptr, sizeof cal);
  uint8_t e[3] = {0};
  if (lr11xx_read(0x010D, e, sizeof e, 100)) {    // GetErrors
    o.errors = (e[1] << 8) | e[2];
    o.osc = (o.errors & 0x0020) ? 0 : 1;          // HF_XOSC_START_ERR
  }
  // The IRQ line isn't checked on this family yet: no LR11xx board here to
  // test the listen against.
  const uint8_t sleep[] = {0x01, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00};
  cmd(sleep, nullptr, sizeof sleep);
  return true;
}

// ------------------------------------------------------------------ entry

bool probe_radio(const RadioDef &r, RadioResult &o, bool keep_selected) {
  memset(&o, 0, sizeof o);
  o.type = r.type;
  o.busy_ms = -1;
  o.osc = -1;
  o.irq = -1;
  o.irq_ms = -1;
  bool needs_busy = r.type != RADIO_SX127X;
  if (r.type == RADIO_NONE || r.type == RADIO_LR2021 || r.cs < 0 || r.rst < 0 || r.sck < 0 || r.miso < 0 ||
      r.mosi < 0 || (needs_busy && r.busy < 0))
    return false;

  cur = r;
  pinMode(r.cs, OUTPUT);
  digitalWrite(r.cs, HIGH);
  if (r.busy >= 0) pinMode(r.busy, INPUT);
  if (r.irq >= 0) pinMode(r.irq, INPUT_PULLDOWN);
  radio_spi.begin(r.sck, r.miso, r.mosi, -1);

  bool found = false;
  switch (r.type) {
    case RADIO_SX126X: found = probe_sx126x(o); break;
    case RADIO_SX127X: found = probe_sx127x(o); break;
    case RADIO_SX128X: found = probe_sx128x(o); break;
    case RADIO_LR11XX: found = probe_lr11xx(o); break;
  }
  if (r.irq < 0 && o.irq != -1) o.irq = -1;

  radio_spi.end();
  const int8_t pins[] = {r.sck, r.miso, r.mosi, r.irq, r.busy, r.txen, r.rxen};
  for (int8_t p : pins)
    if (p >= 0) pinMode(p, INPUT);
  bool keep = keep_selected && found;
  pinMode(r.cs, keep ? INPUT_PULLUP : INPUT);
  pinMode(r.rst, keep ? INPUT_PULLUP : INPUT);
  return found;
}
