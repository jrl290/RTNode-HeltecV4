// ─────────────────────────────────────────────────────────────────────────────
//  Advertise.h — Reticulum interface-discovery announcer for RTNode
//
//  Implements the on-network interface-discovery announce protocol described
//  in the Reticulum manual: https://reticulum.network/manual/interfaces.html
//  The values other nodes check, and RTNode's own defaults, are listed in
//  RETICULUM_BEHAVIOUR.md §5.
//
//  When FIREWALL_MODE is active and the user has enabled "Advertise Device" in
//  the captive-portal configuration, this module periodically sends an
//  RNS announce on a destination with aspects "rnstransport.discovery.interface"
//  whose app_data is:
//
//      bytes([flags]) || msgpack(info_dict) || stamp
//
//  where info_dict contains the documented byte-id fields (interface type,
//  transport ID, latitude/longitude/height, LoRa parameters, etc.) and stamp
//  is an LXMF proof-of-work over a SHA-256/HKDF-SHA256 workblock.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef ADVERTISE_H
#define ADVERTISE_H

#ifdef FIREWALL_MODE

#include <Arduino.h>
#include <Bytes.h>
#include <Identity.h>
#include <Destination.h>
#include <Transport.h>
#include <Reticulum.h>
#include <Cryptography/HKDF.h>
#include <Log.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include "FirewallMode.h"

#include <SHA256.h>

#if defined(ESP32)
    #include <Preferences.h>
#endif
// Externally-defined LoRa parameters (see Config.h / RNode_Firmware.ino)
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int      lora_sf;
extern int      lora_cr;

// Cached node-hash hex string in RTC memory (see RNode_Firmware.ino).
#ifndef NODE_HASH_RTC_MAGIC
#define NODE_HASH_RTC_MAGIC  0x504B4841UL
#endif
extern uint32_t rtc_node_hash_magic;
extern char     rtc_node_hash_hex[33];

// ─── Protocol constants (the wire format; every node reads these) ───────────
#define ADV_FIELD_INTERFACE_TYPE  0x00
#define ADV_FIELD_TRANSPORT       0x01
#define ADV_FIELD_REACHABLE_ON    0x02
#define ADV_FIELD_LATITUDE        0x03
#define ADV_FIELD_LONGITUDE       0x04
#define ADV_FIELD_HEIGHT          0x05
#define ADV_FIELD_PORT            0x06
#define ADV_FIELD_IFAC_NETNAME    0x07
#define ADV_FIELD_IFAC_NETKEY     0x08
#define ADV_FIELD_FREQUENCY       0x09
#define ADV_FIELD_BANDWIDTH       0x0A
#define ADV_FIELD_SPREADINGFACTOR 0x0B
#define ADV_FIELD_CODINGRATE      0x0C
#define ADV_FIELD_MODULATION      0x0D
#define ADV_FIELD_CHANNEL         0x0E
#define ADV_FIELD_TRANSPORT_ID    0xFE
#define ADV_FIELD_NAME            0xFF

#define ADV_FLAG_SIGNED    0x01
#define ADV_FLAG_ENCRYPTED 0x02

// The stamp other nodes check before they accept the advert
// (RETICULUM_BEHAVIOUR.md §5). Nodes on Reticulum 1.5 require value 16 by
// default and ignore a value-14 stamp three times in four; 1.1 used 14.
#define ADV_DEFAULT_STAMP_COST      16
#define ADV_WORKBLOCK_EXPAND_ROUNDS 20
#define ADV_STAMP_SIZE              32 /* SHA-256 / HASHLENGTH/8 */
// The stamp search runs a slice per loop() pass so it never holds up TCP or
// LoRa; in one piece it took 13–28 s at value 14 (bench, 2026-09-28).
#define ADV_STAMP_SLICE_US          2000

// Default announce interval: 6 hours, as other Reticulum nodes default to.
// LoRa airtime is precious; this is intentionally conservative.
#define ADV_DEFAULT_ANNOUNCE_INTERVAL_S (6UL * 60UL * 60UL)
// Initial delay after boot before the first announce — gives the radio,
// transport and any TCP backbone time to come up.
#define ADV_INITIAL_DELAY_MS            (60UL * 1000UL)

// Privacy jitter radius. ~half a kilometre / half a mile.
#define ADV_JITTER_RADIUS_METERS 800.0

// ─── Module state ────────────────────────────────────────────────────────────
static RNS::Destination advertise_destination = {RNS::Type::NONE};
static bool     advertise_initialised      = false;
static bool     advertise_first_announce   = true;
static uint32_t advertise_next_run_ms      = 0;
static uint32_t advertise_announce_interval_ms = ADV_DEFAULT_ANNOUNCE_INTERVAL_S * 1000UL;

// Cached stamp keyed by infohash so we only redo the proof-of-work when the
// advertised parameters actually change.
// Stamp is persisted to ESP32 NVS to survive reboots.
static RNS::Bytes advertise_cached_infohash;
static RNS::Bytes advertise_cached_stamp;

#if defined(ESP32)
static const char* ADV_NVS_NS   = "rtnode";
static const char* ADV_NVS_IH   = "adv_ih";   // infohash (32 bytes)
static const char* ADV_NVS_STAMP = "adv_st";   // stamp (32 bytes)
static const char* ADV_NVS_COST  = "adv_sc";   // stamp value it was made for (absent before value 16)

static void advertise_load_stamp() {
    Preferences prefs;
    if (!prefs.begin(ADV_NVS_NS, true)) return; // read-only
    size_t ih_len = prefs.getBytesLength(ADV_NVS_IH);
    size_t st_len = prefs.getBytesLength(ADV_NVS_STAMP);
    // A stamp cached for a lower value would be ignored by the nodes that
    // require the current one.
    uint8_t cost = prefs.getUChar(ADV_NVS_COST, 0);
    if (ih_len == 32 && st_len == ADV_STAMP_SIZE && cost >= ADV_DEFAULT_STAMP_COST) { // HASHLENGTH/8 = 32
        uint8_t ih_buf[64], st_buf[32];
        prefs.getBytes(ADV_NVS_IH, ih_buf, ih_len);
        prefs.getBytes(ADV_NVS_STAMP, st_buf, st_len);
        advertise_cached_infohash = RNS::Bytes(ih_buf, ih_len);
        advertise_cached_stamp    = RNS::Bytes(st_buf, st_len);
        RNS::verbose("[Advertise] Loaded cached stamp from NVS");
    }
    prefs.end();
}

static void advertise_save_stamp() {
    Preferences prefs;
    if (!prefs.begin(ADV_NVS_NS, false)) return;
    prefs.putBytes(ADV_NVS_IH, advertise_cached_infohash.data(), advertise_cached_infohash.size());
    prefs.putBytes(ADV_NVS_STAMP, advertise_cached_stamp.data(), advertise_cached_stamp.size());
    prefs.putUChar(ADV_NVS_COST, ADV_DEFAULT_STAMP_COST);
    prefs.end();
    RNS::verbose("[Advertise] Saved stamp to NVS");
}
#endif

// ─── MessagePack encoder ────────────────────────────────────────────────────
// Minimal encoder covering the types required by the discovery info dict:
// fixmap/map16, fixstr/str8/str16, bin8/16, bool, float64, uint8/16/32, fixint.
// Output is appended to a RNS::Bytes buffer.

static inline void adv_mp_byte(RNS::Bytes& out, uint8_t b) { out.append(b); }

static inline void adv_mp_bytes(RNS::Bytes& out, const uint8_t* p, size_t n) {
    out.append(p, n);
}

static inline void adv_mp_be16(RNS::Bytes& out, uint16_t v) {
    adv_mp_byte(out, (v >> 8) & 0xFF);
    adv_mp_byte(out, v & 0xFF);
}

static inline void adv_mp_be32(RNS::Bytes& out, uint32_t v) {
    adv_mp_byte(out, (v >> 24) & 0xFF);
    adv_mp_byte(out, (v >> 16) & 0xFF);
    adv_mp_byte(out, (v >> 8)  & 0xFF);
    adv_mp_byte(out, v & 0xFF);
}

static inline void adv_mp_map_header(RNS::Bytes& out, uint32_t n) {
    if (n <= 15) {
        adv_mp_byte(out, 0x80 | (uint8_t)n);
    } else if (n <= 0xFFFF) {
        adv_mp_byte(out, 0xde);
        adv_mp_be16(out, (uint16_t)n);
    } else {
        adv_mp_byte(out, 0xdf);
        adv_mp_be32(out, n);
    }
}

static inline void adv_mp_uint(RNS::Bytes& out, uint64_t v) {
    if (v <= 0x7F) {
        adv_mp_byte(out, (uint8_t)v);
    } else if (v <= 0xFF) {
        adv_mp_byte(out, 0xcc);
        adv_mp_byte(out, (uint8_t)v);
    } else if (v <= 0xFFFF) {
        adv_mp_byte(out, 0xcd);
        adv_mp_be16(out, (uint16_t)v);
    } else if (v <= 0xFFFFFFFFULL) {
        adv_mp_byte(out, 0xce);
        adv_mp_be32(out, (uint32_t)v);
    } else {
        adv_mp_byte(out, 0xcf);
        for (int i = 7; i >= 0; --i) adv_mp_byte(out, (uint8_t)(v >> (i * 8)));
    }
}

static inline void adv_mp_bool(RNS::Bytes& out, bool v) {
    adv_mp_byte(out, v ? 0xc3 : 0xc2);
}

static inline void adv_mp_str(RNS::Bytes& out, const char* s) {
    size_t len = (s == nullptr) ? 0 : strlen(s);
    if (len <= 31) {
        adv_mp_byte(out, 0xa0 | (uint8_t)len);
    } else if (len <= 0xFF) {
        adv_mp_byte(out, 0xd9);
        adv_mp_byte(out, (uint8_t)len);
    } else if (len <= 0xFFFF) {
        adv_mp_byte(out, 0xda);
        adv_mp_be16(out, (uint16_t)len);
    } else {
        adv_mp_byte(out, 0xdb);
        adv_mp_be32(out, (uint32_t)len);
    }
    if (len > 0) adv_mp_bytes(out, (const uint8_t*)s, len);
}

static inline void adv_mp_bin(RNS::Bytes& out, const uint8_t* p, size_t len) {
    if (len <= 0xFF) {
        adv_mp_byte(out, 0xc4);
        adv_mp_byte(out, (uint8_t)len);
    } else if (len <= 0xFFFF) {
        adv_mp_byte(out, 0xc5);
        adv_mp_be16(out, (uint16_t)len);
    } else {
        adv_mp_byte(out, 0xc6);
        adv_mp_be32(out, (uint32_t)len);
    }
    if (len > 0) adv_mp_bytes(out, p, len);
}

// IEEE-754 binary64, big-endian (msgpack float64, prefix 0xcb).
static inline void adv_mp_float64(RNS::Bytes& out, double v) {
    static_assert(sizeof(double) == 8, "msgpack float64 requires IEEE-754 binary64");
    uint8_t buf[8];
    memcpy(buf, &v, 8);
    // Detect host endianness — virtually always little-endian on ESP32.
    const uint16_t endian_test = 1;
    bool host_little = (*reinterpret_cast<const uint8_t*>(&endian_test)) == 1;
    adv_mp_byte(out, 0xcb);
    if (host_little) {
        for (int i = 7; i >= 0; --i) adv_mp_byte(out, buf[i]);
    } else {
        adv_mp_bytes(out, buf, 8);
    }
}

// Pack a small integer key (the field IDs are all <= 0xFF). Values <=127 fit
// as a positive fixint; 0xFE and 0xFF need a uint8 prefix.
static inline void adv_mp_key(RNS::Bytes& out, uint8_t key) {
    adv_mp_uint(out, (uint64_t)key);
}

// ─── Privacy jitter (deterministic per-device) ───────────────────────────────
// Compute a stable lat/lon offset of up to ADV_JITTER_RADIUS_METERS using the
// node's destination hash as the seed. Stable per device → the pin doesn't
// move around between announces, but its precise location is obscured.
static void advertise_apply_jitter(double& lat, double& lon, const RNS::Bytes& seed_hash) {
    if (seed_hash.size() < 8) return;
    const uint8_t* h = seed_hash.data();

    // Two independent uniform [0, 1) values from the hash bytes.
    uint32_t a = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                 ((uint32_t)h[2] << 8)  |  (uint32_t)h[3];
    uint32_t b = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) |
                 ((uint32_t)h[6] << 8)  |  (uint32_t)h[7];
    double u1 = (double)a / 4294967296.0;
    double u2 = (double)b / 4294967296.0;

    // Uniform-disk sampling to avoid bunching at the centre.
    double r     = sqrt(u1) * ADV_JITTER_RADIUS_METERS;
    double theta = 2.0 * M_PI * u2;

    // 1 deg latitude ≈ 111 320 m. 1 deg longitude ≈ 111 320 m * cos(lat).
    double dlat = (r * cos(theta)) / 111320.0;
    double cos_lat = cos(lat * M_PI / 180.0);
    if (cos_lat < 1e-6) cos_lat = 1e-6; // guard against the poles
    double dlon = (r * sin(theta)) / (111320.0 * cos_lat);

    lat += dlat;
    lon += dlon;

    // Clamp back into valid ranges in the unlikely event of a pole-adjacent input.
    if (lat >  90.0) lat =  90.0;
    if (lat < -90.0) lat = -90.0;
    if (lon >  180.0) lon -= 360.0;
    if (lon < -180.0) lon += 360.0;
}

// ─── LXMF stamp (proof-of-work), a slice at a time ──────────────────────────
// A stamp is 32 bytes such that SHA-256(workblock || stamp) has at least
// ADV_DEFAULT_STAMP_COST leading zero bits (LXMF/LXStamper.py). The workblock
// is ADV_WORKBLOCK_EXPAND_ROUNDS rounds of HKDF-SHA256 over the infohash. Each
// round is hashed in as it is made, one round per loop() pass, and never
// stored; the search then starts every candidate from the SHA-256 state after
// the workblock, so a candidate costs one 64-byte block, not 5 KB.

enum AdvertisePhase : uint8_t { ADV_PHASE_IDLE, ADV_PHASE_WORKBLOCK, ADV_PHASE_SEARCH };

static struct {
    AdvertisePhase phase = ADV_PHASE_IDLE;
    RNS::Bytes packed;          // the info being stamped; sent with the stamp
    RNS::Bytes infohash;
    uint8_t    round = 0;       // workblock rounds hashed in so far
    SHA256     workblock;       // SHA-256 state over the workblock so far
    uint8_t    candidate[ADV_STAMP_SIZE];
    uint32_t   attempts = 0;
    uint32_t   started_ms = 0;
} adv_stamp;

static uint32_t adv_leading_zero_bits(const uint8_t* h, uint32_t enough) {
    uint32_t lz = 0;
    for (size_t i = 0; i < 32 && lz < enough; ++i) {
        if (h[i] == 0) { lz += 8; continue; }
        lz += __builtin_clz((uint32_t)h[i]) - 24;
        break;
    }
    return lz;
}

// Workblock round n: HKDF-SHA256(length 256, material = infohash,
// salt = full_hash(infohash || msgpack(n))), hashed into adv_stamp.workblock.
static void adv_hash_workblock_round(uint8_t n) {
    RNS::Bytes salt_input;
    salt_input.append(adv_stamp.infohash);
    adv_mp_uint(salt_input, (uint64_t)n);
    RNS::Bytes salt = RNS::Identity::full_hash(salt_input);
    RNS::Bytes round = RNS::Cryptography::hkdf(256, adv_stamp.infohash, salt);
    adv_stamp.workblock.update(round.data(), round.size());
}

// Tries candidates for up to ADV_STAMP_SLICE_US. True once one qualifies;
// it is left in adv_stamp.candidate.
static bool adv_search_slice() {
    uint32_t start = micros();
    uint8_t h[32];
    do {
        // Count up through the first 8 bytes of a random starting candidate.
        for (int i = 0; i < 8 && ++adv_stamp.candidate[i] == 0; ++i) {}
        SHA256 attempt = adv_stamp.workblock;
        attempt.update(adv_stamp.candidate, ADV_STAMP_SIZE);
        attempt.finalize(h, sizeof(h));
        adv_stamp.attempts++;
        if (adv_leading_zero_bits(h, ADV_DEFAULT_STAMP_COST) >= ADV_DEFAULT_STAMP_COST) return true;
    } while ((uint32_t)(micros() - start) < ADV_STAMP_SLICE_US);
    return false;
}

// ─── Build the discovery info map ────────────────────────────────────────────
// Packs the info dict per RNS/Discovery.py::get_interface_announce_data()
// for INTERFACE_TYPE = "RNodeInterface".
static RNS::Bytes advertise_build_info() {
    RNS::Bytes packed;

    // Determine which optional fields will be included so we can write a
    // correct map header up front. The IFAC network name and passphrase go
    // out only when the user opted in (off by default, as on other nodes):
    // anyone who hears the advert can use them to join.
    bool include_ifac =
        firewall_state.advert_publish_ifac &&
        firewall_state.ifac_enabled &&
        (firewall_state.ifac_netname[0] != '\0' ||
         firewall_state.ifac_passphrase[0] != '\0');

    // Required keys: INTERFACE_TYPE, TRANSPORT, TRANSPORT_ID, NAME,
    //                LATITUDE, LONGITUDE, HEIGHT,
    //                FREQUENCY, BANDWIDTH, SPREADINGFACTOR, CODINGRATE
    // Optional keys: IFAC_NETNAME, IFAC_NETKEY (publish_ifac)
    uint32_t map_entries = 11;
    if (include_ifac) map_entries += 2;

    adv_mp_map_header(packed, map_entries);

    // INTERFACE_TYPE = "RNodeInterface"
    adv_mp_key(packed, ADV_FIELD_INTERFACE_TYPE);
    adv_mp_str(packed, "RNodeInterface");

    // TRANSPORT (bool) — whether transport is enabled on this node
    adv_mp_key(packed, ADV_FIELD_TRANSPORT);
    adv_mp_bool(packed, RNS::Reticulum::transport_enabled());

    // TRANSPORT_ID (bin) — RNS::Transport identity hash (truncated, 16 bytes)
    {
        const RNS::Bytes& tid_hash = RNS::Transport::identity().hash();
        adv_mp_key(packed, ADV_FIELD_TRANSPORT_ID);
        adv_mp_bin(packed, tid_hash.data(), tid_hash.size());
    }

    // NAME (str) — discovery_name. Use the user-configured node name when set,
    // otherwise fall back to a prefix of the node hash hex so each node has a
    // unique identifier visible on maps.
    {
        char name_buf[40];
        const char* adv_name;
        if (firewall_state.node_name[0] != '\0') {
            adv_name = firewall_state.node_name;
        } else {
            const char* hex = (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0')
                              ? rtc_node_hash_hex : "";
            snprintf(name_buf, sizeof(name_buf), "RTNode-%.8s", hex[0] ? hex : "unknown");
            adv_name = name_buf;
        }
        adv_mp_key(packed, ADV_FIELD_NAME);
        adv_mp_str(packed, adv_name);
    }

    // LATITUDE / LONGITUDE (float64) — apply optional privacy jitter.
    {
        double adv_lat = firewall_state.advert_lat;
        double adv_lon = firewall_state.advert_lon;
        if (firewall_state.advert_jitter && advertise_destination) {
            advertise_apply_jitter(adv_lat, adv_lon, advertise_destination.hash());
        }
        adv_mp_key(packed, ADV_FIELD_LATITUDE);
        adv_mp_float64(packed, adv_lat);
        adv_mp_key(packed, ADV_FIELD_LONGITUDE);
        adv_mp_float64(packed, adv_lon);
    }

    // HEIGHT (float64) — not configurable yet; default to 0 metres.
    adv_mp_key(packed, ADV_FIELD_HEIGHT);
    adv_mp_float64(packed, 0.0);

    // RNodeInterface-specific radio parameters (per Discovery.py:144-148).
    adv_mp_key(packed, ADV_FIELD_FREQUENCY);
    adv_mp_uint(packed, (uint64_t)lora_freq);
    adv_mp_key(packed, ADV_FIELD_BANDWIDTH);
    adv_mp_uint(packed, (uint64_t)lora_bw);
    adv_mp_key(packed, ADV_FIELD_SPREADINGFACTOR);
    adv_mp_uint(packed, (uint64_t)lora_sf);
    adv_mp_key(packed, ADV_FIELD_CODINGRATE);
    adv_mp_uint(packed, (uint64_t)lora_cr);

    if (include_ifac) {
        adv_mp_key(packed, ADV_FIELD_IFAC_NETNAME);
        adv_mp_str(packed, firewall_state.ifac_netname);
        adv_mp_key(packed, ADV_FIELD_IFAC_NETKEY);
        adv_mp_str(packed, firewall_state.ifac_passphrase);
    }

    return packed;
}

// ─── Send a discovery announce ──────────────────────────────────────────────
static void advertise_send(const RNS::Bytes& packed, const RNS::Bytes& stamp) {
    // Assemble payload: bytes([flags]) || packed || stamp
    RNS::Bytes app_data;
    app_data.append((uint8_t)0x00); // flags: not signed, not encrypted
    app_data.append(packed);
    app_data.append(stamp);

    RNS::verbose("[Advertise] Sending interface discovery announce, payload size: " +
                 std::to_string((int)app_data.size()) + " bytes");
    advertise_destination.announce(app_data);

    advertise_first_announce = false;
    advertise_next_run_ms    = millis() + advertise_announce_interval_ms;
}

// ─── Public API ─────────────────────────────────────────────────────────────

// Initialise the advertise destination. Call once after RNS has been started
// and Transport::identity() is available. Safe to call multiple times — only
// the first call has any effect.
inline void advertise_init() {
    if (advertise_initialised) return;
    if (!RNS::Transport::identity()) return;

    // Reticulum's discovery destination uses the network identity when one is
    // configured, otherwise the transport identity. We have no concept of a
    // separate network identity in the firmware, so use the transport identity.
    advertise_destination = RNS::Destination(
        RNS::Transport::identity(),
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "rnstransport",
        "discovery.interface"
    );

    advertise_initialised    = true;
    advertise_first_announce = true;
    advertise_next_run_ms    = millis() + ADV_INITIAL_DELAY_MS;
    advertise_announce_interval_ms = ADV_DEFAULT_ANNOUNCE_INTERVAL_S * 1000UL;

#if defined(ESP32)
    advertise_load_stamp();
#endif

    if (firewall_state.advert_enabled) {
        RNS::info("[Advertise] Device advertisement ENABLED — first announce in ~" +
                  std::to_string(ADV_INITIAL_DELAY_MS / 1000) + "s");
    } else {
        RNS::verbose("[Advertise] Device advertisement disabled (configure in portal to enable)");
    }
}

// Periodic loop hook — call from the main loop(). Each call does one small
// step: when an announce is due, builds the info and either reuses the cached
// stamp or starts a new one; while a stamp is being made, hashes one workblock
// round or searches for ADV_STAMP_SLICE_US.
inline void advertise_loop() {
    if (!advertise_initialised) return;
    if (!firewall_state.advert_enabled) return;

    if (adv_stamp.phase == ADV_PHASE_WORKBLOCK) {
        adv_hash_workblock_round(adv_stamp.round++);
        if (adv_stamp.round == ADV_WORKBLOCK_EXPAND_ROUNDS) {
            RNS::Bytes start = RNS::Cryptography::random(ADV_STAMP_SIZE);
            memcpy(adv_stamp.candidate, start.data(), ADV_STAMP_SIZE);
            adv_stamp.attempts = 0;
            adv_stamp.phase = ADV_PHASE_SEARCH;
        }
        return;
    }

    if (adv_stamp.phase == ADV_PHASE_SEARCH) {
        if (!adv_search_slice()) {
            // Bounded, as before: 64 times the expected number of attempts.
            if (adv_stamp.attempts > (1UL << (ADV_DEFAULT_STAMP_COST + 6))) {
                RNS::error("[Advertise] Stamp generation failed; skipping announce");
                adv_stamp.phase = ADV_PHASE_IDLE;
                advertise_next_run_ms = millis() + advertise_announce_interval_ms;
            }
            return;
        }
        RNS::verbose("[Advertise] Stamp found after " + std::to_string(adv_stamp.attempts) +
                     " attempts in " + std::to_string(millis() - adv_stamp.started_ms) + " ms");
        advertise_cached_infohash = adv_stamp.infohash;
        advertise_cached_stamp    = RNS::Bytes(adv_stamp.candidate, ADV_STAMP_SIZE);
#if defined(ESP32)
        advertise_save_stamp();
#endif
        adv_stamp.phase = ADV_PHASE_IDLE;
        advertise_send(adv_stamp.packed, advertise_cached_stamp);
        return;
    }

    uint32_t now = millis();
    // Handle uint32 wrap-around: only treat as "due" when the unsigned
    // difference is small. millis() wraps roughly every 49 days, well after
    // any reasonable announce interval, so a wrap will at worst cause a single
    // announce to fire one cycle early.
    int32_t delta = (int32_t)(now - advertise_next_run_ms);
    if (delta < 0) return;

    RNS::verbose("[Advertise] Building discovery announce");
    RNS::Bytes packed = advertise_build_info();
    RNS::Bytes infohash = RNS::Identity::full_hash(packed);
    if (advertise_cached_infohash.size() > 0 &&
        advertise_cached_infohash == infohash &&
        advertise_cached_stamp.size() == ADV_STAMP_SIZE) {
        RNS::verbose("[Advertise] Reusing cached stamp (info unchanged)");
        advertise_send(packed, advertise_cached_stamp);
        return;
    }

    RNS::verbose("[Advertise] Generating workblock + stamp (value " + std::to_string(ADV_DEFAULT_STAMP_COST) +
                 "), a slice per loop pass");
    adv_stamp.packed     = packed;
    adv_stamp.infohash   = infohash;
    adv_stamp.round      = 0;
    adv_stamp.workblock.reset();
    adv_stamp.started_ms = now;
    adv_stamp.phase      = ADV_PHASE_WORKBLOCK;
}

#endif // FIREWALL_MODE
#endif // ADVERTISE_H
