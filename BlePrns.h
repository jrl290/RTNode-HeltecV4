// ─────────────────────────────────────────────────────────────────────────────
//  BlePrns.h — the Prns native Bluetooth LE protocol, listener side. Used by
//  BleInterface.h.
//
//  The protocol is the work of the Prns authors: Prns
//  (https://github.com/KenAKAFrosty/Prns), its Bluetooth LE interface
//  "PrnsBluetoothAuto", as of Prns 0.3.7 at commit d48e9fc
//  (prns-core/src/interfaces/bluetooth_auto/: handshake.rs, framing.rs,
//  advertisement.rs). Prns has no written spec; that code is the spec.
//  Retichat phones dial it through Reticulum-rust's prns-ble package, which
//  holds the same bytes for the dialer side. It runs on ble-reticulum's
//  service (BleColumba.h) and frames its data the same way.
//
//    CTL …e7   write, notify  Hello → Welcome (or Close)
//    DAT …e8   write, write without response, notify  fragments, both ways
//
//  The central writes a 23-byte Hello to CTL — [01][identity 16][endpoint 2]
//  [L2CAP PSM][link MTU u16 BE][RSSI] — and we answer with our Welcome ([02],
//  the same layout) by notification, or Close [03][reason] if the Hello
//  carries our own identity. A central that finds CTL uses this protocol,
//  otherwise Columba's.
//
//  Copyright (c) 2026 The Prns Authors (the protocol)
//  Copyright (c) 2026 James LaFarge (this implementation for RTNode)
//  SPDX-License-Identifier: MIT OR Apache-2.0
//  The license texts are in licenses/Prns-LICENSE-MIT and
//  licenses/Prns-LICENSE-APACHE. RTNode as a whole is GPL-3.0; this file
//  keeps the Prns license, which GPL-3.0 allows.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef BLE_PRNS_H
#define BLE_PRNS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ble {
namespace prns {

static const char* const CONTROL_UUID = "37145b00-442d-4a94-917f-8f42c5da28e7";
static const char* const DATA_UUID    = "37145b00-442d-4a94-917f-8f42c5da28e8";

// Control messages (handshake.rs).
static const uint8_t  CONTROL_HELLO   = 0x01;
static const uint8_t  CONTROL_WELCOME = 0x02;
static const uint8_t  CONTROL_CLOSE   = 0x03;
static const uint8_t  CLOSE_SELF_CONNECTION = 0x01;
static const size_t   GREETING_LEN    = 23;
static const size_t   IDENTITY_LEN    = 16;
static const uint8_t  ENDPOINT_ESP32[2] = {0x05, 0x00};
static const uint16_t LINK_MTU        = 500;
static const uint8_t  RSSI_UNKNOWN    = 0x80;

// Close [03][reason], at least two bytes.
inline bool is_close(const uint8_t* data, size_t len) {
    return len >= 2 && data[0] == CONTROL_CLOSE;
}

// A whole Hello; its identity is hello_identity().
inline bool is_hello(const uint8_t* data, size_t len) {
    return len >= GREETING_LEN && data[0] == CONTROL_HELLO;
}

inline const uint8_t* hello_identity(const uint8_t* hello) {
    return hello + 1;
}

// Our Welcome: our identity, endpoint ESP32, no L2CAP channel, link MTU 500,
// RSSI unknown.
inline void write_welcome(uint8_t out[GREETING_LEN], const uint8_t identity[IDENTITY_LEN]) {
    out[0] = CONTROL_WELCOME;
    memcpy(out + 1, identity, IDENTITY_LEN);
    memcpy(out + 1 + IDENTITY_LEN, ENDPOINT_ESP32, sizeof(ENDPOINT_ESP32));
    out[19] = 0x00;
    out[20] = LINK_MTU >> 8;
    out[21] = LINK_MTU & 0xFF;
    out[22] = RSSI_UNKNOWN;
}

// The Close for a central that has our own identity.
static const uint8_t CLOSE_SELF[2] = {CONTROL_CLOSE, CLOSE_SELF_CONNECTION};

}  // namespace prns
}  // namespace ble

#endif // BLE_PRNS_H
