// Host-side tests for BlePrns.h and BleColumba.h, the Bluetooth LE protocols
// BleInterface.h speaks. Run with tests/native/run.sh — no board needed.
//
// The Hello and Welcome bytes are the ones Reticulum-rust's prns-ble package
// tests on the phone side (prns-ble/src/wire.rs), so both ends of the link
// check the same bytes.

#include "../../BleColumba.h"
#include "../../BlePrns.h"

#include <cstdio>
#include <cstring>

using namespace ble;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void identity(uint8_t out[prns::IDENTITY_LEN], uint8_t first) {
    for (size_t i = 0; i < prns::IDENTITY_LEN; i++) out[i] = (uint8_t)(first + i);
}

static void prns_welcome() {
    uint8_t id[prns::IDENTITY_LEN];
    identity(id, 0x20);
    uint8_t want[prns::GREETING_LEN] = {0x02};
    memcpy(want + 1, id, sizeof(id));
    const uint8_t tail[] = {0x05, 0x00, 0x00, 0x01, 0xF4, 0x80};
    memcpy(want + 1 + sizeof(id), tail, sizeof(tail));

    uint8_t got[prns::GREETING_LEN];
    memset(got, 0xAA, sizeof(got));
    prns::write_welcome(got, id);
    check(memcmp(got, want, sizeof(want)) == 0,
          "the Welcome is 02, our identity, endpoint ESP32, no L2CAP, MTU 500, RSSI unknown");
}

static void prns_hello() {
    // The Hello a Retichat iPhone writes (endpoint iOS 01 01).
    uint8_t id[prns::IDENTITY_LEN];
    identity(id, 0x10);
    uint8_t hello[prns::GREETING_LEN] = {0x01};
    memcpy(hello + 1, id, sizeof(id));
    const uint8_t tail[] = {0x01, 0x01, 0x00, 0x01, 0xF4, 0x80};
    memcpy(hello + 1 + sizeof(id), tail, sizeof(tail));

    check(prns::is_hello(hello, sizeof(hello)), "a phone's Hello is a Hello");
    check(memcmp(prns::hello_identity(hello), id, sizeof(id)) == 0,
          "the Hello's identity follows its tag");
    check(!prns::is_hello(hello, sizeof(hello) - 1), "a Hello one byte short is not a Hello");

    uint8_t welcome[prns::GREETING_LEN];
    prns::write_welcome(welcome, id);
    check(!prns::is_hello(welcome, sizeof(welcome)), "a Welcome is not a Hello");
}

static void prns_close() {
    check(prns::is_close(prns::CLOSE_SELF, sizeof(prns::CLOSE_SELF)), "CLOSE_SELF is a Close");
    check(prns::CLOSE_SELF[0] == 0x03 && prns::CLOSE_SELF[1] == 0x01,
          "CLOSE_SELF is 03 01, self-connection");
    const uint8_t tag_only[] = {0x03};
    check(!prns::is_close(tag_only, sizeof(tag_only)), "a Close without a reason is not a Close");
    const uint8_t hello_tag[] = {0x01, 0x01};
    check(!prns::is_close(hello_tag, sizeof(hello_tag)), "01 01 is not a Close");
}

static void uuids() {
    // Every characteristic is the service UUID with the last byte changed.
    const char* characteristics[] = {columba::TX_UUID, columba::RX_UUID, columba::IDENTITY_UUID,
                                     prns::CONTROL_UUID, prns::DATA_UUID};
    const size_t prefix = strlen(columba::SERVICE_UUID) - 2;
    for (const char* uuid : characteristics) {
        check(strlen(uuid) == strlen(columba::SERVICE_UUID) &&
                  strncmp(uuid, columba::SERVICE_UUID, prefix) == 0 &&
                  strcmp(uuid, columba::SERVICE_UUID) != 0,
              "a characteristic UUID is the service UUID with a different last byte");
    }
}

static void columba_advertisement() {
    const uint8_t want[] = {0xFF, 0xFF, 0x03, 0x01};
    check(sizeof(columba::ADV_MANUFACTURER_DATA) == sizeof(want) &&
              memcmp(columba::ADV_MANUFACTURER_DATA, want, sizeof(want)) == 0,
          "the advertisement says ble-reticulum v0.3.0, peripheral-only");
}

static void columba_writes() {
    const uint8_t keepalive[] = {0x00};
    const uint8_t two_zeros[] = {0x00, 0x00};
    const uint8_t one[] = {0x01};
    check(columba::is_keepalive(keepalive, sizeof(keepalive)), "00 is a keepalive");
    check(!columba::is_keepalive(two_zeros, sizeof(two_zeros)), "00 00 is not a keepalive");
    check(!columba::is_keepalive(one, sizeof(one)), "01 is not a keepalive");
    check(columba::is_identity(16), "16 bytes is an identity");
    check(!columba::is_identity(15) && !columba::is_identity(17), "15 or 17 bytes is not an identity");
}

static void header_is(uint16_t seq, uint16_t total, const uint8_t want[columba::FRAG_HEADER],
                      const char* what) {
    uint8_t got[columba::FRAG_HEADER];
    columba::write_fragment_header(got, seq, total);
    check(memcmp(got, want, columba::FRAG_HEADER) == 0, what);

    columba::Fragment fragment = {0, 0, 0};
    check(columba::read_fragment(got, sizeof(got), &fragment) && fragment.type == want[0] &&
              fragment.seq == seq && fragment.total == total,
          "a written fragment header reads back");
}

static void fragment_headers() {
    const uint8_t only[] = {0x01, 0x00, 0x00, 0x00, 0x01};
    const uint8_t first[] = {0x01, 0x00, 0x00, 0x00, 0x03};
    const uint8_t middle[] = {0x02, 0x00, 0x01, 0x00, 0x03};
    const uint8_t last[] = {0x03, 0x00, 0x02, 0x00, 0x03};
    const uint8_t wide[] = {0x02, 0x01, 0x00, 0x01, 0x2C};
    header_is(0, 1, only, "a one-fragment packet is a start with total 1");
    header_is(0, 3, first, "fragment 0 of 3 is a start");
    header_is(1, 3, middle, "fragment 1 of 3 is a continue");
    header_is(2, 3, last, "fragment 2 of 3 is an end");
    header_is(256, 300, wide, "sequence and total are big-endian");
}

static void bad_fragments() {
    columba::Fragment fragment;
    const uint8_t short_write[] = {0x01, 0x00, 0x00, 0x00};
    const uint8_t type_zero[] = {0x00, 0x00, 0x00, 0x00, 0x01};
    const uint8_t type_four[] = {0x04, 0x00, 0x00, 0x00, 0x01};
    const uint8_t total_zero[] = {0x01, 0x00, 0x00, 0x00, 0x00};
    const uint8_t past_total[] = {0x03, 0x00, 0x03, 0x00, 0x03};
    check(!columba::read_fragment(short_write, sizeof(short_write), &fragment),
          "four bytes cannot hold a fragment header");
    check(!columba::read_fragment(type_zero, sizeof(type_zero), &fragment), "type 00 is not a fragment");
    check(!columba::read_fragment(type_four, sizeof(type_four), &fragment), "type 04 is not a fragment");
    check(!columba::read_fragment(total_zero, sizeof(total_zero), &fragment), "total 0 is not a fragment");
    check(!columba::read_fragment(past_total, sizeof(past_total), &fragment),
          "a sequence past the total is not a fragment");
}

int main() {
    prns_welcome();
    prns_hello();
    prns_close();
    uuids();
    columba_advertisement();
    columba_writes();
    fragment_headers();
    bad_fragments();
    if (failures) {
        std::fprintf(stderr, "Bluetooth protocols: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("Bluetooth protocols: all passed");
    return 0;
}
