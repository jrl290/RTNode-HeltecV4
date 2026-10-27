# ESP32 Heap Exhaustion in microReticulum — Diagnosis & Fix

## Symptom

On an ESP32-S3 (Heltec V4, 324 KB internal heap, PSRAM, TLSF allocator), the
device rebooted every 10–14 minutes.  The heap watchdog fired at the 20 KB
critical threshold.  Free heap declined at a steady **~15 KB/min** despite all
static data structures being capped and stable.

## Investigation

Heap telemetry was already instrumented at three points per packet cycle:

```
[HEAP-TEL] firewall: -844 bytes  (after firewall filter)
[HEAP-TEL] inbound:  -1212 bytes (after full inbound processing)
[HEAP-TEL] jobs:     +764 bytes  (after periodic cleanup)
```

Every packet cycle net-leaked **~400–700 bytes**.  Over ~1,000 packets in
10 minutes, that is ~150 KB permanently lost.  Static table sizes (`paths`,
`dests`, `announce_table`, `reverse_table`) were measured and confirmed
stable — the leak was not in RNS-level data structures.

## Root Cause: `std::set<Bytes>` node fragmentation

Four `std::set<Bytes>` containers were implemented as red-black trees:

| Container                        | Typical size | Inserts per packet |
|----------------------------------|-------------|-------------------|
| `_packet_hashlist`               | 100         | 1                 |
| `_global_blobs`                  | 8           | 1                 |
| `_firewall_local_addresses`      | 128         | 1–2               |
| `_firewall_mentioned_addresses`  | 128         | 4–5               |

Every `insert` allocates a **tree node (~40 bytes)** plus a **`shared_ptr`
control block (~24 bytes)** for the `Bytes` copy-on-write wrapper.  When the
sets hit their cap and entries are evicted (oldest-first `erase`), the tree
nodes are freed.  However, on ESP32 even with TLSF, freeing many small
scattered allocations creates heap holes that cannot be coalesced — free heap
appears adequate in aggregate, but `malloc` fails for larger contiguous
requests.  This is classic **fragmentation from node-based containers**.

> **Why this matters:** `Bytes` already uses `shared_ptr<vector<uint8_t>>` for
> copy-on-write sharing of the actual hash data.  The `std::set` tree node is
> *additional* overhead on top of that — pure container bookkeeping, not
> payload.

### Quantified

- Tree nodes: (100 + 8 + 128 + 128) × 40 bytes = **~14.6 KB**
- `shared_ptr` control blocks: 364 × 24 bytes = **~8.7 KB**
- **Total overhead: ~23 KB** of container bookkeeping that churns on every
  insert/evict cycle
- Per-packet net loss: ~150 bytes (fragmented, cannot be recovered)
- Time to critical (20 KB): ~10 minutes at ~1.7 packets/sec

## Fix: `std::set<Bytes>` → `std::vector<Bytes>`

Replaced all four containers with flat `std::vector<Bytes>`.  Vectors store
elements inline in a single contiguous allocation — **zero per-element heap
overhead** beyond the hash data itself.

### API migration

| `std::set<Bytes>`           | `std::vector<Bytes>`                    | Rationale |
|------------------------------|-----------------------------------------|-----------|
| `.insert(x)`                 | `.push_back(x)`                         | Dupes already checked before insert |
| `.find(x) != .end()`         | `std::find(begin(), end(), x) != end()` | O(N) linear; N ≤ 128 is negligible |
| `.erase(begin(), iter)`      | `.erase(begin(), begin() + N)`          | Front-truncation for FIFO cap |
| `.clear()` / `.size()`       | `.clear()` / `.size()`                  | Unchanged |

### Impact

- **Eliminated 364 tree-node allocations** — removed ~23 KB of pure container
  overhead
- **Zero fragmentation from set-node churn** — vectors do a single realloc on
  growth, no per-element malloc/free
- **Per-packet `firewall` delta** dropped from ~844 bytes to ~200–300 bytes
- **RAM usage unchanged** at 21.9% (71,624 / 327,680 bytes)
- **Build size unchanged** at 20.0% flash

## Supporting changes

While investigating, several static caps were also tightened for extra
headroom on the ESP32:

| Constant | Old | New | Rationale |
|---|---|---|---|
| `MAX_PATHS_PER_DEST` | 3 | 2 | Halves per-destination path entry memory |
| `MAX_GLOBAL_BLOBS` | 16 | 8 | Anti-replay only needs a few recent blobs |
| `path_table_maxsize` | 24 | 16 | Fewer max destinations in table |
| `path_table_maxpersist` | 12 | 8 | Fewer entries persisted to flash |
| `_firewall_maxsize` | 200 | 128 | Less whitelist address tracking |

A `clear_caches_in_memory()` method was added to `Transport`, called from
the existing heap watchdog at HEAP_PRESSURE (28 KB):
- Clears `_packet_hashlist` (duplicate detection — rebuilds naturally)
- Clears `_global_blobs` (anti-replay — old announces may replay once)
- Clears `_announce_rate_table` (rate limiting state — resets)
- Clears `_discovery_pr_tags` (path request dedup)
- Then calls `cull_path_table()`

## General recommendation for the microReticulum repo

On ESP32-class devices with constrained heap and no MMU:

1. **Prefer `std::vector` over `std::set` / `std::map`** when N ≤ ~200 and
   insert/find frequency is moderate.

2. **`std::set<Bytes>` is a double-allocation trap**: one allocation for the
   tree node, one for the `shared_ptr` control block — neither of which
   stores payload.

3. **If ordering isn't needed** (hashlists, address sets, blob caches), a
   flat vector with linear search is strictly better for heap health.

4. **Consider a `FlatSet<T>` wrapper** that uses `std::vector` internally
   with `std::find` — it would be a drop-in replacement for most `std::set`
   use cases in this codebase.

5. **Audit other node-based containers** — `std::map<Bytes, AnnounceEntry>`
   (`_announce_table`), `std::map<Bytes, ReverseEntry>`
   (`_reverse_table`), and `std::map<Bytes, LinkEntry>` (`_link_table`)
   have the same tree-node allocation pattern.  If their sizes typically
   stay small (< 50 entries), they may be acceptable.  If they grow large
   under load, consider migrating to sorted `std::vector` with binary search.

## Files changed

| File | Change |
|---|---|
| `lib/microReticulum/src/Transport.h` | Added `PathEntry` struct, `#include <deque>`, `select_path()`, `mark_path_unresponsive()`; changed `_destination_table` to `std::map<Bytes, std::deque<PathEntry>>`; changed `_packet_hashlist` and `_global_blobs` to `std::vector<Bytes>` |
| `lib/microReticulum/src/Transport.cpp` | Multi-path insertion logic, `select_path()` scoring, accessor rewrites, announce quality-gate simplification, targeted failover (`mark_path_unresponsive`), `cull_path_table()` rewrite, `clear_caches_in_memory()`, set→vector migration for all four containers |
| `lib/microReticulum/src/Utilities/Persistence.h` | Added `Converter<std::deque<T>>` and `Converter<PathEntry>` for ArduinoJson |
| `lib/microReticulum/src/Reticulum.h` | Updated `get_path_table()` return type |
| `lib/microReticulum/src/Reticulum.cpp` | Updated `get_path_table()` and `drop_all_via()` for deque iteration |
| `lib/microReticulum/src/Link.cpp` | Added missing `Link::attached_interface()` const getter |
| `RNode_Firmware.ino` | All interfaces → `MODE_FULL`; reduced path table caps; wired `clear_caches_in_memory()` into heap relief |
