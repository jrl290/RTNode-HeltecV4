// microReticulum, by Chad Attermann, Apache-2.0 (lib/microReticulum/LICENSE).
// Changed for RTNode (https://github.com/jrl290/RTNode-HeltecV4); the changes
// are in RTNode's git history.

#include "OS.h"

#include "../Type.h"
#include "../Log.h"

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

// ESP-IDF's own heap is built on TLSF and exports the same tlsf_* symbols
// with a different API (IDF 4.3+: tlsf_size(tlsf_t), a three-argument
// tlsf_create_with_pool(), ...). On ESP32 the linker resolves this library's
// TLSF calls to IDF's functions, so a private pool can never work there —
// it silently handed out nothing. ESP32 allocates through heap_caps instead
// (operator new below); the private pool remains for other platforms.
#if defined(RNS_USE_TLSF) && !defined(ESP32)
#define RNS_TLSF_POOL
#endif

using namespace RNS;
using namespace RNS::Utilities;


#if defined(RNS_USE_ALLOCATOR)

#if defined(RNS_USE_TLSF)
#if defined(ESP32)
	//#define BUFFER_SIZE 1024 * 80
	#define BUFFER_SIZE 0
	#define BUFFER_FRACTION 0.8
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	//#define BUFFER_SIZE 1024 * 80
	#define BUFFER_SIZE 0
	#define BUFFER_FRACTION 0.8
#else
	#define BUFFER_SIZE 1024 * 1000
	#define BUFFER_FRACTION 0
#endif

bool _tlsf_init = false;
//char _tlsf_msg[256] = "";
size_t _buffer_size = BUFFER_SIZE;
size_t _contiguous_size = 0;

// The pool's address range. operator delete hands a block to tlsf_free() only
// when it lies inside, so blocks malloc()ed before the pool existed (or after
// it filled) still go back to free().
uintptr_t _tlsf_pool_start = 0;
uintptr_t _tlsf_pool_end = 0;


/*static*/ //tlsf_t OS::_tlsf = tlsf_create_with_pool(malloc(1024 * 1024), 1024 * 1024);
/*static*/ tlsf_t OS::_tlsf = nullptr;
#endif

#if defined(ESP32)
// Set by OS::init_heap() once the Arduino core has brought PSRAM up (it does
// so after C++ static constructors run). From then on operator new places
// objects in PSRAM, keeping internal RAM for WiFi, lwIP, DMA and stacks.
bool _new_in_psram = false;
#endif

uint32_t _new_count = 0;
uint32_t _new_fault = 0;
uint64_t _new_size = 0;
uint32_t _delete_count = 0;
uint32_t _delete_fault = 0;
size_t _min_size = 0;
size_t _max_size = 0;

#if defined(RNS_TLSF_POOL)
static void tlsf_create_pool() {
#if defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	_contiguous_size = dbgHeapFree();
	TRACEF("contiguous_size: %u", _contiguous_size);
	if (_buffer_size == 0) {
		_buffer_size = (size_t)(_contiguous_size * BUFFER_FRACTION);
	}
	// For NRF52 round to kB
	_buffer_size = (size_t)(_buffer_size / 1024) * 1024;
	TRACEF("buffer_size: %u", _buffer_size);
	void* raw_buffer = malloc(_buffer_size);
#else
	_buffer_size = (size_t)BUFFER_SIZE;
	TRACEF("buffer_size: %u", _buffer_size);
	void* raw_buffer = malloc(_buffer_size);
#endif
	if (raw_buffer == nullptr) {
		ERROR("-- allocation for tlsf FAILED");
		//strcpy(_tlsf_msg, "-- allocation for tlsf FAILED!!!");
	}
	else {
#if 1
		OS::_tlsf = tlsf_create_with_pool(raw_buffer, _buffer_size);
		//if (OS::_tlsf == nullptr) {
		//	sprintf(_tlsf_msg, "initialization of tlsf with align=%d, contiguous=%d, size=%d FAILED!!!", tlsf_align_size(), _contiguous_size, _buffer_size);
		//}
		//else {
		//	sprintf(_tlsf_msg, "initialization of tlsf with align=%d, contiguous=%d, size=%d SUCCESSFUL!!!", tlsf_align_size(), _contiguous_size, _buffer_size);
		//}
#else
		Serial.print("raw_buffer: ");
		Serial.println((long)raw_buffer);
		Serial.print("align_size: ");
		Serial.println((long)tlsf_align_size());
		void* aligned_buffer = (void*)(((size_t)raw_buffer + (tlsf_align_size() - 1)) & ~(tlsf_align_size() - 1));
		Serial.print("aligned_buffer: ");
		Serial.println((long)aligned_buffer);
		OS::_tlsf = tlsf_create_with_pool(aligned_buffer, BUFFER_SIZE-(size_t)((uint32_t)aligned_buffer - (uint32_t)raw_buffer));
		//tlfs = tlsf_create_with_pool(aligned_buffer, buffer_size--(size_t)((uint32_t)aligned_buffer - (uint32_t)raw_buffer));
#endif
		if (OS::_tlsf == nullptr) {
			ERROR("-- initialization of tlsf FAILED");
		}
	}
	if (OS::_tlsf != nullptr) {
		_tlsf_pool_start = (uintptr_t)raw_buffer;
		_tlsf_pool_end = _tlsf_pool_start + _buffer_size;
	}
}
#endif

/*static*/ void OS::init_heap() {
#if defined(ESP32)
	_new_in_psram = ESP.getPsramSize() > 0;
#endif
#ifdef RTNODE_BENCH_NO_PSRAM
	// Bench only: run as a board whose PSRAM did not start (a V4 with octal
	// PSRAM under the quad build, issue #44).
	_new_in_psram = false;
#endif
}

// CBA Added attribute weak to avoid collision with new override on nrf52
void* operator new(size_t size) {
//__attribute__((weak)) void* operator new(size_t size) {
#if defined(RNS_TLSF_POOL)
	//if (OS::_tlsf == nullptr) {
	if (!_tlsf_init) {
		_tlsf_init = true;
		tlsf_create_pool();
	}
#endif
	++_new_count;
	_new_size += size;
	if (size < _min_size || _min_size == 0) {
		_min_size = size;
	}
	//if (size > _max_size) {
	if (size < 4192 && size > _max_size) {
		_max_size = size;
	}
	void* p = nullptr;
#if defined(ESP32)
	if (_new_in_psram) {
		p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	}
	if (p == nullptr) {
		// Before OS::init_heap(), on boards without PSRAM, or with PSRAM full.
		p = malloc(size);
		++_new_fault;
	}
#elif defined(RNS_TLSF_POOL)
	if (OS::_tlsf != nullptr) {
		p = tlsf_malloc(OS::_tlsf, size);
	}
	if (p == nullptr) {
		// No pool on this board, or the pool is full.
		p = malloc(size);
		++_new_fault;
	}
#else
	//TRACEF("--- allocating memory (%u bytes)", size);
	p = malloc(size);
	//TRACEF("--- allocated memory (%u bytes) (addr=%lx)", size, p);
#endif
	return p;
}
 
// CBA Added attribute weak to avoid collision with new override on nrf52
void operator delete(void* p) {
//__attribute__((weak)) void operator delete(void* p) {
#if defined(ESP32)
	// heap_caps_malloc() and malloc() blocks both go back through free().
	free(p);
#elif defined(RNS_TLSF_POOL)
	uintptr_t address = (uintptr_t)p;
	if (OS::_tlsf != nullptr && address >= _tlsf_pool_start && address < _tlsf_pool_end) {
		tlsf_free(OS::_tlsf, p);
	}
	else {
		//TRACEF("--- freeing memory (addr=%lx)", p);
		free(p);
		++_delete_fault;
	}
#else
	//TRACEF("--- freeing memory (addr=%lx)", p);
	//TRACE("--- freeing memory");
	free(p);
#endif
	++_delete_count;
#if defined(RNS_USE_TLSF)
	//if (_delete_count == _new_count) {
	//	TRACE("TLFS deinitializing");
	//	OS::dump_memory_stats();
	//	tlsf_destroy(OS::_tlsf);
	//	OS::_tlsf = nullptr;
	//}
#endif
}

#if defined(RNS_TLSF_POOL)
uint32_t _tlsf_used_count = 0;
uint32_t _tlsf_used_size = 0;
uint32_t _tlsf_free_count = 0;
uint32_t _tlsf_free_size = 0;
uint32_t _tlsf_free_max_size = 0;
void tlsf_mem_walker(void* ptr, size_t size, int used, void* user)
{
	if (used) {
		_tlsf_used_count++;
		_tlsf_used_size += size;
	}
	else {
		_tlsf_free_count++;
		_tlsf_free_size += size;
		if (size > _tlsf_free_max_size) {
			_tlsf_free_max_size = size;
		}
	}
}
void dump_tlsf_stats() {
	_tlsf_used_count = 0;
	_tlsf_used_size = 0;
	_tlsf_free_count = 0;
	_tlsf_free_size = 0;
	_tlsf_free_max_size = 0;
	//TRACEF("TLSF Message: %s", _tlsf_msg);
	// The walk visits every block, so only pay for it when the TRACE output
	// it feeds will actually be printed.
	if (OS::_tlsf == nullptr || loglevel() < LOG_TRACE) {
		return;
	}
	tlsf_walk_pool(tlsf_get_pool(OS::_tlsf), tlsf_mem_walker, nullptr);
	HEAD("TLSF Stats", LOG_TRACE);
	TRACEF("Buffer Size:     %u", _buffer_size);
	TRACEF("Contiguous Size: %u", _contiguous_size);
	TRACEF("Used Count:      %u", _tlsf_used_count);
	TRACEF("Used Size:       %u (%u%% used)", _tlsf_used_size, (unsigned)((double)_tlsf_used_size / (double)_buffer_size * 100.0));
	TRACEF("Free Count:      %u", _tlsf_free_count);
	TRACEF("Free Size:       %u (%u%% free)", _tlsf_free_size, (unsigned)((double)_tlsf_free_size / (double)_buffer_size * 100.0));
	TRACEF("Max Free Size:   %u (%u%% fragmented)\n", _tlsf_free_max_size, (unsigned)(100.0 - (double)_tlsf_free_max_size / (double)_tlsf_free_size * 100.0));
}
#endif

/*static*/ bool OS::heap_in_psram() {
#if defined(ESP32)
	return _new_in_psram;
#else
	return false;
#endif
}

/*static*/ uint32_t OS::heap_fallback_count() {
	return _new_fault;
}

/*static*/ void OS::dump_allocator_stats() {
	HEAD("Allocator Stats", LOG_TRACE);
	TRACEF("New Count:    %u", _new_count);
	TRACEF("New Fault:    %u", _new_fault);
	TRACEF("Delete Count: %u", _delete_count);
	TRACEF("Delete Fault: %u", _delete_fault);
	TRACEF("Active Count: %u", (_new_count - _delete_count));
	TRACEF("Min Size: %u", _min_size);
	TRACEF("Max Size: %u", _max_size);
	TRACEF("Avg Size: %u\n", (size_t)(_new_size / _new_count));
#if defined(RNS_TLSF_POOL)
	dump_tlsf_stats();
#endif
}

#else

/*static*/ void OS::init_heap() {}
/*static*/ bool OS::heap_in_psram() { return false; }
/*static*/ uint32_t OS::heap_fallback_count() { return 0; }

#endif	// RNS_USE_ALLOCATOR


size_t maxContiguousAllocation() {
	// Brute-force determine maximum allocation size
	//const size_t block_size = 256;
	const size_t block_size = 32;
	size_t block_count;
	for (block_count = 1; ; block_count++) {
		void* ptr = malloc(block_count * block_size);
		if (ptr == nullptr) {
			break;
		}
		free(ptr);
	}
	return (block_count - 1) * block_size;
}

/*static*/ FileSystem OS::_filesystem = {Type::NONE};
/*static*/ uint64_t OS::_time_offset = 0;

/*static*/ size_t OS::heap_size() {
#if defined(ESP32)
	return ESP.getHeapSize();
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	return dbgHeapTotal();
#else
	return 0;
#endif
}

/*static*/ size_t OS::heap_available() {
#if defined(ESP32)
	return ESP.getFreeHeap();
	//return ESP.getMaxAllocHeap();
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	return dbgHeapFree();
#else
	return 0;
#endif
}

/*static*/ void OS::dump_heap_stats() {
	HEAD("Heap Stats", LOG_TRACE);
#if defined(ESP32)
	TRACEF("Heap size:       %u", ESP.getHeapSize());
	TRACEF("Heap free:       %u (%u%% free)", ESP.getFreeHeap(), (unsigned)((double)ESP.getFreeHeap() / (double)ESP.getHeapSize() * 100.0));
	//TRACEF("Heap free:       %u (%u%% free)", xPortGetFreeHeapSize(), (unsigned)((double)xPortGetFreeHeapSize() / (double)xPort * 100.0));
	TRACEF("Heap min free:   %u", ESP.getMinFreeHeap());
	//TRACEF("Heap min free:   %u", xPortGetMinimumEverFreeHeapSize());
	TRACEF("Heap max alloc:  %u (%u%% fragmented)", ESP.getMaxAllocHeap(), (unsigned)(100.0 - (double)ESP.getMaxAllocHeap() / (double)ESP.getFreeHeap() * 100.0));
	//TRACEF("Heap max alloc:  %u (%u%% fragmented)", ESP.getMaxAllocHeap(), (unsigned)(100.0 - (double)ESP.getMaxAllocHeap() / (double)xPortGetFreeHeapSize() * 100.0));
	TRACEF("PSRAM size:      %u", ESP.getPsramSize());
	TRACEF("PSRAM free:      %u (%u%% free)", ESP.getFreePsram(), (ESP.getPsramSize() > 0) ? (unsigned)((double)ESP.getFreePsram() / (double)ESP.getPsramSize() * 100.0) : 0);
	TRACEF("PSRAM min free:  %u", ESP.getMinFreePsram());
	TRACEF("PSRAM max alloc: %u (%u%% fragmented)", ESP.getMaxAllocPsram(), (ESP.getFreePsram() > 0) ? (unsigned)(100.0 - (double)ESP.getMaxAllocPsram() / (double)ESP.getFreePsram() * 100.0) : 0);
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	if (loglevel() == LOG_TRACE) {
		dbgMemInfo();
	}
#endif
#if defined(RNS_USE_ALLOCATOR)
	OS::dump_allocator_stats();
#endif
}
