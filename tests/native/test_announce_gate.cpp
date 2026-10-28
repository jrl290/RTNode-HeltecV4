// Host-side tests for lib/microReticulum/src/AnnounceGate.h, ingress control
// as RETICULUM_BEHAVIOUR.md §1 describes it.
// Run with tests/native/run.sh — no board needed.

#include "../../lib/microReticulum/src/AnnounceGate.h"

#include <cstdio>
#include <string>

using Gate = RNS::AnnounceGate<std::string>;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static const uint64_t HOUR = 60ULL * 60 * 1000;

// count arrivals, spacing_ms apart, the first at start.
static void arrive(Gate& gate, uint64_t start, int count, uint64_t spacing_ms) {
    for (int i = 0; i < count; i++) { gate.arrived(start + i * spacing_ms); }
}

static void rate_needs_three_arrivals() {
    Gate gate(0);
    arrive(gate, 1000, 2, 10);
    Gate::Reading reading = gate.look(1010);
    check(reading.rate == 0 && !reading.flooding, "two arrivals give no rate, however close together");
    gate.arrived(1020);
    check(gate.look(1020).rate > 0, "a third arrival gives a rate");
}

static void young_and_old_thresholds() {
    // 4 arrivals over 0.6 s: about 6.7 a second, between the two thresholds.
    Gate young(0);
    arrive(young, 1000, 4, 200);
    Gate::Reading reading = young.look(1600);
    check(reading.threshold == 3 && reading.flooding && reading.began, "a source under 2 hours old floods above 3 a second");

    uint64_t later = 2 * HOUR + 1000;
    Gate old(0);
    arrive(old, later, 4, 200);
    reading = old.look(later + 600);
    check(reading.threshold == 10 && !reading.flooding, "an older source floods only above 10 a second");
}

static void only_48_arrivals_are_kept() {
    Gate gate(0);
    arrive(gate, 1000, 60, 10);  // the last at 1590
    // The 48 kept start at 1120: 48 / 0.47 s.
    double rate = gate.look(1590).rate;
    check(rate > 102.12 && rate < 102.13, "the rate counts only the 48 most recent arrivals");
}

// A flood: 10 arrivals 100 ms apart from 1000, seen at 1900.
static Gate flooded() {
    Gate gate(0);
    arrive(gate, 1000, 10, 100);
    Gate::Reading reading = gate.look(1900);
    check(reading.began && reading.flooding, "the flood begins when the rate goes over the threshold");
    return gate;
}

static void flood_ends_after_15_quiet_seconds() {
    Gate gate = flooded();
    Gate::Reading reading = gate.look(6900);
    check(reading.flooding && !reading.ended, "under the threshold but quiet for less than 15 s: still flooding");
    reading = gate.look(16900);
    check(reading.flooding && !reading.ended, "quiet for exactly 15 s: still flooding");
    reading = gate.look(17000);
    check(reading.ended && reading.flooding, "quiet for more than 15 s: ends, and that reading still counts as flooding");
    check(!gate.look(17100).flooding, "after the end, the source is calm");
}

static void a_flood_kept_up_does_not_end() {
    Gate gate = flooded();
    // Bursts every 10 s keep the rate at or over the threshold at each look.
    for (uint64_t t = 10000; t <= 60000; t += 10000) {
        arrive(gate, t, 40, 10);
        check(gate.look(t + 400).flooding, "a source that keeps flooding stays flooding");
    }
}

static void holding_rules() {
    Gate gate(0);
    check(gate.hold("A", "a1", 3, 2, true), "a new destination is held");
    check(gate.hold("B", "b1", 1, 2, true), "a second one fits under a limit of 2");
    check(!gate.hold("C", "c1", 1, 2, true), "a third is refused at the limit");
    check(gate.hold("A", "a2", 2, 2, true), "a newer announce replaces the held one at the limit");
    check(!gate.hold("D", "d1", 1, 9, false), "with no room overall, a new destination is refused");
    check(gate.hold("B", "b2", 1, 9, false), "with no room overall, a held destination is still replaced");
    check(gate.held() == 2, "replacing does not add");
}

static void release_order_and_timing() {
    Gate gate = flooded();  // first release no earlier than 16900
    gate.hold("far", "far", 5, 256, true);
    gate.hold("near1", "near1", 1, 256, true);
    gate.hold("near2", "near2", 1, 256, true);
    std::string out;
    check(!gate.release(16000, out), "nothing is let in while the source floods");
    gate.look(17000);  // ends the flood
    check(gate.release(17000, out) && out == "near1", "fewest hops first, the one held longest on a tie");
    check(!gate.release(21999, out), "the next is not let in before 5 s");
    check(gate.release(22000, out) && out == "near2", "the next comes 5 s later");
    check(gate.release(27000, out) && out == "far", "then the farthest");
    check(!gate.release(32000, out) && gate.held() == 0, "nothing left");
}

static void release_waits_for_a_calm_rate() {
    Gate gate = flooded();
    gate.hold("X", "x", 1, 256, true);
    gate.look(17000);  // ends the flood
    arrive(gate, 17000, 60, 5);  // a fresh burst, not yet looked at: all 48 kept are new
    std::string out;
    check(!gate.release(17300, out), "no release while the rate is at or over the threshold");
}

int main() {
    rate_needs_three_arrivals();
    young_and_old_thresholds();
    only_48_arrivals_are_kept();
    flood_ends_after_15_quiet_seconds();
    a_flood_kept_up_does_not_end();
    holding_rules();
    release_order_and_timing();
    release_waits_for_a_calm_rate();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_announce_gate: all passed\n");
    return 0;
}
