#include "Interface.h"

#include "Identity.h"
#include "Transport.h"
#include "Reticulum.h"
#include "Cryptography/Hashes.h"
#include "Cryptography/HKDF.h"

using namespace RNS;
using namespace RNS::Type::Interface;

/*static*/ uint8_t Interface::DISCOVER_PATHS_FOR = MODE_FULL | MODE_ACCESS_POINT | MODE_GATEWAY | MODE_BOUNDARY | MODE_ROAMING;

void Interface::setup_ifac(const char* ifac_netname, const char* ifac_netkey) {
	assert(_impl);
	if (ifac_netname == nullptr && ifac_netkey == nullptr) {
		return;
	}
	// If both are empty strings, treat as no IFAC
	bool has_netname = (ifac_netname != nullptr && ifac_netname[0] != '\0');
	bool has_netkey = (ifac_netkey != nullptr && ifac_netkey[0] != '\0');
	if (!has_netname && !has_netkey) {
		return;
	}

	TRACE("Interface::setup_ifac: setting up IFAC for " + _impl->_name);

	// Build ifac_origin = SHA256(netname) || SHA256(netkey)
	Bytes ifac_origin;
	if (has_netname) {
		Bytes netname_bytes((const uint8_t*)ifac_netname, strlen(ifac_netname));
		Bytes hash = Identity::full_hash(netname_bytes);
		ifac_origin = ifac_origin + hash;
	}
	if (has_netkey) {
		Bytes netkey_bytes((const uint8_t*)ifac_netkey, strlen(ifac_netkey));
		Bytes hash = Identity::full_hash(netkey_bytes);
		ifac_origin = ifac_origin + hash;
	}

	// Hash the combined origin
	Bytes ifac_origin_hash = Identity::full_hash(ifac_origin);

	// Derive ifac_key via HKDF(salt=IFAC_SALT, ikm=ifac_origin_hash, length=64)
	Bytes salt(IFAC_SALT, IFAC_SALT_SIZE);
	_impl->_ifac_key = Cryptography::hkdf(64, ifac_origin_hash, salt);

	// Create an identity from the derived key (64 bytes = 32 X25519 + 32 Ed25519)
	Identity ifac_id(false);  // don't auto-generate keys
	ifac_id.load_private_key(_impl->_ifac_key);
	_impl->_ifac_id = ifac_id;

	// Set _ifac_identity to non-empty to flag IFAC as enabled
	// (Transport checks this with operator bool)
	_impl->_ifac_identity = ifac_id.get_public_key();

	TRACE("Interface::setup_ifac: IFAC configured, ifac_size=" + std::to_string(_impl->_ifac_size));
}

void InterfaceImpl::handle_outgoing(const Bytes& data) {
	//TRACE("InterfaceImpl.handle_outgoing: data: " + data.toHex());
	TRACE("InterfaceImpl.handle_outgoing");
	_txb += data.size();
}

void InterfaceImpl::handle_incoming(const Bytes& data) {
	//TRACE("InterfaceImpl.handle_incoming: data: " + data.toHex());
	TRACE("InterfaceImpl.handle_incoming");
	_rxb += data.size();
	// Create temporary Interface encapsulating our own shared impl
	std::shared_ptr<InterfaceImpl> self = shared_from_this();
	Interface interface(self);
	// Pass data on to transport for handling
	Transport::inbound(data, interface);
}

void Interface::handle_incoming(const Bytes& data) {
	//TRACE("Interface.handle_incoming: data: " + data.toHex());
	TRACE("Interface.handle_incoming");
	assert(_impl);
/*
	_impl->_rxb += data.size();
	// Pass data on to transport for handling
	Transport::inbound(data, *this);
*/
	_impl->handle_incoming(data);
}

void Interface::process_announce_queue() {
/*
	if not hasattr(self, "announce_cap"):
		self.announce_cap = RNS.Reticulum.ANNOUNCE_CAP

	if hasattr(self, "announce_queue"):
		try:
			now = time.time()
			stale = []
			for a in self.announce_queue:
				if now > a["time"]+RNS.Reticulum.QUEUED_ANNOUNCE_LIFE:
					stale.append(a)

			for s in stale:
				if s in self.announce_queue:
					self.announce_queue.remove(s)

			if len(self.announce_queue) > 0:
				min_hops = min(entry["hops"] for entry in self.announce_queue)
				entries = list(filter(lambda e: e["hops"] == min_hops, self.announce_queue))
				entries.sort(key=lambda e: e["time"])
				selected = entries[0]

				double now = OS::time();
				uint32_t wait_time = 0;
				if (_impl->_bitrate > 0 && _impl->_announce_cap > 0) {
					uint32_t tx_time = (len(selected["raw"])*8) / _impl->_bitrate;
					wait_time = (tx_time / _impl->_announce_cap);
				}
				_impl->_announce_allowed_at = now + wait_time;

				self.on_outgoing(selected["raw"])

				if selected in self.announce_queue:
					self.announce_queue.remove(selected)

				if len(self.announce_queue) > 0:
					timer = threading.Timer(wait_time, self.process_announce_queue)
					timer.start()

		except Exception as e:
			self.announce_queue = []
			RNS.log("Error while processing announce queue on "+str(self)+". The contained exception was: "+str(e), RNS.LOG_ERROR)
			RNS.log("The announce queue for this interface has been cleared.", RNS.LOG_ERROR)
*/
}

/*
void ArduinoJson::convertFromJson(JsonVariantConst src, RNS::Interface& dst) {
	TRACE(">>> Deserializing Interface");
TRACE(">>> Interface pre: " + dst.debugString());
	if (!src.isNull()) {
		RNS::Bytes hash;
		hash.assignHex(src.as<const char*>());
		TRACE(">>> Querying Transport for Interface hash " + hash.toHex());
		// Query transport for matching interface
		dst = Transport::find_interface_from_hash(hash);
TRACE(">>> Interface post: " + dst.debugString());
	}
	else {
		dst = {RNS::Type::NONE};
TRACE(">>> Interface post: " + dst.debugString());
	}
}
*/

// ── Ingress control (RETICULUM_BEHAVIOUR.md §1) ──────────────────────────────
// Transport::inbound() asks whether to hold an announce; Transport's 5-second
// round lets held ones in. The rules themselves live in AnnounceGate.h.

uint64_t RNS::ingress_clock() {
	return Utilities::OS::ltime() - Utilities::OS::getTimeOffset();
}

static std::string source_name(const Interface& interface, int client) {
	if (client < 0) { return interface.toString(); }
	return interface.toString() + " client " + std::to_string(client);
}

// Logs what a look at a source found; the bench tests read these lines.
static void report(const Interface& interface, int client, const IngressGate& gate, const IngressGate::Reading& reading) {
	if (reading.began) {
		NOTICEF("[INGRESS] %s burst: %.1f announces/s > %.1f, holding new destinations",
		        source_name(interface, client).c_str(), reading.rate, reading.threshold);
	}
	if (reading.ended) {
		NOTICEF("[INGRESS] %s burst over, %u announces held",
		        source_name(interface, client).c_str(), (unsigned)gate.held());
	}
}

void Interface::announce_arrived() {
	assert(_impl);
	int client;
	_impl->ingress_source(client).arrived(ingress_clock());
}

bool Interface::holding_new_announces() {
	assert(_impl);
	int client;
	IngressGate& gate = _impl->ingress_source(client);
	IngressGate::Reading reading = gate.look(ingress_clock());
	report(*this, client, gate, reading);
	return reading.flooding;
}

bool Interface::hold_new_announce(const Bytes& destination_hash, const Bytes& raw, uint8_t hops, bool room_overall) {
	assert(_impl);
	if (hops >= Type::Transport::PATHFINDER_M - 1) { return false; }
	int client;
	IngressGate& gate = _impl->ingress_source(client);
	// The full allowance fits in PSRAM, not in a board's internal RAM.
	size_t limit = Utilities::OS::heap_in_psram() ? IngressGate::HOLD_LIMIT : IngressGate::HOLD_LIMIT_SMALL;
	return gate.hold(destination_hash, raw, hops, limit, room_overall);
}

void Interface::release_held(std::vector<Bytes>& released) {
	assert(_impl);
	uint64_t now = ingress_clock();
	_impl->for_each_ingress([&](IngressGate& gate, int client) {
		report(*this, client, gate, gate.look(now));
		Bytes announce;
		if (gate.release(now, announce)) {
			released.push_back(announce);
		}
	});
}

size_t Interface::held_announces() const {
	assert(_impl);
	size_t held = 0;
	_impl->for_each_ingress([&](IngressGate& gate, int) { held += gate.held(); });
	return held;
}
