// microReticulum, by Chad Attermann, Apache-2.0 (lib/microReticulum/LICENSE).
// Changed for RTNode (https://github.com/jrl290/RTNode-HeltecV4); the changes
// are in RTNode's git history.

#include "Reticulum.h"

#include "Transport.h"
#include "Log.h"

//#include <TransistorNoiseSource.h>
#include <RNG.h>

#ifdef ARDUINO
#include <Arduino.h>
//#include <TransistorNoiseSource.h>
#endif

using namespace RNS;
using namespace RNS::Type::Reticulum;
using namespace RNS::Utilities;

/*static*/ //std::string Reticulum::_storagepath;
/*static*/ char Reticulum::_storagepath[FILEPATH_MAXSIZE];
/*static*/ //std::string Reticulum::_cachepath;
/*static*/ char Reticulum::_cachepath[FILEPATH_MAXSIZE];

/*static*/ const Reticulum& Reticulum::_instance = {Type::NONE};

/*static*/ bool Reticulum::__transport_enabled = false;
/*static*/ bool Reticulum::__link_mtu_discovery = RNS::Type::Reticulum::LINK_MTU_DISCOVERY;
/*static*/ bool Reticulum::__remote_management_enabled = false;
/*static*/ bool Reticulum::__use_implicit_proof = true;
/*static*/ bool Reticulum::__allow_probes = false;
/*static*/ bool Reticulum::panic_on_interface_error = false;

#ifdef ARDUINO
// Noise source to seed the random number generator.
//TransistorNoiseSource noise(A1);
#endif

/*
Initialises and starts a Reticulum instance. This must be
done before any other operations, and Reticulum will not
pass any traffic before being instantiated.

:param configdir: Full path to a Reticulum configuration directory.
*/


//def __init__(self,configdir=None, loglevel=None, logdest=None, verbosity=None):
Reticulum::Reticulum() : _object(new Object()) {
	MEM("Reticulum default object creating..., this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));

	// Initialize random number generator
	TRACE("Initializing RNG...");
	RNG.begin("Reticulum");
	TRACE("RNG initial random value: " + std::to_string(Cryptography::randomnum()));

#ifdef ARDUINO
	// Add a noise source to the list of sources known to RNG.
	//RNG.addNoiseSource(noise);
 #endif



// CBA TEST
#ifdef ARDUINO
	//_storagepath = "";
	strncpy(_storagepath, "", FILEPATH_MAXSIZE);
	//_cachepath = "/cache";
	strncpy(_cachepath, "/cache", FILEPATH_MAXSIZE);
#else
	//_storagepath = ".";
	strncpy(_storagepath, ".", FILEPATH_MAXSIZE);
	//_cachepath = "./cache";
	strncpy(_cachepath, "./cache", FILEPATH_MAXSIZE);
#endif

#ifdef ARDUINO
#if defined(RNS_USE_FS)
	// load time offset from file if it exists
	try {
		char time_offset_path[FILEPATH_MAXSIZE];
		snprintf(time_offset_path, FILEPATH_MAXSIZE, "%s/time_offset", _storagepath);
		if (OS::file_exists(time_offset_path)) {
			Bytes buf;
			if (OS::read_file(time_offset_path, buf) == 8) {
				uint64_t offset = *(uint64_t*)buf.data();
				DEBUG("Read time offset of " + std::to_string(offset) + " from file");
				OS::setTimeOffset(offset);
			}
		}
	}
	catch (std::exception& e) {
		ERRORF("Failed to load time offset, the contained exception was: %s", e.what());
	}
#endif
#endif

	// Initialize time-based variables *after* time offset update
	_object->_last_data_persist = OS::time();
	_object->_last_time_persist = OS::time();
	_object->_last_cache_clean = 0.0;
	_object->_jobs_last_run = OS::time();


	// CBA Moved to start() so Transport is not started  until after interfaces are setup
	//Transport::start(*this);


	MEM("Reticulum default object created, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_object.get()));
}

void Reticulum::start() {

	INFO("Total memory: " + std::to_string(OS::heap_size()));
	INFO("Total flash: " + std::to_string(OS::storage_size()));

	INFO("Starting Transport...");
	Transport::start(*this);
}

void Reticulum::loop() {
	assert(_object);
	if (!_object->_is_connected_to_shared_instance) {

		// One [RLOOP] line when a pass takes 100 ms or more, by part.
		const uint32_t t0 = micros();

		// Perform Reticulum housekeeping
		if (OS::time() > (_object->_jobs_last_run + JOB_INTERVAL)) {
			jobs();
			_object->_jobs_last_run = OS::time();
		}
		const uint32_t t_jobs = micros();

		// Perform interface processing
		for (auto& [hash, interface] : Transport::get_interfaces()) {
			interface.loop();
		}
		const uint32_t t_ifaces = micros();

		// Perform Filesystem processing
		FileSystem& filesystem = OS::get_filesystem();
		if (filesystem) {
			filesystem.loop();
		}
		const uint32_t t_fs = micros();

		// Perform Transport processing
		RNS::Transport::loop();
		const uint32_t t_end = micros();
		if (t_end - t0 >= 100000) {
			Serial.printf("[RLOOP] t=%lu %lums jobs=%lu interfaces=%lu fs=%lu transport=%lu\r\n",
			              (unsigned long)millis(), (unsigned long)((t_end - t0) / 1000),
			              (unsigned long)((t_jobs - t0) / 1000), (unsigned long)((t_ifaces - t_jobs) / 1000),
			              (unsigned long)((t_fs - t_ifaces) / 1000), (unsigned long)((t_end - t_fs) / 1000));
		}
	}
	// Perform random number gnerator housekeeping
	RNG.loop();
}

void Reticulum::jobs() {

	double now = OS::time();

#if 1
	// CBA Detect low-memory condition and reset
	if (OS::heap_size() > 0) {
		uint8_t remaining = (uint8_t)((double)OS::heap_available() / (double)OS::heap_size() * 100.0);
		if (remaining <= 2) {
			head("DETECTED LOW-MEMORY CONDITION (" + std::to_string(remaining) + "%), RESETTING!!!", LOG_CRITICAL);
			persist_data();
#if defined(ESP32)
			ESP.restart();
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
			//dbgDumpMemory();
			NVIC_SystemReset();
#endif
		}
	}
#endif

	// One [RJOBS] line when either takes 50 ms or more.
	uint32_t clean_us = 0, persist_us = 0;
	if (now > _object->_last_cache_clean + CLEAN_INTERVAL) {
		uint32_t start = micros();
		clean_caches();
		clean_us = micros() - start;
		_object->_last_cache_clean = OS::time();
	}

	if (now > _object->_last_data_persist + (_object->_persisted_once ? PERSIST_INTERVAL : FIRST_PERSIST_DELAY)) {
		uint32_t start = micros();
		persist_data();
		persist_us = micros() - start;
	}
	else if (now > _object->_last_time_persist + TIME_PERSIST_INTERVAL) {
		persist_time_offset();
	}
	if (clean_us >= 50000 || persist_us >= 50000) {
		Serial.printf("[RJOBS] t=%lu clean=%lums persist=%lums\r\n", (unsigned long)millis(),
		              (unsigned long)(clean_us / 1000), (unsigned long)(persist_us / 1000));
	}
}

// CBA TODO
/*
void Reticulum::start_local_interface() {
}

void Reticulum::apply_config() {
}

void Reticulum::_add_interface(self,interface, mode = None, configured_bitrate=None, ifac_size=None, ifac_netname=None, ifac_netkey=None, announce_cap=None, announce_rate_target=None, announce_rate_grace=None, announce_rate_penalty=None):
*/

void Reticulum::should_persist_data() {
	if (OS::time() > _object->_last_data_persist + GRACIOUS_PERSIST_INTERVAL) {
		persist_data();
	}
}

void Reticulum::persist_data() {
	TRACE("Persisting transport and identity data...");
	Transport::persist_data();
	Identity::persist_data();
	persist_time_offset();

	_object->_last_data_persist = OS::time();
	_object->_persisted_once = true;
}

void Reticulum::persist_time_offset() {
#ifdef ARDUINO
#if defined(RNS_USE_FS)
	// write time offset to file
	try {
		char time_offset_path[FILEPATH_MAXSIZE];
		snprintf(time_offset_path, FILEPATH_MAXSIZE, "%s/time_offset", _storagepath);
		uint64_t offset = OS::ltime();
		DEBUGF("Writing time offset of %llu to file %s", offset, time_offset_path);
		Bytes buf((uint8_t*)&offset, sizeof(offset));
		OS::write_file(time_offset_path, buf);
	}
	catch (std::exception& e) {
		ERRORF("Failed to write time offset, the contained exception was: %s", e.what());
	}
#endif
#endif
	_object->_last_time_persist = OS::time();
}

void Reticulum::clean_caches() {
	TRACE("Cleaning resource and packet caches...");
	double now = OS::time();

#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)
/*
	// Clean resource caches
	for (auto& filename : OS::list_directory(resourcepath) {
		try {
			if (filename.length() == (Type::Identity::HASHLENGTH//8)*2) {
				char filepath[FILEPATH_MAXSIZE];
				snprintf(filepath, FILEPATH_MAXSIZE, "%s/%s", _resourcepath, filename.c_str());
			}
		}
		catch (std::exception& e) {
			ERROR("Error while cleaning resources cache, the contained exception was: %s", e.what());
		}
	}

	// Clean packet caches
	for (auto& filename : OS::list_directory(_cachepath.c_str())) {
		try {
			if (filename.length() == (Type::Identity::HASHLENGTH/8)*2) {
				char filepath[FILEPATH_MAXSIZE];
				snprintf(filepath, FILEPATH_MAXSIZE, "%s/%s", _cachepath, filename.c_str());
			}
		}
		catch (std::exception& e) {
			ERROR("Error while cleaning packet cache, the contained exception was: %s", e.what());
		}
	}
*/

	Transport::clean_caches();

	// CBA
	Identity::cull_known_destinations();
#endif

}

void Reticulum::clear_caches() {
	TRACE("Clearing resource and packet caches...");

	try {
		char destination_table_path[FILEPATH_MAXSIZE];
		snprintf(destination_table_path, FILEPATH_MAXSIZE, "%s/destination_table", _storagepath);
		OS::remove_file(destination_table_path);

		OS::remove_directory(_cachepath);

#ifdef ARDUINO
		char time_offset_path[FILEPATH_MAXSIZE];
		snprintf(time_offset_path, FILEPATH_MAXSIZE, "%s/time_offset", _storagepath);
		OS::remove_file(time_offset_path);
#endif
	}
	catch (std::exception& e) {
		ERRORF("Failed to clear cache file(s), the contained exception was: %s", e.what());
	}
}


const std::map<Bytes, std::deque<Transport::PathEntry>>& Reticulum::get_path_table() const {
/*
	path_table = []
	for dst_hash in Transport::destination_table:
		entry = {
			"hash": dst_hash,
			"timestamp": Transport::destination_table[dst_hash][0],
			"via": Transport::destination_table[dst_hash][1],
			"hops": Transport::destination_table[dst_hash][2],
			"expires": Transport::destination_table[dst_hash][3],
			"interface": str(Transport::destination_table[dst_hash][5]),
		}
		path_table.append(entry)

	return path_table
*/
	return Transport::get_destination_table();
}

const std::vector<std::pair<Bytes, Transport::RateEntry>>& Reticulum::get_rate_table() const {
/*
	rate_table = []
	for dst_hash in Transport::announce_rate_table:
		entry = {
			"hash": dst_hash,
			"last": Transport::announce_rate_table[dst_hash]["last"],
			"rate_violations": Transport::announce_rate_table[dst_hash]["rate_violations"],
			"blocked_until": Transport::announce_rate_table[dst_hash]["blocked_until"],
			"timestamps": Transport::announce_rate_table[dst_hash]["timestamps"],
		}
		rate_table.append(entry)

	return rate_table
*/
	return Transport::get_announce_rate_table();
}

bool Reticulum::drop_path(const Bytes& destination) {
	return Transport::expire_path(destination);
}

uint16_t Reticulum::drop_all_via(const Bytes& transport_hash) {
	uint16_t dropped_count = 0;
	for (const auto& [destination_hash, deque] : Transport::get_destination_table()) {
		for (const auto& entry : deque) {
			if (entry.next_hop == transport_hash) {
				Transport::expire_path(destination_hash);
				++dropped_count;
				break;
			}
		}
	}
	return dropped_count;
}

void Reticulum::drop_announce_queues() {
	Transport::drop_announce_queues();
}

const std::string Reticulum::get_next_hop_if_name(const Bytes& destination) const {
	return Transport::next_hop_interface(destination).name();
}

double Reticulum::get_first_hop_timeout(const Bytes& destination) const {
	return Transport::first_hop_timeout(destination);
}

const Bytes Reticulum::get_next_hop(const Bytes& destination) const {
	return Transport::next_hop(destination);
}

size_t Reticulum::get_link_count() const {
	return Transport::get_link_table().size();
}


