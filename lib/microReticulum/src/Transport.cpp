#include "Transport.h"
#include "Link.h"

#include "Reticulum.h"
#include "Destination.h"
#include "Identity.h"
#include "Packet.h"
#include "Interface.h"
#include "Log.h"
#include "Cryptography/Random.h"
#include "Cryptography/HKDF.h"
#include "Utilities/OS.h"
#include "Utilities/Persistence.h"

#include <algorithm>
#include <unistd.h>
#include <time.h>

using namespace RNS;
using namespace RNS::Type::Transport;
using namespace RNS::Utilities;

// ── Flat-map helpers (vector<pair<Bytes,T>> replaces std::map<Bytes,T>) ────
// Eliminates per-element tree-node allocation. Linear search is fine
// for N ≤ ~1000 on ESP32.

template <typename T>
static typename std::vector<std::pair<Bytes, T>>::iterator
flatmap_find(std::vector<std::pair<Bytes, T>>& vec, const Bytes& key) {
	for (auto it = vec.begin(); it != vec.end(); ++it) {
		if (it->first == key) return it;
	}
	return vec.end();
}

template <typename T>
static typename std::vector<std::pair<Bytes, T>>::const_iterator
flatmap_find(const std::vector<std::pair<Bytes, T>>& vec, const Bytes& key) {
	for (auto it = vec.begin(); it != vec.end(); ++it) {
		if (it->first == key) return it;
	}
	return vec.end();
}

template <typename T>
static void flatmap_upsert(std::vector<std::pair<Bytes, T>>& vec, const Bytes& key, const T& value) {
	auto it = flatmap_find(vec, key);
	if (it != vec.end()) {
		it->second = value;
	} else {
		vec.push_back({key, value});
	}
}

template <typename T>
static bool flatmap_erase(std::vector<std::pair<Bytes, T>>& vec, const Bytes& key) {
	auto it = flatmap_find(vec, key);
	if (it != vec.end()) {
		vec.erase(it);
		return true;
	}
	return false;
}

// ── Flat-set helpers (vector<Bytes> replaces std::set<Bytes>) ──────────────

static bool flatset_contains(const std::vector<Bytes>& vec, const Bytes& key) {
	return std::find(vec.begin(), vec.end(), key) != vec.end();
}

static void flatset_insert(std::vector<Bytes>& vec, const Bytes& key) {
	if (!flatset_contains(vec, key)) {
		vec.push_back(key);
	}
}

#if defined(INTERFACES_SET)
///*static*/ std::set<std::reference_wrapper<const Interface>, std::less<const Interface>> Transport::_interfaces;
/*static*/ std::set<std::reference_wrapper<Interface>, std::less<Interface>> Transport::_interfaces;
#elif defined(INTERFACES_LIST)
/*static*/ std::list<std::reference_wrapper<Interface>> Transport::_interfaces;
#elif defined(INTERFACES_MAP)
/*static*/ std::map<Bytes, Interface&> Transport::_interfaces;
#endif
#if defined(DESTINATIONS_SET)
/*static*/ std::set<Destination> Transport::_destinations;
#elif defined(DESTINATIONS_MAP)
/*static*/ std::map<Bytes, Destination> Transport::_destinations;
#endif
/*static*/ std::set<Link> Transport::_pending_links;
/*static*/ std::set<Link> Transport::_active_links;
/*static*/ std::vector<Bytes> Transport::_packet_hashlist;
/*static*/ std::vector<Bytes> Transport::_global_blobs;
/*static*/ std::list<PacketReceipt> Transport::_receipts;

/*static*/ std::map<Bytes, Transport::AnnounceEntry> Transport::_announce_table;
/*static*/ std::map<Bytes, std::deque<Transport::PathEntry>> Transport::_destination_table;
/*static*/ std::vector<std::pair<Bytes, Transport::ReverseEntry>> Transport::_reverse_table;
/*static*/ std::map<Bytes, Transport::LinkEntry> Transport::_link_table;
/*static*/ std::map<Bytes, Transport::AnnounceEntry> Transport::_held_announces;
/*static*/ std::set<HAnnounceHandler> Transport::_announce_handlers;
/*static*/ std::map<Bytes, Transport::TunnelEntry> Transport::_tunnels;
/*static*/ std::vector<std::pair<Bytes, Transport::RateEntry>> Transport::_announce_rate_table;
/*static*/ std::vector<std::pair<Bytes, double>> Transport::_path_requests;

/*static*/ std::vector<std::pair<Bytes, Transport::PathRequestEntry>> Transport::_discovery_path_requests;
/*static*/ std::set<Bytes> Transport::_discovery_pr_tags;

/*static*/ std::set<Destination> Transport::_control_destinations;
/*static*/ std::set<Bytes> Transport::_control_hashes;
/*static*/ std::map<Bytes, Bytes> Transport::_pending_local_path_requests;

/*static*/ double Transport::_start_time				= 0.0;
/*static*/ bool Transport::_jobs_locked					= false;
/*static*/ bool Transport::_jobs_running				= false;
/*static*/ float Transport::_job_interval				= 0.250;
/*static*/ double Transport::_jobs_last_run				= 0.0;
/*static*/ double Transport::_links_last_checked		= 0.0;
/*static*/ float Transport::_links_check_interval		= 1.0;
/*static*/ double Transport::_receipts_last_checked		= 0.0;
/*static*/ float Transport::_receipts_check_interval	= 1.0;
/*static*/ double Transport::_announces_last_checked	= 0.0;
/*static*/ float Transport::_announces_check_interval	= 1.0;
/*static*/ double Transport::_tables_last_culled		= 0.0;
// CBA MCU
/*static*/ //float Transport::_tables_cull_interval		= 5.0;
/*static*/ float Transport::_tables_cull_interval		= 60.0;
/*static*/ bool Transport::_saving_path_table			= false;
// CBA ACCUMULATES
// CBA MCU
/*static*/ //uint16_t Transport::_hashlist_maxsize		= 1000000;
/*static*/ //uint16_t Transport::_hashlist_maxsize		= 100;
/*static*/ uint16_t Transport::_hashlist_maxsize		= 100;
// CBA ACCUMULATES
// CBA MCU
/*static*/ //uint16_t Transport::_max_pr_tags			= 32000;
/*static*/ uint16_t Transport::_max_pr_tags				= 32;

// CBA
// CBA ACCUMULATES
/*static*/ uint16_t Transport::_path_table_maxsize		= 100;
// CBA ACCUMULATES
/*static*/ uint16_t Transport::_path_table_maxpersist	= 100;
/*static*/ double Transport::_last_saved				= 0.0;
/*static*/ float Transport::_save_interval				= 3600.0;
/*static*/ uint32_t Transport::_destination_table_crc	= 0;

/*static*/ Reticulum Transport::_owner({Type::NONE});

// FIREWALL MODE Whitelist 1: addresses of local devices (from LoRa and LocalTCP interfaces)
static std::vector<Bytes> _firewall_local_addresses;
// FIREWALL MODE Whitelist 2: addresses mentioned in packets from local devices
static std::vector<Bytes> _firewall_mentioned_addresses;
static const uint16_t _firewall_maxsize = 200;
// FIREWALL MODE pinned addresses: destinations owned by this node that must always be
// reachable from the backbone (e.g. a management destination). Never culled.
static std::vector<Bytes> _firewall_pinned_addresses;

/*static*/ void Transport::firewall_pin_local_destination(const Bytes& addr) {
	if (!addr) return;
	if (std::find(_firewall_pinned_addresses.begin(), _firewall_pinned_addresses.end(), addr)
	    == _firewall_pinned_addresses.end()) {
		_firewall_pinned_addresses.push_back(addr);
	}
}

// ── Whitelist helpers: enforce uniqueness on push (no per-node alloc) ──
static void wl1_push(const Bytes& addr) {
	if (!addr) return;
	if (std::find(_firewall_local_addresses.begin(), _firewall_local_addresses.end(), addr)
	    == _firewall_local_addresses.end()) {
		_firewall_local_addresses.push_back(addr);
	}
}
static bool wl2_push(const Bytes& addr) {
	if (!addr) return false;
	if (std::find(_firewall_mentioned_addresses.begin(), _firewall_mentioned_addresses.end(), addr)
	    == _firewall_mentioned_addresses.end()) {
		_firewall_mentioned_addresses.push_back(addr);
		return true;
	}
	return false;
}

// ── Watched-destination logging ────────────────────────────────────────
// Define WATCH_LOG to only log packets touching specific named destinations.
// Comment out to restore full logging.
#define WATCH_LOG
#ifdef WATCH_LOG
struct WatchedDest {
	const char* name;
	Bytes       hash;
};
static const WatchedDest _watched[] = {
	{"Meshchat",  Bytes("\x13\xf4\xb1\x4d\xd3\x64\xa6\x72\xe8\x53\xa3\x7f\xb5\x34\x67\x8c")},
	{"Sideband",  Bytes("\x29\xf9\xa2\x0c\xb0\x47\x60\xed\xbc\x49\x7d\x66\x8c\x25\x5a\xb7")},
	{"Browser",   Bytes("\x03\x58\x19\x2f\x7b\x88\xb0\x78\x68\xe8\xd8\x32\xce\xa5\x1b\x7e")},
};
// Populated at runtime: ratchet hashes and link IDs → parent destination name
static std::map<Bytes, const char*> _watch_aliases;

static void watch_add_alias(const Bytes& alias, const char* name) {
	if (alias && _watch_aliases.find(alias) == _watch_aliases.end()) {
		_watch_aliases[alias] = name;
	}
}
static const char* watch_resolve(const Bytes& addr) {
	if (!addr) return nullptr;
	for (auto& w : _watched) if (addr == w.hash) return w.name;
	auto it = _watch_aliases.find(addr);
	return (it != _watch_aliases.end()) ? it->second : nullptr;
}
// Check if a packet touches any watched destination; return name if so.
static const char* watch_match(const Packet& packet) {
	const char* n;
	if ((n = watch_resolve(packet.destination_hash()))) return n;
	if (packet.packet_type() == Type::Packet::LINKREQUEST) {
		Bytes lid = Link::link_id_from_lr_packet(packet);
		if ((n = watch_resolve(lid))) return n;
	}
	if (packet.data().size() >= Type::Identity::TRUNCATED_HASHLENGTH/8) {
		if ((n = watch_resolve(packet.data().left(Type::Identity::TRUNCATED_HASHLENGTH/8)))) return n;
	}
	if (packet.packet_type() == Type::Packet::ANNOUNCE
	    && packet.context_flag() == Type::Packet::FLAG_SET
	    && packet.data().size() >= 116) {
		Bytes rh = Identity::truncated_hash(packet.data().mid(84, 32));
		if ((n = watch_resolve(rh))) return n;
	}
	if (packet.transport_id() && (n = watch_resolve(packet.transport_id()))) return n;
	return nullptr;
}
// Helper: human-readable zone tag
static inline const char* zone_tag(bool is_backbone) {
	return is_backbone ? "WAN" : "LAN";
}
// Helper: short hash for logging
static std::string short_hash(const Bytes& h) {
	if (!h) return "none";
	return h.toHex().substr(0,8);
}
#define WLOG(pkt, msg) do { \
	const char* _wn = watch_match(pkt); \
	if (_wn) { \
		Interface _rif = pkt.receiving_interface(); \
		bool _from_bb = _rif && is_backbone_interface(_rif); \
		Serial.print("["); Serial.print(_wn); Serial.print("] "); \
		Serial.printf("[%s] - FROM: %s (%s) - %s\n", \
			pkt_type_name(pkt.packet_type()), \
			_rif ? _rif.toString().c_str() : "?", \
			zone_tag(_from_bb), \
			std::string(msg).c_str()); \
	} \
} while(0)
#else
#define WLOG(fmt, ...) do {} while(0)
#endif

// FIREWALL MODE: Check if an interface is the backbone
static bool is_backbone_interface(const Interface& iface) {
	return iface.is_backbone();
}
static bool is_trusted_local_interface(const Interface& iface) {
	return iface.is_local_client();
}
// Human-readable packet type abbreviations
static const char* pkt_type_name(uint8_t t) {
	switch (t) {
		case 0: return "DATA";
		case 1: return "ANNC";
		case 2: return "LREQ";
		case 3: return "PROOF";
		default: return "?";
	}
}
// Human-readable context name for resource tracing
static const char* ctx_name(uint8_t ctx) {
	switch (ctx) {
		case 0x01: return "RESOURCE";
		case 0x02: return "RESOURCE_ADV";
		case 0x03: return "RESOURCE_REQ";
		case 0x04: return "RESOURCE_HMU";
		case 0x05: return "RESOURCE_PRF";
		case 0x06: return "RESOURCE_ICL";
		case 0x07: return "RESOURCE_RCL";
		default:   return nullptr;
	}
}
static inline bool is_resource_ctx(uint8_t ctx) {
	return ctx >= 0x01 && ctx <= 0x07;
}
/*static*/ Identity Transport::_identity({Type::NONE});

// CBA
/*static*/ Transport::Callbacks Transport::_callbacks;

// CBA Stats
/*static*/ uint32_t Transport::_packets_sent = 0;
/*static*/ uint32_t Transport::_packets_received = 0;
/*static*/ uint32_t Transport::_destinations_added = 0;
/*static*/ size_t Transport::_last_memory = 0;
/*static*/ size_t Transport::_last_flash = 0;

/*static*/ void Transport::start(const Reticulum& reticulum_instance) {
	INFO("Transport starting...");
	_jobs_running = true;
	_owner = reticulum_instance;

	// Initialize time-based variables *after* time offset update
	_jobs_last_run = OS::time();
	_links_last_checked = OS::time();
	_receipts_last_checked = OS::time();
	_announces_last_checked = OS::time();
	_tables_last_culled = OS::time();
	_last_saved = OS::time();

	// ensure required directories exist
	if (!OS::directory_exists(Reticulum::_cachepath)) {
		VERBOSE("No cache directory, creating...");
		if (!OS::create_directory(Reticulum::_cachepath)) {
			HEAD("Failed to create cache directory — packet cache will be unavailable", RNS::LOG_CRITICAL);
		}
	}

	if (!_identity) {
		char transport_identity_path[Type::Reticulum::FILEPATH_MAXSIZE];
		snprintf(transport_identity_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/transport_identity", Reticulum::_storagepath);
		DEBUG("Checking for transport identity...");
		try {
			if (OS::file_exists(transport_identity_path)) {
				_identity = Identity::from_file(transport_identity_path);
			}

			if (!_identity) {
				VERBOSE("No valid Transport Identity in storage, creating...");
				_identity = Identity();
				_identity.to_file(transport_identity_path);
			}
			else {
				VERBOSE("Loaded Transport Identity from storage");
			}
		}
		catch (std::exception& e) {
			ERRORF("Failed to check for transport identity, the contained exception was: %s", e.what());
		}
	}

// TODO
/*
	packet_hashlist_path = Reticulum::storagepath + "/packet_hashlist";
	if (!owner.is_connected_to_shared_instance()) {
		if (os.path.isfile(packet_hashlist_path)) {
			try {
			}
			catch (std::exception& e) {
				ERRORF("Could not load packet hashlist from storage, the contained exception was: %s", e.what());
			}
		}
	}
*/

	// Create transport-specific destination for path request
	Destination path_request_destination({Type::NONE}, Type::Destination::IN, Type::Destination::PLAIN, APP_NAME, "path.request");
	path_request_destination.set_packet_callback(path_request_handler);
	// CBA ACCUMULATES
	_control_destinations.insert(path_request_destination);
	// CBA ACCUMULATES
	_control_hashes.insert(path_request_destination.hash());
	NOTICE("PATH-REQ-DST: " + path_request_destination.hash().toHex().substr(0,8) + " — path requests to this hash will seed whitelist");

	// Create transport-specific destination for tunnel synthesize
	Destination tunnel_synthesize_destination({Type::NONE}, Type::Destination::IN, Type::Destination::PLAIN, APP_NAME, "tunnel.synthesize");
	tunnel_synthesize_destination.set_packet_callback(tunnel_synthesize_handler);
	// CBA BUG?
	// CBA ACCUMULATES
	_control_destinations.insert(tunnel_synthesize_destination);
	// CBA ACCUMULATES
	_control_hashes.insert(tunnel_synthesize_destination.hash());
	DEBUG("Created transport-specific tunnel synthesize destination " + tunnel_synthesize_destination.hash().toHex());

	_jobs_running = false;

	// CBA Threading

	if (true) {
		INFO("Transport mode is enabled");

		// Read in path table and then write and clean in case any entries are invalid
		read_path_table();
		DEBUG("Writing path table and cleaning caches to clean-up any orphaned paths/files");
		write_path_table();
		clean_caches();

		read_tunnel_table();

		// Create transport-specific destination for probe requests
		if (Reticulum::probe_destination_enabled()) {
			Destination probe_destination(_identity, Type::Destination::IN, Type::Destination::SINGLE, APP_NAME, "probe");
			probe_destination.accepts_links(false);
			probe_destination.set_proof_strategy(Type::Destination::PROVE_ALL);
			DEBUG("Created probe responder destination " + probe_destination.hash().toHex());
			probe_destination.announce();
			NOTICE("Transport Instance will respond to probe requests on " + probe_destination.toString());
		}

		VERBOSE("Transport instance " + _identity.toString() + " started");
		_start_time = OS::time();
	}

// TODO
//#ifndef NDEBUG
	// CBA DEBUG
	dump_stats();
//#endif
}

/*static*/ void Transport::loop() {
	if (OS::time() > (_jobs_last_run + _job_interval)) {
		jobs();
		_jobs_last_run = OS::time();
	}
}

/*static*/ void Transport::jobs() {
	//TRACE("Transport::jobs()");

	// Heap telemetry: snapshot at jobs entry
	size_t _jobs_heap_entry = OS::heap_available();

	std::vector<Packet> outgoing;
	std::set<Bytes> path_requests;
	int count;
	_jobs_running = true;

	try {
		if (!_jobs_locked) {

			// Process active and pending link lists
			if (OS::time() > (_links_last_checked + _links_check_interval)) {
				std::set<Link> pending_links(_pending_links);
				for (auto& link : pending_links) {
					if (link.status() == Type::Link::CLOSED) {
						// If we are not a Transport Instance, finding a pending link
						// that was never activated will trigger removal of the path
						// via the link's interface, leaving backup paths intact.
						if (false) {
							// Remove only the path on the failed link's interface,
							// so other paths (e.g. LoRa) survive the failure.
							Interface failed_iface = link.attached_interface();
							if (failed_iface) {
								DEBUG("Removing path to " + link.destination().hash().toHex() + " via " + failed_iface.toString() + " (link closed, backup paths survive)");
								mark_path_unresponsive(link.destination().hash(), failed_iface.get_hash());
							} else {
								expire_path(link.destination().hash());
							}

							// If we are connected to a shared instance, it will take
							// care of sending out a new path request. If not, we will
							// send one directly.
							if (!_owner.is_connected_to_shared_instance()) {
								double last_path_request = 0;
								auto iter = flatmap_find(_path_requests, link.destination().hash());
								if (iter != _path_requests.end()) {
									last_path_request = (*iter).second;
								}

								if ((OS::time() - last_path_request) > Type::Transport::PATH_REQUEST_MI) {
									DEBUG("Trying to rediscover path for " + link.destination().hash().toHex() + " since an attempted link was never established");
									//if (path_requests.find(link.destination().hash()) == path_requests.end()) {
									if (path_requests.count(link.destination().hash()) == 0) {
										// CBA ACCUMULATES
										path_requests.insert(link.destination().hash());
									}
								}
							}
						}

						_pending_links.erase(link);
					}
				}
				std::set<Link> active_links(_active_links);
				for (auto& link : active_links) {
					if (link.status() == Type::Link::CLOSED) {
						_active_links.erase(link);
					}
				}

				_links_last_checked = OS::time();
			}

			// Process receipts list for timed-out packets
			if (OS::time() > (_receipts_last_checked + _receipts_check_interval)) {
				while (_receipts.size() > Type::Transport::MAX_RECEIPTS) {
					PacketReceipt culled_receipt = _receipts.front();
					_receipts.pop_front();
					culled_receipt.set_timeout(-1);
					culled_receipt.check_timeout();
				}

				std::list<PacketReceipt> cull_receipts;
				for (auto& receipt : _receipts) {
					receipt.check_timeout();
					if (receipt.status() != Type::PacketReceipt::SENT) {
						cull_receipts.push_back(receipt);
					}
				}
				// CBA since modifying of collection while iterating is forbidden
				for (auto& receipt : _receipts) {
					cull_receipts.remove(receipt);
				}

				_receipts_last_checked = OS::time();
			}

			// Process announces needing retransmission
			if (OS::time() > (_announces_last_checked + _announces_check_interval)) {
				DEBUG("DIAG: ANNOUNCE-TBL size=" + std::to_string(_announce_table.size()));
				for (auto& [destination_hash, announce_entry] : _announce_table) {
				//for (auto& pair : _announce_table) {
				//	const auto& destination_hash = pair.first;
				//	auto& announce_entry = pair.second;
//TRACE("[0] announce entry data size: " + std::to_string(announce_entry._packet.data().size()));
					DEBUG("DIAG: ANNOUNCE-ENTRY dest=" + destination_hash.toHex().substr(0,8) + " retries=" + std::to_string(announce_entry._retries) + " block=" + std::to_string(announce_entry._block_rebroadcasts) + " timeout_in=" + std::to_string(announce_entry._retransmit_timeout - OS::time()));
					if (announce_entry._retries > 0 && announce_entry._retries >= Type::Transport::LOCAL_REBROADCASTS_MAX) {
						TRACE("Completed announce processing for " + destination_hash.toHex() + ", local rebroadcast limit reached");
						// CBA OK to modify collection here since we're immediately exiting iteration
						_announce_table.erase(destination_hash);
						break;
					}
					else if (announce_entry._retries > Type::Transport::PATHFINDER_R) {
						DEBUG("DIAG: ANNOUNCE-CULL dest=" + destination_hash.toHex().substr(0,8) + " retries=" + std::to_string(announce_entry._retries) + " reason=retry_limit");
						TRACE("Completed announce processing for " + destination_hash.toHex() + ", retry limit reached");
						// CBA OK to modify collection here since we're immediately exiting iteration
						_announce_table.erase(destination_hash);
						break;
					}
					else {
						if (OS::time() > announce_entry._retransmit_timeout) {
							TRACE("Performing announce processing for " + destination_hash.toHex() + "...");
							announce_entry._retransmit_timeout = OS::time() + Type::Transport::PATHFINDER_G + Type::Transport::PATHFINDER_RW;
							announce_entry._retries += 1;
							Type::Packet::context_types announce_context = Type::Packet::CONTEXT_NONE;
							if (announce_entry._block_rebroadcasts) {
								announce_context = Type::Packet::PATH_RESPONSE;
							}
							Identity announce_identity(Identity::recall(announce_entry._packet.destination_hash()));
							//Destination announce_destination(announce_identity, Type::Destination::OUT, Type::Destination::SINGLE, "unknown", "unknown");
							//announce_destination.hash(announce_entry._packet.destination_hash());
							Destination announce_destination(announce_identity, Type::Destination::OUT, Type::Destination::SINGLE, announce_entry._packet.destination_hash());
							//P announce_destination.hexhash = announce_destination.hash.hex()

//if (announce_entry._attached_interface) {
//TRACE("[1] interface is valid");
//TRACE("[1] interface: " + announce_entry._attached_interface.debugString());
//TRACE("[1] interface: " + announce_entry._attached_interface.toString());
//}
							Packet new_packet(
								announce_destination,
								//{Type::NONE},
								announce_entry._attached_interface,
								//{Type::NONE},
								announce_entry._packet.data(),
								Type::Packet::ANNOUNCE,
								announce_context,
								Type::Transport::TRANSPORT,
								Type::Packet::HEADER_2,
								Transport::_identity.hash(),
								true,
								announce_entry._packet.context_flag()
							);

							new_packet.hops(announce_entry._hops);
							if (announce_entry._block_rebroadcasts) {
								DEBUG("Rebroadcasting announce as path response for " + announce_destination.hash().toHex() + " with hop count " + std::to_string(new_packet.hops()));
								DEBUG("DIAG: SENDING PATH-RESP announce for " + announce_destination.hash().toHex().substr(0,8) + " hops=" + std::to_string(new_packet.hops()) + " attached=" + (announce_entry._attached_interface ? announce_entry._attached_interface.toString() : "NONE"));
							}
							else {
								DEBUG("Rebroadcasting announce for " + announce_destination.hash().toHex() + " with hop count " + std::to_string(new_packet.hops()));
							}
							
							outgoing.push_back(new_packet);

							// This handles an edge case where a peer sends a past
							// request for a destination just after an announce for
							// said destination has arrived, but before it has been
							// rebroadcast locally. In such a case the actual announce
							// is temporarily held, and then reinserted when the path
							// request has been served to the peer.
							auto iter =_held_announces.find(destination_hash);
							if (iter != _held_announces.end()) {
								auto held_entry = (*iter).second;
								_held_announces.erase(iter);
								//_announce_table[destination_hash] = held_entry;
								//_announce_table.insert_or_assign({destination_hash, held_entry});
								_announce_table.erase(destination_hash);
								// CBA ACCUMULATES
								_announce_table.insert({destination_hash, held_entry});
								DEBUG("Reinserting held announce into table");
								// CBA Must break after erase to avoid iterator invalidation
								// (same pattern as the other two erases above in this loop)
								break;
							}
						}
					}
				}

				_announces_last_checked = OS::time();
			}

			// Cull held announces that are older than 60 seconds or if map exceeds cap
			{
				const double held_timeout = 60.0;
				const uint16_t held_maxsize = 32;
				auto iter = _held_announces.begin();
				while (iter != _held_announces.end()) {
					if (OS::time() > ((*iter).second._timestamp + held_timeout)) {
						DEBUG("Culling expired held announce for " + (*iter).first.toHex());
						iter = _held_announces.erase(iter);
					} else {
						++iter;
					}
				}
				while (_held_announces.size() > held_maxsize) {
					DEBUG("Culling oldest held announce (cap " + std::to_string(held_maxsize) + ")");
					_held_announces.erase(_held_announces.begin());
				}
			}

			// Cull the packet hashlist if it has reached its max size
			if (_packet_hashlist.size() > _hashlist_maxsize) {
				size_t excess = _packet_hashlist.size() - _hashlist_maxsize;
				_packet_hashlist.erase(_packet_hashlist.begin(), _packet_hashlist.begin() + excess);
			}

#ifdef FIREWALL_MODE
			// Cull the firewall mentioned addresses if it has reached its max size
			if (_firewall_mentioned_addresses.size() > _firewall_maxsize) {
				size_t excess = _firewall_mentioned_addresses.size() - _firewall_maxsize;
				_firewall_mentioned_addresses.erase(_firewall_mentioned_addresses.begin(), _firewall_mentioned_addresses.begin() + excess);
			}

			// Cull the firewall local addresses if it has reached its max size
			if (_firewall_local_addresses.size() > _firewall_maxsize) {
				size_t excess = _firewall_local_addresses.size() - _firewall_maxsize;
				_firewall_local_addresses.erase(_firewall_local_addresses.begin(), _firewall_local_addresses.begin() + excess);
			}
#endif

			// Cull the path request tags list if it has reached its max size
			if (_discovery_pr_tags.size() > _max_pr_tags) {
				std::set<Bytes>::iterator iter = _discovery_pr_tags.begin();
				std::advance(iter, _discovery_pr_tags.size() - _max_pr_tags);
				_discovery_pr_tags.erase(_discovery_pr_tags.begin(), iter);
			}

			if (OS::time() > (_tables_last_culled + _tables_cull_interval)) {

				// CBA Disabled following since we're calling immediately after adding to path table now
				// Cull the path table if it has reached its max size
				//cull_path_table();

				// Cull the reverse table according to timeout
				std::vector<Bytes> stale_reverse_entries;
				for (const auto& [packet_hash, reverse_entry] : _reverse_table) {
					if (OS::time() > (reverse_entry._timestamp + REVERSE_TIMEOUT)) {
						stale_reverse_entries.push_back(packet_hash);
					}
				}

				// Cull the link table according to timeout
				std::vector<Bytes> stale_links;
				for (const auto& [link_id, link_entry] : _link_table) {
					if (link_entry._validated) {
						if (OS::time() > (link_entry._timestamp + LINK_TIMEOUT)) {
							stale_links.push_back(link_id);
						}
					}
					else {
						if (OS::time() > link_entry._proof_timeout) {
							stale_links.push_back(link_id);

							double last_path_request = 0.0;
							const auto& iter = flatmap_find(_path_requests, link_entry._destination_hash);
							if (iter != _path_requests.end()) {
								last_path_request = (*iter).second;
							}

							uint8_t lr_taken_hops = link_entry._hops;

							bool path_request_throttle = (OS::time() - last_path_request) < PATH_REQUEST_MI;
							bool path_request_conditions = false;
							
							// If the path has been invalidated between the time of
							// making the link request and now, try to rediscover it
							if (!has_path(link_entry._destination_hash)) {
								DEBUG("Trying to rediscover path for " + link_entry._destination_hash.toHex() + " since an attempted link was never established, and path is now missing");
								path_request_conditions = true;
							}

							// If this link request was originated from a local client
							// attempt to rediscover a path to the destination, if this
							// has not already happened recently.
							else if (!path_request_throttle && lr_taken_hops == 0) {
								DEBUG("Trying to rediscover path for " + link_entry._destination_hash.toHex() + " since an attempted local client link was never established");
								path_request_conditions = true;
							}

							// If the link destination was previously only 1 hop
							// away, this likely means that it was local to one
							// of our interfaces, and that it roamed somewhere else.
							// In that case, try to discover a new path.
							else if (!path_request_throttle && hops_to(link_entry._destination_hash) == 1) {
								DEBUG("Trying to rediscover path for " + link_entry._destination_hash.toHex() + " since an attempted link was never established, and destination was previously local to an interface on this instance");
								path_request_conditions = true;
							}

							// If the link destination was previously only 1 hop
							// away, this likely means that it was local to one
							// of our interfaces, and that it roamed somewhere else.
							// In that case, try to discover a new path.
							else if ( !path_request_throttle and lr_taken_hops == 1) {
								DEBUG("Trying to rediscover path for " + link_entry._destination_hash.toHex() + " since an attempted link was never established, and link initiator is local to an interface on this instance");
								path_request_conditions = true;
							}

							if (path_request_conditions) {
								if (path_requests.count(link_entry._destination_hash) == 0) {
									// CBA ACCUMULATES
									path_requests.insert(link_entry._destination_hash);
								}

								if (false) {
									// Remove the failed outbound interface's path,
									// leaving backup paths (e.g. LoRa) intact so
									// select_path() falls through naturally.
									if (link_entry._outbound_interface) {
										DEBUG("Removing path to " + link_entry._destination_hash.toHex() + " via " + link_entry._outbound_interface.toString() + " (link establishment failed, backup paths survive)");
										mark_path_unresponsive(link_entry._destination_hash, link_entry._outbound_interface.get_hash());
									} else {
										expire_path(link_entry._destination_hash);
									}
								}
							}
						}
					}
				}

				// Cull the path table: remove expired entries and empty deques
				std::vector<Bytes> stale_paths;
				{
					double now = OS::time();
					for (auto& [destination_hash, deque] : _destination_table) {
						// Remove expired individual entries
						deque.erase(std::remove_if(deque.begin(), deque.end(),
							[now](const PathEntry& e) { return e.is_expired(now); }),
							deque.end());

						// Remove entries whose interface no longer exists
						deque.erase(std::remove_if(deque.begin(), deque.end(),
							[](const PathEntry& e) {
								Interface iface = find_interface_from_hash(e.receiving_interface);
								return !iface;
							}), deque.end());

						if (deque.empty()) {
							stale_paths.push_back(destination_hash);
						}
					}
					for (const auto& destination_hash : stale_paths) {
						_destination_table.erase(destination_hash);
						DEBUG("Path to " + destination_hash.toHex() + " timed out and was removed");
					}
				}

				// Cull the pending discovery path requests table
				std::vector<Bytes> stale_discovery_path_requests;
				for (const auto& [destination_hash, path_entry] : _discovery_path_requests) {
					if (OS::time() > path_entry._timeout) {
						stale_discovery_path_requests.push_back(destination_hash);
						NOTICE("DISCOVERY-EXPIRED: path request for " + destination_hash.toHex().substr(0,8) + " timed out after " + std::to_string((int)Type::Transport::PATH_REQUEST_TIMEOUT) + "s");
					}
				}

				// Cull the path requests table (entries only needed for PATH_REQUEST_MI throttling, 20s)
				{
					std::vector<Bytes> stale_path_requests;
					for (const auto& [destination_hash, timestamp] : _path_requests) {
						if (OS::time() > (timestamp + PATH_REQUEST_MI * 2)) {
							stale_path_requests.push_back(destination_hash);
						}
					}
					for (const Bytes& destination_hash : stale_path_requests) {
						flatmap_erase(_path_requests, destination_hash);
					}
				}

				// Cull pending local path requests for interfaces that no longer exist
				{
					std::vector<Bytes> stale_plpr;
					for (const auto& [destination_hash, iface_hash] : _pending_local_path_requests) {
						if (_interfaces.count(iface_hash) == 0) {
							stale_plpr.push_back(destination_hash);
						}
					}
					for (const Bytes& destination_hash : stale_plpr) {
						_pending_local_path_requests.erase(destination_hash);
					}
				}

				// Cull the tunnel table
				count = 0;
				std::vector<Bytes> stale_tunnels;
				for (const auto& [tunnel_id, tunnel_entry] : _tunnels) {
					if (OS::time() > tunnel_entry._expires) {
						stale_tunnels.push_back(tunnel_id);
						TRACE("Tunnel " + tunnel_id.toHex() + " timed out and was removed");
					}
					else {
						std::vector<Bytes> stale_tunnel_paths;
						for (const auto& [destination_hash, destination_entry] : tunnel_entry._serialised_paths) {
							if (OS::time() > (destination_entry._timestamp + DESTINATION_TIMEOUT)) {
								stale_tunnel_paths.push_back(destination_hash);
								TRACE("Tunnel path to " + destination_hash.toHex() + " timed out and was removed");
							}
						}

						//for (const auto& destination_hash : stale_tunnel_paths) {
						for (const Bytes& destination_hash : stale_tunnel_paths) {
							const_cast<TunnelEntry&>(tunnel_entry)._serialised_paths.erase(destination_hash);
							++count;
						}
					}
				}
				if (count > 0) {
					TRACE("Removed " + std::to_string(count) + " tunnel paths");
				}
				
				remove_reverse_entries(stale_reverse_entries);
				remove_links(stale_links);
				remove_paths(stale_paths);
				remove_discovery_path_requests(stale_discovery_path_requests);
				remove_tunnels(stale_tunnels);

//#ifndef NDEBUG
				dump_stats();
//#endif

				_tables_last_culled = OS::time();
			}

			// CBA Periodically persist data
			//if (OS::time() > (_last_saved + _save_interval)) {
			//	persist_data();
			//	_last_saved = OS::time();
			//}
		}
		else {
			// Transport jobs were locked, do nothing
		}
	}
	catch (std::exception& e) {
		ERROR("An exception occurred while running Transport jobs.");
		ERRORF("The contained exception was: %s", e.what());
	}

	_jobs_running = false;

	// Heap telemetry: snapshot at jobs exit (MUTED)
	// {
	// 	size_t _jobs_heap_exit = OS::heap_available();
	// 	int _jobs_delta = (int)_jobs_heap_exit - (int)_jobs_heap_entry;
	// 	if (_jobs_delta < -64 || _jobs_delta > 64) {
	// 		VERBOSEF("[HEAP-TEL] jobs: %d bytes (heap=%u)", _jobs_delta, (uint32_t)_jobs_heap_exit);
	// 	}
	// }

	// CBA send announce retransmission packets
	for (auto& packet : outgoing) {
		DEBUG("DIAG: OUTGOING announce dest=" + packet.destination_hash().toHex().substr(0,8) + " type=" + std::to_string(packet.packet_type()) + " ctx=" + std::to_string(packet.context()) + " attached=" + (packet.attached_interface() ? packet.attached_interface().toString() : "NONE"));
		packet.send();
	}

	// CBA send link-related path requests
	for (auto& destination_hash : path_requests) {
		request_path(destination_hash);
	}
}

/*static*/ void Transport::transmit(Interface& interface, const Bytes& raw) {
	TRACE("Transport::transmit()");
	// CBA
	if (_callbacks._transmit_packet) {
		try {
			_callbacks._transmit_packet(raw, interface);
		}
		catch (std::exception& e) {
			DEBUG("Error while executing transmit packet callback. The contained exception was: " + std::string(e.what()));
		}
	}
	try {
		//if hasattr(interface, "ifac_identity") and interface.ifac_identity != None:
		if (interface.ifac_identity()) {
			// Calculate packet access code by signing the raw packet
			// and taking the last ifac_size bytes of the signature
			Bytes signature = interface.ifac_id().sign(raw);
			Bytes ifac = signature.right(interface.ifac_size());

			// Generate mask via HKDF
			Bytes mask = Cryptography::hkdf(
				raw.size() + interface.ifac_size(),
				ifac,
				interface.ifac_key()
			);

			// Set IFAC flag in header byte 0
			uint8_t new_header0 = raw[0] | 0x80;
			uint8_t new_header1 = raw[1];

			// Assemble new payload: new_header + ifac + raw[2:]
			Bytes new_raw;
			new_raw.append(new_header0);
			new_raw.append(new_header1);
			new_raw.append(ifac);
			new_raw.append(raw.mid(2));

			// Mask payload
			Bytes masked_raw;
			for (size_t i = 0; i < new_raw.size(); i++) {
				if (i == 0) {
					// Mask first header byte, keep IFAC flag set
					masked_raw.append((uint8_t)((new_raw[i] ^ mask[i]) | 0x80));
				}
				else if (i == 1 || i > (size_t)(interface.ifac_size() + 1)) {
					// Mask second header byte and payload
					masked_raw.append((uint8_t)(new_raw[i] ^ mask[i]));
				}
				else {
					// Don't mask the IFAC itself
					masked_raw.append(new_raw[i]);
				}
			}

			// Send it
			interface.send_outgoing(masked_raw);
		}
		else {
			interface.send_outgoing(raw);
		}
	}
	catch (std::exception& e) {
		ERROR("Error while transmitting on " + interface.toString() + ". The contained exception was: " + e.what());
	}
}

/*static*/ bool Transport::outbound(Packet& packet) {
	TRACE("Transport::outbound()");
	++_packets_sent;

	if (!packet.destination()) {
		//throw std::invalid_argument("Can not send packet with no destination.");
		ERROR("Can not send packet with no destination");
		return false;
	}

	TRACE("Transport::outbound: destination=" + packet.destination_hash().toHex() + " hops=" + std::to_string(packet.hops()));

	while (_jobs_running) {
		TRACE("Transport::outbound: sleeping...");
		OS::sleep(0.0005);
	}

	_jobs_locked = true;

	bool sent = false;
	double outbound_time = OS::time();

	// Check if we have a known path for the destination in the path table
	if (packet.packet_type() != Type::Packet::ANNOUNCE && packet.destination().type() != Type::Destination::PLAIN && packet.destination().type() != Type::Destination::GROUP && has_path(packet.destination_hash())) {
		// ── Multi-path hedging ───────────────────────────────────────
		// Walk all paths in score order (best first).  Each path gets
		// the packet.  If the path is stale (multi-hop transport only,
		// hops >= 2), fire a path_request to refresh it AND continue to
		// the next-best path so delivery isn't gated on a dead route.
		// The first fresh path stops the hedge.
		TRACE("Transport::outbound: Path to destination is known, hedging...");
		std::vector<PathEntry> paths = select_all_paths(packet.destination_hash());
		for (auto& entry : paths) {
			uint8_t hops = entry.hops;
			Interface outbound_interface = find_interface_from_hash(entry.receiving_interface);
			Bytes nh = entry.next_hop;

			if (!outbound_interface) continue;

			// ── Build and transmit on this path ─────────────────
			if (hops > 1) {
				TRACE("Forwarding packet to next closest interface...");
				if (packet.header_type() == Type::Packet::HEADER_1) {
					uint8_t new_flags = (Type::Packet::HEADER_2) << 6 | (Type::Transport::TRANSPORT) << 4 | (packet.flags() & 0b00001111);
					Bytes new_raw(512);
					new_raw << new_flags;
					new_raw << packet.raw().mid(1,1);
					new_raw << nh;
					new_raw << packet.raw().mid(2);
					transmit(outbound_interface, new_raw);
					sent = true;
				}
			}
			else if (hops == 1 && _owner.is_connected_to_shared_instance()) {
				TRACE("Transport::outbound: Sending packet for directly connected interface to shared instance...");
				if (packet.header_type() == Type::Packet::HEADER_1) {
					uint8_t new_flags = (Type::Packet::HEADER_2) << 6 | (Type::Transport::TRANSPORT) << 4 | (packet.flags() & 0b00001111);
					Bytes new_raw(512);
					new_raw << new_flags;
					new_raw << packet.raw().mid(1, 1);
					new_raw << nh;
					new_raw << packet.raw().mid(2);
					transmit(outbound_interface, new_raw);
					sent = true;
				}
			}
			else {
				TRACE("Transport::outbound: Sending packet over directly connected interface...");
				transmit(outbound_interface, packet.raw());
				sent = true;
			}

			// ── Staleness check (all hops) ────────────────────
			// Applies to ALL paths, not just multi-hop. A hops=1 path
			// through a migrated/disconnected client will go stale and
			// the hedge falls through to the next-best path (e.g. backbone).
			double now = OS::time();
			bool is_stale = ((now - entry.timestamp) > (double)Type::Transport::PATH_STALE_THRESHOLD);

			if (is_stale) {
				// Fire path_request on this path's interface to probe liveness
				request_path(packet.destination_hash(), outbound_interface);

				// Shorten expiry so dead paths don't duplicate forever
				auto iter = _destination_table.find(packet.destination_hash());
				if (iter != _destination_table.end()) {
					for (auto& e : iter->second) {
						if (e.packet_hash == entry.packet_hash) {
							double shortened = now + (double)Type::Transport::PATH_STALE_THRESHOLD;
							if (shortened < e.expires) {
								e.expires = shortened;
							}
							break;
						}
					}
				}
				// Continue to next-best path (hedge)
			} else {
				// Fresh path — delivery covered, stop hedging
				break;
			}
		}
		// ── No path succeeded ─────────────────────────────────
		if (!sent) {
			WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (?) - NO-PATH hops=" + std::to_string(packet.hops()) + " — all paths failed, dropped");
		}
	}
	// If we don't have a known path for the destination, we'll
	// broadcast the packet on all outgoing interfaces, or the
	// just the relevant interface if the packet has an attached
	// interface, or belongs to a link.
	else {
		TRACE("Transport::outbound: Path to destination is unknown");
		bool stored_hash = false;
#if defined(INTERFACES_SET)
		for (const Interface& interface : _interfaces) {
#elif defined(INTERFACES_LIST)
		for (Interface& interface : _interfaces) {
#elif defined(INTERFACES_MAP)
		for (auto& [hash, interface] : _interfaces) {
#endif
			TRACE("Transport::outbound: Checking interface " + interface.toString());
			if (interface.OUT()) {
				bool should_transmit = true;

				if (packet.destination().type() == Type::Destination::LINK) {
					if (!packet.destination_link()) throw std::invalid_argument("Packet is not associated with a Link");
					if (packet.destination_link().status() == Type::Link::CLOSED) {
						TRACE("Transport::outbound: Pscket destination is link-closed, not transmitting");
						should_transmit = false;
					}
					// CBA Bug? Destination has no member attached_interface
				}
				
				if (packet.attached_interface() && interface != packet.attached_interface()) {
					TRACE("Transport::outbound: Packet has wrong attached interface, not transmitting");
					should_transmit = false;
				}

				if (packet.packet_type() == Type::Packet::ANNOUNCE) {
#ifdef FIREWALL_MODE
					// Firewall mode: announce broadcasts are always allowed.
					// No mode gating, no bandwidth caps — LAN announces go to
					// every interface including WAN, and whitelisted WAN
					// announces go to every LAN interface.
					if (packet.attached_interface() && interface == packet.attached_interface()) {
						// If the announce is pinned to a specific interface,
						// only transmit on that one.
					} else if (!packet.attached_interface()) {
						// No attached_interface means broadcast to all.
					} else {
						should_transmit = false;
					}
#else
					if (!packet.attached_interface()) {
						TRACE("Transport::outbound: Packet has no attached interface");
						if (interface.mode() == Type::Interface::MODE_ACCESS_POINT) {
							TRACE("Blocking announce broadcast on " + interface.toString() + " due to AP mode");
							should_transmit = false;
						}
						else if (interface.mode() == Type::Interface::MODE_ROAMING) {
							//local_destination = next((d for d in Transport.destinations if d.hash == packet.destination_hash), None)
							//Destination local_destination({Type::NONE});
#if defined(DESTINATIONS_SET)
							bool found_local = false;
							for (auto& destination : _destinations) {
								if (destination.hash() == packet.destination_hash()) {
									//local_destination = destination;
									found_local = true;
									break;
								}
							}
                            //if local_destination != None:
							//if (local_destination) {
							if (found_local) {
#elif defined(DESTINATIONS_MAP)
							auto iter = _destinations.find(packet.destination_hash());
							//if (iter != _destinations.end()) {
							//	local_destination = (*iter).second;
							//}
							if (iter != _destinations.end()) {
#endif
								TRACE("Allowing announce broadcast on roaming-mode interface from instance-local destination");
							}
							else {
								const Interface& from_interface = next_hop_interface(packet.destination_hash());
								//if from_interface == None or not hasattr(from_interface, "mode"):
								if (!from_interface || from_interface.mode() == Type::Interface::MODE_NONE) {
									should_transmit = false;
									if (!from_interface) {
										TRACE("Blocking announce broadcast on " + interface.toString() + " since next hop interface doesn't exist");
									}
									else if (from_interface.mode() == Type::Interface::MODE_NONE) {
										TRACE("Blocking announce broadcast on " + interface.toString() + " since next hop interface has no mode configured");
									}
								}
								else {
									if (from_interface.mode() == Type::Interface::MODE_ROAMING) {
										TRACE("Blocking announce broadcast on " + interface.toString() + " due to roaming-mode next-hop interface");
										should_transmit = false;
									}
									else if (from_interface.mode() == Type::Interface::MODE_BOUNDARY) {
										TRACE("Blocking announce broadcast on " + interface.toString() + " due to boundary-mode next-hop interface");
										should_transmit = false;
									}
								}
							}
						}
						else if (interface.mode() == Type::Interface::MODE_BOUNDARY) {
							//local_destination = next((d for d in Transport.destinations if d.hash == packet.destination_hash), None)
							// next and filter pattern?
							// next(iterable, default)
							// list comprehension: [x for x in xyz if x in a]
							// CBA TODO confirm that above pattern just selects the first matching destination
#if defined(DESTINATIONS_SET)
							//Destination local_destination({Type::Destination::NONE});
							bool found_local = false;
							for (auto& destination : _destinations) {
								if (destination.hash() == packet.destination_hash()) {
									//local_destination = destination;
									found_local = true;
									break;
								}
							}
                            //if local_destination != None:
							//if (local_destination) {
							if (found_local) {
#elif defined(DESTINATIONS_MAP)
							auto iter = _destinations.find(packet.destination_hash());
							if (iter != _destinations.end()) {
#endif
								TRACE("Allowing announce broadcast on boundary-mode interface from instance-local destination");
							}
							else {
								const Interface& from_interface = next_hop_interface(packet.destination_hash());
								if (!from_interface || from_interface.mode() == Type::Interface::MODE_NONE) {
									should_transmit = false;
									if (!from_interface) {
										TRACE("Blocking announce broadcast on " + interface.toString() + " since next hop interface doesn't exist");
									}
									else if (from_interface.mode() == Type::Interface::MODE_NONE) {
										TRACE("Blocking announce broadcast on "  + interface.toString() + " since next hop interface has no mode configured");
									}
								}
								else {
									if (from_interface.mode() == Type::Interface::MODE_ROAMING) {
										TRACE("Blocking announce broadcast on " + interface.toString() + " due to roaming-mode next-hop interface");
										should_transmit = false;
									}
								}
							}
						}
						else {
							// Currently, annouces originating locally are always
							// allowed, and do not conform to bandwidth caps.
							// TODO: Rethink whether this is actually optimal.
							if (packet.hops() > 0) {

// TODO
								bool queued_announces = (interface.announce_queue().size() > 0);
								if (!queued_announces && outbound_time > interface.announce_allowed_at()) {
									uint16_t wait_time = 0;
									if (interface.bitrate() > 0 && interface.announce_cap() > 0) {
										uint16_t tx_time = (packet.raw().size() * 8) / interface.bitrate();
										wait_time = (tx_time / interface.announce_cap());
									}
#if defined(INTERFACES_SET)
									const_cast<Interface&>(interface).announce_allowed_at(outbound_time + wait_time);
#else
									interface.announce_allowed_at(outbound_time + wait_time);
#endif
								}
								else {
									should_transmit = false;
									if (interface.announce_queue().size() < Type::Reticulum::MAX_QUEUED_ANNOUNCES) {
										bool should_queue = true;
										for (auto& entry : interface.announce_queue()) {
											if (entry._destination == packet.destination_hash()) {
												uint64_t emission_timestamp = announce_emitted(packet);
												should_queue = false;
												if (emission_timestamp > entry._emitted) {
													entry._time = outbound_time;
													entry._hops = packet.hops();
													entry._emitted = emission_timestamp;
													entry._raw = packet.raw();
												}
												break;
											}
										}
										if (should_queue) {
											RNS::AnnounceEntry entry(
												packet.destination_hash(),
												outbound_time,
												packet.hops(),
												announce_emitted(packet),
												packet.raw()
											);

											queued_announces = (interface.announce_queue().size() > 0);
#if defined(INTERFACES_SET)
											const_cast<Interface&>(interface).add_announce(entry);
#else
											// CBA ACCUMULATES
											interface.add_announce(entry);
#endif

											if (!queued_announces) {
												double wait_time = std::max(interface.announce_allowed_at() - OS::time(), (double)0);

												// CBA TODO THREAD?

												if (wait_time < 1000) {
													TRACE("Added announce to queue (height " + std::to_string(interface.announce_queue().size()) + ") on " + interface.toString() + " for processing in " + std::to_string((int)wait_time) + " ms");
												}
												else {
													TRACE("Added announce to queue (height " + std::to_string(interface.announce_queue().size()) + ") on " + interface.toString() + " for processing in " + std::to_string(OS::round(wait_time/1000,1)) + " s");
												}
											}
											else {
												double wait_time = std::max(interface.announce_allowed_at() - OS::time(), (double)0);
												if (wait_time < 1000) {
													TRACE("Added announce to queue (height " + std::to_string(interface.announce_queue().size()) + ") on " + interface.toString() + " for processing in " + std::to_string((int)wait_time) + " ms");
												}
												else {
													TRACE("Added announce to queue (height " + std::to_string(interface.announce_queue().size()) + ") on " + interface.toString() + " for processing in " + std::to_string(OS::round(wait_time/1000,1)) + " s");
												}
											}
										}
									}
									else {
									}
								}
							}
							else {
							}
						}
					}
#endif // FIREWALL_MODE — announce gating
				}
						
				if (should_transmit) {
					TRACE("Transport::outbound: Packet transmission allowed");
					if (packet.packet_type() == Type::Packet::ANNOUNCE) {
						DEBUG("DIAG: TX-OUT announce dest=" + packet.destination_hash().toHex().substr(0,8) + " on " + interface.toString());
					}
					if (!stored_hash) {
						// CBA ACCUMULATES
						_packet_hashlist.push_back(packet.packet_hash());
						stored_hash = true;
					}

					// TODO: Re-evaluate potential for blocking
					// def send_packet():
					//     Transport.transmit(interface, packet.raw)
					// thread = threading.Thread(target=send_packet)
					// thread.daemon = True
					// thread.start()

#if defined(INTERFACES_SET)
					transmit(const_cast<Interface&>(interface), packet.raw());
#else
					transmit(interface, packet.raw());
#endif
					sent = true;
				}
				else {
					TRACE("Transport::outbound: Packet transmission refused");
					if (packet.packet_type() == Type::Packet::ANNOUNCE) {
						DEBUG("DIAG: TX-REFUSED announce dest=" + packet.destination_hash().toHex().substr(0,8) + " refused on " + interface.toString());
					}
				}
			}
		}
	}

	if (sent) {
		packet.sent(true);
		packet.sent_at(OS::time());

		// Don't generate receipt if it has been explicitly disabled
		if (packet.create_receipt() &&
			// Only generate receipts for DATA packets
			packet.packet_type() == Type::Packet::DATA &&
			// Don't generate receipts for PLAIN destinations
			packet.destination().type() != Type::Destination::PLAIN &&
			// Don't generate receipts for link-related packets
			!(packet.context() >= Type::Packet::KEEPALIVE && packet.context() <= Type::Packet::LRPROOF) &&
			// Don't generate receipts for resource packets
			!(packet.context() >= Type::Packet::RESOURCE && packet.context() <= Type::Packet::RESOURCE_RCL)) {

			PacketReceipt receipt(packet);
			packet.receipt(receipt);
			// CBA ACCUMULATES
			_receipts.push_back(receipt);
		}
		
		cache_packet(packet);
	}

	_jobs_locked = false;
	return sent;
}

/*static*/ bool Transport::packet_filter(const Packet& packet) {
	// TODO: Think long and hard about this.
	// Is it even strictly necessary with the current
	// transport rules?
	if (packet.context() == Type::Packet::KEEPALIVE) {
		return true;
	}
	if (packet.context() == Type::Packet::RESOURCE_REQ) {
		return true;
	}
	if (packet.context() == Type::Packet::RESOURCE_PRF) {
		return true;
	}
	if (packet.context() == Type::Packet::RESOURCE) {
		return true;
	}
	if (packet.context() == Type::Packet::CACHE_REQUEST) {
		return true;
	}
	if (packet.context() == Type::Packet::CHANNEL) {
		return true;
	}

	if (packet.destination_type() == Type::Destination::PLAIN) {
		if (packet.packet_type() != Type::Packet::ANNOUNCE) {
			if (packet.hops() > 1) {
				DEBUG("Dropped PLAIN packet " + packet.packet_hash().toHex() + " with " + std::to_string(packet.hops()) + " hops");
				return false;
			}
			else {
				return true;
			}
		}
		else {
			DEBUG("Dropped invalid PLAIN announce packet");
			return false;
		}
	}

	if (packet.destination_type() == Type::Destination::GROUP) {
		if (packet.packet_type() != Type::Packet::ANNOUNCE) {
			if (packet.hops() > 1) {
				DEBUG("Dropped GROUP packet " + packet.packet_hash().toHex() + " with " + std::to_string(packet.hops()) + " hops");
				return false;
			}
			else {
				return true;
			}
		}
		else {
			DEBUG("Dropped invalid GROUP announce packet");
			return false;
		}
	}

	if (std::find(_packet_hashlist.begin(), _packet_hashlist.end(), packet.packet_hash()) == _packet_hashlist.end()) {
		TRACE("Transport::packet_filter: packet not previously seen");
		return true;
	}
	else {
		if (packet.packet_type() == Type::Packet::ANNOUNCE) {
			if (packet.destination_type() == Type::Destination::SINGLE) {
				TRACE("Transport::packet_filter: packet previously seen but is SINGLE ANNOUNCE");
				return true;
			}
			else {
				DEBUG("Dropped invalid announce packet");
				return false;
			}
		}
	}

	DEBUG("FILTERED duplicate packet " + packet.packet_hash().toHex().substr(0,8) + " dest=" + packet.destination_hash().toHex().substr(0,8) + " type=" + std::to_string(packet.packet_type()) + " ctx=" + std::to_string(packet.context()));
	DEBUG("DIAG: FILTERED dup dest=" + packet.destination_hash().toHex().substr(0,8) + " type=" + std::to_string(packet.packet_type()));
	return false;
}

/*static*/ void Transport::inbound(const Bytes& raw_in, const Interface& interface /*= {Type::NONE}*/) {
	TRACEF("Transport::inbound: received %d bytes", raw_in.size());
	++_packets_received;

	// Heap telemetry: snapshot at entry
	size_t _heap_at_entry = OS::heap_available();
	// CBA
	if (_callbacks._receive_packet) {
		try {
			_callbacks._receive_packet(raw_in, interface);
		}
		catch (std::exception& e) {
			DEBUG("Error while executing receive packet callback. The contained exception was: " + std::string(e.what()));
		}
	}

	// Mutable copy of raw data for IFAC processing
	Bytes raw = raw_in;

	// If interface access codes are enabled,
	// we must authenticate each packet.
	if (raw.size() > 2) {
		if (interface && interface.ifac_identity()) {
			// Check that IFAC flag is set
			if ((raw[0] & 0x80) == 0x80) {
				if (raw.size() > (size_t)(2 + interface.ifac_size())) {
					// Extract IFAC
					Bytes ifac = raw.mid(2, interface.ifac_size());

					// Generate mask
					Bytes mask = Cryptography::hkdf(
						raw.size(),
						ifac,
						interface.ifac_key()
					);

					// Unmask payload
					Bytes unmasked_raw;
					for (size_t i = 0; i < raw.size(); i++) {
						if (i <= 1 || i > (size_t)(interface.ifac_size() + 1)) {
							// Unmask header bytes and payload
							unmasked_raw.append((uint8_t)(raw[i] ^ mask[i]));
						}
						else {
							// Don't unmask IFAC itself
							unmasked_raw.append(raw[i]);
						}
					}
					raw = unmasked_raw;

					// Unset IFAC flag
					uint8_t new_header0 = raw[0] & 0x7F;
					uint8_t new_header1 = raw[1];

					// Re-assemble packet without IFAC bytes
					Bytes new_raw;
					new_raw.append(new_header0);
					new_raw.append(new_header1);
					new_raw.append(raw.mid(2 + interface.ifac_size()));

					// Calculate expected IFAC
					Bytes expected_signature = interface.ifac_id().sign(new_raw);
					Bytes expected_ifac = expected_signature.right(interface.ifac_size());

					// Check it
					if (ifac == expected_ifac) {
						raw = new_raw;
					}
					else {
						TRACE("Transport::inbound: IFAC authentication failed, dropping packet");
						return;
					}
				}
				else {
					TRACE("Transport::inbound: packet too short for IFAC, dropping");
					return;
				}
			}
			else {
				// If the IFAC flag is not set, but should be, drop the packet
				TRACE("Transport::inbound: IFAC required but flag not set, dropping packet");
				return;
			}
		}
		else {
			// If the interface does not have IFAC enabled,
			// check the received packet IFAC flag.
			if ((raw[0] & 0x80) == 0x80) {
				// If the flag is set, drop the packet
				TRACE("Transport::inbound: IFAC flag set but interface has no IFAC, dropping packet");
				return;
			}
		}
	}
	else {
		return;
	}

	while (_jobs_running) {
		TRACE("Transport::inbound: sleeping...");
		OS::sleep(0.0005);
	}

	if (!_identity) {
		WARNING("Transport::inbound: No identity!");
		return;
	}

	_jobs_locked = true;

	Packet packet(RNS::Destination(RNS::Type::NONE), raw);
	if (!packet.unpack()) {
		WARNING("Transport::inbound: Packet unpack failed!");
		return;
	}
#ifndef NDEBUG
	TRACE("Transport::inbound: packet: " + packet.debugString());
#endif

	TRACE("Transport::inbound: destination=" + packet.destination_hash().toHex() + " hops=" + std::to_string(packet.hops()));

	packet.receiving_interface(interface);
	packet.hops(packet.hops() + 1);

	// Helper: determine destination zone from path table / local dests
	auto dest_zone = [&](const Bytes& hash) -> const char* {
		if (!hash) return "?";
		// Check local registered destinations
#if defined(DESTINATIONS_MAP)
		if (_destinations.find(hash) != _destinations.end()) return "LAN";
#elif defined(DESTINATIONS_SET)
		for (auto& d : _destinations) { if (d.hash() == hash) return "LAN"; }
#endif
		// Check control hashes
		if (_control_hashes.find(hash) != _control_hashes.end()) return "CTL";
		// Check path table
		if (has_path(hash)) {
			const PathEntry* entry = select_path(hash);
			if (entry) {
				Interface iface = find_interface_from_hash(entry->receiving_interface);
				if (iface && iface.is_backbone()) return "WAN";
				return "LAN";
			}
		}
		return "?";
	};

// TODO
	//if (packet_filter(packet)) {
	// CBA
	bool accept = true;
	if (_callbacks._filter_packet) {
		try {
			accept = _callbacks._filter_packet(packet);
		}
		catch (std::exception& e) {
			DEBUG("Error while executing filter packet callback. The contained exception was: " + std::string(e.what()));
		}
	}
	if (accept) {
		accept = packet_filter(packet);
	}
	if (accept) {
		TRACE("Transport::inbound: Packet accepted by filter");

		// FIREWALL MODE: Comprehensive firewall for backbone traffic.
		//
		// Three rules:
		//   1. Addresses that touch local interfaces (RNode/LoRa, LocalTCP)
		//      get whitelisted on the backbone interface.
		//   2. Every packet referencing a whitelisted address — ALL identifiers
		//      in that packet also get whitelisted (link hashes, announces,
		//      requests, proofs, truncated hashes, transport IDs, EVERYTHING).
		//   3. Everything else gets blocked on the backbone interface.
		//
		//   4. TRANSITIVE WHITELIST: For every packet, extract ALL
		//      identifiable addresses. If ANY address is already in a
		//      whitelist, add ALL to the secondary whitelist. This covers
		//      link IDs, ratchet IDs, transport IDs, path request targets,
		//      and announce ratchets — in both directions.
#ifdef FIREWALL_MODE
		{
			bool is_backbone = is_backbone_interface(packet.receiving_interface());
			bool is_trusted_local = is_trusted_local_interface(packet.receiving_interface());

			// ── Extract all addresses from this packet ──────────
			// Two tiers:
			//   TIER 1 (seed + check): user-facing destinations —
			//     dest_hash, link_id, path_req_target, announce_ratchet.
			//     These get added to WL#2 on pass.
			//   TIER 2 (check only): relay infrastructure —
			//     truncated_hash, transport_id.  These can PERMIT a
			//     packet through but are NEVER added to WL#2, because
			//     they represent relay nodes, not user traffic.
			auto wl_add = [&](const Bytes& addr, const char* tag) {
				if (!addr) return;
				if (_control_hashes.find(addr) != _control_hashes.end()) return;
				if (wl2_push(addr)) {
					NOTICE("WL#2 ADD - " + addr.toHex().substr(0,8) + " (from " + zone_tag(strcmp(tag,"backbone")==0) + ")");
				}
			};
			auto wl_known = [&](const Bytes& addr) -> bool {
				return std::find(_firewall_pinned_addresses.begin(), _firewall_pinned_addresses.end(), addr) != _firewall_pinned_addresses.end()
				    || std::find(_firewall_local_addresses.begin(), _firewall_local_addresses.end(), addr) != _firewall_local_addresses.end()
				    || std::find(_firewall_mentioned_addresses.begin(), _firewall_mentioned_addresses.end(), addr) != _firewall_mentioned_addresses.end();
			};

			// Tier 1: user-facing addresses (seeded + checked)
			std::vector<Bytes> addrs;
			addrs.push_back(packet.destination_hash());
			if (packet.packet_type() == Type::Packet::LINKREQUEST) {
				addrs.push_back(Link::link_id_from_lr_packet(packet));
			}
			// Path request: target destination in first 16 bytes of data
			if (_control_hashes.find(packet.destination_hash()) != _control_hashes.end()
			    && packet.data().size() >= Type::Identity::TRUNCATED_HASHLENGTH/8) {
				addrs.push_back(packet.data().left(Type::Identity::TRUNCATED_HASHLENGTH/8));
			}
			// Announce: extract ratchet ID (offset 84, 32 bytes, context_flag=FLAG_SET)
			if (packet.packet_type() == Type::Packet::ANNOUNCE
			    && packet.context_flag() == Type::Packet::FLAG_SET
			    && packet.data().size() >= 116) {
				Bytes ratchet_pubkey = packet.data().mid(84, 32);
				addrs.push_back(Identity::truncated_hash(ratchet_pubkey));
			}

			// ── Watched-destination alias tracking ────────────
			// Populate ratchet and link aliases so WLOG resolves them.
#ifdef WATCH_LOG
			{
				const char* wn = watch_resolve(packet.destination_hash());
				if (wn) {
					if (packet.packet_type() == Type::Packet::ANNOUNCE
					    && packet.context_flag() == Type::Packet::FLAG_SET
					    && packet.data().size() >= 116) {
						Bytes rh = Identity::truncated_hash(packet.data().mid(84, 32));
						watch_add_alias(rh, wn);
					}
					if (packet.packet_type() == Type::Packet::LINKREQUEST) {
						Bytes lid = Link::link_id_from_lr_packet(packet);
						watch_add_alias(lid, wn);
					}
				}
			}
#endif

			// Tier 2: relay infrastructure (checked, NEVER seeded)
			std::vector<Bytes> relay_addrs;
			if (packet.header_type() == Type::Packet::HEADER_2 && packet.transport_id()) {
				relay_addrs.push_back(packet.transport_id());
			}
			// Note: getTruncatedHash() is NOT included — it's a
			// per-packet content hash, not a node identity. It can
			// never match a whitelist entry and seeding it wastes a
			// WL#2 slot with garbage.

			// ── Transitive match: any Tier 1 or Tier 2 address known? ──
			bool any_known = false;
			for (auto& a : addrs) {
				if (wl_known(a)) { any_known = true; break; }
			}
			if (!any_known) {
				for (auto& a : relay_addrs) {
					if (wl_known(a)) { any_known = true; break; }
				}
			}

			if (is_backbone) {
				// === BACKBONE PACKET ===
				// Proofs are responses to traffic we already forwarded.
				// They carry no source/destination identity — just a
				// packet body hash.  Exempt from whitelist; the reverse
				// table handles routing, and forged proofs fail signature
				// validation downstream.
				if (packet.packet_type() == Type::Packet::PROOF) {
					WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - WL-PASS PROOF exempt hops=" + std::to_string(packet.hops()) + " sz=" + std::to_string(packet.raw().size()));
					// Fall through to reverse-table routing below
				}
				// Only allowed if at least one address in the packet is
				// already whitelisted (WL#1 or WL#2). No other gates.
				else if (!any_known) {
					WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - WL-BLOCK hops=" + std::to_string(packet.hops()) + " sz=" + std::to_string(packet.raw().size()));
					return;
				}
				// Transitive: add all addresses to WL#2
				for (auto& a : addrs) { wl_add(a, "backbone"); }
				WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - WL-PASS hops=" + std::to_string(packet.hops()) + " sz=" + std::to_string(packet.raw().size()));
			}
			else if (is_trusted_local) {
				// === LOCAL DEVICE PACKET ===
				// Always whitelist ALL addresses from local packets.
				// LoRa and registered TCP server clients are trusted; their traffic seeds the
				// secondary whitelist for return traffic from WAN.
				for (auto& a : addrs) { wl_add(a, "local"); }
				WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - WL-PASS hops=" + std::to_string(packet.hops()) + " sz=" + std::to_string(packet.raw().size()));
			}
		}
#endif

		// Heap telemetry: snapshot after firewall filter (MUTED)
		// {
		// 	size_t _heap_after_boundary = OS::heap_available();
		// 	int _boundary_delta = (int)_heap_after_boundary - (int)_heap_at_entry;
		// 	if (_boundary_delta < -64) {
		// 		VERBOSEF("[HEAP-TEL] firewall: %d bytes (bma=%u phl=%u)", _boundary_delta, _firewall_mentioned_addresses.size(), _packet_hashlist.size());
		// 	}
		// }

	// By default, remember packet hashes to avoid routing
		// loops in the network, using the packet filter.
		bool remember_packet_hash = true;

		// If this packet belongs to a link in our link table,
		// we'll have to defer adding it to the filter list.
		// In some cases, we might see a packet over a shared-
		// medium interface, belonging to a link that transports
		// or terminates with this instance, but before it would
		// normally reach us. If the packet is appended to the
		// filter list at this point, link transport will break.
		if (_link_table.find(packet.destination_hash()) != _link_table.end()) {
			remember_packet_hash = false;
		}

		// If this is a link request proof, don't add it until
		// we are sure it's not actually somewhere else in the
		// routing chain.
		if (packet.packet_type() == Type::Packet::PROOF && packet.context() == Type::Packet::LRPROOF) {
			remember_packet_hash = false;
		}

		if (remember_packet_hash) {
			// CBA ACCUMULATES
			_packet_hashlist.push_back(packet.packet_hash());
		}
		cache_packet(packet);

#ifdef FIREWALL_MODE
		// Log ALL non-announce packets arriving from local (non-backbone) interfaces
		if (!is_backbone_interface(packet.receiving_interface()) && packet.packet_type() != Type::Packet::ANNOUNCE) {
			WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LAN-IN hops=" + std::to_string(packet.hops()) + " sz=" + std::to_string(packet.raw().size()));
		}
		// Resource packet trace: log any resource-context packet entering inbound
		if (is_resource_ctx(packet.context())) {
			WLOG(packet, std::string(ctx_name(packet.context())) + " IN  hops=" + std::to_string(packet.hops()) + " sz=" + std::to_string(packet.raw().size()) + " dst=" + short_hash(packet.destination_hash()));
		}
#endif
		
		// Firewall mode: no local-client distinction. All devices are
		// either LAN (non-backbone) or WAN (backbone).
		const bool from_local_client = false;
		const bool for_local_client = false;
		const bool for_local_client_link = false;
		const bool proof_for_local_client = false;

		// Transport is always enabled in firewall mode
		const bool transport_active = true;

		// If packet is not destined for a local transport-specific destination
		if (_control_hashes.find(packet.destination_hash()) == _control_hashes.end()) {
			// PLAIN BROADCAST: delivered to all interfaces
			if (packet.destination_type() == Type::Destination::PLAIN && packet.transport_type() == Type::Transport::BROADCAST) {
#if defined(INTERFACES_SET)
				for (const Interface& interface : _interfaces) {
#elif defined(INTERFACES_LIST)
				for (Interface& interface : _interfaces) {
#elif defined(INTERFACES_MAP)
				for (auto& [hash, interface] : _interfaces) {
#endif
					if (interface != packet.receiving_interface()) {
						TRACE("Transport::inbound: Broadcasting packet on " + interface.toString());
#if defined(INTERFACES_SET)
						transmit(const_cast<Interface&>(interface), packet.raw());
#else
						transmit(interface, packet.raw());
#endif
					}
				}
			}
		}

		////////////////////////////////
		// TRANSPORT HANDLING
		////////////////////////////////

		// General transport handling (always active in firewall mode)
		{
			TRACE("Transport::inbound: Performing general transport handling");

			// If this is a cache request, and we can fullfill
			// it, do so and stop processing. Otherwise resume
			// normal processing.
			if (packet.context() == Type::Packet::CACHE_REQUEST) {
				if (cache_request_packet(packet)) {
					TRACE("Transport::inbound: Cached packet");
					return;
				}
			}

			// If the packet is in transport, check whether we
			// are the designated next hop, and process it
			// accordingly if we are.
			if (packet.transport_id() && packet.packet_type() != Type::Packet::ANNOUNCE) {
				TRACE("Transport::inbound: Packet is in transport...");
				if (packet.transport_id() == _identity.hash()) {
					TRACE("Transport::inbound: We are designated next-hop");
					if (has_path(packet.destination_hash())) {
						TRACE("Transport::inbound: Found next-hop path to destination");
						const PathEntry* entry = select_path(packet.destination_hash());
						if (!entry) {
							TRACE("Got packet in transport, but selected path expired. Dropping packet.");
							return;
						}
						Bytes next_hop = entry->next_hop;
						uint8_t remaining_hops = entry->hops;
						
						// CBA RESERVE
						//Bytes new_raw;
						Bytes new_raw(512);
						if (remaining_hops > 1) {
							// Just increase hop count and transmit
							//new_raw  = packet.raw[0:1]
							new_raw << packet.raw().left(1);
							//new_raw += struct.pack("!B", packet.hops)
							new_raw << packet.hops();
							//new_raw += next_hop
							new_raw << next_hop;
							//new_raw += packet.raw[(RNS.Identity.TRUNCATED_HASHLENGTH//8)+2:]
							new_raw << packet.raw().mid((Type::Identity::TRUNCATED_HASHLENGTH/8)+2);
						}
						else if (remaining_hops == 1) {
							// Strip transport headers and transmit
							//new_flags = (RNS.Packet.HEADER_1) << 6 | (Transport.BROADCAST) << 4 | (packet.flags & 0b00001111)
							uint8_t new_flags = (Type::Packet::HEADER_1) << 6 | (Type::Transport::BROADCAST) << 4 | (packet.flags() & 0b00001111);
							//new_raw = struct.pack("!B", new_flags)
							new_raw << new_flags;
							//new_raw += struct.pack("!B", packet.hops)
							new_raw << packet.hops();
							//new_raw += packet.raw[(RNS.Identity.TRUNCATED_HASHLENGTH//8)+2:]
							new_raw << packet.raw().mid((Type::Identity::TRUNCATED_HASHLENGTH/8)+2);
						}
						else if (remaining_hops == 0) {
							// Just increase hop count and transmit
							//new_raw  = packet.raw[0:1]
							new_raw << packet.raw().left(1);
							//new_raw += struct.pack("!B", packet.hops)
							new_raw << packet.hops();
							//new_raw += packet.raw[2:]
							new_raw << packet.raw().mid(2);
						}

						Interface outbound_interface = find_interface_from_hash(entry->receiving_interface);

#ifdef FIREWALL_MODE
						// In firewall mode, never route a packet from backbone back to backbone.
						// The upstream server sent us this packet because we are the next hop,
						// so the destination must be on our local side.
						if (is_backbone_interface(packet.receiving_interface()) && is_backbone_interface(outbound_interface)) {
							// Path table incorrectly points to backbone. Skip forwarding.
						}
						else
#endif
						{
						if (packet.packet_type() == Type::Packet::LINKREQUEST) {
							TRACE("Transport::inbound: Packet is next-hop LINKREQUEST");
							double now = OS::time();
							double proof_timeout = Transport::extra_link_proof_timeout(packet.receiving_interface())
								+ now + Type::Link::ESTABLISHMENT_TIMEOUT_PER_HOP * std::max((uint8_t)1, remaining_hops);

							// === MTU Clamping (v1.0.12) ===
							// When forwarding a LINKREQUEST through this transport node,
							// clamp the link MTU signalling to min(prev-hop, next-hop)
							// interface HW_MTU.  Without this, endpoints negotiate a
							// segment size that exceeds this node's buffer capacity,
							// causing silent truncation and resource transfer stalls.
							uint16_t path_mtu = Link::mtu_from_lr_packet(packet);
							if (path_mtu > 0) {
								uint16_t ph_mtu = packet.receiving_interface().HW_MTU();
								uint16_t nh_mtu = outbound_interface.HW_MTU();
								if (nh_mtu == 0) {
									DEBUG("MTU CLAMP: No next-hop HW MTU, stripping link MTU signalling");
									new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
								} else if (!outbound_interface.AUTOCONFIGURE_MTU() && !outbound_interface.FIXED_MTU()) {
									DEBUG("MTU CLAMP: Outbound interface doesn't support MTU config, stripping link MTU signalling");
									new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
								} else {
									if (nh_mtu < path_mtu || (ph_mtu > 0 && ph_mtu < path_mtu)) {
										uint16_t clamped = std::min(nh_mtu, (ph_mtu > 0) ? ph_mtu : nh_mtu);
										RNS::Type::Link::link_mode mode = Link::mode_from_lr_packet(packet);
										Bytes clamped_mtu_bytes = Link::signalling_bytes(clamped, mode);
										new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE) + clamped_mtu_bytes;
										DEBUGF("MTU CLAMP: path=%u ph=%u nh=%u -> clamped=%u", path_mtu, ph_mtu, nh_mtu, clamped);
									}
								}
							}

							LinkEntry link_entry(
								now,
								next_hop,
								outbound_interface,
								remaining_hops,
								packet.receiving_interface(),
								packet.hops(),
								packet.destination_hash(),
								false,
								proof_timeout
							);
							// CBA ACCUMULATES
							_link_table.insert({Link::link_id_from_lr_packet(packet), link_entry});
						}
						else {
							TRACE("Transport::inbound: Packet is next-hop other type");
							ReverseEntry reverse_entry(
								packet.receiving_interface(),
								outbound_interface,
								OS::time()
							);
							// CBA ACCUMULATES
							Bytes th = packet.getTruncatedHash();
							flatmap_erase(_reverse_table, th); _reverse_table.push_back({th, reverse_entry});
							wl2_push(th);
						}
						TRACE("Transport::outbound: Sending packet to next hop...");
						WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - FWD: " + outbound_interface.toString() + " (" + zone_tag(is_backbone_interface(outbound_interface)) + ") transport hops=" + std::to_string(remaining_hops));
#if defined(INTERFACES_SET)
						transmit(const_cast<Interface&>(outbound_interface), new_raw);
#else
						transmit(outbound_interface, new_raw);
#endif
						// Update timestamp on the selected path entry
						{
							auto iter = _destination_table.find(packet.destination_hash());
							if (iter != _destination_table.end()) {
								for (auto& e : iter->second) {
									if (e.packet_hash == entry->packet_hash) {
										e.timestamp = OS::time();
										break;
									}
								}
							}
						}
						} // firewall mode else
					}
					else {
#ifdef FIREWALL_MODE
						// No path to destination. Link_ids are handled by the
						// link transport section below — only request a path
						// for locally-originated packets to unknown destinations.
						{
							bool from_backbone = is_backbone_interface(packet.receiving_interface());
							if (!from_backbone && _link_table.find(packet.destination_hash()) == _link_table.end()) {
								WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - NO-PATH next-hop, requesting");
								request_path(packet.destination_hash(), packet.receiving_interface());
							}
						}
#else
						TRACE("Got packet in transport, but no known path to final destination " + packet.destination_hash().toHex() + ". Dropping packet.");
#endif
					}
				}
				else {
					TRACE("Transport::inbound: We are not designated next-hop so not transporting");
				}
			}
			else {
				TRACE("Transport::inbound: Either packet is announce or packet has no next-hop (possibly for a local destination)");
#ifdef FIREWALL_MODE
				// FIREWALL MODE: If this packet came from a local interface and we
				// have a path to the destination, wrap it with transport headers
				// and forward it through the backbone as the first transport hop.
				// Skip ANNOUNCE and PROOF packets — announces have their own handling,
				// and link proofs (LRPROOF) are handled by the LRPROOF transport code.
				// Also skip packets destined for locally-registered destinations
				// (e.g. path request handler) — those must be processed locally.
				bool is_local_destination = false;
#if defined(DESTINATIONS_MAP)
				is_local_destination = (_destinations.find(packet.destination_hash()) != _destinations.end());
#elif defined(DESTINATIONS_SET)
				for (auto& dest : _destinations) {
					if (dest.hash() == packet.destination_hash()) { is_local_destination = true; break; }
				}
#endif
				NOTICE("FWD-CHECK - FROM: " + packet.receiving_interface().toString() + " (" + zone_tag(is_backbone_interface(packet.receiving_interface())) + ") TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") " + (is_local_destination ? "LOCAL" : "FWD"));
				if (is_local_destination) {
					NOTICE("SKIP-FWD - TO: " + short_hash(packet.destination_hash()) + " is local dest");
				}
				// Forward link proofs (delivery confirmations on established links)
				if (!is_local_destination && packet.packet_type() == Type::Packet::PROOF) {
					auto link_it = _link_table.find(packet.destination_hash());
					if (link_it != _link_table.end()) {
						LinkEntry& le = (*link_it).second;
						Interface out_iface = find_interface_from_hash(le._outbound_interface.get_hash());
						NOTICE("[PROOF] - FROM: " + packet.receiving_interface().toString() + " (" + zone_tag(is_backbone_interface(packet.receiving_interface())) + ") - TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - FWD: " + (out_iface ? out_iface.toString() : "?") + " (" + (out_iface ? zone_tag(is_backbone_interface(out_iface)) : "?") + ") LINK-PROOF");
						if (out_iface) transmit(out_iface, packet.raw());
					}
				}
				if (!is_local_destination && packet.packet_type() != Type::Packet::ANNOUNCE && packet.packet_type() != Type::Packet::PROOF) {
					bool is_from_backbone = is_backbone_interface(packet.receiving_interface());
					if (!is_from_backbone) {
						const PathEntry* entry = select_path(packet.destination_hash());
						if (entry) {
							Interface outbound_interface = find_interface_from_hash(entry->receiving_interface);
							NOTICE("[" + std::string(pkt_type_name(packet.packet_type())) + "] - FROM: " + packet.receiving_interface().toString() + " (" + zone_tag(is_backbone_interface(packet.receiving_interface())) + ") - TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - FWD: " + (outbound_interface ? outbound_interface.toString() : "?") + " (" + (outbound_interface ? zone_tag(is_backbone_interface(outbound_interface)) : "?") + ") hops=" + std::to_string(entry->hops));
							if (!outbound_interface) {
								// Path entry references an interface that no longer exists
								// (e.g. after reboot with persistent path table).
								// Request a fresh path instead of crashing.
								DEBUG(" Saved path entry has stale interface hash — requesting fresh path");
								request_path(packet.destination_hash());
								return;
							}
							Bytes next_hop = entry->next_hop;
							uint8_t remaining_hops = entry->hops;

							// Build outgoing packet based on remaining hops,
							// mirroring standard transport forwarding logic.
							Bytes new_raw(512);
							if (remaining_hops > 1) {
								// Multi-hop: wrap with HEADER_2/TRANSPORT,
								// setting transport_id = next_hop (the next
								// transport node's identity hash from announce).
								uint8_t new_flags = (Type::Packet::HEADER_2) << 6
									| (Type::Transport::TRANSPORT) << 4
									| (packet.flags() & 0b00001111);
								new_raw << new_flags;
								new_raw << packet.hops();
								new_raw << next_hop;          // insert transport_id
								new_raw << packet.raw().mid(2); // destination_hash + payload
							}
							else {
								// Single hop (remaining_hops <= 1): destination is
								// directly reachable. Send as HEADER_1/BROADCAST
								// (no transport header), matching standard transport
								// behaviour for final-hop delivery.
								uint8_t new_flags = (Type::Packet::HEADER_1) << 6
									| (Type::Transport::BROADCAST) << 4
									| (packet.flags() & 0b00001111);
								new_raw << new_flags;
								new_raw << packet.hops();
								new_raw << packet.raw().mid(2); // destination_hash + payload
							}

							// Create link_table or reverse_table entry for return path
							if (packet.packet_type() == Type::Packet::LINKREQUEST) {
								double now = OS::time();
								double proof_timeout = Transport::extra_link_proof_timeout(packet.receiving_interface())
									+ now + Type::Link::ESTABLISHMENT_TIMEOUT_PER_HOP
									* std::max((uint8_t)1, remaining_hops);

								// === MTU Clamping (v1.0.12) ===
								uint16_t path_mtu = Link::mtu_from_lr_packet(packet);
								if (path_mtu > 0) {
									uint16_t ph_mtu = packet.receiving_interface().HW_MTU();
									uint16_t nh_mtu = outbound_interface.HW_MTU();
									if (nh_mtu == 0) {
										new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
									} else if (!outbound_interface.AUTOCONFIGURE_MTU() && !outbound_interface.FIXED_MTU()) {
										new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
									} else if (nh_mtu < path_mtu || (ph_mtu > 0 && ph_mtu < path_mtu)) {
										uint16_t clamped = std::min(nh_mtu, (ph_mtu > 0) ? ph_mtu : nh_mtu);
										RNS::Type::Link::link_mode mode = Link::mode_from_lr_packet(packet);
										Bytes clamped_mtu_bytes = Link::signalling_bytes(clamped, mode);
										new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE) + clamped_mtu_bytes;
										DEBUGF("MTU CLAMP: local->backbone path=%u ph=%u nh=%u -> %u", path_mtu, ph_mtu, nh_mtu, clamped);
									}
								}

								LinkEntry link_entry(
									now, next_hop, outbound_interface, remaining_hops,
									packet.receiving_interface(), packet.hops(),
									packet.destination_hash(), false, proof_timeout
								);
								// Each LINKREQUEST gets its own entry (unique link_id)
								_link_table.insert({Link::link_id_from_lr_packet(packet), link_entry});
							}
							else {
								ReverseEntry reverse_entry(
									packet.receiving_interface(), outbound_interface, OS::time()
								);
								Bytes th = packet.getTruncatedHash();
								flatmap_erase(_reverse_table, th); _reverse_table.push_back({th, reverse_entry});
								// Whitelist the truncated hash so the return proof
								// from the backbone isn't blocked by the firewall.
								wl2_push(th);
							}

							DEBUG(" Forwarding local packet (" + std::to_string(remaining_hops) + " hops, " + std::to_string(new_raw.size()) + " bytes) to " + outbound_interface.toString() + " for " + packet.destination_hash().toHex());
							transmit(outbound_interface, new_raw);
							// Update timestamp on selected path entry
							{
								auto iter = _destination_table.find(packet.destination_hash());
								if (iter != _destination_table.end()) {
									for (auto& e : iter->second) {
										if (e.packet_hash == entry->packet_hash) {
											e.timestamp = OS::time();
											break;
										}
									}
								}
							}
						}
						else {
							// Only request path if the destination is not a link_id
							// (link data packets are handled by link transport below,
							// not by standard transport path lookup).
							if (_link_table.find(packet.destination_hash()) == _link_table.end()) {
								WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - NO-PATH requesting");
								request_path(packet.destination_hash(), packet.receiving_interface());
							}
						}
					}
					else {
						// FIREWALL MODE REVERSE: Packet came from backbone,
						// check if destination is a local LoRa device and forward it.
						if (std::find(_firewall_local_addresses.begin(), _firewall_local_addresses.end(), packet.destination_hash()) != _firewall_local_addresses.end()) {
							const PathEntry* entry2 = select_path(packet.destination_hash());
							if (entry2) {
								Bytes next_hop2 = entry2->next_hop;
								uint8_t remaining_hops2 = entry2->hops;
								Interface outbound_interface2 = find_interface_from_hash(entry2->receiving_interface);

								// Build properly routed packet based on remaining hops,
								// mirroring the standard transport forwarding logic.
								Bytes new_raw(512);
								if (remaining_hops2 > 1) {
									// Multi-hop: wrap with HEADER_2/TRANSPORT
									uint8_t new_flags = (Type::Packet::HEADER_2) << 6
										| (Type::Transport::TRANSPORT) << 4
										| (packet.flags() & 0b00001111);
									new_raw << new_flags;
									new_raw << packet.hops();
									new_raw << next_hop2;           // transport_id
									new_raw << packet.raw().mid(2); // destination_hash + payload
								}
								else {
									// Direct or single-hop: send as HEADER_1
									new_raw << packet.raw().left(1);
									new_raw << packet.hops();
									new_raw << packet.raw().mid(2);
								}

								// Create link_table or reverse_table entry for return traffic
								if (packet.packet_type() == Type::Packet::LINKREQUEST) {
									double now = OS::time();
									double proof_timeout = Transport::extra_link_proof_timeout(packet.receiving_interface())
										+ now + Type::Link::ESTABLISHMENT_TIMEOUT_PER_HOP
										* std::max((uint8_t)1, remaining_hops2);

									// === MTU Clamping (v1.0.12) ===
									uint16_t path_mtu = Link::mtu_from_lr_packet(packet);
									if (path_mtu > 0) {
										uint16_t ph_mtu = packet.receiving_interface().HW_MTU();
										uint16_t nh_mtu = outbound_interface2.HW_MTU();
										if (nh_mtu == 0) {
											new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
										} else if (!outbound_interface2.AUTOCONFIGURE_MTU() && !outbound_interface2.FIXED_MTU()) {
											new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE);
										} else if (nh_mtu < path_mtu || (ph_mtu > 0 && ph_mtu < path_mtu)) {
											uint16_t clamped = std::min(nh_mtu, (ph_mtu > 0) ? ph_mtu : nh_mtu);
											RNS::Type::Link::link_mode mode = Link::mode_from_lr_packet(packet);
											Bytes clamped_mtu_bytes = Link::signalling_bytes(clamped, mode);
											new_raw = new_raw.left(new_raw.size() - Type::Link::LINK_MTU_SIZE) + clamped_mtu_bytes;
											DEBUGF("MTU CLAMP: backbone->local path=%u ph=%u nh=%u -> %u", path_mtu, ph_mtu, nh_mtu, clamped);
										}
									}

									LinkEntry link_entry(
										now, next_hop2, outbound_interface2, remaining_hops2,
										packet.receiving_interface(), packet.hops(),
										packet.destination_hash(), false, proof_timeout
									);
									_link_table.insert({Link::link_id_from_lr_packet(packet), link_entry});
									DEBUG(" Created link_table entry for backbone LINKREQUEST, link_id=" + Link::link_id_from_lr_packet(packet).toHex());
								}
								else {
									ReverseEntry reverse_entry(
										packet.receiving_interface(), outbound_interface2, OS::time()
									);
									flatmap_erase(_reverse_table, packet.getTruncatedHash()); _reverse_table.push_back({packet.getTruncatedHash(), reverse_entry});
								}

								DEBUG(" Forwarding backbone packet (" + std::to_string(remaining_hops2) + " hops) to local device for " + packet.destination_hash().toHex() + " via " + outbound_interface2.toString());
								transmit(outbound_interface2, new_raw);
								// Update timestamp on selected path entry
								{
									auto iter = _destination_table.find(packet.destination_hash());
									if (iter != _destination_table.end()) {
										for (auto& e : iter->second) {
											if (e.packet_hash == entry2->packet_hash) {
												e.timestamp = OS::time();
												break;
											}
										}
									}
								}
							}
						}
					}
				}
#endif
			}

			// Link transport handling. Directs packets according
			// to entries in the link tables
			if (packet.packet_type() != Type::Packet::ANNOUNCE && packet.packet_type() != Type::Packet::LINKREQUEST && packet.context() != Type::Packet::LRPROOF) {
				TRACE("Transport::inbound: Checking if packet is meant for link transport...");
				auto link_iter = _link_table.find(packet.destination_hash());
				if (link_iter != _link_table.end()) {
					LinkEntry& link_entry = (*link_iter).second;
					NOTICE("LINK-XPORT: pkt " + packet.destination_hash().toHex().substr(0,8) + " type=" + std::to_string(packet.packet_type()) + " ctx=" + std::to_string(packet.context()) + " hops=" + std::to_string(packet.hops()) + " from=" + packet.receiving_interface().toString() + " sz=" + std::to_string(packet.raw().size()));
					NOTICE("LINK-XPORT: entry hops=" + std::to_string(link_entry._hops) + " rem=" + std::to_string(link_entry._remaining_hops) + " recv=" + link_entry._receiving_interface.toString() + " out=" + link_entry._outbound_interface.toString() + " val=" + std::to_string(link_entry._validated));
					// If receiving and outbound interface is
					// the same for this link, direction doesn't
					// matter, and we simply send the packet on.
					Interface outbound_interface({Type::NONE});
					if (link_entry._outbound_interface == link_entry._receiving_interface) {
						// But check that taken hops matches one
						// of the expectede values.
						if (packet.hops() == link_entry._remaining_hops || packet.hops() == link_entry._hops) {
							TRACE("Transport::inbound: Link inbound/outbound interfaes are same, transporting on same interface");
							outbound_interface = link_entry._outbound_interface;
						}
					}
					else {
						// If interfaces differ, we transmit on
						// the opposite interface of what the
						// packet was received on.
						if (packet.receiving_interface() == link_entry._outbound_interface) {
							// Reverse direction (from destination back toward initiator).
							// Match Python reference Transport.py line 1611: exact match.
							
							
							if (packet.hops() == link_entry._remaining_hops) {
								outbound_interface = link_entry._receiving_interface;
							}
							else {
								NOTICE("LINK-XPORT: HOP MISMATCH (from outbound) pkt.hops=" + std::to_string(packet.hops()) + " expected=" + std::to_string(link_entry._remaining_hops));
								if (is_resource_ctx(packet.context())) {
									WLOG(packet, std::string(ctx_name(packet.context())) + " HOP-MISMATCH rev pkt.hops=" + std::to_string(packet.hops()) + " expected=" + std::to_string(link_entry._remaining_hops));
								}
							}
						}
						else if (packet.receiving_interface() == link_entry._receiving_interface) {
							// Also check that expected hop count matches
							if (packet.hops() == link_entry._hops) {
								outbound_interface = link_entry._outbound_interface;
							}
							else {
								NOTICE("LINK-XPORT: HOP MISMATCH (from receiving) pkt.hops=" + std::to_string(packet.hops()) + " expected=" + std::to_string(link_entry._hops));
								if (is_resource_ctx(packet.context())) {
									WLOG(packet, std::string(ctx_name(packet.context())) + " HOP-MISMATCH fwd pkt.hops=" + std::to_string(packet.hops()) + " expected=" + std::to_string(link_entry._hops));
								}
							}
						}
						else {
							NOTICE("LINK-XPORT: IFACE MISMATCH recv=" + packet.receiving_interface().toString() + " entry_recv=" + link_entry._receiving_interface.toString() + " entry_out=" + link_entry._outbound_interface.toString());
						}
					}

					if (outbound_interface) {
						NOTICE("LINK-XPORT: FWD to " + outbound_interface.toString());
						if (is_resource_ctx(packet.context())) {
							WLOG(packet, std::string(ctx_name(packet.context())) + " FWD to " + outbound_interface.toString() + " (" + zone_tag(is_backbone_interface(outbound_interface)) + ")");
						}
						// Add this packet to the filter hashlist now that
						// we have determined it's actually our turn to
						// process it (matching Python Transport line 1544).
						_packet_hashlist.push_back(packet.packet_hash());
						// CBA RESERVE
						//Bytes new_raw;
						Bytes new_raw(512);
						//new_raw = packet.raw[0:1]
						new_raw << packet.raw().left(1);
						//new_raw += struct.pack("!B", packet.hops)
						new_raw << packet.hops();
						//new_raw += packet.raw[2:]
						new_raw << packet.raw().mid(2);
						transmit(outbound_interface, new_raw);
						link_entry._timestamp = OS::time();
					}
					else {
						NOTICE("LINK-XPORT: DROPPED (no outbound interface resolved)");
					}
				}
				else {
					NOTICE("LINK-XPORT: dest " + packet.destination_hash().toHex().substr(0,8) + " NOT in link_table (size=" + std::to_string(_link_table.size()) + ")");
					if (is_resource_ctx(packet.context())) {
						WLOG(packet, std::string(ctx_name(packet.context())) + " NO-LINKTABLE (size=" + std::to_string(_link_table.size()) + ")");
					}
				}
			}
		}

		////////////////////////////////
		// LOCAL HANDLING
		////////////////////////////////

		// Announce handling. Handles logic related to incoming
		// announces, queueing rebroadcasts of these, and removal
		// of queued announce rebroadcasts once handed to the next node.
		if (packet.packet_type() == Type::Packet::ANNOUNCE) {
			TRACE("Transport::inbound: Packet is ANNOUNCE");
			DEBUG("DIAG: ANNOUNCE-IN dest=" + packet.destination_hash().toHex().substr(0,8) + " iface=" + packet.receiving_interface().toString() + " hops=" + std::to_string(packet.hops()));
			Bytes received_from;
#if defined(DESTINATIONS_SET)
			//Destination local_destination({Type::NONE});
			bool found_local = false;
			for (auto& destination : _destinations) {
				if (destination.hash() == packet.destination_hash()) {
					//local_destination = destination;
					found_local = true;
					break;
				}
			}
            //if local_destination == None and RNS.Identity.validate_announce(packet): 
			//if (!local_destination && Identity::validate_announce(packet)) {
			if (!found_local && Identity::validate_announce(packet)) {
#elif defined(DESTINATIONS_MAP)
			auto iter = _destinations.find(packet.destination_hash());
			if (iter == _destinations.end() && Identity::validate_announce(packet)) {
#endif
				TRACE("Transport::inbound: Packet is announce for non-local destination, processing...");
#ifdef FIREWALL_MODE
				// A valid announce received from LoRa or the local TCP server proves
				// that destination is locally reachable. Record it before replay/path
				// dedup, since the same announce may already have arrived via WAN.
				if (is_trusted_local_interface(packet.receiving_interface())) {
					wl1_push(packet.destination_hash());
					NOTICE("WL#1 ADD - " + packet.destination_hash().toHex().substr(0,8) + " (from LAN)");
				}
#endif
				if (packet.transport_id()) {
					received_from = packet.transport_id();
					
					// Check if this is a next retransmission from
					// another node. If it is, we're removing the
					// announce in question from our pending table
					if (Reticulum::transport_enabled() && _announce_table.count(packet.destination_hash()) > 0) {
						//AnnounceEntry& announce_entry = _announce_table[packet.destination_hash()];
						AnnounceEntry& announce_entry = (*_announce_table.find(packet.destination_hash())).second;
						
						if ((packet.hops() - 1) == announce_entry._hops) {
							DEBUG("Heard a local rebroadcast of announce for " + packet.destination_hash().toHex());
							announce_entry._local_rebroadcasts += 1;
							if (announce_entry._local_rebroadcasts >= LOCAL_REBROADCASTS_MAX) {
								DEBUG("Max local rebroadcasts of announce for " + packet.destination_hash().toHex() + " reached, dropping announce from our table");
								_announce_table.erase(packet.destination_hash());
							}
						}

						if ((packet.hops() - 1) == (announce_entry._hops + 1) && announce_entry._retries > 0) {
							double now = OS::time();
							if (now < announce_entry._timestamp) {
								DEBUG("Rebroadcasted announce for " + packet.destination_hash().toHex() + " has been passed on to another node, no further tries needed");
								_announce_table.erase(packet.destination_hash());
							}
						}
					}
				}
				else {
					received_from = packet.destination_hash();
				}

				// Check if this announce should be inserted into
				// announce and destination tables
				bool should_add = false;

				// First, check that the announce is not for a destination
				// local to this system, and that hops are less than the max
				// CBA TODO determine why packet destination hash is being searched in destinations again since we entered this logic becuase it did not exist above
				//if (not any(packet.destination_hash == d.hash for d in Transport.destinations) and packet.hops < Transport.PATHFINDER_M+1):
#if defined(DESTINATIONS_SET)
				bool found_local = false;
				for (auto& destination : _destinations) {
					if (destination.hash() == packet.destination_hash()) {
						found_local = true;
						break;
					}
				}
				if (!found_local && packet.hops() < (PATHFINDER_M+1)) {
#elif defined(DESTINATIONS_MAP)
				auto iter = _destinations.find(packet.destination_hash());
				if (iter == _destinations.end() && packet.hops() < (PATHFINDER_M+1)) {
#endif
					// ── Multi-Path Path Table Insertion ──────────────────────
					// 2 leaf outcomes instead of 11: replay check only.
					// All valid announces coexist; select_path() picks best at forwarding time.
					Bytes random_blob = packet.data().mid(Type::Identity::KEYSIZE/8 + Type::Identity::NAME_HASH_LENGTH/8, Type::Identity::RANDOM_HASH_LENGTH/8);

					// Step 1: Anti-replay via global blob set
					if (std::find(_global_blobs.begin(), _global_blobs.end(), random_blob) != _global_blobs.end()) {
						should_add = false;
					} else {
						should_add = true;
						_global_blobs.push_back(random_blob);
						// Cap global_blobs at MAX_GLOBAL_BLOBS (FIFO eviction)
						if (_global_blobs.size() > MAX_GLOBAL_BLOBS) {
							_global_blobs.erase(_global_blobs.begin());
						}
					}

					// NOTE: The boundary-mode announce echo blocking that was here
					// has been removed. The standard random_blob check above
					// already prevents echoes (same blob = same announce bounced
					// through backbone). Removing the boundary-specific blocking
					// allows:
					//  1. Local TCP clients to receive backbone announces for
					//     destinations that also have LoRa paths.
					//  2. The V3 destination table to contain the backbone path
					//     when it is equal-or-better, enabling correct transport
					//     routing for TCP client LINKREQUESTs.
					// LoRa-originated announces that arrive first will still be
					// preferred (equal hops = first-arrival wins via random_blob),
					// until the next re-announce cycle changes the path.

					if (should_add) {
						double now = OS::time();

						bool rate_blocked = false;

// TODO
						uint8_t retries = 0;
						uint8_t announce_hops = packet.hops();
						uint8_t local_rebroadcasts = 0;
						bool block_rebroadcasts = false;
						Interface attached_interface = {Type::NONE};
						
						double retransmit_timeout = now + (Cryptography::random() * PATHFINDER_RW);

						double expires = now + PATHFINDER_E;

						// ── Multi-Path Insertion ────────────────────────────
						// 1. Build PathEntry from announce fields
						// 2. Dedup within dest by packet_hash
						// 3. Push front (newest-first), truncate to MAX_PATHS_PER_DEST

						TRACE("Caching packet " + packet.get_hash().toHex());
						if (RNS::Transport::cache_packet(packet, true)) {
							packet.cached(true);
						}
						TRACE("Adding destination " + packet.destination_hash().toHex() + " to path table");

						PathEntry new_entry(
							now,
							received_from,
							announce_hops,
							expires,
							packet.receiving_interface().get_hash(),
							packet.get_hash()
						);

						auto& deque = _destination_table[packet.destination_hash()];
						bool is_new_dest = deque.empty();

						// Remove any existing entry with the same packet_hash (same announce)
						deque.erase(std::remove_if(deque.begin(), deque.end(),
							[&new_entry](const PathEntry& e) {
								return e.packet_hash == new_entry.packet_hash;
							}), deque.end());

						// Prepend newest entry
						deque.push_front(new_entry);

						// Cap at MAX_PATHS_PER_DEST
						while (deque.size() > MAX_PATHS_PER_DEST) {
							deque.pop_back();
						}

						if (is_new_dest) {
							++_destinations_added;
							cull_path_table();
						}

						// ── Announce table insertion / retransmission ──────
						// In firewall mode, every announce (LAN or whitelisted WAN)
						// gets rebroadcast to all interfaces.  No DISCOVERY_ADD
						// dependency — announces propagate naturally.
#ifdef FIREWALL_MODE
						if (packet.context() != Type::Packet::PATH_RESPONSE) {
#else
						if (Reticulum::transport_enabled() && packet.context() != Type::Packet::PATH_RESPONSE) {
#endif
							if (rate_blocked) {
								DEBUG("Blocking rebroadcast of announce from " + packet.destination_hash().toHex() + " due to excessive announce rate");
							}
							else {
								// attached_interface stays NONE in firewall mode:
								// the announce will be broadcast to ALL interfaces,
								// not just the one it arrived on.
								AnnounceEntry announce_entry(
									now,
									retransmit_timeout,
									retries,
									received_from,
									announce_hops,
									packet,
									local_rebroadcasts,
									block_rebroadcasts,
									attached_interface
								);
								_announce_table.erase(packet.destination_hash());
								_announce_table.insert({packet.destination_hash(), announce_entry});
							}
						}

						DEBUG("Destination " + packet.destination_hash().toHex() + " is now " + std::to_string(announce_hops) + " hops away via " + received_from.toHex() + " on " + packet.receiving_interface().toString());
						DEBUG("DIAG: STORED path " + packet.destination_hash().toHex().substr(0,8) + " hops=" + std::to_string(announce_hops) + " iface=" + packet.receiving_interface().toString());

						//TRACE("Transport::inbound: Destination " + packet.destination_hash().toHex() + " has data: " + packet.data().toHex());
						//TRACE("Transport::inbound: Destination " + packet.destination_hash().toHex() + " has text: " + packet.data().toString());

// TODO
/*
						// If the receiving interface is a tunnel, we add the
						// announce to the tunnels table
						if (packet.receiving_interface().tunnel_id()) {
							tunnel_entry = Transport.tunnels[packet.receiving_interface.tunnel_id];
							paths = tunnel_entry[2];
							paths[packet.destination_hash] = destination_table_entry;
							expires = OS::time() + Transport::DESTINATION_TIMEOUT;
							tunnel_entry[3] = expires;
							DEBUG("Path to " + packet.destination_hash().toHex() + " associated with tunnel " + packet.receiving_interface().tunnel_id().toHex());
						}
*/

						// Call externally registered callbacks from apps
						// wanting to know when an announce arrives
						if (packet.context() != Type::Packet::PATH_RESPONSE) {
							TRACE("Transport::inbound: Not path response, sending to announce handler...");
							for (auto& handler : _announce_handlers) {
								TRACE("Transport::inbound: Checking filter of announce handler...");
								try {
									// Check that the announced destination matches
									// the handlers aspect filter
									bool execute_callback = false;
									Identity announce_identity(Identity::recall(packet.destination_hash()));
									if (handler->aspect_filter().empty()) {
										// If the handlers aspect filter is set to
										// None, we execute the callback in all cases
										execute_callback = true;
									}
									else {
										Bytes handler_expected_hash = Destination::hash_from_name_and_identity(handler->aspect_filter().c_str(), announce_identity);
										if (packet.destination_hash() == handler_expected_hash) {
											execute_callback = true;
										}
									}
									if (execute_callback) {
										// CBA TODO Why does app data come from recall instead of from this announce packet?
										handler->received_announce(
											packet.destination_hash(),
											announce_identity,
											Identity::recall_app_data(packet.destination_hash())
										);
									}
								}
								catch (std::exception& e) {
									ERROR("Error while processing external announce callback.");
									ERRORF("The contained exception was: %s", e.what());
								}
							}
						}
					}
				}
				else {
					TRACE("Transport::inbound: Packet is announce for local destination, not processing");
				}
			}
			else {
				TRACE("Transport::inbound: Packet is announce for local destination, not processing");
			}
		}

		// Handling for link requests to local destinations
		else if (packet.packet_type() == Type::Packet::LINKREQUEST) {
			WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LINKREQ-IN");
			if (!packet.transport_id() || packet.transport_id() == _identity.hash()) {
				TRACE("Transport::inbound: Checking if LINKREQUEST is for local destination");
				bool found_local = false;
#if defined(DESTINATIONS_SET)
				for (auto& destination : _destinations) {
					if (destination.hash() == packet.destination_hash() && destination.type() == packet.destination_type()) {
#elif defined(DESTINATIONS_MAP)
				auto iter = _destinations.find(packet.destination_hash());
				if (iter != _destinations.end()) {
					auto& destination = (*iter).second;
					if (destination.type() == packet.destination_type()) {
#endif
						TRACE("Transport::inbound: Found local destination for LINKREQUEST");
						packet.destination(destination);
						found_local = true;
#if defined(DESTINATIONS_SET)
						const_cast<Destination&>(destination).receive(packet);
#else
						destination.receive(packet);
#endif
					}
				}
#ifdef FIREWALL_MODE
				// Forward non-local link requests from non-backbone to backbone
				if (!found_local && !is_backbone_interface(packet.receiving_interface())) {
					// Match Python: use the path table to find the correct
					// outbound interface, not broadcast to all backbones.
					Interface outbound_iface({Type::NONE});
					if (has_path(packet.destination_hash())) {
						const PathEntry* entry = select_path(packet.destination_hash());
						if (entry) {
							outbound_iface = find_interface_from_hash(entry->receiving_interface);
						}
					}
					// Fallback: use first connected backbone interface
					if (!outbound_iface) {
						for (auto& [hash, iface] : _interfaces) {
							if (is_backbone_interface(iface)) {
								outbound_iface = iface;
								break;
							}
						}
					}
					WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - FWD: " + outbound_iface.toString() + " (" + zone_tag(is_backbone_interface(outbound_iface)) + ") LINKREQ");
					if (outbound_iface && outbound_iface != packet.receiving_interface()) {
						double now = OS::time();
						uint8_t actual_hops = 1;
						if (has_path(packet.destination_hash())) {
							actual_hops = hops_to(packet.destination_hash());
							if (actual_hops < 1) actual_hops = 1;
						}
						LinkEntry link_entry(now, packet.destination_hash(), outbound_iface, actual_hops,
							packet.receiving_interface(), packet.hops(),
							packet.destination_hash(), false,
							Transport::extra_link_proof_timeout(packet.receiving_interface())
								+ now + (Type::Link::ESTABLISHMENT_TIMEOUT_PER_HOP * actual_hops));
						Bytes link_id = Link::link_id_from_lr_packet(packet);
						_link_table.erase(link_id);
						_link_table.insert({link_id, link_entry});
						wl2_push(link_id);
						transmit(outbound_iface, packet.raw());
					}
				}
#endif
			}
		}
		
		// Handling for data packets to local destinations
		else if (packet.packet_type() == Type::Packet::DATA) {
			TRACE("Transport::inbound: Packet is DATA");
			if (packet.destination_type() == Type::Destination::LINK) {
				// Data is destined for a link
				TRACE("Transport::inbound: Packet is DATA for a LINK");
				std::set<Link> active_links(_active_links);
				for (auto& link : active_links) {
					if (link.link_id() == packet.destination_hash()) {
						TRACE("Transport::inbound: Packet is DATA for an active LINK");
						packet.link(link);
						const_cast<Link&>(link).receive(packet);
					}
				}
			}
			else {
				// Data is basic (not destined for a link)
#if defined(DESTINATIONS_SET)
				for (auto& destination : _destinations) {
					if (destination.hash() == packet.destination_hash() && destination.type() == packet.destination_type()) {
#elif defined(DESTINATIONS_MAP)
				auto iter = _destinations.find(packet.destination_hash());
				if (iter != _destinations.end()) {
					// Data is for a local destination
					WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LAN-DELIVER");
					auto& destination = (*iter).second;
					if (destination.type() == packet.destination_type()) {
						TRACE("Transport::inbound: Packet destination type " + std::to_string(packet.destination_type()) + " matched, processing");
#endif
						packet.destination(destination);
#if defined(DESTINATIONS_SET)
						const_cast<Destination&>(destination).receive(packet);
#else
						destination.receive(packet);
#endif

						if (destination.proof_strategy() == Type::Destination::PROVE_ALL) {
							packet.prove();
						}
						else if (destination.proof_strategy() == Type::Destination::PROVE_APP) {
							if (destination.callbacks()._proof_requested) {
								try {
									if (destination.callbacks()._proof_requested(packet)) {
										packet.prove();
									}
								}
								catch (std::exception& e) {
									ERROR(std::string("Error while executing proof request callback. The contained exception was: ") + e.what());
								}
							}
						}
					}
					else {
						DEBUG("Transport::inbound: Packet destination type " + std::to_string(packet.destination_type()) + " mismatch, ignoring");
					}
				}
				else {
					DEBUG("Transport::inbound: Local destination " + packet.destination_hash().toHex() + " not found, not handling packet locally");
				}
			}
		}

		// Handling for proofs and link-request proofs
		else if (packet.packet_type() == Type::Packet::PROOF) {
			TRACE("Transport::inbound: Packet is PROOF");
			if (packet.context() == Type::Packet::LRPROOF) {
				TRACE("Transport::inbound: Packet is LINK PROOF");
				// This is a link request proof, check if it
				// needs to be transported
				if ((true) && _link_table.find(packet.destination_hash()) != _link_table.end()) {
					DEBUG("LRPROOF-XPORT: handling proof for link " + packet.destination_hash().toHex().substr(0,8));
					LinkEntry& link_entry = (*_link_table.find(packet.destination_hash())).second;
					WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - FWD: " + link_entry._receiving_interface.toString() + " (" + zone_tag(is_backbone_interface(link_entry._receiving_interface)) + ") LRPROOF");
					DEBUG("LRPROOF-XPORT: recv_iface=" + packet.receiving_interface().toString() + " entry_out=" + link_entry._outbound_interface.toString() + " entry_recv=" + link_entry._receiving_interface.toString());

					bool interface_match = (packet.receiving_interface() == link_entry._outbound_interface);
					if (interface_match) {
						try {
							size_t expected_size = (Type::Identity::SIGLENGTH/8 + Type::Link::ECPUBSIZE/2);
							size_t expected_size_with_mtu = expected_size + Type::Link::LINK_MTU_SIZE;
							if (packet.data().size() == expected_size || packet.data().size() == expected_size_with_mtu) {
								Bytes signalling_bytes;
								if (packet.data().size() == expected_size_with_mtu) {
									signalling_bytes = Link::signalling_bytes(Link::mtu_from_lp_packet(packet), Link::mode_from_lp_packet(packet));
								}

								Bytes peer_pub_bytes = packet.data().mid(Type::Identity::SIGLENGTH/8, Type::Link::ECPUBSIZE/2);
								Identity peer_identity = Identity::recall(link_entry._destination_hash);
								if (!peer_identity) {
									WLOG(packet, "TO: " + short_hash(link_entry._destination_hash) + " (" + dest_zone(link_entry._destination_hash) + ") - LRPROOF-FAIL: no identity in cache");
								}
								else {
								DEBUG("LRPROOF-XPORT: peer identity recalled for " + link_entry._destination_hash.toHex().substr(0,8));
								Bytes peer_sig_pub_bytes = peer_identity.get_public_key().mid(Type::Link::ECPUBSIZE/2, Type::Link::ECPUBSIZE/2);

								Bytes signed_data = packet.destination_hash() + peer_pub_bytes + peer_sig_pub_bytes + signalling_bytes;
								Bytes signature = packet.data().left(Type::Identity::SIGLENGTH/8);

								if (peer_identity.validate(signature, signed_data)) {
									DEBUG("LRPROOF-XPORT: VALIDATED, forwarding to " + link_entry._receiving_interface.toString());
									// CBA RESERVE
									//Bytes new_raw = packet.raw().left(1);
									Bytes new_raw(512);
									new_raw << packet.raw().left(1);
									new_raw << packet.hops();
									new_raw << packet.raw().mid(2);
									DEBUG("LRPROOF-XPORT: new_raw size=" + std::to_string(new_raw.size()) + " hops=" + std::to_string(packet.hops()) + " flags=0x" + new_raw.left(1).toHex() + " dest=" + new_raw.mid(2, 10).toHex().substr(0,16));
									link_entry._validated = true;
									transmit(link_entry._receiving_interface, new_raw);
									DEBUG("LRPROOF-XPORT: transmit() returned OK");
								}
								else {
									WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LRPROOF-FAIL: invalid signature");
								}
								} // end peer_identity valid
							}
							else {
								WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LRPROOF-FAIL: bad size " + std::to_string(packet.data().size()) + " expected " + std::to_string(expected_size));
							}
						}
						catch (std::exception& e) {
							WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LRPROOF-FAIL: " + std::string(e.what()));
						}
					}
					else {
						WLOG(packet, "TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LRPROOF-FAIL: iface mismatch");
					}
				}
				else {
					// Not in link_table or transport not enabled — check
					// if we can deliver it to a local pending link
					NOTICE("[PROOF] - TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - LRPROOF-DROP not in link_table (lt=" + std::to_string(_link_table.size()) + " pl=" + std::to_string(_pending_links.size()) + ")");
					// CBA Must make a copy of _pending_links before traversing since it gets modified
					//for (auto link : _pending_links) {
					std::set<Link> pending_links(_pending_links);
					for (auto& link : pending_links) {
						TRACEF("Checking for link request handling by pending link %s", link.link_id().toHex().c_str());
						if (link.link_id() == packet.destination_hash()) {
							TRACE("Requesting pending link to validate proof");
							const_cast<Link&>(link).validate_proof(packet);
						}
					}
				}
			}
			else if (packet.context() == Type::Packet::RESOURCE_PRF) {
				TRACE("Transport::inbound: Packet is RESOURCE PROOF");
				std::set<Link> active_links(_active_links);
				for (auto& link : active_links) {
					if (link.link_id() == packet.destination_hash()) {
						const_cast<Link&>(link).receive(packet);
					}
				}
			}
			else {
				TRACE("Transport::inbound: Packet is regular PROOF");
				if (packet.destination_type() == Type::Destination::LINK) {
					std::set<Link> active_links(_active_links);
					for (auto& link : active_links) {
						if (link.link_id() == packet.destination_hash()) {
							packet.link(link);
						}
					}
				}

				Bytes proof_hash;
				if (packet.data().size() == Type::PacketReceipt::EXPL_LENGTH) {
					proof_hash = packet.data().left(Type::Identity::HASHLENGTH/8);
				}

				// Check if this proof needs to be transported
				if ((true) && flatmap_find(_reverse_table, packet.destination_hash()) != _reverse_table.end()) {
					ReverseEntry reverse_entry = (*flatmap_find(_reverse_table, packet.destination_hash())).second;
					NOTICE("[PROOF] - FROM: " + packet.receiving_interface().toString() + " (" + zone_tag(is_backbone_interface(packet.receiving_interface())) + ") - TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - FWD: " + reverse_entry._receiving_interface.toString() + " (" + zone_tag(is_backbone_interface(reverse_entry._receiving_interface)) + ") REV");
					if (packet.receiving_interface() == reverse_entry._outbound_interface) {
						TRACE("Proof received on correct interface, transporting it via " + reverse_entry._receiving_interface.toString());
						// CBA RESERVE
						//Bytes new_raw = packet.raw().left(1);
						Bytes new_raw(512);
						new_raw << packet.raw().left(1);
						new_raw << packet.hops();
						new_raw << packet.raw().mid(2);
						transmit(reverse_entry._receiving_interface, new_raw);
					}
					else {
						DEBUG("Proof received on wrong interface, not transporting it.");
					}
				}
				else {
					NOTICE("[PROOF] - TO: " + short_hash(packet.destination_hash()) + " (" + dest_zone(packet.destination_hash()) + ") - NO-REV cannot forward");
				}

				std::list<PacketReceipt> cull_receipts;
				for (auto& receipt : _receipts) {
					bool receipt_validated = false;
					if (proof_hash) {
						// Only test validation if hash matches
						if (receipt.hash() == proof_hash) {
							receipt_validated = receipt.validate_proof_packet(packet);
						}
					}
					else {
						// TODO: This looks like it should actually
						// be rewritten when implicit proofs are added.

						// In case of an implicit proof, we have
						// to check every single outstanding receipt
						receipt_validated = receipt.validate_proof_packet(packet);
					}

					// CBA TODO requires modifying of collection while iterating which is forbidden
					if (receipt_validated) {
						cull_receipts.push_back(receipt);
					}
				}
				// CBA since modifying of collection while iterating is forbidden
				for (auto& receipt : _receipts) {
					cull_receipts.remove(receipt);
				}
			}
		}
	}

	// Heap telemetry: snapshot at exit (MUTED)
	// {
	// 	size_t _heap_at_exit = OS::heap_available();
	// 	int _inbound_delta = (int)_heap_at_exit - (int)_heap_at_entry;
	// 	static uint32_t _tel_pkt_count = 0;
	// 	++_tel_pkt_count;
	// 	if (_inbound_delta < -64 || (_tel_pkt_count % 100 == 0)) {
	// 		VERBOSEF("[HEAP-TEL] inbound: %d bytes (heap=%u pin=%u bma=%u phl=%u lt=%u revr=%u)",
	// 			_inbound_delta, (uint32_t)_heap_at_exit, _packets_received,
	// 			_firewall_mentioned_addresses.size(), _packet_hashlist.size(),
	// 			_link_table.size(), _reverse_table.size());
	// 	}
	// }

	_jobs_locked = false;
}

/*static*/ void Transport::synthesize_tunnel(const Interface& interface) {
// TODO

}

/*static*/ void Transport::tunnel_synthesize_handler(const Bytes& data, const Packet& packet) {
// TODO

}

/*static*/ void Transport::handle_tunnel(const Bytes& tunnel_id, const Interface& interface) {
// TODO

}

/*static*/ void Transport::register_interface(Interface& interface) {
	TRACE("Transport: Registering interface " + interface.get_hash().toHex() + " " + interface.toString());
#if defined(INTERFACES_SET)
	_interfaces.insert(interface);
#elif defined(INTERFACES_LIST)
	_interfaces.push_back(interface);
#elif defined(INTERFACES_MAP)
	_interfaces.insert({interface.get_hash(), interface});
#endif
	// CBA TODO set or add transport as listener on interface to receive incoming packets?
}

/*static*/ void Transport::register_local_client_interface(Interface& interface) {
	interface.is_local_client(true);
	interface.is_backbone(false);
	TRACE("Transport: Registered trusted local client interface " + interface.toString());
}

/*static*/ void Transport::deregister_interface(const Interface& interface) {
	TRACE("Transport: Deregistering interface " + interface.toString());
#if defined(INTERFACES_SET)
	//for (auto iter = _interfaces.begin(); iter != _interfaces.end(); ++iter) {
	//	if ((*iter).get() == interface) {
	//		_interfaces.erase(iter);
	//		TRACE("Transport::deregister_interface: Found and removed interface " + (*iter).get().toString());
	//		break;
	//	}
	//}
	//auto iter = _interfaces.find(interface);
	auto iter = _interfaces.find(const_cast<Interface&>(interface));
	if (iter != _interfaces.end()) {
		_interfaces.erase(iter);
		TRACE("Transport::deregister_interface: Found and removed interface " + (*iter).get().toString());
	}
#elif defined(INTERFACES_LIST)
	for (auto iter = _interfaces.begin(); iter != _interfaces.end(); ++iter) {
		if ((*iter).get() == interface) {
			_interfaces.erase(iter);
			TRACE("Transport::deregister_interface: Found and removed interface " + (*iter).get().toString());
			break;
		}
	}
#elif defined(INTERFACES_MAP)
	auto iter = _interfaces.find(interface.get_hash());
	if (iter != _interfaces.end()) {
		TRACE("Transport::deregister_interface: Found and removed interface " + (*iter).second.toString());
		_interfaces.erase(iter);
	}
#endif
}

/*static*/ void Transport::register_destination(Destination& destination) {
	//TRACE("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	TRACE("Transport: Registering destination " + destination.toString());
	destination.mtu(Type::Reticulum::MTU);
	if (destination.direction() == Type::Destination::IN) {
#if defined(DESTINATIONS_SET)
		for (auto& registered_destination : _destinations) {
			if (destination.hash() == registered_destination.hash()) {
				throw std::runtime_error("Attempt to register an already registered destination.");
			}
		}

		// CBA ACCUMULATES
		_destinations.insert(destination);
#elif defined(DESTINATIONS_MAP)
		auto iter = _destinations.find(destination.hash());
		if (iter != _destinations.end()) {
			throw std::runtime_error("Attempt to register an already registered destination.");
		}

		// CBA ACCUMULATES
		_destinations.insert({destination.hash(), destination});
#endif

		if (_owner && _owner.is_connected_to_shared_instance()) {
			if (destination.type() == Type::Destination::SINGLE) {
				TRACE("Transport:register_destination: Announcing destination " + destination.toString());
				destination.announce({}, true);
			}
		}
	}
	else {
		TRACE("Transport:register_destination: Skipping registration (not direction IN) of destination " + destination.toString());
	}

/*
#if defined(DESTINATIONS_SET)
	for (const Destination& destination : _destinations) {
#elif defined(DESTINATIONS_MAP)
	for (auto& [hash, destination] : _destinations) {
#endif
		TRACE("Transport::register_destination: Listed destination " + destination.toString());
	}
*/
	//TRACE("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
}

/*static*/ void Transport::deregister_destination(const Destination& destination) {
	TRACE("Transport: Deregistering destination " + destination.toString());
#if defined(DESTINATIONS_SET)
	if (_destinations.find(destination) != _destinations.end()) {
		_destinations.erase(destination);
		TRACE("Transport::deregister_destination: Found and removed destination " + destination.toString());
	}
#elif defined(DESTINATIONS_MAP)
	auto iter = _destinations.find(destination.hash());
	if (iter != _destinations.end()) {
		_destinations.erase(iter);
		TRACE("Transport::deregister_destination: Found and removed destination " + (*iter).second.toString());
	}
#endif
}

/*static*/ void Transport::register_link(Link& link) {
	TRACE("Transport: Registering link " + link.toString());
	if (link.initiator()) {
		// CBA ACCUMULATES
		_pending_links.insert(link);
	}
	else {
		// CBA ACCUMULATES
		_active_links.insert(link);
	}
}

/*static*/ void Transport::activate_link(Link& link) {
	TRACE("Transport: Activating link " + link.toString());
	if (_pending_links.find(link) != _pending_links.end()) {
		if (link.status() != Type::Link::ACTIVE) {
			throw std::runtime_error("Invalid link state for link activation: " + std::to_string(link.status()));
		}
		_pending_links.erase(link);
		// CBA ACCUMULATES
		_active_links.insert(link);
		link.status(Type::Link::ACTIVE);
	}
	else {
		ERROR("Attempted to activate a link that was not in the pending table");
	}
}

/*
Registers an announce handler.

:param handler: Must be an object with an *aspect_filter* attribute and a *received_announce(destination_hash, announced_identity, app_data)* callable. See the :ref:`Announce Example<example-announce>` for more info.
*/
/*static*/ void Transport::register_announce_handler(HAnnounceHandler handler) {
	TRACE("Transport: Registering announce handler " + handler->aspect_filter());
	_announce_handlers.insert(handler);
}

/*
Deregisters an announce handler.

:param handler: The announce handler to be deregistered.
*/
/*static*/ void Transport::deregister_announce_handler(HAnnounceHandler handler) {
	TRACE("Transport: Deregistering announce handler " + handler->aspect_filter());
	if (_announce_handlers.find(handler) != _announce_handlers.end()) {
		_announce_handlers.erase(handler);
		TRACE("Transport::deregister_announce_handler: Found and removed handler" + handler->aspect_filter());
	}
}

/*static*/ Interface Transport::find_interface_from_hash(const Bytes& interface_hash) {
#if defined(INTERFACES_SET)
	for (const Interface& interface : _interfaces) {
		if (interface.get_hash() == interface_hash) {
			TRACE("Transport::find_interface_from_hash: Found interface " + interface.toString());
			return interface;
		}
	}
#elif defined(INTERFACES_LIST)
	for (Interface& interface : _interfaces) {
		if (interface.get_hash() == interface_hash) {
			TRACE("Transport::find_interface_from_hash: Found interface " + interface.toString());
			return interface;
		}
	}
#elif defined(INTERFACES_MAP)
	auto iter = _interfaces.find(interface_hash);
	if (iter != _interfaces.end()) {
		TRACE("Transport::find_interface_from_hash: Found interface " + (*iter).second.toString());
		return (*iter).second;
	}
#endif

	return {Type::NONE};
}

/*static*/ bool Transport::should_cache_packet(const Packet& packet) {
	// TODO: Rework the caching system. It's currently
	// not very useful to even cache Resource proofs,
	// disabling it for now, until redesigned.
	// if packet.context == RNS.Packet.RESOURCE_PRF:
	//     return True

	return false;
}

// When caching packets to storage, they are written
// exactly as they arrived over their interface. This
// means that they have not had their hop count
// increased yet! Take note of this when reading from
// the packet cache.
/*static*/ bool Transport::cache_packet(const Packet& packet, bool force_cache /*= false*/) {
	TRACE("Checking to see if packet " + packet.get_hash().toHex() + " should be cached");
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
	if (should_cache_packet(packet) || force_cache) {
		TRACE("Saving packet " + packet.get_hash().toHex() + " to storage");
		try {
			char packet_cache_path[Type::Reticulum::FILEPATH_MAXSIZE];
			snprintf(packet_cache_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/%s", Reticulum::_cachepath, packet.get_hash().toHex().c_str());
			return (Persistence::serialize(packet, packet_cache_path) > 0);
		}
		catch (std::exception& e) {
			ERROR("Error writing packet to cache. The contained exception was: " + std::string(e.what()));
		}
	}
#endif
	return false;
}

/*static*/ Packet Transport::get_cached_packet(const Bytes& packet_hash) {
	TRACE("Loading packet " + packet_hash.toHex() + " from cache storage");
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
	try {

		char packet_cache_path[Type::Reticulum::FILEPATH_MAXSIZE];
		snprintf(packet_cache_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/%s", Reticulum::_cachepath, packet_hash.toHex().c_str());
		Packet packet({Type::NONE});
		if (Persistence::deserialize(packet, packet_cache_path) > 0) {
			packet.unpack();
		}
		return packet;
	}
	catch (std::exception& e) {
		ERROR("Exception occurred while getting cached packet.");
		ERRORF("The contained exception was: %s", e.what());
	}
#endif
	return {Type::NONE};
}

/*static*/ bool Transport::clear_cached_packet(const Bytes& packet_hash) {
	TRACE("Clearing packet " + packet_hash.toHex() + " from cache storage");
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
	try {
		char packet_cache_path[Type::Reticulum::FILEPATH_MAXSIZE];
		snprintf(packet_cache_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/%s", Reticulum::_cachepath, packet_hash.toHex().c_str());
		double start_time = OS::time();
		bool success = RNS::Utilities::OS::remove_file(packet_cache_path);
		double diff_time = OS::time() - start_time;
		if (diff_time < 1.0) {
			DEBUG("Remove cached packet in " + std::to_string((int)(diff_time*1000)) + " ms");
		}
		else {
			DEBUG("Remove cached packet in " + std::to_string(diff_time) + " s");
		}
	}
	catch (std::exception& e) {
		ERROR("Exception occurred while clearing cached packet.");
		ERRORF("The contained exception was: %s", e.what());
	}
#endif
	return false;
}

/*static*/ bool Transport::cache_request_packet(const Packet& packet) {
	if (packet.data().size() == Type::Identity::HASHLENGTH/8) {
		const Packet& cached_packet = get_cached_packet(packet.data());

		if (cached_packet) {
			// If the packet was retrieved from the local
			// cache, replay it to the Transport instance,
			// so that it can be directed towards it original
			// destination.
			inbound(cached_packet.raw(), cached_packet.receiving_interface());
			return true;
		}
		else {
			return false;
		}
	}
	else {
		return false;
	}
}

/*static*/ void Transport::cache_request(const Bytes& packet_hash, const Destination& destination) {
	const Packet& cached_packet = get_cached_packet(packet_hash);
	if (cached_packet) {
		// The packet was found in the local cache,
		// replay it to the Transport instance.
		inbound(cached_packet.raw(), cached_packet.receiving_interface());
	}
	else {
		// The packet is not in the local cache,
		// query the network.
		Packet request(destination, packet_hash, Type::Packet::DATA, Type::Packet::CACHE_REQUEST);
		request.send();
	}
}

/*static*/ const Transport::PathEntry* Transport::select_path(const Bytes& destination_hash) {
	auto iter = _destination_table.find(destination_hash);
	if (iter == _destination_table.end()) return nullptr;

	const auto& deque = iter->second;
	double now = OS::time();
	const PathEntry* best = nullptr;
	double best_score = -1.0;

	for (const auto& entry : deque) {
		if (entry.is_expired(now)) continue;
		Interface iface = find_interface_from_hash(entry.receiving_interface);
		uint32_t bitrate = iface ? iface.bitrate() : 0;
		double s = entry.score(bitrate);
		if (s > best_score) {
			best_score = s;
			best = &entry;
		}
	}
	return best;
}

/*static*/ std::vector<Transport::PathEntry> Transport::select_all_paths(const Bytes& destination_hash) {
	std::vector<PathEntry> result;
	auto iter = _destination_table.find(destination_hash);
	if (iter == _destination_table.end()) return result;

	double now = OS::time();
	for (const auto& entry : iter->second) {
		if (entry.is_expired(now)) continue;
		result.push_back(entry);
	}
	// Sort by score descending (best first)
	std::sort(result.begin(), result.end(),
		[&](const PathEntry& a, const PathEntry& b) {
			Interface iface_a = find_interface_from_hash(a.receiving_interface);
			Interface iface_b = find_interface_from_hash(b.receiving_interface);
			uint32_t br_a = iface_a ? iface_a.bitrate() : 0;
			uint32_t br_b = iface_b ? iface_b.bitrate() : 0;
			return a.score(br_a) > b.score(br_b);
		});
	return result;
}

/*static*/ bool Transport::remove_path(const Bytes& destination_hash) {
	if (_destination_table.erase(destination_hash) > 0) {
		// CBA also remove cached announce packet if exists
	}
	return false;
}

/*
:param destination_hash: A destination hash as *bytes*.
:returns: *True* if a path to the destination is known, otherwise *False*.
*/
/*static*/ bool Transport::has_path(const Bytes& destination_hash) {
	return select_path(destination_hash) != nullptr;
}

/*
:param destination_hash: A destination hash as *bytes*.
:returns: The number of hops to the specified destination, or ``RNS.Transport.PATHFINDER_M`` if the number of hops is unknown.
*/
/*static*/ uint8_t Transport::hops_to(const Bytes& destination_hash) {
	const PathEntry* entry = select_path(destination_hash);
	if (entry) {
		return entry->hops;
	}
	else {
		return PATHFINDER_M;
	}
}

/*
:param destination_hash: A destination hash as *bytes*.
:returns: The destination hash as *bytes* for the next hop to the specified destination, or *None* if the next hop is unknown.
*/
/*static*/ Bytes Transport::next_hop(const Bytes& destination_hash) {
	const PathEntry* entry = select_path(destination_hash);
	if (entry) {
		return entry->next_hop;
	}
	else {
		return {};
	}
}

/*
:param destination_hash: A destination hash as *bytes*.
:returns: The interface for the next hop to the specified destination, or *None* if the interface is unknown.
*/
/*static*/ Interface Transport::next_hop_interface(const Bytes& destination_hash) {
	const PathEntry* entry = select_path(destination_hash);
	if (entry) {
		return find_interface_from_hash(entry->receiving_interface);
	}
	else {
		return {Type::NONE};
	}
}

/*static*/ uint32_t Transport::next_hop_interface_bitrate(const Bytes& destination_hash) {
	const Interface& interface = next_hop_interface(destination_hash);
	if (interface) {
		return interface.bitrate();
	}
	else {
		return 0;
	}
}

/*static*/ uint16_t Transport::next_hop_interface_hw_mtu(const Bytes& destination_hash) {
	const Interface& interface = next_hop_interface(destination_hash);
	if (interface) {
		if (interface.AUTOCONFIGURE_MTU() || interface.FIXED_MTU()) return interface.HW_MTU();
		else return 0;
	}
	else {
		return 0;
	}
}

/*static*/ double Transport::next_hop_per_bit_latency(const Bytes& destination_hash) {
	uint32_t bitrate = next_hop_interface_bitrate(destination_hash);
	if (bitrate > 0) {
		return (1.0/(double)bitrate);
	}
	else {
		return 0.0;
	}
}

/*static*/ double Transport::next_hop_per_byte_latency(const Bytes& destination_hash) {
	double per_bit_latency = next_hop_per_bit_latency(destination_hash);
	if (per_bit_latency > 0.0) {
		return per_bit_latency*8.0;
	}
	else {
		return 0.0;
	}
}

/*static*/ double Transport::first_hop_timeout(const Bytes& destination_hash) {
	double latency = next_hop_per_byte_latency(destination_hash);
	if (latency > 0.0) {
		return RNS::Type::Reticulum::MTU * latency + RNS::Type::Reticulum::DEFAULT_PER_HOP_TIMEOUT;
	}
	else {
		return RNS::Type::Reticulum::DEFAULT_PER_HOP_TIMEOUT;
	}
}

/*static*/ double Transport::extra_link_proof_timeout(const Interface& interface) {
	if (interface) {
		return ((1.0/(double)interface.bitrate())*8.0)*RNS::Type::Reticulum::MTU;
	}
	else {
		return 0.0;
	}
}

/*static*/ bool Transport::expire_path(const Bytes& destination_hash) {
	// Remove ALL entries for this destination (hard delete, not soft-expire)
	return _destination_table.erase(destination_hash) > 0;
}

/*static*/ bool Transport::mark_path_unresponsive(const Bytes& destination_hash, const Bytes& blocked_interface /*= {}*/) {
	auto iter = _destination_table.find(destination_hash);
	if (iter == _destination_table.end()) return false;

	auto& deque = iter->second;
	if (!blocked_interface) {
		// No interface specified: remove all paths (backward compat)
		_destination_table.erase(iter);
		return true;
	}

	// Remove only entries matching the blocked interface
	size_t before = deque.size();
	deque.erase(std::remove_if(deque.begin(), deque.end(),
		[&blocked_interface](const PathEntry& e) {
			return e.receiving_interface == blocked_interface;
		}), deque.end());

	// If deque is now empty, remove the whole destination entry
	if (deque.empty()) {
		_destination_table.erase(iter);
	}
	return deque.size() < before;
}
/*
Requests a path to the destination from the network. If
another reachable peer on the network knows a path, it
will announce it.

:param destination_hash: A destination hash as *bytes*.
:param on_interface: If specified, the path request will only be sent on this interface. In normal use, Reticulum handles this automatically, and this parameter should not be used.
*/
///*static*/ void Transport::request_path(const Bytes& destination_hash, const Interface& on_interface /*= {Type::NONE}*/, const Bytes& tag /*= {}*/, bool recursive /*= false*/) {
/*static*/ void Transport::request_path(const Bytes& destination_hash, const Interface& on_interface, const Bytes& tag /*= {}*/, bool recursive /*= false*/) {
	Bytes request_tag;
	if (!tag) {
		request_tag = Identity::get_random_hash();
	}
	else {
		request_tag = tag;
	}

	Bytes path_request_data;
	if (true) {
		path_request_data = destination_hash + _identity.hash() + request_tag;
	}
	else {
		path_request_data = destination_hash + request_tag;
	}

	Destination path_request_dst({Type::NONE}, Type::Destination::OUT, Type::Destination::PLAIN, Type::Transport::APP_NAME, "path.request");
	Packet packet(path_request_dst, on_interface, path_request_data, Type::Packet::DATA, Type::Packet::CONTEXT_NONE, Type::Transport::BROADCAST, Type::Packet::HEADER_1);

	if (on_interface && recursive) {
// TODO
		bool queued_announces = (on_interface.announce_queue().size() > 0);
		if (queued_announces) {
			TRACE("Blocking recursive path request on " + on_interface.toString() + " due to queued announces");
			return;
		}
		else {
			double now = OS::time();
			if (now < on_interface.announce_allowed_at()) {
				TRACE("Blocking recursive path request on " + on_interface.toString() + " due to active announce cap");
				return;
			}
			else {
				uint32_t wait_time = 0;
				if ( on_interface.bitrate() > 0 && on_interface.announce_cap() > 0) {
					uint32_t tx_time = ((path_request_data.size() + Type::Reticulum::HEADER_MINSIZE)*8) / on_interface.bitrate();
					wait_time = (tx_time / on_interface.announce_cap());
				}
				const_cast<Interface&>(on_interface).announce_allowed_at(now + wait_time);
			}
		}
	}

	packet.send();
	flatmap_upsert(_path_requests, destination_hash, OS::time());
}

/*static*/ void Transport::request_path(const Bytes& destination_hash) {
	return request_path(destination_hash, {Type::NONE});
}

/*static*/ void Transport::path_request_handler(const Bytes& data, const Packet& packet) {
	TRACE("Transport::path_request_handler");
	if (data.size() >= 16) { WLOG(packet, "PATH-REQ for " + data.left(16).toHex().substr(0,8) + " from " + packet.receiving_interface().toString()); }
	try {
		// If there is at least bytes enough for a destination
		// hash in the packet, we assume those bytes are the
		// destination being requested.
		if (data.size() >= Type::Identity::TRUNCATED_HASHLENGTH/8) {
			Bytes destination_hash = data.left(Type::Identity::TRUNCATED_HASHLENGTH/8);
			//TRACE("Transport::path_request_handler: destination_hash: " + destination_hash.toHex());
			// If there is also enough bytes for a transport
			// instance ID and at least one tag byte, we
			// assume the next bytes to be the trasport ID
			// of the requesting transport instance.
			Bytes requesting_transport_instance;
			if (data.size() > (Type::Identity::TRUNCATED_HASHLENGTH/8)*2) {
				requesting_transport_instance = data.mid(Type::Identity::TRUNCATED_HASHLENGTH/8, Type::Identity::TRUNCATED_HASHLENGTH/8);
				//TRACE("Transport::path_request_handler: requesting_transport_instance: " + requesting_transport_instance.toHex());
			}

			Bytes tag_bytes;
			if (data.size() > Type::Identity::TRUNCATED_HASHLENGTH/8*2) {
				tag_bytes = data.mid(Type::Identity::TRUNCATED_HASHLENGTH/8*2);
			}
			else if (data.size() > Type::Identity::TRUNCATED_HASHLENGTH/8) {
				tag_bytes = data.mid(Type::Identity::TRUNCATED_HASHLENGTH/8);
			}

			if (tag_bytes) {
				//TRACE("Transport::path_request_handler: tag_bytes: " + tag_bytes.toHex());
				if (tag_bytes.size() > Type::Identity::TRUNCATED_HASHLENGTH/8) {
					tag_bytes = tag_bytes.left(Type::Identity::TRUNCATED_HASHLENGTH/8);
				}

				Bytes unique_tag = destination_hash + tag_bytes;
				//TRACE("Transport::path_request_handler: unique_tag: " + unique_tag.toHex());

				if (_discovery_pr_tags.find(unique_tag) == _discovery_pr_tags.end()) {
					// CBA ACCUMULATES
					_discovery_pr_tags.insert(unique_tag);

					path_request(
						destination_hash,
						false,
						packet.receiving_interface(),
						requesting_transport_instance,
						tag_bytes
					);
				}
				else {
					NOTICE("[PKT] PATH-REQ-DUP for " + destination_hash.toHex().substr(0,8));
				}
			}
			else {
				DEBUG("Ignoring tagless path request for " + destination_hash.toHex());
			}
		}
	}
	catch (std::exception& e) {
		ERROR("Error while handling path request. The contained exception was: " + std::string(e.what()));
	}
}

/*static*/ void Transport::path_request(const Bytes& destination_hash, bool is_from_local_client, const Interface& attached_interface, const Bytes& requestor_transport_id /*= {}*/, const Bytes& tag /*= {}*/) {
	TRACE("Transport::path_request");
	bool should_search_for_unknown = false;
	std::string interface_str;

	if (attached_interface) {
		should_search_for_unknown = true;
		interface_str = " on " + attached_interface.toString();
	}

	DEBUG("Path request for destination " + destination_hash.toHex() + interface_str);

	bool destination_exists_on_local_client = false;
	if (false) {
		if (has_path(destination_hash)) {
			TRACE("Transport::path_request_handler: entry found for destination " + destination_hash.toHex());
			Interface iface = next_hop_interface(destination_hash);
			if (false) {
				destination_exists_on_local_client = true;
				// CBA ACCUMULATES
				_pending_local_path_requests.insert({destination_hash, attached_interface.get_hash()});
			}
		}
		else {
			TRACE("Transport::path_request_handler: entry not found for destination " + destination_hash.toHex());
		}
	}

	auto destination_iter = _destination_table.find(destination_hash);
	//local_destination = next((d for d in Transport.destinations if d.hash == destination_hash), None)
#if defined(DESTINATIONS_SET)
	Destination local_destination({Type::NONE});
	for (auto& destination : _destinations) {
		if (destination.hash() == destination_hash) {
			local_destination = destination;
			break;
		}
	}
    //if local_destination != None:
	if (local_destination) {
#elif defined(DESTINATIONS_MAP)
	auto iter = _destinations.find(destination_hash);
	if (iter != _destinations.end()) {
		auto& local_destination = (*iter).second;
#endif
		local_destination.announce({Bytes::NONE}, true, attached_interface, tag);
		DEBUG("Answering path request for destination " + destination_hash.toHex() + interface_str + ", destination is local to this system");
	}
	else if ((true) && has_path(destination_hash)) {
		TRACE("Transport::path_request_handler: entry found for destination " + destination_hash.toHex());
		const PathEntry* entry = select_path(destination_hash);
		if (!entry) {
			TRACE("path_request: selected path expired for " + destination_hash.toHex());
			return;
		}
		const Packet& announce_packet = get_cached_packet(entry->packet_hash);
		const Bytes& next_hop = entry->next_hop;
		if (!announce_packet) {
			// Cache file missing or corrupt — remove the stale entry and bail
			WARNING("path_request: removing stale path to " + destination_hash.toHex() + " due to missing announce packet cache");
			// Remove the specific entry with the missing cache
			auto iter = _destination_table.find(destination_hash);
			if (iter != _destination_table.end()) {
				iter->second.erase(std::remove_if(iter->second.begin(), iter->second.end(),
					[&entry](const PathEntry& e) { return e.packet_hash == entry->packet_hash; }),
					iter->second.end());
				if (iter->second.empty()) _destination_table.erase(iter);
			}
			return;
		}
		const Interface& receiving_interface = find_interface_from_hash(entry->receiving_interface);

		if (requestor_transport_id && entry->next_hop == requestor_transport_id) {
				// TODO: Find a bandwidth efficient way to invalidate our
				// known path on this signal. The obvious way of signing
				// path requests with transport instance keys is quite
				// inefficient. There is probably a better way. Doing
				// path invalidation here would decrease the network
				// convergence time. Maybe just drop it?
				DEBUG("Not answering path request for destination " + destination_hash.toHex() + interface_str + ", since next hop is the requestor");
			}
			else {
				DEBUG("Answering path request for destination " + destination_hash.toHex() + interface_str + ", path is known");
				DEBUG("DIAG: PATH-RESP for " + destination_hash.toHex().substr(0,8) + interface_str);

				double now = OS::time();
				uint8_t retries = Type::Transport::PATHFINDER_R;
				uint8_t local_rebroadcasts = 0;
				bool block_rebroadcasts = true;
				// Use PathEntry.hops (correct hop count from announce, not stale wire bytes)
				uint8_t announce_hops = entry->hops;

				double retransmit_timeout = now + Type::Transport::PATH_REQUEST_GRACE;

				// This handles an edge case where a peer sends a past
				// request for a destination just after an announce for
				// said destination has arrived, but before it has been
				// rebroadcast locally. In such a case the actual announce
				// is temporarily held, and then reinserted when the path
				// request has been served to the peer.
				auto announce_iter = _announce_table.find(announce_packet.destination_hash());
				if (announce_iter != _announce_table.end()) {
					AnnounceEntry& held_entry = (*announce_iter).second;
					// CBA ACCUMULATES
					_held_announces.insert({announce_packet.destination_hash(), held_entry});
					// BUG FIX: Must erase old entry before insert(),
					// since std::map::insert() is a no-op when key exists.
					// Python dict assignment overwrites, but C++ insert does not.
					_announce_table.erase(announce_iter);
				}

				AnnounceEntry announce_entry(
					now,
					retransmit_timeout,
					retries,
					next_hop,
					announce_hops,
					announce_packet,
					local_rebroadcasts,
					block_rebroadcasts,
					attached_interface
				);
				// CBA ACCUMULATES
				_announce_table.insert({announce_packet.destination_hash(), announce_entry});
			}
		}
	else if (should_search_for_unknown) {
		TRACE("Transport::path_request_handler: searching for unknown path to " + destination_hash.toHex());
		if (flatmap_find(_discovery_path_requests, destination_hash) != _discovery_path_requests.end()) {
			DEBUG("There is already a waiting path request for destination " + destination_hash.toHex() + " on behalf of path request" + interface_str);
		}
		else {
			// Forward path request on all interfaces.
			// Only skip same-interface forwarding for point-to-point
			// interfaces (backbone) — echoing back to the sender on a
			// point-to-point link is just noise.  Broadcast interfaces
			// (LoRa) reach different nodes when retransmitted, and any
			// echo is already caught by _discovery_path_requests /
			// _discovery_pr_tags dedup.
			DEBUG("Attempting to discover unknown path to destination " + destination_hash.toHex() + " on behalf of path request" + interface_str);

#if defined(FIREWALL_MODE)
			// Track this destination in Whitelist 2 so the path
			// response announce from the backbone will be allowed through
			wl2_push(destination_hash);
#endif

#if defined(INTERFACES_SET)
			for (const Interface& interface : _interfaces) {
#elif defined(INTERFACES_LIST)
			for (Interface& interface : _interfaces) {
#elif defined(INTERFACES_MAP)
			for (auto& [hash, interface] : _interfaces) {
#endif
				if (interface == attached_interface && interface.is_backbone()) {
					TRACE("Transport::path_request: not requesting path back to sender on backbone " + interface.toString());
				} else {
					TRACE("Transport::path_request: requesting path on interface " + interface.toString());
					request_path(destination_hash, interface, tag, true);
				}
			}
		}
	}
	else {
		DEBUG("Ignoring path request for destination " + destination_hash.toHex() + interface_str + ", no path known");
	}
}

/*static*/ uint64_t Transport::announce_emitted(const Packet& packet) {
	Bytes random_blob = packet.data().mid(RNS::Type::Identity::KEYSIZE/8+RNS::Type::Identity::NAME_HASH_LENGTH/8, 10);
	if (random_blob) {
		return OS::from_bytes_big_endian(random_blob.data() + 5, 5);
	}
	return 0;
}

/*static*/ void Transport::drop_announce_queues() {
	// No-op in firewall mode: announce queues handled by jobs() loop
}

/*static*/ void Transport::write_packet_hashlist() {
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
// TODO

#endif
}

//#define CUSTOM 1

/*static*/ bool Transport::read_path_table() {
	DEBUG("Transport::read_path_table");
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
	char destination_table_path[Type::Reticulum::FILEPATH_MAXSIZE];
	snprintf(destination_table_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/destination_table", Reticulum::_storagepath);
	if (!_owner.is_connected_to_shared_instance() && OS::file_exists(destination_table_path)) {
		try {
#if CUSTOM
TRACEF("Transport::start: buffer capacity %d bytes", Persistence::_buffer.capacity());
			if (RNS::Utilities::OS::read_file(destination_table_path, Persistence::_buffer) > 0) {
				TRACEF("Transport::start: read: %d bytes", Persistence::_buffer.size());
#ifndef NDEBUG
				// CBA DEBUG Dump path table
TRACEF("Transport::start: buffer addr: 0x%X", Persistence::_buffer.data());
TRACEF("Transport::start: buffer size %d bytes", Persistence::_buffer.size());
				//TRACE("SERIALIZED: destination_table");
				//TRACE(Persistence::_buffer.toString());
#endif
#ifdef USE_MSGPACK
				DeserializationError error = deserializeMsgPack(Persistence::_document, Persistence::_buffer.data());
#else
				DeserializationError error = deserializeJson(Persistence::_document, Persistence::_buffer.data());
#endif
				TRACEF("Transport::start: doc size: %d bytes", Persistence::_buffer.size());
				if (!error) {
					// Calculate crc for dirty-checking before write
					_destination_table_crc = Crc::crc32(0, Persistence::_buffer.data(), Persistence::_buffer.size());
					_destination_table = Persistence::_document.as<std::map<Bytes, std::deque<PathEntry>>>();
#else	// CUSTOM
				// Calculate crc for dirty-checking before write
				if (Persistence::deserialize(_destination_table, destination_table_path, _destination_table_crc) > 0) {
#endif	// CUSTOM

					TRACEF("Transport::start: successfully deserialized path table with %d entries", _destination_table.size());
					std::vector<Bytes> invalid_paths;
					double now = OS::time();
					for (auto& [destination_hash, deque] : _destination_table) {
						// Remove individual entries with missing packet cache or interface
						deque.erase(std::remove_if(deque.begin(), deque.end(),
							[](PathEntry& entry) {
								Interface iface = find_interface_from_hash(entry.receiving_interface);
								Packet pkt = get_cached_packet(entry.packet_hash);
								if (!iface || !pkt) {
									return true; // remove — interface or cache missing
								}
								// Drop paths through interfaces that have no connected
								// clients at boot.  TCP client connections don't survive
								// reboots, so saved paths through LocalTcpInterface are
								// dead until the client reconnects and re-announces.
								if (!iface.isConnected()) {
									return true; // remove — no clients connected
								}
								return false;
							}), deque.end());
						if (deque.empty()) {
							invalid_paths.push_back(destination_hash);
						}
						// Reset timestamps to boot time minus staleness threshold.
						// Without NTP, saved timestamps are seconds-since-boot from
						// a previous session and are meaningless after reboot.
						// Starting stale causes the first packet to each destination
						// to hedge immediately, firing a path_request to revalidate.
						double stale_time = now - (double)Type::Transport::PATH_STALE_THRESHOLD - 1.0;
						for (auto& entry : deque) {
							entry.timestamp = stale_time;
						}
					}
					for (const auto& destination_hash : invalid_paths) {
						_destination_table.erase(destination_hash);
					}

					// Enforce maxsize on loaded paths (trim lowest-score if over limit)
					if (_destination_table.size() > _path_table_maxsize) {
						DEBUGF("Transport::start: trimming loaded path table from %d to %d entries", _destination_table.size(), _path_table_maxsize);
						cull_path_table();
					}

					// Memory diagnostic after path table load
					size_t total_entries = 0;
					for (const auto& [hash, deque] : _destination_table) {
						total_entries += deque.size();
					}
					DEBUGF("Transport::start: path table: %d dests, %d total entries",
						_destination_table.size(), total_entries);

					return true;
				}
				else {
					TRACE("Transport::start: failed to deserialize");
				}
#if CUSTOM
			}
			else {
				TRACE("Transport::start: destination table read failed");
			}
#else	// CUSTOM
#endif	// CUSTOM

			VERBOSEF("Loaded %d valid path table entries from storage", _destination_table.size());

		}
		catch (std::exception& e) {
			ERRORF("Could not load destination table from storage, the contained exception was: %s", e.what());
		}
	}
#endif
	return false;
}

/*static*/ bool Transport::write_path_table() {
	DEBUG("Transport::write_path_table");

	if (Transport::_owner.is_connected_to_shared_instance()) {
		return true;
	}

	bool success = false;
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
	if (_saving_path_table) {
		double wait_interval = 0.2;
		double wait_timeout = 5;
		double wait_start = OS::time();
		while (_saving_path_table) {
			OS::sleep(wait_interval);
			if (OS::time() > (wait_start + wait_timeout)) {
				ERROR("Could not save path table to storage, waiting for previous save operation timed out.");
				return false;
			}
		}
	}

	try {
		_saving_path_table = true;
		double save_start = OS::time();
		DEBUGF("Saving %d path table entries to storage...", _destination_table.size());

		// Enforce maxpersist: create a trimmed copy for serialization
		// keeping only the destinations with the best-score paths
		std::map<Bytes, std::deque<PathEntry>> persist_table;
		if (_destination_table.size() <= _path_table_maxpersist) {
			persist_table = _destination_table;
		}
		else {
			// Sort destinations by best path score, keep top N
			double now = OS::time();
			std::vector<std::pair<Bytes, double>> scored;
			for (auto& [dest_hash, deque] : _destination_table) {
				double best_score = -1.0;
				for (const auto& entry : deque) {
					if (entry.is_expired(now)) continue;
					Interface iface = find_interface_from_hash(entry.receiving_interface);
					double s = entry.score(iface ? iface.bitrate() : 0);
					if (s > best_score) best_score = s;
				}
				if (best_score < 0) best_score = 0;
				scored.push_back({dest_hash, best_score});
			}
			std::sort(scored.begin(), scored.end(),
				[](const std::pair<Bytes, double>& a, const std::pair<Bytes, double>& b) { return a.second > b.second; });
			for (size_t i = 0; i < _path_table_maxpersist && i < scored.size(); i++) {
				auto iter = _destination_table.find(scored[i].first);
				if (iter != _destination_table.end()) {
					persist_table.insert(*iter);
				}
			}
			DEBUGF("Trimmed path table from %d to %d destinations for persistence", _destination_table.size(), persist_table.size());
		}
#if CUSTOM
		{
			Persistence::_document.set(persist_table);
			TRACEF("Transport::write_path_table: doc size %d bytes", Persistence::_document.memoryUsage());

			//size_t size = 8192;
			size_t size = Persistence::_buffer.capacity();
TRACE("Transport::write_path_table: obtaining buffer size " + std::to_string(size) + " bytes");
			uint8_t* buffer = Persistence::_buffer.writable(size);
TRACE("Transport::write_path_table: buffer addr: " + std::to_string((long)buffer));
#ifdef USE_MSGPACK
			size_t length = serializeMsgPack(Persistence::_document, buffer, size);
#else
			size_t length = serializeJson(Persistence::_document, buffer, size);
#endif
			TRACEF("Transport::write_path_table: serialized %d bytes", length);
			if (length < size) {
				Persistence::_buffer.resize(length);
			}
		}
		if (Persistence::_buffer.size() > 0) {
			char destination_table_path[Type::Reticulum::FILEPATH_MAXSIZE];
			snprintf(destination_table_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/destination_table", Reticulum::_storagepath);
#ifndef NDEBUG
			// CBA DEBUG Dump path table
TRACE("Transport::write_path_table: buffer addr: " + std::to_string((long)Persistence::_buffer.data()));
TRACE("Transport::write_path_table: buffer size " + std::to_string(Persistence::_buffer.size()) + " bytes");
			//TRACE("SERIALIZED: destination_table");
			//TRACE(Persistence::_buffer.toString());
#endif
			// Check crc to see if data has changed before writing
			uint32_t crc = Crc::crc32(0, Persistence::_buffer.data(), Persistence::_buffer.size());
			if (_destination_table_crc > 0 && crc == _destination_table_crc) {
				TRACE("Transport::write_path_table: no change detected, skipping write");
			}
			else if (RNS::Utilities::OS::write_file(destination_table_path, Persistence::_buffer) == Persistence::_buffer.size()) {
				TRACEF("Transport::write_path_table: wrote %d entries, %d bytes", _destination_table.size(), Persistence::_buffer.size());
				_destination_table_crc = crc;
				success = true;

#ifndef NDEBUG
				// CBA DEBUG Dump path table
				//TRACE("FILE: destination_table");
				//if (OS::read_file("/destination_table", Persistence::_buffer) > 0) {
				//	TRACE(Persistence::_buffer.toString());
				//}
#endif
			}
			else {
				TRACE("Transport::write_path_table: write failed");
			}
		}
		else {
			TRACE("Transport::write_path_table: failed to serialize");
		}
#else	// CUSTOM
		uint32_t crc = Persistence::crc(persist_table);
		if (_destination_table_crc > 0 && crc == _destination_table_crc) {
			TRACE("Transport::write_path_table: no change detected, skipping write");
		}
		else {
			TRACE("Transport::write_path_table: change detected, writing...");
			char destination_table_path[Type::Reticulum::FILEPATH_MAXSIZE];
			snprintf(destination_table_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/destination_table", Reticulum::_storagepath);
			if (Persistence::serialize(persist_table, destination_table_path, _destination_table_crc) > 0) {
				TRACEF("Transport::write_path_table: wrote %d entries, %d bytes", persist_table.size(), Persistence::_buffer.size());
				success = true;
			}
		}
#endif	// CUSTOM

		if (success) {
			double save_time = OS::time() - save_start;
			if (save_time < 1.0) {
				//DEBUG("Saved " + std::to_string(_destination_table.size()) + " path table entries in " + std::to_string(OS::round(save_time * 1000, 1)) + " ms");
				DEBUGF("Saved %d path table entries in %d ms", _destination_table.size(), (int)(save_time*1000));
			}
			else {
				//DEBUG("Saved " + std::to_string(_destination_table.size()) + " path table entries in " + std::to_string(OS::round(save_time, 1)) + " s");
				DEBUGF("Saved %d path table entries in %d s", _destination_table.size(), save_time);
			}
		}
	}
	catch (std::exception& e) {
		ERRORF("Could not save path table to storage, the contained exception was: %s", e.what());
	}
#endif

	_saving_path_table = false;

	return success;
}

/*static*/ void Transport::read_tunnel_table() {
	DEBUG("Transport::read_tunnel_table");
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
// TODO

#endif
}

/*static*/ void Transport::write_tunnel_table() {
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
// TODO

#endif
}

/*static*/ void Transport::persist_data() {
	TRACE("Transport::persist_data()");
	write_packet_hashlist();
	write_path_table();
	write_tunnel_table();
}

/*static*/ void Transport::clean_caches() {
	TRACE("Transport::clean_caches()");
#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
	// CBA Remove cached packets no longer in path list
	std::list<std::string> files = OS::list_directory(Reticulum::_cachepath);
    for (auto& file : files) {
		TRACE("Transport::clean_caches: Checking for use of cached packet " + file);
		bool found = false;
		for (auto& [destination_hash, deque] : _destination_table) {
			for (auto& entry : deque) {
				if (file.compare(entry.packet_hash.toHex()) == 0) {
					found = true;
					break;
				}
			}
			if (found) break;
		}
		if (!found) {
			TRACE("Transport::clean_caches: No matching path found, removing cached packet " + file);
			char packet_cache_path[Type::Reticulum::FILEPATH_MAXSIZE];
			snprintf(packet_cache_path, Type::Reticulum::FILEPATH_MAXSIZE, "%s/%s", Reticulum::_cachepath, file.c_str());
			OS::remove_file(packet_cache_path);
		}
	}
#endif
}

/*static*/ void Transport::clear_caches_in_memory() {
	TRACE("Transport::clear_caches_in_memory()");

	// Clear the packet hashlist (duplicate detection, ~100 × 40 bytes = ~4KB)
	if (!_packet_hashlist.empty()) {
		size_t before = _packet_hashlist.size();
		_packet_hashlist.clear();
		DEBUGF("Transport::clear_caches_in_memory: cleared %d packet hashlist entries", before);
	}

	// Clear global anti-replay blobs (capped at 8 × ~80 bytes = ~640 bytes)
	if (!_global_blobs.empty()) {
		size_t before = _global_blobs.size();
		_global_blobs.clear();
		DEBUGF("Transport::clear_caches_in_memory: cleared %d global blobs", before);
	}

	// Clear announce rate table
	if (!_announce_rate_table.empty()) {
		size_t before = _announce_rate_table.size();
		_announce_rate_table.clear();
		DEBUGF("Transport::clear_caches_in_memory: cleared %d announce rate entries", before);
	}

	// Clear discovery path request tags
	if (!_discovery_pr_tags.empty()) {
		size_t before = _discovery_pr_tags.size();
		_discovery_pr_tags.clear();
		DEBUGF("Transport::clear_caches_in_memory: cleared %d discovery PR tags", before);
	}

	cull_path_table();
}

/*static*/ void Transport::dump_stats() {

	OS::dump_heap_stats();

	size_t mem_used = OS::heap_size() - OS::heap_available();
	size_t flash_used = OS::storage_size() - OS::storage_available();

	if (_last_memory == 0) {
		_last_memory = mem_used;
	}
	if (_last_flash == 0) {
		_last_flash = flash_used;
	}

	// memory
	// storage
	// _destinations
	// _destination_table
	// _reverse_table
	// _announce_table
	// _held_announces
	HEADF(LOG_VERBOSE, "heap: %u/%u (%u%%) [%+d] flash: %u/%u (%u%%) [%+d] paths: %u dsts: %u revr: %u annc: %u held: %u", mem_used, OS::heap_size(), (int)((double)mem_used / (double)OS::heap_size() * 100.0), mem_used - _last_memory, flash_used, OS::storage_size(), (int)((double)flash_used / (double)OS::storage_size() * 100.0), flash_used - _last_flash, _destination_table.size(), _destinations.size(), _reverse_table.size(), _announce_table.size(), _held_announces.size());

	// _path_requests
	// _discovery_path_requests
	// _pending_local_path_requests
	// _discovery_pr_tags
	// _control_destinations
	// _control_hashes
	VERBOSEF("preqs: %u dpreqs: %u ppreqs: %u dprt: %u cdsts: %u chshs: %u", _path_requests.size(), _discovery_path_requests.size(), _pending_local_path_requests.size(), _discovery_pr_tags.size(), _control_destinations.size(), _control_hashes.size());

	// _packet_hashlist
	// _receipts
	// _link_table
	// _pending_links
	// _active_links
	// _tunnels
	uint32_t destination_path_responses = 0;
	for (auto& [destination_hash, destination] : _destinations) {
		destination_path_responses += destination.path_responses().size();
	}
	uint32_t interface_announces = 0;
	for (auto& [interface_hash, interface] : _interfaces) {
		interface_announces += interface.announce_queue().size();
	}
	VERBOSEF("phl: %u rcp: %u lt: %u pl: %u al: %u tun: %u", _packet_hashlist.size(), _receipts.size(), _link_table.size(), _pending_links.size(), _active_links.size(), _tunnels.size());
	VERBOSEF("bla: %u bma: %u", _firewall_local_addresses.size(), _firewall_mentioned_addresses.size());
	VERBOSEF("pin: %u pout: %u padd: %u dpr: %u ikd: %u ia: %u\r\n", _packets_received, _packets_sent, _destinations_added, destination_path_responses, Identity::_known_destinations.size(), interface_announces);

	_last_memory = mem_used;
	_last_flash = flash_used;

}

/*static*/ void Transport::dump_whitelists() {
#ifdef FIREWALL_MODE
	Serial.printf("[WL#1] %u entries:", _firewall_local_addresses.size());
	for (auto& addr : _firewall_local_addresses) {
		Serial.printf(" %s", addr.toHex().substr(0,8).c_str());
	}
	Serial.printf("\r\n");
	Serial.printf("[WL#2] %u entries:", _firewall_mentioned_addresses.size());
	for (auto& addr : _firewall_mentioned_addresses) {
		Serial.printf(" %s", addr.toHex().substr(0,8).c_str());
	}
	Serial.printf("\r\n");
#endif
}

/*static*/ void Transport::exit_handler() {
	TRACE("Transport::exit_handler()");
	if (!_owner.is_connected_to_shared_instance()) {
		persist_data();
	}
}

/*static*/ Destination Transport::find_destination_from_hash(const Bytes& destination_hash) {
	TRACE("Transport::find_destination_from_hash: Searching for destination " + destination_hash.toHex());
#if defined(DESTINATIONS_SET)
	for (const Destination& destination : _destinations) {
		if (destination.get_hash() == destination_hash) {
			TRACE("Transport::find_destination_from_hash: Found destination " + destination.toString());
			return destination;
		}
	}
#elif defined(DESTINATIONS_MAP)
	auto iter = _destinations.find(destination_hash);
	if (iter != _destinations.end()) {
		TRACE("Transport::find_destination_from_hash: Found destination " + (*iter).second.toString());
		return (*iter).second;
	}
#endif

	return {Type::NONE};
}

/*static*/ void Transport::cull_path_table() {
	TRACE("Transport::cull_path_table()");
	double now = OS::time();

	// Pass 1: Remove individual expired entries from each deque
	std::vector<Bytes> empty_dests;
	for (auto& [dest_hash, deque] : _destination_table) {
		deque.erase(std::remove_if(deque.begin(), deque.end(),
			[now](const PathEntry& e) { return e.is_expired(now); }),
			deque.end());
		if (deque.empty()) {
			empty_dests.push_back(dest_hash);
		}
	}
	// Remove empty destination entries
	for (const auto& dest_hash : empty_dests) {
		_destination_table.erase(dest_hash);
	}

	// Pass 2: If still over maxsize, evict destinations with the lowest-score best path
	if (_destination_table.size() > _path_table_maxsize) {
		// Build sorted list: (dest_hash, best_score) ascending
		std::vector<std::pair<Bytes, double>> scored;
		for (auto& [dest_hash, deque] : _destination_table) {
			double best_score = -1.0;
			for (const auto& entry : deque) {
				if (entry.is_expired(now)) continue;
				Interface iface = find_interface_from_hash(entry.receiving_interface);
				double s = entry.score(iface ? iface.bitrate() : 0);
				if (s > best_score) best_score = s;
			}
			if (best_score < 0) best_score = 0; // all expired, score 0
			scored.push_back({dest_hash, best_score});
		}
		std::sort(scored.begin(), scored.end(),
			[](const std::pair<Bytes, double>& a, const std::pair<Bytes, double>& b) { return a.second < b.second; });

		uint16_t count = 0;
		for (const auto& [dest_hash, score] : scored) {
			if (_destination_table.size() <= _path_table_maxsize) break;
			TRACE("Transport::cull_path_table: Removing destination " + dest_hash.toHex() + " from path table (score=" + std::to_string(score) + ")");
			_destination_table.erase(dest_hash);
			++count;
		}
		if (count > 0) {
			DEBUG("Removed " + std::to_string(count) + " path(s) from path table");
		}
	}
}
	
/*static*/ uint16_t Transport::remove_reverse_entries(const std::vector<Bytes>& hashes) {
	uint16_t count = 0;
	for (const auto& truncated_packet_hash : hashes) {
		flatmap_erase(_reverse_table, truncated_packet_hash);
		++count;
	}
	if (count > 0) {
		TRACEF("Released %u reverse table entries", count);
	}
	return count;
}

/*static*/ uint16_t Transport::remove_links(const std::vector<Bytes>& hashes) {
	uint16_t count = 0;
	for (const auto& link_id : hashes) {
		_link_table.erase(link_id);
		++count;
	}
	if (count > 0) {
		TRACEF("Released %u links", count);
	}
	return count;
}

/*static*/ uint16_t Transport::remove_paths(const std::vector<Bytes>& hashes) {
	uint16_t count = 0;
	for (const auto& destination_hash : hashes) {
		//_destination_table.erase(destination_hash);
		remove_path(destination_hash);
		++count;
	}
	if (count > 0) {
		TRACEF("Released %u paths", count);
	}
	return count;
}

/*static*/ uint16_t Transport::remove_discovery_path_requests(const std::vector<Bytes>& hashes) {
	uint16_t count = 0;
	for (const auto& destination_hash : hashes) {
		flatmap_erase(_discovery_path_requests, destination_hash);
		++count;
		// Also clean up _discovery_pr_tags so future path requests
		// for this destination are not permanently suppressed.
		// unique_tag = destination_hash (16B) + tag_bytes (1-16B)
		for (auto it = _discovery_pr_tags.begin(); it != _discovery_pr_tags.end(); ) {
			if (it->size() >= destination_hash.size() && it->left(destination_hash.size()) == destination_hash) {
				it = _discovery_pr_tags.erase(it);
			} else {
				++it;
			}
		}
	}
	if (count > 0) {
		TRACEF("Released %u waiting path requests", count);
	}
	return count;
}

/*static*/ uint16_t Transport::remove_tunnels(const std::vector<Bytes>& hashes) {
	uint16_t count = 0;
	for (const auto& tunnel_id : hashes) {
		_tunnels.erase(tunnel_id);
		++count;
	}
	if (count > 0) {
		TRACEF("Released %u tunnels", count);
	}
	return count;
}
