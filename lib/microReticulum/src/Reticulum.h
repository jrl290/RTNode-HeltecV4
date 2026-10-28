// microReticulum, by Chad Attermann, Apache-2.0 (lib/microReticulum/LICENSE).
// Changed for RTNode (https://github.com/jrl290/RTNode-HeltecV4); the changes
// are in RTNode's git history.

#pragma once

#include "Transport.h"
#include "Log.h"
#include "Type.h"
#include "Utilities/OS.h"

#include <vector>
#include <map>
#include <string>
#include <memory>
#include <cassert>
#include <stdint.h>

namespace RNS {

	// IFAC salt used for key derivation: a protocol constant, the same on every
	// Reticulum node, or IFAC keys would not match.
	static const uint8_t IFAC_SALT[] = {
		0xad, 0xf5, 0x4d, 0x88, 0x2c, 0x9a, 0x9b, 0x80,
		0x77, 0x1e, 0xb4, 0x99, 0x5d, 0x70, 0x2d, 0x4a,
		0x3e, 0x73, 0x33, 0x91, 0xb2, 0xa0, 0xf5, 0x3f,
		0x41, 0x6d, 0x9f, 0x90, 0x7e, 0x55, 0xcf, 0xf8
	};
	static const size_t IFAC_SALT_SIZE = sizeof(IFAC_SALT);

	class Reticulum {

	public:


		// The default configuration path will be expanded to a directory
		// named ".reticulum" inside the current users home directory
		//static std::string _storagepath;
		static char _storagepath[Type::Reticulum::FILEPATH_MAXSIZE];
		//static std::string _cachepath;
		static char _cachepath[Type::Reticulum::FILEPATH_MAXSIZE];

		static const Reticulum& _instance;

		static bool __transport_enabled;
        static bool __link_mtu_discovery;
        static bool __remote_management_enabled;
		static bool __use_implicit_proof;
		static bool __allow_probes;
		static bool panic_on_interface_error;

	public:
		// Return the currently running Reticulum instance
		inline static const Reticulum& get_instance() { return _instance; }

	public:
		Reticulum();
		Reticulum(Type::NoneConstructor none) {
			MEM("Reticulum empty object created, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
		}
		Reticulum(const Reticulum& reticulum) : _object(reticulum._object) {
			MEM("Reticulum object copy created, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
		}
		virtual ~Reticulum() {
			MEM("Reticulum object destroyed, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
		}

		inline Reticulum& operator = (const Reticulum& reticulum) {
			_object = reticulum._object;
			MEM("Reticulum object copy created by assignment, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
			return *this;
		}
		inline operator bool() const {
			return _object.get() != nullptr;
		}
		inline bool operator < (const Reticulum& reticulum) const {
			return _object.get() < reticulum._object.get();
		}

	public:
		void start();
		void loop();
		void jobs();
		void should_persist_data();
		void persist_data();
		void persist_time_offset();
		void clean_caches();
		void clear_caches();
		//void __create_default_config();
		//void rpc_loop();
		//void get_interface_stats() const;
		const std::map<Bytes, std::deque<Transport::PathEntry>>& get_path_table() const;
		const std::vector<std::pair<Bytes, Transport::RateEntry>>& get_rate_table() const;
		bool drop_path(const Bytes& destination);
		uint16_t drop_all_via(const Bytes& transport_hash);
		void drop_announce_queues();
		const std::string get_next_hop_if_name(const Bytes& destination) const;
		double get_first_hop_timeout(const Bytes& destination) const;
		const Bytes get_next_hop(const Bytes& destination) const;
		size_t get_link_count() const;
		//void get_packet_rssi(const Bytes& packet_hash) const;
		//void get_packet_snr(const Bytes& packet_hash) const;
		//void get_packet_q(const Bytes& packet_hash) const;

		/*
		Returns whether proofs sent are explicit or implicit.

		:returns: True if the current running configuration specifies to use implicit proofs. False if not.
		*/
		inline static bool should_use_implicit_proof() { return __use_implicit_proof; }

		/*
		Returns whether Transport is enabled for the running
		instance.

		When Transport is enabled, Reticulum will
		route traffic for other peers, respond to path requests
		and pass announces over the network.

		:returns: True if Transport is enabled, False if not.
		*/
		inline static bool transport_enabled() { return __transport_enabled; }
		inline static void transport_enabled(bool transport_enabled) { __transport_enabled = transport_enabled; }

		/*
		Returns whether link MTU discovery is enabled for the running
		instance.

		When link MTU discovery is enabled, Reticulum will
		automatically upgrade link MTUs to the highest supported
		value, increasing transfer speed and efficiency.

		:returns: True if link MTU discovery is enabled, False if not.
		*/
		inline static bool link_mtu_discovery() { return __link_mtu_discovery; }
		inline static void link_mtu_discovery(bool link_mtu_discovery) { __link_mtu_discovery = link_mtu_discovery; }

		/*
		Returns whether remote management is enabled for the
		running instance.

		When remote management is enabled, authenticated peers
		can remotely query and manage this instance.

		:returns: True if remote management is enabled, False if not.
		*/
		inline static bool remote_management_enabled() { return __remote_management_enabled; }
		inline static void remote_management_enabled(bool remote_management_enabled) { __remote_management_enabled = remote_management_enabled; }

		inline static bool probe_destination_enabled() { return __allow_probes; }
		inline static void probe_destination_enabled(bool allow_probes) { __allow_probes = allow_probes; }

		// getters/setters
		inline bool is_connected_to_shared_instance() const { assert(_object); return _object->_is_connected_to_shared_instance; }

	private:
		class Object {
		public:
			Object() {
				MEM("Reticulum data object created, this: " + std::to_string((uintptr_t)this));
			}
			virtual ~Object() {
				MEM("Reticulum data object destroyed, this: " + std::to_string((uintptr_t)this));
			}
		private:

			uint16_t _local_interface_port = 37428;
			uint16_t _local_control_port   = 37429;
			bool _share_instance       = true;


			bool _is_shared_instance = false;
			bool _is_connected_to_shared_instance = false;
			bool _is_standalone_instance = false;
			double _last_data_persist = Utilities::OS::time();
			bool _persisted_once = false;     // first save waits FIRST_PERSIST_DELAY
			double _last_time_persist = Utilities::OS::time();
			double _last_cache_clean = 0.0;

			// CBA
			double _jobs_last_run = Utilities::OS::time();

		friend class Reticulum;
		};
		std::shared_ptr<Object> _object;

	};

}
