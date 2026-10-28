// ─────────────────────────────────────────────────────────────────────────────
//  BleColumba.h — ble-reticulum's Bluetooth LE protocol, peripheral side, as
//  Columba phones speak it. Used by BleInterface.h.
//
//  The protocol is torlando-tech's: ble-reticulum
//  (https://github.com/torlando-tech/ble-reticulum), protocol v2.2 (GATT
//  service, characteristics, identity, fragments) and v0.3.0 (the role
//  advertisement), at commit 07d9413, the one Columba 2.2.6 pins. The
//  keepalive is Columba's (https://github.com/torlando-tech/columba, MPL-2.0):
//  RTNode follows it so that Columba keeps the link; no Columba code is
//  used. Prns's native protocol (BlePrns.h) shares the service, the identity
//  length and the fragment header.
//
//    service   37145b00-442d-4a94-917f-8f42c5da28e3
//    TX  …e4   read, notify   us → central, fragments
//    RX  …e5   write          central → us, identity then fragments
//    ID  …e6   read           our 16-byte transport identity hash
//
//  A central's first write of exactly 16 bytes to RX is its identity; a
//  1-byte 0x00 is a keepalive, and both sides send one every 15 s when idle
//  (Columba 1.x, on reticulum-kt, drops a peer it has heard nothing from for
//  45 s). Data is
//  framed as [type][sequence u16 BE][total u16 BE][data], type 01 start, 02
//  continue, 03 end; a one-fragment packet is 01 with total 1.
//
//  Copyright (c) 2025 torlando-tech (the ble-reticulum protocol)
//  Copyright (c) 2026 James LaFarge (this implementation for RTNode)
//  SPDX-License-Identifier: MIT
//  The license text is in licenses/ble-reticulum-LICENSE. RTNode as a whole
//  is GPL-3.0; this file keeps ble-reticulum's license, which GPL-3.0 allows.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef BLE_COLUMBA_H
#define BLE_COLUMBA_H

#include <stddef.h>
#include <stdint.h>

namespace ble {
namespace columba {

static const char* const SERVICE_UUID  = "37145b00-442d-4a94-917f-8f42c5da28e3";
static const char* const TX_UUID       = "37145b00-442d-4a94-917f-8f42c5da28e4";
static const char* const RX_UUID       = "37145b00-442d-4a94-917f-8f42c5da28e5";
static const char* const IDENTITY_UUID = "37145b00-442d-4a94-917f-8f42c5da28e6";

static const size_t   IDENTITY_LEN = 16;
static const uint32_t BITRATE_GUESS = 700000;        // BLEInterface.BITRATE_GUESS
static const uint8_t  KEEPALIVE = 0x00;
static const uint32_t KEEPALIVE_EVERY_MS = 15000;    // Columba's CONNECTION_KEEPALIVE_INTERVAL_MS

// v0.3.0 manufacturer data: company 0xFFFF (little-endian), version 3, then
// the flags. Peripheral-only, so that centrals connect to us and RTNode never
// has to connect out.
static const uint8_t ADV_FLAG_PERIPHERAL_ONLY = 0x01;
static const uint8_t ADV_MANUFACTURER_DATA[4] = {0xFF, 0xFF, 0x03, ADV_FLAG_PERIPHERAL_ONLY};

static const uint8_t FRAG_START    = 0x01;
static const uint8_t FRAG_CONTINUE = 0x02;
static const uint8_t FRAG_END      = 0x03;
static const size_t  FRAG_HEADER   = 5;

inline bool is_keepalive(const uint8_t* data, size_t len) {
    return len == 1 && data[0] == KEEPALIVE;
}

// Before a central is identified, a write of exactly IDENTITY_LEN bytes to
// RX is its identity.
inline bool is_identity(size_t len) {
    return len == IDENTITY_LEN;
}

struct Fragment {
    uint8_t  type;
    uint16_t seq;
    uint16_t total;
};

// The header of a written fragment. False if the write is too short to hold
// one, or the header is not a valid fragment of a packet.
inline bool read_fragment(const uint8_t* data, size_t len, Fragment* out) {
    if (len < FRAG_HEADER) return false;
    out->type  = data[0];
    out->seq   = ((uint16_t)data[1] << 8) | data[2];
    out->total = ((uint16_t)data[3] << 8) | data[4];
    return out->type >= FRAG_START && out->type <= FRAG_END && out->total != 0 && out->seq < out->total;
}

// Fragment `seq` of `total`; its data follows the header.
inline void write_fragment_header(uint8_t* out, uint16_t seq, uint16_t total) {
    out[0] = seq == 0 ? FRAG_START : (seq == total - 1 ? FRAG_END : FRAG_CONTINUE);
    out[1] = seq >> 8;
    out[2] = seq & 0xFF;
    out[3] = total >> 8;
    out[4] = total & 0xFF;
}

}  // namespace columba
}  // namespace ble

#endif // BLE_COLUMBA_H
