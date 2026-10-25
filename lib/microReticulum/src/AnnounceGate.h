#pragma once

// AnnounceGate: holds back announces from a source that is flooding, and lets
// them in slowly once it calms down. Written from RTNode's
// RETICULUM_BEHAVIOUR.md §1, "Ingress control"; the numbers are explained
// there.
//
// One gate per source (an interface, or a client slot on an interface that
// serves several clients). It knows nothing about packets or interfaces: Blob
// is any copyable type with ==, so the gate also builds and runs on a host
// (tests/native/test_announce_gate.cpp). Times are milliseconds on one
// monotonic clock, passed in by the caller.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace RNS {

template <typename Blob>
class AnnounceGate {
public:
	static constexpr uint8_t  REMEMBERED_ARRIVALS    = 48;
	static constexpr uint8_t  TOO_FEW_FOR_A_RATE     = 2;      // a rate needs more arrivals than this
	static constexpr uint32_t ARRIVAL_FORGOTTEN_MS   = 10000;  // an older arrival may be forgotten
	static constexpr uint64_t YOUNG_SOURCE_MS        = 2ULL * 60 * 60 * 1000;
	static constexpr double   YOUNG_FLOOD_RATE       = 3;      // announces per second
	static constexpr double   FLOOD_RATE             = 10;
	static constexpr uint32_t CALM_TO_END_MS         = 15000;
	static constexpr uint32_t FIRST_RELEASE_AFTER_MS = 15000;
	static constexpr uint32_t RELEASE_SPACING_MS     = 5000;
	static constexpr uint16_t HOLD_LIMIT             = 256;    // per source
	static constexpr uint16_t HOLD_LIMIT_SMALL       = 32;     // boards without PSRAM
	static constexpr uint16_t HOLD_LIMIT_ALL         = 512;    // all sources together
	static constexpr uint16_t HOLD_LIMIT_ALL_SMALL   = 32;

	// What one look at the source found, for the caller to act on and log.
	struct Reading {
		bool flooding = false;  // hold announces for new destinations
		bool began = false;     // the flood began at this reading
		bool ended = false;     // the flood ended at this reading (flooding stays true)
		double rate = 0;        // announces per second
		double threshold = 0;   // the rate above which this source floods
	};

	explicit AnnounceGate(uint64_t now = 0) : _born(now) {}

	void arrived(uint64_t now) { _arrivals.add((uint32_t)now); }

	Reading look(uint64_t now) {
		Reading reading;
		reading.threshold = threshold(now);
		reading.rate = _arrivals.rate((uint32_t)now);
		if (!_flooding) {
			if (reading.rate > reading.threshold) {
				_flooding = true;
				_last_flooding_reading = now;
				_next_release = now + FIRST_RELEASE_AFTER_MS;
				reading.flooding = true;
				reading.began = true;
			}
			return reading;
		}
		reading.flooding = true;
		if (reading.rate >= reading.threshold) {
			_last_flooding_reading = now;
		}
		else if (now > _last_flooding_reading + CALM_TO_END_MS && _arrivals.count() >= TOO_FEW_FOR_A_RATE) {
			_flooding = false;
			reading.ended = true;
		}
		return reading;
	}

	// Holds an announce, replacing any held for the same destination. A new
	// destination is taken only with room left overall (room_overall) and
	// under limit for this source. False: not held.
	bool hold(const Blob& destination, const Blob& announce, uint8_t hops, size_t limit, bool room_overall) {
		for (Held& held : _held) {
			if (held.destination == destination) {
				held.announce = announce;
				held.hops = hops;
				return true;
			}
		}
		if (!room_overall || _held.size() >= limit) { return false; }
		_held.push_back(Held{destination, announce, hops});
		return true;
	}

	// The round's release, after look(): the nearest held announce, when the
	// source is calm and its turn has come.
	bool release(uint64_t now, Blob& announce) {
		if (_flooding || _held.empty() || now < _next_release) { return false; }
		if (_arrivals.rate((uint32_t)now) >= threshold(now)) { return false; }
		auto nearest = std::min_element(_held.begin(), _held.end(),
		                                [](const Held& a, const Held& b) { return a.hops < b.hops; });
		announce = nearest->announce;
		_held.erase(nearest);
		_next_release = now + RELEASE_SPACING_MS;
		return true;
	}

	size_t held() const { return _held.size(); }

private:
	struct Held {
		Blob destination;
		Blob announce;
		uint8_t hops;
	};

	// The most recent arrivals, oldest first, in a fixed ring.
	class Arrivals {
	public:
		void add(uint32_t at) {
			_at[(_first + _count) % REMEMBERED_ARRIVALS] = at;
			if (_count == REMEMBERED_ARRIVALS) { _first = (_first + 1) % REMEMBERED_ARRIVALS; }
			else { _count++; }
		}
		// Arrivals per second since the oldest; then forgets the oldest if it
		// has aged out.
		double rate(uint32_t now) {
			uint8_t counted = _count;
			if (counted <= TOO_FEW_FOR_A_RATE) { return 0; }
			uint32_t since_oldest = now - _at[_first];
			if (since_oldest > ARRIVAL_FORGOTTEN_MS) {
				_first = (_first + 1) % REMEMBERED_ARRIVALS;
				_count--;
			}
			if (since_oldest == 0) { return 0; }
			return counted / (since_oldest / 1000.0);
		}
		uint8_t count() const { return _count; }
	private:
		uint32_t _at[REMEMBERED_ARRIVALS] = {};
		uint8_t _first = 0;
		uint8_t _count = 0;
	};

	double threshold(uint64_t now) const {
		return now - _born < YOUNG_SOURCE_MS ? YOUNG_FLOOD_RATE : FLOOD_RATE;
	}

	uint64_t _born;
	Arrivals _arrivals;
	bool _flooding = false;
	uint64_t _last_flooding_reading = 0;
	uint64_t _next_release = 0;
	std::vector<Held> _held;
};

}
