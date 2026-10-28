// ─────────────────────────────────────────────────────────────────────────────
//  BleInterface.h — Bluetooth LE peers for RTNode (PERFORMANCE_STRATEGY.md,
//  order of work step 7). V4 firewall builds with RTNODE_BLE.
//
//  A GATT peripheral speaking two protocols on one service, and advertising
//  ble-reticulum v0.3.0's peripheral-only flag so that centrals connect to
//  us and RTNode never has to connect out. The protocols are other
//  projects' work, each in its own file under its authors' license:
//
//    BleColumba.h  ble-reticulum's protocol, as Columba phones speak it
//                  (torlando-tech, MIT): the service, TX/RX/ID, fragments
//    BlePrns.h     the Prns native protocol (The Prns Authors, MIT OR
//                  Apache-2.0): CTL/DAT, Hello/Welcome/Close
//
//  This file is RTNode's own (GPL-3.0): the slots, NimBLE, the queues, the
//  link speed and the Reticulum interfaces.
//
//  Each peer holds one of BLE_SLOTS fixed slots, and each slot is its own
//  Reticulum interface, registered at boot and online while its peer is
//  identified. The slots are trusted local interfaces, exactly like LoRa. When a peer leaves, the paths, links and reverse entries
//  through its slot are forgotten, so the slot's next peer never receives
//  them.
//
//  NimBLE runs its host in its own task on core 0. Its callbacks only copy
//  what happened into queues allocated at boot; loop() does everything else.
//  A write that finds the write queue full waits in the host task until
//  loop() makes room, and nothing is dropped: while it waits, NimBLE takes
//  no more data from the controller, the controller holds the phone's next
//  packets back, and the phone's own flow control pauses its writes. NimBLE
//  holds no lock while it runs a write callback, and loop()'s own NimBLE
//  calls never wait for the host task. Outgoing fragments go out as
//  notifications from loop(), up to FRAGMENTS_PER_PASS per slot per pass.
//  When NimBLE is out of buffers (ENOMEM) the fragment waits for the next
//  pass: that is the only backpressure signal NimBLE 1.4 gives for
//  notifications, and each occurrence is counted.
//
//  Link speed. Once a peer is identified we ask for the fastest link it
//  will take:
//    - the largest link-layer packets (251 bytes; a 500-byte packet then
//      crosses in 2-3 air packets instead of 19);
//    - the 2M PHY (Bluetooth 5 phones; a 4.2 phone stays on 1M);
//    - 15 ms connection events, the shortest Apple accepts from an accessory.
//  The phone decides; each outcome is logged. Prns writes may come with or
//  without response on DAT: a phone that sends without response puts
//  several fragments in one connection event instead of one per round trip.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef BLE_INTERFACE_H
#define BLE_INTERFACE_H

#if defined(FIREWALL_MODE) && defined(RTNODE_BLE)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <vector>
#include <Interface.h>
#include <Transport.h>
#include <Identity.h>
#include <Log.h>
#include "ResourceMonitor.h"
// Config.h defines MTU (508), which NimBLE's headers use as a parameter name.
#pragma push_macro("MTU")
#undef MTU
#include <NimBLEDevice.h>
#pragma pop_macro("MTU")
#include "BleColumba.h"
#include "BlePrns.h"

namespace ble {

static const size_t   IDENTITY_LEN  = columba::IDENTITY_LEN;
static_assert(prns::IDENTITY_LEN == IDENTITY_LEN, "both protocols identify a peer by 16 bytes");
static const size_t   VALUE_MAX     = 512;   // largest attribute value
static const uint16_t PACKET_MAX    = 512;   // Reticulum packets are ≤ 500
static const int      SLOTS         = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
// Packets queued per slot: more than RNS's largest Resource window (75
// parts, Resource.WINDOW_MAX_FAST), which can arrive from the backbone far
// faster than Bluetooth carries it. In PSRAM, 49 KB per slot.
static const int      TX_DEPTH      = 96;
static const int      CONTROL_DEPTH = 32;
static const int      WRITE_DEPTH   = 128;   // in PSRAM, 66 KB
static const int      WRITES_PER_PASS    = 32;
static const int      FRAGMENTS_PER_PASS = 8;    // per slot
// Link parameters asked of each identified peer. Apple's Accessory Design
// Guidelines, "Connection Parameters": the interval a multiple of 15 ms
// (min == max == 15 ms allowed), and a supervision timeout of 2-6 s.
static const uint16_t FAST_INTERVAL = 12;    // 15 ms, in 1.25 ms units
// Asks for it: at identification, and once more if the phone slows the link
// again afterwards (Android returns to its own 45 ms default when service
// discovery ends). Past that the phone has decided.
static const uint8_t  FAST_ASKS_MAX = 2;
static const uint16_t TIMEOUT_MIN   = 200;   // 2 s, in 10 ms units
static const uint16_t TIMEOUT_MAX   = 600;   // 6 s
static const uint16_t LL_OCTETS_MAX = 251;   // Data Length Extension's largest payload
static const uint16_t LL_TIME_MAX   = 2120;  // µs for 251 octets on the 1M PHY
static const uint16_t ADV_INTERVAL_MIN = 160;   // 100 ms, in 0.625 ms units
static const uint16_t ADV_INTERVAL_MAX = 240;   // 150 ms

// ─── Events from the NimBLE host task ────────────────────────────────────────
enum ControlKind : uint8_t { CONTROL_CONNECT, CONTROL_DISCONNECT, CONTROL_MTU, CONTROL_PARAMS, CONTROL_PHY };
struct ControlEvent {
    uint8_t  kind;
    uint16_t conn;
    uint16_t value;    // MTU, disconnect reason, or PHYs (TX << 8 | RX)
    int16_t  status;   // of a connection parameter or PHY update
};
enum Channel : uint8_t { CHANNEL_COLUMBA_RX, CHANNEL_CONTROL, CHANNEL_DATA };
struct WriteEvent {
    uint16_t conn;
    uint16_t len;
    uint8_t  channel;
    uint8_t  data[VALUE_MAX];
};

static QueueHandle_t control_queue = nullptr;
static QueueHandle_t write_queue   = nullptr;
static WriteEvent    host_write;     // NimBLE host task only
static WriteEvent    loop_write;     // loop() only
// Written by the host task; read by loop(). A write wait is a write that
// found the write queue full and waited for room.
static volatile uint32_t control_drops = 0, write_waits = 0;

// ─── A slot: one peer, one Reticulum interface ──────────────────────────────
class BlePeerInterface : public RNS::InterfaceImpl {
public:
    explicit BlePeerInterface(const char* name) : RNS::InterfaceImpl(name) {
        _IN = true;
        _OUT = true;
        _HW_MTU = 500;
        _FIXED_MTU = true;
        _bitrate = columba::BITRATE_GUESS;   // Prns's figure too
    }

    enum Protocol : uint8_t { PROTOCOL_COLUMBA, PROTOCOL_PRNS };

    // Peer state, owned by loop().
    bool     used = false;
    bool     identified = false;
    Protocol protocol = PROTOCOL_COLUMBA;
    uint16_t conn = 0;
    uint16_t att_mtu = 23;
    uint16_t interval = 0;        // connection interval, 1.25 ms units
    uint16_t latency = 0;
    uint16_t timeout = 0;         // supervision timeout, 10 ms units
    uint8_t  tx_phy = 1, rx_phy = 1;
    uint8_t  fast_asks = 0;       // 15 ms intervals asked for on this connection
    bool     fast_refused = false;
    uint8_t  identity[IDENTITY_LEN] = {};
    uint32_t connected_ms = 0;
    uint32_t last_sent_ms = 0;   // last notification queued to this peer

    // Reassembly of the fragments the peer writes.
    uint8_t  rx[PACKET_MAX];
    uint16_t rx_len = 0, rx_total = 0, rx_next = 0;
    bool     rx_active = false;

    // Packets waiting to go out (TX_DEPTH of them, in PSRAM, from start()),
    // and how far the first one has got.
    struct Packet { uint16_t len; uint8_t data[PACKET_MAX]; };
    Packet*  tx = nullptr;
    uint8_t  tx_head = 0, tx_count = 0;
    uint16_t tx_seq = 0, tx_total = 0;

    // Counters since boot.
    uint32_t rx_packets = 0, rx_bytes = 0, tx_packets = 0, tx_bytes = 0;
    uint32_t tx_drops = 0, bad_fragments = 0, keepalives = 0, before_identity = 0;
    uint32_t enomem = 0, notify_errors = 0, peers = 0, prns_peers = 0, bad_control = 0;
    uint32_t keepalives_sent = 0;

    void attach(uint16_t handle) {
        used = true;
        identified = false;
        conn = handle;
        att_mtu = 23;
        interval = latency = timeout = 0;
        tx_phy = rx_phy = 1;
        fast_asks = 0;
        fast_refused = false;
        connected_ms = millis();
        rx_active = false;
        tx_head = tx_count = 0;
        tx_seq = tx_total = 0;
    }

    void release() {
        used = false;
        identified = false;
        _online = false;
        rx_active = false;
        tx_head = tx_count = 0;
        tx_seq = tx_total = 0;
        // The next peer is a new source for ingress control; its held
        // announces go with it, as a detached spawned interface's do.
        _ingress = RNS::IngressState();
    }

    void set_online(Protocol which) {
        identified = true;
        protocol = which;
        last_sent_ms = millis();
        _online = true;
        peers++;
        if (which == PROTOCOL_PRNS) prns_peers++;
    }

    std::string label() const { return toString(); }

    void deliver(const uint8_t* data, size_t len) {
        rx_packets++;
        rx_bytes += len;
        res::Timed res_rx(res::RX);
        handle_incoming(RNS::Bytes(data, len));
    }

protected:
    virtual void send_outgoing(const RNS::Bytes& data) override {
        if (!_online || !tx) return;
        if (tx_count >= TX_DEPTH || data.size() > PACKET_MAX) {
            if (tx_drops++ == 0) {
                Serial.printf("[BLE] %s: outgoing queue full, dropping (counted)\r\n", toString().c_str());
            }
            return;
        }
        Packet& packet = tx[(tx_head + tx_count) % TX_DEPTH];
        packet.len = data.size();
        memcpy(packet.data, data.data(), data.size());
        tx_count++;
        InterfaceImpl::handle_outgoing(data);
    }
};

static BlePeerInterface*     slots[SLOTS] = {};
static std::vector<RNS::Interface> slot_interfaces;   // Transport's handles; they own the slots
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* control_char = nullptr;
static NimBLECharacteristic* data_char = nullptr;
static uint8_t               our_identity[IDENTITY_LEN];
static uint8_t               our_address[6];   // static random, most significant byte first
static uint32_t              orphan_writes = 0, self_connections = 0, replaced = 0;
static uint32_t              last_report_ms = 0;
static bool                  started = false;

static BlePeerInterface* slot_for(uint16_t conn, int* index = nullptr) {
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i] && slots[i]->used && slots[i]->conn == conn) {
            if (index) *index = i;
            return slots[i];
        }
    }
    return nullptr;
}

static std::string short_identity(const uint8_t* identity) {
    char text[9];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x", identity[0], identity[1], identity[2], identity[3]);
    return text;
}

// ─── NimBLE host task: copy and hand over, nothing else ─────────────────────
static int on_gap_event(ble_gap_event* event, void* arg) {
    ControlEvent control = {};
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) return 0;
            control.kind = CONTROL_CONNECT;
            control.conn = event->connect.conn_handle;
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            control.kind = CONTROL_DISCONNECT;
            control.conn = event->disconnect.conn.conn_handle;
            control.value = event->disconnect.reason;
            break;
        case BLE_GAP_EVENT_MTU:
            control.kind = CONTROL_MTU;
            control.conn = event->mtu.conn_handle;
            control.value = event->mtu.value;
            break;
        case BLE_GAP_EVENT_CONN_UPDATE:
            control.kind = CONTROL_PARAMS;
            control.conn = event->conn_update.conn_handle;
            control.status = event->conn_update.status;
            break;
        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
            control.kind = CONTROL_PHY;
            control.conn = event->phy_updated.conn_handle;
            control.value = (uint16_t)(event->phy_updated.tx_phy << 8) | event->phy_updated.rx_phy;
            control.status = event->phy_updated.status;
            break;
        default:
            return 0;
    }
    if (xQueueSend(control_queue, &control, 0) != pdTRUE) control_drops++;
    return 0;
}

class WriteCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit WriteCallbacks(Channel which) : channel(which) {}
    void onWrite(NimBLECharacteristic* characteristic, ble_gap_conn_desc* desc) override {
        NimBLEAttValue value = characteristic->getValue();
        host_write.conn = desc->conn_handle;
        host_write.channel = channel;
        host_write.len = value.length() < VALUE_MAX ? value.length() : VALUE_MAX;
        memcpy(host_write.data, value.data(), host_write.len);
        if (xQueueSend(write_queue, &host_write, 0) != pdTRUE) {
            // Full: wait here for loop() to make room (see the top of this file).
            write_waits++;
            xQueueSend(write_queue, &host_write, portMAX_DELAY);
        }
    }
private:
    Channel channel;
};
static WriteCallbacks rx_callbacks(CHANNEL_COLUMBA_RX);
static WriteCallbacks control_callbacks(CHANNEL_CONTROL);
static WriteCallbacks data_callbacks(CHANNEL_DATA);

// ─── Boot ────────────────────────────────────────────────────────────────────
// Before Reticulum starts: the slots' interfaces, offline until a peer comes.
inline void register_interfaces() {
    static const char* names[] = {"BLE1", "BLE2", "BLE3", "BLE4", "BLE5", "BLE6", "BLE7", "BLE8"};
    static_assert(SLOTS <= 8, "name the extra slots");
    slot_interfaces.reserve(SLOTS);
    for (int i = 0; i < SLOTS; i++) {
        slots[i] = new BlePeerInterface(names[i]);
        slot_interfaces.emplace_back(slots[i]);
        slot_interfaces[i].mode(RNS::Type::Interface::MODE_FULL);
        RNS::Transport::register_interface(slot_interfaces[i]);
        // Trusted and local, exactly like LoRa (James, 2026-09-28).
        RNS::Transport::register_local_client_interface(slot_interfaces[i]);
    }
}

// Once Transport has its identity: the GATT service and advertising.
inline void start() {
    if (started || !slots[0] || !RNS::Transport::identity()) return;
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    control_queue = xQueueCreate(CONTROL_DEPTH, sizeof(ControlEvent));
    // The write queue's storage and each slot's outgoing packets go in
    // PSRAM; only loop() and the NimBLE task touch them, never an interrupt.
    static StaticQueue_t write_queue_state;
    uint8_t* write_storage = (uint8_t*)heap_caps_malloc(WRITE_DEPTH * sizeof(WriteEvent), MALLOC_CAP_SPIRAM);
    bool have_memory = write_storage != nullptr;
    for (int i = 0; i < SLOTS && have_memory; i++) {
        slots[i]->tx = (BlePeerInterface::Packet*)heap_caps_malloc(
            TX_DEPTH * sizeof(BlePeerInterface::Packet), MALLOC_CAP_SPIRAM);
        have_memory = slots[i]->tx != nullptr;
    }
    if (!have_memory) {
        // No PSRAM (the caller checks, firewall_ble_has_psram()), or none
        // left: NimBLE's own pools live there too and would assert.
        Serial.println("[BLE] not started: no PSRAM for its memory");
        heap_caps_free(write_storage);
        for (int i = 0; i < SLOTS; i++) {
            heap_caps_free(slots[i]->tx);
            slots[i]->tx = nullptr;
        }
        return;
    }
    write_queue = xQueueCreateStatic(WRITE_DEPTH, sizeof(WriteEvent), write_storage, &write_queue_state);
    const RNS::Bytes& identity_hash = RNS::Transport::identity().hash();
    memcpy(our_identity, identity_hash.data(), IDENTITY_LEN);

    NimBLEDevice::init("");
    // Our own address: static random, fixed per chip (a hash of its MAC), as
    // Prns does. The public address is the one this board had under stock
    // RNode firmware, and a phone once paired with that keeps using its
    // cached RNode service table ("Reticulum service not found"). A static
    // random address has its top two bits set, so it also sorts high, and
    // v2.2 peers that decide by address comparison dial us.
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        RNS::Bytes seed(mac, sizeof(mac));
        seed.append((const uint8_t*)"rtnode-ble", 10);
        RNS::Bytes digest = RNS::Identity::full_hash(seed);
        memcpy(our_address, digest.data(), sizeof(our_address));
        our_address[0] |= 0xC0;
        uint8_t little_endian[6];
        for (int i = 0; i < 6; i++) little_endian[i] = our_address[5 - i];
        ble_hs_id_set_rnd(little_endian);
        NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    }
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setCustomGapHandler(on_gap_event);

    NimBLEServer* server = NimBLEDevice::createServer();
    NimBLEService* service = server->createService(columba::SERVICE_UUID);
    // Columba's characteristics (BleColumba.h).
    tx_char = service->createCharacteristic(columba::TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx_char =
        service->createCharacteristic(columba::RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx_char->setCallbacks(&rx_callbacks);
    NimBLECharacteristic* id_char = service->createCharacteristic(columba::IDENTITY_UUID, NIMBLE_PROPERTY::READ);
    id_char->setValue(our_identity, IDENTITY_LEN);
    // Prns's (BlePrns.h).
    control_char = service->createCharacteristic(prns::CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    control_char->setCallbacks(&control_callbacks);
    data_char = service->createCharacteristic(prns::DATA_UUID,
                                              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    data_char->setCallbacks(&data_callbacks);
    service->start();

    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.setCompleteServices(NimBLEUUID(columba::SERVICE_UUID));
    data.setManufacturerData(std::string((const char*)columba::ADV_MANUFACTURER_DATA,
                                         sizeof(columba::ADV_MANUFACTURER_DATA)));
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setAdvertisementData(data);
    advertising->setScanResponse(false);
    advertising->setMinInterval(ADV_INTERVAL_MIN);
    advertising->setMaxInterval(ADV_INTERVAL_MAX);
    advertising->start();

    started = true;
    last_report_ms = millis();
    char address_text[18];
    snprintf(address_text, sizeof(address_text), "%02x:%02x:%02x:%02x:%02x:%02x", our_address[0], our_address[1],
             our_address[2], our_address[3], our_address[4], our_address[5]);
    Serial.printf("[BLE] up: %d slots, identity %s, address %s (random); internal %u -> %u (%+d), "
                  "psram %u -> %u (%+d)\r\n",
                  SLOTS, short_identity(our_identity).c_str(), address_text,
                  (unsigned)internal_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (int)internal_before,
                  (unsigned)psram_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (int)psram_before);
}

// ─── loop(): peers come and go ───────────────────────────────────────────────
// The link's connection parameters as they stand.
static void read_params(BlePeerInterface* slot) {
    ble_gap_conn_desc desc;
    if (ble_gap_conn_find(slot->conn, &desc) != 0) return;
    slot->interval = desc.conn_itvl;
    slot->latency = desc.conn_latency;
    slot->timeout = desc.supervision_timeout;
}

static void log_params(BlePeerInterface* slot, const char* what) {
    Serial.printf("[BLE] %s: %s: interval %u.%02u ms, latency %u, supervision timeout %u ms\r\n",
                  slot->label().c_str(), what, (unsigned)(slot->interval * 125 / 100),
                  (unsigned)(slot->interval * 125 % 100), (unsigned)slot->latency, (unsigned)slot->timeout * 10);
}

static void on_connect(uint16_t conn) {
    for (int i = 0; i < SLOTS; i++) {
        if (!slots[i]->used) {
            slots[i]->attach(conn);
            read_params(slots[i]);
            log_params(slots[i], "connected");
            break;
        }
    }
    int used = 0;
    for (int i = 0; i < SLOTS; i++) used += slots[i]->used ? 1 : 0;
    // Advertising stops on connect; keep it up while a slot is free.
    if (used < SLOTS) NimBLEDevice::startAdvertising();
}

static void on_disconnect(uint16_t conn, uint16_t reason) {
    int index = -1;
    BlePeerInterface* slot = slot_for(conn, &index);
    if (!slot) return;
    size_t forgotten = 0;
    if (slot->identified) {
        forgotten = RNS::Transport::forget_interface_routes(slot_interfaces[index]);
        Serial.printf("[BLE] %s: peer %s left (reason 0x%x) after %lus; forgot %u routes through it\r\n",
                      slot->label().c_str(), short_identity(slot->identity).c_str(), reason,
                      (unsigned long)((millis() - slot->connected_ms) / 1000), (unsigned)forgotten);
    }
    slot->release();
}

static bool notify(uint16_t conn, NimBLECharacteristic* characteristic, const uint8_t* data, size_t len) {
    os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return false;
    return ble_gattc_notify_custom(conn, characteristic->getHandle(), om) == 0;   // consumes om
}

// A 15 ms interval, when the link runs slower (FAST_ASKS_MAX). A peer that
// already runs 15 ms events or faster (Android asks for 11.25-15 ms itself)
// is not slowed down; one that refused is not asked again.
static void ask_fast_interval(BlePeerInterface* slot) {
    if (slot->interval <= FAST_INTERVAL || slot->fast_refused || slot->fast_asks >= FAST_ASKS_MAX) return;
    slot->fast_asks++;
    ble_gap_upd_params wanted = {};
    wanted.itvl_min = FAST_INTERVAL;
    wanted.itvl_max = FAST_INTERVAL;
    wanted.latency = 0;
    // The phone's own supervision timeout, brought into Apple's range.
    wanted.supervision_timeout = slot->timeout < TIMEOUT_MIN ? TIMEOUT_MIN
                               : slot->timeout > TIMEOUT_MAX ? TIMEOUT_MAX : slot->timeout;
    int rc = ble_gap_update_params(slot->conn, &wanted);
    Serial.printf("[BLE] %s: asked for a 15 ms interval (rc %d)\r\n", slot->label().c_str(), rc);
}

// Asks for the fastest link the peer will take (see the top of this file).
// Each answer arrives as its own event and is logged there.
static void request_fast_link(BlePeerInterface* slot) {
    int packets = ble_gap_set_data_len(slot->conn, LL_OCTETS_MAX, LL_TIME_MAX);
    int phy = ble_gap_set_prefered_le_phy(slot->conn, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
                                          BLE_GAP_LE_PHY_CODED_ANY);
    Serial.printf("[BLE] %s: asked for %u-byte link-layer packets (rc %d) and the 2M PHY (rc %d)\r\n",
                  slot->label().c_str(), (unsigned)LL_OCTETS_MAX, packets, phy);
    ask_fast_interval(slot);
}

// The central's identity: its first 16-byte write to RX (Columba), or the
// identity in its Hello (Prns).
static void on_identity(int index, const uint8_t* identity, BlePeerInterface::Protocol protocol) {
    BlePeerInterface* slot = slots[index];
    if (memcmp(identity, our_identity, IDENTITY_LEN) == 0) {
        self_connections++;
        if (protocol == BlePeerInterface::PROTOCOL_PRNS) {
            notify(slot->conn, control_char, prns::CLOSE_SELF, sizeof(prns::CLOSE_SELF));
        }
        NimBLEDevice::getServer()->disconnect(slot->conn);
        return;
    }
    // The same peer on another slot: it reconnected (Android rotates its
    // address) and the old link is gone from its side. Keep the new one.
    for (int i = 0; i < SLOTS; i++) {
        if (i != index && slots[i]->used && slots[i]->identified &&
            memcmp(slots[i]->identity, identity, IDENTITY_LEN) == 0) {
            replaced++;
            NimBLEDevice::getServer()->disconnect(slots[i]->conn);
        }
    }
    memcpy(slot->identity, identity, IDENTITY_LEN);
    slot->set_online(protocol);
    Serial.printf("[BLE] %s: %s peer %s identified, MTU %u\r\n", slot->label().c_str(),
                  protocol == BlePeerInterface::PROTOCOL_PRNS ? "Prns" : "Columba",
                  short_identity(identity).c_str(), (unsigned)slot->att_mtu);
    read_params(slot);
    request_fast_link(slot);
}

// A Prns Hello, answered with our Welcome (BlePrns.h).
static void on_control(int index, const uint8_t* data, size_t len) {
    BlePeerInterface* slot = slots[index];
    if (prns::is_close(data, len)) {
        NimBLEDevice::getServer()->disconnect(slot->conn);
        return;
    }
    if (slot->identified || !prns::is_hello(data, len)) {
        slot->bad_control++;
        return;
    }
    uint8_t welcome[prns::GREETING_LEN];
    prns::write_welcome(welcome, our_identity);
    const uint8_t* identity = prns::hello_identity(data);
    if (memcmp(identity, our_identity, IDENTITY_LEN) != 0 && !notify(slot->conn, control_char, welcome, sizeof(welcome))) {
        // The Welcome could not go out, so the link would never settle.
        slot->notify_errors++;
        NimBLEDevice::getServer()->disconnect(slot->conn);
        return;
    }
    on_identity(index, identity, BlePeerInterface::PROTOCOL_PRNS);
}

static void on_write(const WriteEvent& write) {
    int index = -1;
    BlePeerInterface* slot = slot_for(write.conn, &index);
    if (!slot) {
        orphan_writes++;
        return;
    }
    const uint8_t* data = write.data;
    size_t len = write.len;
    if (write.channel == CHANNEL_CONTROL) {
        on_control(index, data, len);
        return;
    }
    if (columba::is_keepalive(data, len)) {
        slot->keepalives++;
        return;
    }
    if (!slot->identified) {
        if (write.channel == CHANNEL_COLUMBA_RX && columba::is_identity(len)) {
            on_identity(index, data, BlePeerInterface::PROTOCOL_COLUMBA);
        }
        else {
            slot->before_identity++;
        }
        return;
    }
    // Each protocol's fragments arrive on its own characteristic.
    bool expected = slot->protocol == BlePeerInterface::PROTOCOL_PRNS ? write.channel == CHANNEL_DATA
                                                                        : write.channel == CHANNEL_COLUMBA_RX;
    if (!expected) {
        slot->bad_fragments++;
        return;
    }
    if (len < columba::FRAG_HEADER) {
        slot->bad_fragments++;
        return;
    }
    // Both protocols frame fragments as ble-reticulum does (BleColumba.h).
    columba::Fragment fragment;
    if (!columba::read_fragment(data, len, &fragment)) {
        slot->bad_fragments++;
        slot->rx_active = false;
        return;
    }
    const uint8_t* payload = data + columba::FRAG_HEADER;
    size_t payload_len = len - columba::FRAG_HEADER;
    if (fragment.seq == 0) {
        slot->rx_len = 0;
        slot->rx_total = fragment.total;
        slot->rx_next = 0;
        slot->rx_active = true;
    }
    else if (!slot->rx_active || fragment.total != slot->rx_total || fragment.seq != slot->rx_next) {
        // GATT delivers one link's writes in order, so a gap means a lost
        // write (write_drops) or a confused peer; drop the packet.
        slot->bad_fragments++;
        slot->rx_active = false;
        return;
    }
    if (slot->rx_len + payload_len > PACKET_MAX) {
        slot->bad_fragments++;
        slot->rx_active = false;
        return;
    }
    memcpy(slot->rx + slot->rx_len, payload, payload_len);
    slot->rx_len += payload_len;
    slot->rx_next++;
    if (slot->rx_next == slot->rx_total) {
        slot->rx_active = false;
        slot->deliver(slot->rx, slot->rx_len);
    }
}

// One fragment of the slot's first queued packet, as a notification. False
// when nothing more should go to this slot on this pass.
static bool send_next_fragment(BlePeerInterface* slot) {
    static uint8_t fragment[VALUE_MAX];
    const BlePeerInterface::Packet& packet = slot->tx[slot->tx_head];
    size_t usable = slot->att_mtu > 3 ? slot->att_mtu - 3 : 20;
    if (usable > VALUE_MAX) usable = VALUE_MAX;
#ifdef RTNODE_BLE_CHUNK_MAX
    // Bench only: small fragments, to exercise a peer's reassembly.
    if (usable > RTNODE_BLE_CHUNK_MAX + columba::FRAG_HEADER) usable = RTNODE_BLE_CHUNK_MAX + columba::FRAG_HEADER;
#endif
    size_t chunk = usable - columba::FRAG_HEADER;
    if (slot->tx_seq == 0) slot->tx_total = (packet.len + chunk - 1) / chunk;
    size_t offset = (size_t)slot->tx_seq * chunk;
    size_t n = packet.len - offset < chunk ? packet.len - offset : chunk;
    columba::write_fragment_header(fragment, slot->tx_seq, slot->tx_total);
    memcpy(fragment + columba::FRAG_HEADER, packet.data + offset, n);

    os_mbuf* om = ble_hs_mbuf_from_flat(fragment, columba::FRAG_HEADER + n);
    if (!om) {
        slot->enomem++;
        return false;
    }
    NimBLECharacteristic* out = slot->protocol == BlePeerInterface::PROTOCOL_PRNS ? data_char : tx_char;
    int rc = ble_gattc_notify_custom(slot->conn, out->getHandle(), om);   // consumes om
    if (rc == BLE_HS_ENOMEM) {
        slot->enomem++;
        return false;
    }
    if (rc != 0) {
        // The link is going or gone; this packet will not arrive whole.
        slot->notify_errors++;
        slot->tx_seq = 0;
        slot->tx_head = (slot->tx_head + 1) % TX_DEPTH;
        slot->tx_count--;
        return false;
    }
    slot->last_sent_ms = millis();
    slot->tx_seq++;
    if (slot->tx_seq == slot->tx_total) {
        slot->tx_packets++;
        slot->tx_bytes += packet.len;
        slot->tx_seq = 0;
        slot->tx_head = (slot->tx_head + 1) % TX_DEPTH;
        slot->tx_count--;
    }
    return true;
}

static void report() {
    Serial.printf("[BLE] t=%lu control drops %lu write waits %lu orphans %lu self %lu replaced %lu internal %u\r\n",
                  (unsigned long)millis(), (unsigned long)control_drops, (unsigned long)write_waits,
                  (unsigned long)orphan_writes, (unsigned long)self_connections, (unsigned long)replaced,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    for (int i = 0; i < SLOTS; i++) {
        BlePeerInterface* s = slots[i];
        if (!s->used && s->peers == 0) continue;
        Serial.printf("[BLE]   %s %s %s peer %s mtu %u interval %u.%02u ms phy %u/%u peers %lu (prns %lu) "
                      "rx %lu/%lu tx %lu/%lu queued %u "
                      "drops %lu bad %lu/%lu keepalive in/out %lu/%lu early %lu enomem %lu errors %lu\r\n",
                      s->label().c_str(), s->used ? (s->identified ? "up" : "joining") : "free",
                      s->identified ? (s->protocol == BlePeerInterface::PROTOCOL_PRNS ? "prns" : "columba") : "-",
                      s->identified ? short_identity(s->identity).c_str() : "-", (unsigned)s->att_mtu,
                      (unsigned)(s->interval * 125 / 100), (unsigned)(s->interval * 125 % 100),
                      (unsigned)s->tx_phy, (unsigned)s->rx_phy,
                      (unsigned long)s->peers, (unsigned long)s->prns_peers,
                      (unsigned long)s->rx_packets, (unsigned long)s->rx_bytes,
                      (unsigned long)s->tx_packets, (unsigned long)s->tx_bytes, (unsigned)s->tx_count,
                      (unsigned long)s->tx_drops, (unsigned long)s->bad_fragments, (unsigned long)s->bad_control,
                      (unsigned long)s->keepalives, (unsigned long)s->keepalives_sent, (unsigned long)s->before_identity,
                      (unsigned long)s->enomem, (unsigned long)s->notify_errors);
    }
}

inline bool is_started() { return started; }

// Peers connected and identified right now (the display's Bluetooth bubble).
inline uint8_t connected_peers() {
    uint8_t n = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i] && slots[i]->used && slots[i]->identified) n++;
    }
    return n;
}

inline void loop() {
    if (!started) return;

    ControlEvent control;
    while (xQueueReceive(control_queue, &control, 0) == pdTRUE) {
        if (control.kind == CONTROL_CONNECT) on_connect(control.conn);
        else if (control.kind == CONTROL_DISCONNECT) on_disconnect(control.conn, control.value);
        else if (control.kind == CONTROL_MTU) {
            BlePeerInterface* slot = slot_for(control.conn);
            if (slot) slot->att_mtu = control.value;
        }
        else if (control.kind == CONTROL_PARAMS) {
            BlePeerInterface* slot = slot_for(control.conn);
            if (!slot) continue;
            read_params(slot);
            char what[40];
            snprintf(what, sizeof(what), "parameters updated (status %d)", (int)control.status);
            log_params(slot, what);
            // A failed update is the phone refusing ours, unless it failed
            // only because the phone's own update was running at the same
            // moment (LL Procedure Collision, Different Transaction
            // Collision): that one's outcome arrives as the next update.
            bool collision = control.status == BLE_HS_ERR_HCI_BASE + 0x23 ||
                             control.status == BLE_HS_ERR_HCI_BASE + 0x2A;
            if (control.status != 0 && !collision) slot->fast_refused = true;
            else if (control.status == 0 && slot->identified) ask_fast_interval(slot);
        }
        else if (control.kind == CONTROL_PHY) {
            BlePeerInterface* slot = slot_for(control.conn);
            if (!slot) continue;
            if (control.status == 0) {
                slot->tx_phy = control.value >> 8;
                slot->rx_phy = control.value & 0xFF;
            }
            // PHY 1 is 1M, 2 is 2M; a 4.2 phone answers the request with an error and stays on 1M.
            Serial.printf("[BLE] %s: PHY %u/%u (update status 0x%x)\r\n", slot->label().c_str(),
                          (unsigned)slot->tx_phy, (unsigned)slot->rx_phy, (unsigned)control.status);
        }
    }
    // Writes, a bounded number per pass so a busy peer cannot hold up the
    // loop; a write that finds the queue full waits in the host task.
    for (int i = 0; i < WRITES_PER_PASS && xQueueReceive(write_queue, &loop_write, 0) == pdTRUE; i++) {
        on_write(loop_write);
    }
    uint32_t now_ms = millis();
    for (int i = 0; i < SLOTS; i++) {
        BlePeerInterface* slot = slots[i];
        if (!slot->used || !slot->identified) continue;
        if (slot->tx_count > 0) {
            for (int n = 0; n < FRAGMENTS_PER_PASS && slot->tx_count > 0 && send_next_fragment(slot); n++) {}
        }
        else if (slot->protocol == BlePeerInterface::PROTOCOL_COLUMBA &&
                 now_ms - slot->last_sent_ms >= columba::KEEPALIVE_EVERY_MS) {
            if (notify(slot->conn, tx_char, &columba::KEEPALIVE, 1)) {
                slot->keepalives_sent++;
                slot->last_sent_ms = now_ms;
            }
        }
    }

    uint32_t now = millis();
    if (now - last_report_ms >= 60000) {
        last_report_ms = now;
        report();
    }
}

}  // namespace ble

#endif // FIREWALL_MODE && RTNODE_BLE
#endif // BLE_INTERFACE_H
