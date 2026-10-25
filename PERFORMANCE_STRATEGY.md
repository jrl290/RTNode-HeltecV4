# RTNode Performance and Memory Strategy

Read with [CORE_PRINCIPLES.md](CORE_PRINCIPLES.md) and the workspace
`DESIGN_PRINCIPLES.md`. This file sets the budgets, the measurements, and the
order of work for any change that spends RTNode's CPU, RAM, flash, airtime or
radio time.

It was written on 2026-09-27, ahead of three pieces of work that all draw on
the same budget:

- **#43** — WiFi degrades under transport load (30–55 % ICMP loss, backbone
  link drops), stable when idle.
- **#44** — Heltec V4 boards with 8 MB octal PSRAM run with no PSRAM at all.
- **Bluetooth LE** — an ad-hoc interface speaking the Prns-native protocol,
  with Columba compatibility.

Each can be made to work on its own and still break the others. Bluetooth in
particular takes RAM from the same pool WiFi uses and airtime from the same
2.4 GHz radio, so it must not start until #43 is understood and the harness
below exists.

## The machine we are budgeting

| Resource | Heltec V4 (2 MB PSRAM) | Notes |
|---|---|---|
| CPU | 2 × Xtensa LX7, 240 MHz | Core 0: WiFi, lwIP, Bluetooth controller (pinned by the SDK). Core 1: Arduino `loop()` — all RTNode and Reticulum work. |
| Internal SRAM | 320 KB heap region. Static use 73,776 B (22.5 %) in the 2026-09-27 build. 170–200 KB free in field logs. | WiFi RX buffers, lwIP, task stacks, and (later) the Bluetooth controller and host all live here. Watchdog sheds state at 28 KB free and resets at 20 KB. |
| PSRAM | 2 MB QSPI, meant to hold the TLSF pool and so all Reticulum objects — but field logs show it ~98 % free, so the pool is not being created (see "PSRAM is not backing the Reticulum heap"). 8 MB octal on newer boards, not initialised by today's build (#44). | Not usable for DMA. `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` lets WiFi/lwIP spill into it. |
| Flash | 16 MB; app 1.33 MB (20.3 %) | A flash write or erase disables the cache and stalls **both** cores for its duration. |
| 2.4 GHz radio | WiFi only today | Bluetooth will time-share this one radio with WiFi. |
| LoRa | e.g. SF10 / BW125 ≈ 1 kbps | One 167-byte announce is 0.6–2 s of airtime. |

## Rules for every change

1. **Measure before theorising.** A performance problem is investigated by
   making its stalls and drops report themselves (see Instrumentation), not by
   reasoning from symptoms. #43 already has several plausible causes; only
   measurement separates them.
2. **State the cost up front.** Every change says what it adds to internal
   heap (steady and peak), PSRAM, static RAM, flash, loop time per packet, and
   airtime — and the harness confirms it.
3. **Nothing blocks `loop()`.** Waiting on hardware or the network completes
   on an event: the SX1262 TxDone interrupt, a Bluetooth write confirmation, a
   socket becoming writable. No busy-waits, no blocking connects or writes.
   (DESIGN_PRINCIPLES §1: name the event.)
4. **Every queue is bounded, allocated at boot, and counts its drops.** The
   first drop is logged; all drops are counted and appear in the resource line.
5. **No per-packet heap allocation where a pool will do; no node-based
   containers** ([ESP32_HEAP_FRAGMENTATION.md](ESP32_HEAP_FRAGMENTATION.md)).
6. **Work that scales with what peers send is bounded where it enters.** The
   firewall already does this for WAN traffic. LAN-side *transport* peers — a
   `rnsd` on the local TCP server today, a `prnsd` laptop over Bluetooth
   tomorrow — can inject backbone-scale announce load through the trusted
   side (CORE_PRINCIPLES.md, "LAN-Side Rule"). They get an explicit ingress
   budget.
7. **Internal SRAM is for WiFi, Bluetooth, DMA and stacks.** Bulk state goes to
   PSRAM through the TLSF pool.
8. **Flash writes are budgeted like airtime.** No per-packet flash writes on
   paths whose rate a peer controls.
9. **Done means the harness passes on hardware**, on the private bench first
   (the workspace rule: production is not the test bed), not that it compiles.

## Budgets

Proposed values; the "baseline" column is filled in by step 1 of the order of
work. A change that moves a metric past its budget does not merge until the
budget is renegotiated here, in writing.

| Metric | Budget (proposed) | Baseline | Why this number |
|---|---|---|---|
| Min free internal heap, 30-min standard load | ≥ 48 KB | idle 198 KB; #43 flood **3.5 KB** | 20 KB above the 28 KB shed threshold |
| Largest free internal block | ≥ 24 KB | idle 192 KB; flood 12.5 KB | WiFi RX and lwIP need contiguous buffers |
| Static RAM (`.data` + `.bss`) | ≤ 25 % | 22.5 % | today plus Bluetooth tables |
| Main-loop iteration, p99 / max | ≤ 5 ms / ≤ 50 ms | idle ≤ 1 ms / 212 ms; flood ≤ 5 ms / **58.5 s** | keeps TCP, LoRa RX and timers serviced |
| Time `loop()` spends blocked in LoRa TX | 0 | 1.6 s per 168-byte frame; 33 frames in one call under flood | rule 3 |
| Flash writes per minute, standard load | ≤ 6 | idle 1 (`/time_offset`, 16–166 ms); flood up to 62 | each can stall both cores (rule 8) |
| WiFi health under standard load | ICMP loss < 1 %, RTT p95 < 150 ms, 0 TCP link drops per 30 min | idle 0 %, p95 182 ms (modem sleep); flood **73 %**, WiFi wedged | the #43 reporter's measure |
| Watchdog resets, 30-min soak | 0 | 0 in 5 min of flood, but one stall came within 1.5 s of the 60 s panic | |
| LoRa airtime | within the configured airtime locks and regional duty cycle | — | |

## Instrumentation: every stall and drop reports itself

Build this first (step 1). Counters on hot paths; logs only on transitions.

- **Resource line** — one `[RES]` line every 60 s: internal free / min /
  largest block, PSRAM free, TLSF used / peak, loop iterations with p99 and max
  iteration time, milliseconds `loop()` spent blocked (by cause: LoRa TX, flash,
  TCP connect/write), LoRa TX queue depth / high-water / drops, announces
  accepted and dropped per interface, flash writes and total flash-busy ms,
  TCP per-client backlog and drops, WiFi RSSI and disconnects; Bluetooth
  slots, handshakes, and drops once it exists. It folds in the existing
  `[HEAP]` line and the Transport status line.
- **Event marks** with a millisecond timestamp: LoRa TX start/end, flash
  write start/end, heap-pressure stage changes, TCP link up/down. These line
  up against host-side ping timestamps to attribute each loss burst.
- Everything a drop path does is counted — "silent failures mask each other".

## The load harness

The gate every change passes. It runs on the bench: a V4 (and a V4 with
octal PSRAM once #44 lands), a stock RNode as the LoRa peer, the home AP, and
a host running `rnsd`. The staging RPi joins for Bluetooth (Linux `prnsd`).

| Scenario | What runs | Measures |
|---|---|---|
| A. Idle | 10 min, no peers | baseline of every metric |
| B. LAN transport flood (#43) | `rnsd` as a transport instance, `TCPClientInterface` into the local TCP server, fed by `test-harnesses/staging/py/announce_flood.py` at 1, 3 and 10 announces/s; LoRa SF10/BW125 | ping 5/s to the node for 10 min (loss, RTT p50/p95/max), TCP link drops, `[RES]` lines |
| C. WAN flood | same stream through a backbone slot | firewall path cost |
| D. LoRa flood | announces and path requests sent over LoRa from a stock RNode running `rnsd` (earlier runs were ad-hoc scripts, see LEARNED_SO_FAR.md 2026-05-10; script it) | LoRa ingress cost |
| E. Soak | B at 3/s for 30 min | watchdog resets, heap trend |
| F. Bluetooth (later) | B plus four Bluetooth peers: `prnsd` on the Mac, `prnsd` on the RPi, a Hopspot on a spare V4, a Columba phone | the same, plus coexistence effect on WiFi |

A run produces a results directory: the serial log, host ping output, flood
output, and a one-line summary appended to the Results log below. The script
lives in `tests/` alongside the proof-probe harnesses.

Host-side logic (codecs, parsers, the Bluetooth policy state machine) is
unit-tested without a board by `tests/native/run.sh`.

## Baseline, 2026-09-27

Heltec V4.2 (GC1109 PA, 2 MB quad PSRAM), v1.0.50 plus the instrumentation
(`bench/instrumentation` 60b1aad), bench overrides (backbones off, local TCP
server on, TX power capped at 11 dBm effective). Raw output under
`tests/bench-results/` (not in git).

**Scenario A, idle, 10 min.** 0 % ping loss; RTT p50 97 ms, p95 182 ms, max
293 ms — WiFi modem sleep, which nothing turns off. No loop stalls after boot.
Internal heap steady at 204 KB free (min 198 KB).

**Scenario B, #43 flood at 1 announce/s, 5 min.** Reproduces #43, worse:

- **73 % ping loss, then none answered at all.** From +82 s the node stopped
  answering ICMP and ARP (the Mac reported "Host is down") and was still
  unreachable 2.5 min after the flood ended — while its own log reported
  WiFi connected at −33 dBm and 120 KB free. The WiFi watchdog, which only
  reacts to a disconnect, never fired. Only a reboot recovers it.
- **The loop was blocked about 85 % of the time.** LoRa stalls grew as the
  queue filled (1.8, 3.6, 8.9, 28, 35 s) up to 58.5 s for 33 frames in one
  call — 1.5 s short of the task-watchdog panic. The queue overflowed
  (20 drops).
- **Flash bursts between them:** up to 54 cache writes and 10 s of flash in
  one stall; 142 flash operations of 20 ms or more in 5 min.
- **A TCP keepalive write blocked 10 s and failed**, dropping the flood client
  (the reporter's "link DOWN").
- **Internal heap fell from 204 KB to 3.5 KB** (largest block 12.5 KB) —
  every Reticulum object is in internal RAM (next section). That, not the
  blocked loop alone, is the likely cause of the wedge.

**Smaller costs found on the way**

- The display pushes a full frame over I2C about 6 times a second (~25 ms
  each, ~17 % of the loop) even when blanked: `update_display()` clears and
  pushes on every interval ([Display.h:1224](Display.h)).
- `persist_data` rewrites `/time_offset` every minute: 16–166 ms with both
  cores stalled.
- One idle stall of 1.5 s had no instrumented cause; `[STALL]` now reports
  unattributed time (`other=`) so the next one is visible.
- Pre-existing bug: `boundary_nominal_path_table_maxpersist` is set from
  `probe_destination_enabled()` ([RNode_Firmware.ino:1009](RNode_Firmware.ino)),
  so restoring the cap after heap pressure would persist at most one path.

**Bench note.** On this board's USB-Serial/JTAG port an RTS reset can leave
the chip in the ROM bootloader — silent, off WiFi — and with a battery
attached that survives every reset. esptool's `--after watchdog_reset`
starts the app; if a board is truly stuck, disconnect battery and USB, hold
PRG while reconnecting, and flash. The harness falls back to the watchdog
reset.

## Issue #43: what the code says

Read-only investigation, 2026-09-27. "Proven" means read in the code;
"inferred" needs the harness to confirm. Line numbers are at v1.0.50.

**Proven**

1. **LoRa TX drains the whole queue in one blocking call.** At SF10/BW125
   the modem bitrate (976 bps) is under the rate-limit thresholds, so
   `tx_queue_handler` always calls `flush_queue()`
   ([RNode_Firmware.ino:2742](RNode_Firmware.ino)), which transmits every
   queued packet back to back ([1606–1628](RNode_Firmware.ino)) with no CSMA
   between them. Each `sx126x::endPacket()` busy-polls the IRQ status over
   SPI until TxDone ([sx126x.cpp:466–473](sx126x.cpp)); DIO1 is not routed to
   TxDone. A full queue of announces (~36 × 1.6 s) blocks `loop()` for about
   a minute.
2. **That can reset the node.** `loop()` is the only task-watchdog feed, and
   the watchdog panics after 60 s ([RNode_Firmware.ino:80, 493](RNode_Firmware.ino)).
3. **While `loop()` is blocked, TCP is not serviced.** Accept, reads and
   keepalives all run from `loop()` ([TcpInterface.h](TcpInterface.h)).
   Once a socket's receive mailbox (6 segments) fills, lwIP stops ACKing, and
   a Linux `rnsd` peer aborts the connection after its 24 s
   `TCP_USER_TIMEOUT` (RNS `TCPInterface.py:84`) — a match for the reported
   "link DOWN".
4. **Firewall builds send every announce to LoRa, twice, with no cap.**
   `FIREWALL_MODE` compiles out announce bandwidth caps
   ([Transport.cpp:1084–1096](lib/microReticulum/src/Transport.cpp)), ingress
   `rate_blocked` is hard-wired false, and the LoRa interface never sets
   `_bitrate` or `_announce_cap`, so Transport sees LoRa as 1 kbps with no
   cap. Each accepted announce is rebroadcast at ~0–0.5 s and again ~5.5 s
   later. The Python reference caps LoRa announces at 2 % of bitrate.
5. **Every path-changing announce is written to flash as its own file**
   (`cache_packet(packet, true)`,
   [Transport.cpp:2459](lib/microReticulum/src/Transport.cpp)); every 60 s
   `clean_caches` deletes the unreferenced ones and `persist_data` rewrites
   `/time_offset`.
6. **Amplifiers:** Ed25519 announce verification runs before the replay
   check; the replay window holds only 16 entries; the announce table
   retires at most one entry per second; `VERBOSE` logging calls
   `Serial.flush`, which waits up to 100 ms per line with a USB host attached;
   the display pushes ~1 KB over I2C about 7 times a second; `WiFiClient::write`
   can block for 10 s or more on a weak link.
7. **Nothing sets WiFi power save**, so the core default (modem sleep) applies.

**Inferred**

- The whole-queue flush explains the TCP drops, but not pings lost *during*
  airtime: WiFi and lwIP run on core 0 and answer ICMP without `loop()`.
  Two candidates remain: the flash writes in 5 (a flash write stalls both
  cores, and this SDK build does not keep WiFi/lwIP in IRAM), and RF or supply
  coupling from the V4's PA, which defaults to 28 dBm. The harness separates
  them by lining host ping timestamps up against the `[Boundary] TXCFG` /
  `TXDONE` serial marks and the flash-write marks, then repeating at 2 dBm and
  with announce caching disabled.
- The reporter's setup is the CORE_PRINCIPLES hazard exactly: a transport
  `rnsd` on the trusted LAN side relaying backbone announces.

**Ping loss correction.** Until 2026-09-27 evening the harness estimated
pings sent from elapsed time; ping runs slightly slower than 5/s, so the last
~20 always counted as lost. Losses above are recounted from sequence numbers
(ping's own statistics agree).

**Found while testing the cache:** once flood announces fill the path table
(24 destinations), a new destination arriving on the same interface with the
same hop count scores the same as the flood entries, and `cull_path_table()`
can evict it at once — a legitimate peer is locked out until the flood's
entries expire. The ingress budget (rule 6) and a recency tie-break in the
cull are the fixes to evaluate.

**Head-of-line blocking on LoRa (2026-09-27, stock RNode on the bench).** The
LoRa TX queue is FIFO, so a proof or link packet waits behind announce
rebroadcasts (1.8 s each at SF10); event-driven TX adds a CSMA gap (~0.8 s
at SF10) before every packet, where the old whole-queue flush sent back to
back. A proof round trip measured 6.3 s. Options: send non-announce packets
first (changes order, not what is sent), and/or the LoRa announce cap
(declined for now, 2026-09-27).

**Bench note.** The stock RNode (Heltec V3, `/dev/cu.usbserial-0001`) kept its
radio off ("Radio state mismatch", state offline) because its stored target
firmware hash was blank EEPROM; fixed with `rnodeconf --firmware-hash
<actual hash>` (read with `--get-firmware-hash`), as in LEARNED_SO_FAR.md.

**Ingress control (2026-09-27; James: "ingress only for now").** Python
RNS 1.5.2's announce ingress control, ported into microReticulum
([Interface.cpp](lib/microReticulum/src/Interface.cpp),
[Transport.cpp](lib/microReticulum/src/Transport.cpp), constants in
[Type.h](lib/microReticulum/src/Type.h)). *Since 2026-10-25 the port is gone:
the behaviour, differences below included, is described in
[RETICULUM_BEHAVIOUR.md](RETICULUM_BEHAVIOUR.md) §1 and the code
([AnnounceGate.h](lib/microReticulum/src/AnnounceGate.h)) is written from that
description, because RTNode (GPL-3) cannot carry code translated from the
Python implementation (Reticulum License).* Each source keeps its last 48
announce arrivals; its rate is their count over the time since the oldest
(three needed; arrivals over 10 s old are forgotten one per check). Past
3/s from a source younger than two hours (10/s once established), announces
for destinations not in the path table are held — unless a path request is
waiting for them. The burst ends once the rate has stayed under the
threshold for 15 s; then one held announce goes back through `inbound()`
every 5 s, fewest hops first. Where RTNode differs from the reference, and
why:

- *State per TCP client.* Python spawns an interface per client of a TCP
  server; RTNode's local server is one interface. With one shared state a
  flooding client held a second client's announce (path test failed), so each
  client slot has its own, reset on connect.
- *Every announce counts* toward the rate, where Python counts only those
  whose signature verifies: verifying signatures is the work a flood makes an
  ESP32 do. Released announces are verified as normal.
- *A total cap.* Python caps 256 per interface. A node has up to eight TCP
  clients, the backbone slots and LoRa, so the total is capped at 512 (about
  128 KB of PSRAM); without PSRAM, 32 per source and in total. Announces that
  find the store full are dropped and counted (`ic_drop` in `[RES]`).
- *Releases wait for the burst to end.* Python releases while a burst is
  still active too, and `inbound()` then holds the announce again; the
  outcome is the same. Released announces skip ingress control on their way
  back: the TCP server would credit them to whichever client sent data last.
- *Path requests are not ingress-controlled yet.* 1.5.2 also counts path
  requests per source (3/s new, 8/s established) and, in a burst, stops
  forwarding them on. Not ported (a follow-up candidate).

Timing runs on milliseconds since boot, not `OS::time()`, which jumps when
Reticulum restores its saved offset after interfaces register (a
registration stamp made every source look hours old).

**Port from the wrong reference, caught.** The first port followed
`Reticulum-master/`, which is RNS **1.1.3**: six samples, 3.5/12 per second,
a 60 s hold, then 300 s before releasing one every 30 s — a client that
announced two destinations at once had the second held for six minutes, and
256 held took two hours to drain. 1.5.2 (in `.venv`, the version the
workspace audits against) replaced all of that. Check `RNS/_version.py`
before porting from the mirror.

Cost: about 250 B of PSRAM per held announce (256 held measured at ~65 KB);
per source 48 × 4 B of arrival times, in PSRAM with the interface.

**Found with it: early returns stopped `jobs()`.** `inbound()` and
`outbound()` set `_jobs_locked` and must clear it on the way out; Python
clears it before every return. Several returns here did not — unpack
failures, the firewall's WL-BLOCK drop, cache requests, expired or stale
paths — nor did the first cut of the ingress hold. Until another packet went
all the way through, `jobs()` did nothing: no announce rebroadcasts, table
culls or link checks. Under an all-held flood it never ran, and a legitimate
client's announce was not rebroadcast until an unrelated client connected
18 s later. A scope guard now clears the lock on every exit (5e40d23).
Released firmware has the WL-BLOCK case: on a backbone, each dropped unknown
packet pauses `jobs()` until the next packet passes.

**Device advertisement stamp (2026-09-28).** Two faults in
[Advertise.h](Advertise.h), found while testing the IFAC change:

- *Value.* RNS 1.5.2 raised the discovery stamp value from 14 to 16, and a
  1.5.2 node requires 16 by default (`required_discovery_value`). RTNode
  stamped at 14, so such a node ignored its advert three times in four.
- *Blocking.* The proof-of-work ran in one piece inside `loop()`, copying the
  5 KB workblock into a new buffer and hashing all of it for every
  candidate: 12.7 s and 28.0 s of dead node on the bench at value 14 — past
  a Linux `rnsd` peer's 24 s TCP timeout — whenever the advertised info
  changes. Value 16 is four times the work.

Now each workblock round is hashed in as it is made (one per loop pass,
never stored) and every candidate starts from the SHA-256 state after the
workblock, so it costs one 64-byte block; the search runs 2 ms per loop
pass. Value-16 stamps took 0.8 s and 1.5 s with no stall. The NVS stamp
cache records the value, so a stamp cached at 14 is not reused.

**Bluetooth interface, first cut (2026-09-28).** RTNode speaks
ble-reticulum's protocol v2.2 as Columba 2.2.6 does (ble-reticulum 07d9413):
a GATT peripheral with RX, TX and identity characteristics, a central's
first 16-byte write as its identity, 5-byte fragment headers, 0x00
keepalives. It advertises v0.3.0's peripheral-only flag (manufacturer data
`ff ff 03 01`, the same field Prns uses), so centrals connect to us and
RTNode has no client code. Columba decides direction by MAC, and Android
gives apps `02:00:00:00:00:00` as their own, so a phone always connects.
Four fixed slots, each a Reticulum interface registered at boot and
**trusted local exactly like LoRa** (James); when a peer leaves, the paths,
links and reverse entries through its slot are forgotten
(`Transport::forget_interface_routes`). Costs 38 KB internal RAM and 23 KB
PSRAM at start.

- *Tests.* `tests/bench_ble.py --serve` gains a relay mode (a v2.2 central
  bridging packets to UDP, run from Terminal), and `tests/bench_ble_rns.py`
  runs Reticulum through it: announces both ways and a Link with 10 echoes
  of 431-byte packets, PASS unfragmented, with the peer fragmenting at 95
  bytes, and with both sides fragmenting at 100 (`RTNODE_BLE_CHUNK_MAX`).
  `tests/bench_ble_columba.py` finds a real Columba phone through the node
  and sends it an LXMF message: PASS, path in 1.24 s, delivered.
- *Android shows one link twice.* When Columba connects, Android also
  reports the link to Columba's own GATT server; Columba's deduplication
  keeps the client side and cancels the phantom server side, and the link
  survives.
- *Found: forwarded path requests were never answered.* A path request
  for an unknown destination was forwarded but the asker was not recorded,
  so the response went nowhere until the asker asked again. Now answered on
  the asker's interface as Python does (b0c7a86); this also fixes TCP
  clients asking for LoRa-only or backbone destinations. (Rewritten
  2026-10-25 from RETICULUM_BEHAVIOUR.md §2.)

**Fix direction** (each measured on its own, rule 1)

- Event-driven LoRa TX: route TxDone to DIO1, send one packet per TxDone
  event, run CSMA per packet, and return to `loop()` between packets.
  **Done** (0bb88d4).
- Reference-parity announce policy on LoRa: set the interface bitrate from
  SF/BW and restore an announce cap in firewall builds. **Declined for now**
  (2026-09-27).
- An announce-ingress budget for LAN-side transport peers (rule 6). **Done**:
  Python's ingress control, above.
- Flash writes off the per-announce path if the flash A/B implicates them.
  **Done** (e03d5d1, RAM announce cache).

## PSRAM is not backing the Reticulum heap (quad V4, today)

The TLSF pool is created on the first C++ `new`: it takes 80 % of PSRAM if
`ESP.getPsramSize()` is non-zero, otherwise it latches plain `malloc` for
good ([OS.cpp:52–77](lib/microReticulum/src/Utilities/OS.cpp)). The V4 build
does not initialise PSRAM at boot (`CONFIG_SPIRAM_BOOT_INIT` is off); the
Arduino core does it later in `initArduino()`, which runs after C++ static
constructors. A `new` in any static constructor therefore sees no PSRAM.
Field evidence: `log.log` from a V4 reports `psram=2054639` bytes free, so
no ~1.6 MB pool was ever taken.

Consequence: every Reticulum object — paths, announces, links, packets —
lives in the same internal SRAM that WiFi, lwIP and the watchdog thresholds
depend on, and the V4's PSRAM sits idle. This changes every internal-heap
number above.

**Confirmed on hardware** (`[RES] boot ... tlsf=off(malloc)` with 2 MB PSRAM
present). **Step 2, first attempt** (`bench/instrumentation` fd548b9): creating
the pool from `setup()` works — 1.6 MB taken from PSRAM — but TLSF then
returns no block at all (every allocation fell back to `malloc`), which
suggests `tlsf_add_pool()` fails; `tlsf_create_with_pool()` ignores that and
prints the reason with `printf`, which does not reach the log. The pool has
probably never worked on any board. Two findings for the fix: `operator
delete` must route frees by address (blocks from before the pool must go to
`free()`), and TLSF has no locking while `new` runs on several tasks.
**Root cause:** ESP-IDF's own heap is built on TLSF and exports the same
`tlsf_*` symbols with a different API (IDF 4.3+: `tlsf_size(tlsf_t)`, a
three-argument `tlsf_create_with_pool`). The firmware links IDF's functions
(`addr2line` → `heap_tlsf.c`), so microReticulum's pool code has been calling
them with the wrong arguments; a standalone probe calling `tlsf_size()`
crashed inside `heap_tlsf.c:693`. **Fix (072c873):** on ESP32, `operator new`
asks ESP-IDF's heap for PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`) once
`OS::init_heap()` has seen PSRAM, and `delete` is `free()`; IDF's heap is
already TLSF-based and thread-safe. The private pool stays for nRF52.
Result under the #43 flood: minimum internal heap 191 KB (was 3.5 KB), ping
loss 4 % (was 73 %), no WiFi wedge.

## Issue #44: octal PSRAM

Read-only investigation, 2026-09-27, using Prns's V4-R8 board support as the
reference (Prns has not tested its R8 support on hardware either).

- **The board:** an ESP32-S3R8 (8 MB octal PSRAM in package), 16 MB DIO flash.
  Radio, front-end detection, display bus, button and VBAT pins match the V4.
- **Pins that must change:** octal PSRAM uses GPIO33–37 as data and strobe
  lines. RTNode's V4 map drives three of them — GPIO35 (LED), GPIO36 (Vext),
  GPIO37 (battery-divider control). On the R8, Vext is GPIO40 and the divider
  is read ungated. Driving 35–37 in an octal build would take lines away from
  PSRAM and corrupt it; in today's quad build on an R8 it is harmless, but
  Vext is never switched on, so the OLED is probably dark.
- **Today on an R8:** PSRAM init fails, the node runs with none (V3-like
  memory), and both flashers classify it as a V4 because the chip reports
  "PSRAM". Bluetooth, on by default from v1.0.51, keeps its memory in PSRAM,
  so it starts only where PSRAM did (`firewall_ble_has_psram()`); before
  that check it would have failed its first allocation and restarted the
  node every boot. `-DRTNODE_BENCH_NO_PSRAM=1` makes a bench build run as
  an R8 does (Reticulum heap and Bluetooth off PSRAM): 2026-09-28, boots
  clean with Bluetooth off, LAN contract checks 8/8, ~170 KB internal free.
- **Two builds are required.** The prebuilt quad (`dio_qspi`) and octal
  (`dio_opi`) SDK variants link different PSRAM drivers. The bootloader and
  partition images are shared; only the app differs. A wrong build does not
  brick either board (both boot without PSRAM).
- **Detection:** esptool's chip features report "Embedded PSRAM 8MB" or
  "2MB" from eFuse (esptool.py 4.x, and esptool-js 0.4.6 used by the web
  flasher). Prns's own flashers do not tell the variants apart. The
  bundled esptool 4.5.1 reports neither, and would misread any V4 as a V3.
- **Open:** the R8's LED pin, its real eFuse value, and whether the octal
  build's 80 MHz PSRAM clock is stable (Prns chose 40 MHz). Need a board.
- **Touch list:** a new `dio_opi` env; a V4-R8 sub-variant in Boards.h
  (Vext 40, no LED on 35, no divider control on 37) and guards at every LED
  and Vext use; a boot line reporting PSRAM mode and size; flash.py and the
  web flasher's detection and firmware map; release packaging, mirror and
  versions lists; README.

## Bluetooth LE: its share of the budget

- **RAM decided at boot.** Firewall builds release the Bluetooth controller's
  memory at startup, and that cannot be undone without a reboot, so Bluetooth
  is a boot-time setting. **Measured (spike, 2026-09-28, NimBLE-Arduino 1.4.3,
  four connection slots, host memory in PSRAM):** about 65 KB of internal
  RAM. 22 KB is a larger static image (IRAM +14.8 KB, DRAM +7.5 KB) plus the
  controller memory no longer released; NimBLE's start takes 38.2 KB more
  (16.4 KB goes to PSRAM). With the host in internal RAM instead, start costs
  49.0 KB. Under scenario B with one busy connection, internal free stays at
  ~143 KB (low 138 KB, largest block 127 KB) against the 48 KB floor.
  Image +194 KB.
- **One radio.** Bluetooth advertising, scanning and connection events take
  time from WiFi. Scan duty trades directly against #43-style WiFi loss, so
  scenario F measures WiFi health with Bluetooth on. Prns's ESP32-S3 values
  (idle scan 200 ms per 1 s; discovery backs off while links are busy) are the
  starting point. **Measured:** with a 5/s LAN flood and one peer echoing
  180-byte frames at up to 19/s, WiFi lost no pings in four runs and its p95
  stayed at 146–166 ms (plain build 166 ms); the worst ping rose from 291 ms
  to 240–600 ms. Scan duty cost the Bluetooth link instead: echo p95 151 ms
  at 10 % (50 ms per 500 ms), 346 ms at Prns's 20 %. So scan little, and less
  while links are busy, as Prns does.
- **WiFi power save.** ESP-IDF's coexistence guidance expects WiFi modem sleep
  while Bluetooth is enabled. **Verified, and it is fatal:** turning power
  save off (`WiFi.setSleep(false)`, the #43 reporter's partial workaround)
  with Bluetooth on logs "Should enable WiFi modem sleep when both WiFi and
  Bluetooth are enabled" and aborts, every boot. RTNode never turns it off
  today; any future option that does must be refused while Bluetooth is on.
- **Four fixed peer slots**, each its own interface, registered at boot.
  Reassembly buffers and queues are allocated once.
- **Access control is IFAC.** Neither the Prns-native nor the Columba protocol
  has a password or pairing step; Reticulum's IFAC (network name and
  passphrase, as on LoRa) is the only compatible gate. Three limits:
  - It filters packets, not connections, so a stranger can still hold one of
    the four slots. A peer whose packets fail IFAC is closed and its identity
    backed off — the failed check is the deterministic event.
  - Peers must be configured to match. `prnsd` documents IFAC for every
    interface; the Prns Hopspot has no IFAC setting at all; Columba is
    unknown.
  - It costs one Ed25519 signature per packet in each direction (outbound
    sign at [Transport.cpp:894](lib/microReticulum/src/Transport.cpp); inbound
    verification re-signs). Measure it before offering it on Bluetooth.
- **Bluetooth peers are LAN-side** (decision below), so rule 6 applies to
  them from day one: a `prnsd` laptop with backbone connections is a LAN
  transport peer.

## Order of work

1. **Instrumentation, harness, baseline.** Build `[RES]`, the event marks and
   a boot line reporting whether the TLSF pool is in PSRAM; script scenarios
   A–E; run them on today's firmware. Scenario B should reproduce #43. Fill in
   the baseline column.
2. **Put the Reticulum heap in PSRAM** (quad V4): fix the TLSF init order.
   Small and high-value — it moves every Reticulum object out of the internal
   SRAM WiFi needs — so it goes first and gets its own re-baseline, which the
   #43 work is then measured against.
3. **#43.** Event-driven LoRa TX; reference-parity announce cap on LoRa; an
   ingress budget for LAN transport peers; flash writes off the per-announce
   path if the A/B implicates them. One change at a time; scenario B must pass
   before moving on.
4. **#44.** V4-R8 build variant, safe pin map, detection in both flashers.
   Needs an R8 on the bench.
5. **IFAC publishing.** The device advertisement includes the IFAC network
   name *and passphrase* whenever IFAC and advertising are both on
   ([Advertise.h](Advertise.h), `include_ifac`), although the portal tells
   users IFAC restricts access. The reference RNS publishes them only with
   `publish_ifac = yes`, which defaults to off. Make it opt-in, default off,
   before Bluetooth adds a second IFAC-protected medium. **Done**
   (2026-09-28, branch `fix/ifac-publish-opt-in`, off `main`): portal
   setting "Publish Network Access (IFAC)", default No, config byte 0x23D
   (never used before, so every existing save reads "no"). Found alongside
   it, fixed on `fix/advert-stamp-nonblocking`: see "Device advertisement
   stamp" below.
6. **Bluetooth spike.** NimBLE on a V4, advertising, scanning and holding four
   connections, under scenario B. Record heap and WiFi-health cost. **Done**
   (2026-09-28) with one peer, as James asked: `BleSpike.h`, env
   `rtnode_heltec_v4_bench_ble`, `tests/bench_ble.py`. Costs are in
   "Bluetooth LE: its share of the budget"; the memory for four connections
   is reserved at start, so it is in the figures. Not yet measured: four live
   peers, 2M PHY, L2CAP.
7. **Bluetooth native GATT** → **Columba characteristics** → **L2CAP**, each
   through scenario F. **Columba first** (2026-09-28, the peer on the bench):
   `BleInterface.h`. See "Bluetooth interface, first cut" below. **Prns's
   native protocol** followed the same day (04aa2a1), tested against prnsd
   0.3.7. **On by default** in the V4 release build (James), with a portal
   switch applied at boot (06efcfb). L2CAP still to do.

Each step's results go in the log below before the next step starts.

Alongside this plan, [CONTRACT_TESTS.md](CONTRACT_TESTS.md) (2026-09-28)
checks that the node still does what apps rely on — LAN routing across TCP,
Bluetooth and LoRa, the WAN firewall, links and resources, and all three
radios at once — and found six bugs on the way, five of them in LoRa and
firewall forwarding. Its soak is the coexistence measure for step 7.

## Decisions

- 2026-09-27 — Bluetooth peers are LAN-side peers, alongside LAN TCP and LoRa;
  never the backbone zone.
- 2026-09-27 — The Bluetooth interface hosts the Columba characteristics as
  well as the Prns-native ones.
- 2026-09-27 — Bluetooth targets the V4 only: the 2 MB board now, the
  octal-PSRAM board once #44 ships. Not the V3.
- 2026-09-28 — Bluetooth peers are trusted local interfaces, exactly like
  LoRa.
- 2026-09-28 — Bluetooth is on by default in V4 builds; the portal can turn
  it off (takes effect at the next boot).

## Results log

| Date | Firmware | Board | Scenario | Min heap | Loop p99 / max | Ping loss / p95 | Link drops | Notes |
|---|---|---|---|---|---|---|---|---|
| 2026-09-27 | v1.0.50 + inst (60b1aad) | V4.2 | A idle 10 min | 198 KB | ≤ 1 ms / 212 ms | 0 % / 182 ms | 0 | modem-sleep RTT; display ~6 pushes/s |
| 2026-09-27 | v1.0.50 + inst (60b1aad) | V4.2 | B flood 1/s 5 min | 3.5 KB | ≤ 5 ms / 58.5 s | 73 % / — | 1 | WiFi wedged from +82 s until reboot; 20 LoRa queue drops |
| 2026-09-27 | + PSRAM heap (072c873) | V4.2 | B flood 1/s 5 min | 191 KB | ≤ 5 ms / 58.9 s | 1.2 % / 209 ms | 1 | WiFi healthy; PSRAM 2.06→1.69 MB free; **task-watchdog reboot at +282 s** (LoRa flush > 60 s); 40 queue drops |
| 2026-09-27 | + event-driven LoRa TX (0bb88d4) | V4.2 | B flood 1/s 5 min | 191 KB | ≤ 1 ms / 17.4 s | 0 % / 186 ms | 0 | LoRa TX blocks loop 10–16 ms/min (was up to 85 s); no reboot; stalls now the once-a-minute cache cleanup: 69–90 deletes + ~8 s unattributed each; 300 cache writes, 208 deletes in 5 min; 354 queue drops (flood ≫ SF10 capacity) |
| 2026-09-27 | + RAM announce cache | V4.2 | B flood 1/s 5 min | 194 KB | ≤ 1 ms / 2.1 s | 0 % / 190 ms | 0 | 5 stalls (was 166); flash 0.6–1.5 s/min (was 17–20 s); path requests answered from the RAM cache in 0.48 s (`tests/bench_path_response.py`) |
| 2026-09-27 | + RAM announce cache (e03d5d1) | V4.2 + stock RNode (Heltec V3, fw 1.86) | lora-to-local-tcp proof probe | — | — | — | — | **LoRa RX and TX verified with event-driven TX**: path in 4.3 s, delivered with proof, RTT 6.3 s. The proof waited behind a queued 183-byte announce (FIFO, 1.84 s airtime) plus a 0.8 s CSMA gap per packet |
| 2026-09-27 | + ingress control (1.1.3 rules), per interface | V4.2 | B flood 5/s 150 s | 198 KB | ≤ 5 ms / 319 ms | 0 % / — | 0 | burst detected at once; path table stayed at 1 entry; **path test FAIL**: the shared state held a second client's announce |
| 2026-09-27 | + per-client state (b17ec04) | V4.2 | B flood 5/s 150 s | 195 KB | ≤ 5 ms / 398 ms | 0 % / 168 ms | 0 | path test INVALID: A's announce not rebroadcast for 18 s — `jobs()` locked by the hold's early return |
| 2026-09-27 | + jobs lock guard (5e40d23) | V4.2 | B flood 5/s 150 s | 197 KB | ≤ 5 ms / 427 ms | 0 % / 166 ms | 0 | **path test PASS**, 1.40 s mid-flood, answered by RTNode (2 hops); 256 held |
| 2026-09-27 | + jobs lock guard (5e40d23) | V4.2 | B flood 1/s 10 min | 192 KB | ≤ 1 ms / 2.6 s | 0 % / 186 ms | 0 | below the burst threshold, nothing held; 13 stalls (once-a-minute persistence, as before); 186 LoRa TX = 334 s airtime of 600 s; 919 LoRa queue drops (announce cap declined) |
| 2026-09-27 | + total cap, ic_drop (0fc5626) | V4.2 | B flood 5/s 150 s | 197 KB | ≤ 5 ms / 446 ms | 0 % / 175 ms | 0 | path test PASS 1.34 s; ic_held=256 ic_drop=471 |
| 2026-09-27 | 1.5.2 ingress rules (d0d15cf) | V4.2 | ingress release test | — | — | — | — | `tests/bench_ingress_release.py` PASS: burst at the third announce; nothing leaked during the hold; burst over 16.5 s after the flood stopped; releases 0.5 s later, then 5.2 s apart, oldest first |
| 2026-09-27 | 1.5.2 ingress rules (d0d15cf) | V4.2 | B flood 5/s 150 s | 197 KB | ≤ 5 ms / 521 ms | 0 % / 166 ms | 0 | path test PASS 0.90 s; ic_held=256 ic_drop=488 |
| 2026-09-27 | 1.5.2 ingress rules (d0d15cf) | V4.2 | B flood 10/s 150 s | 197 KB | ≤ 5 ms / 401 ms | 0 % / 157 ms | 0 | path test PASS 0.43 s; ic_held=256 ic_drop=1226 |
| 2026-09-28 | + publish_ifac opt-in (77f3492) | V4.2 | advert test, `tests/bench_advert_ifac.py` | — | — | — | — | PASS both ways: saved setting unset → no credentials; opted in → name and passphrase. Stamp at value 14 blocked `loop()` 28.0 s and 12.7 s; one came out at 14, ignored by a default 1.5.2 node |
| 2026-09-28 | + sliced value-16 stamp (f4569b3) | V4.2 | advert test | — | — | — | — | PASS both ways as a default 1.5.2 listener; stamps of value 18 in 0.8 s and 1.5 s (15,466 and 30,460 attempts), no `[STALL]` |
| 2026-09-28 | BLE spike (2e6aea2), host in internal RAM | V4.2 | B flood 5/s, advertising + 10 % scan, no peer | 127 KB | ≤ 5 ms / 298 ms | 1/737 / 149 ms | 0 | NimBLE start −49.0 KB internal; internal free 132 KB |
| 2026-09-28 | same | V4.2 + Mac peer | B flood 5/s, connected, no traffic | 127 KB | ≤ 5 ms / 468 ms | 0 % / 153 ms | 0 | MTU 517 |
| 2026-09-28 | same | V4.2 + Mac peer | B flood 5/s, echo 5/s × 180 B | 127 KB | ≤ 5 ms / 339 ms | 0 % / 166 ms | 0 | 700/700 echoed, RTT p50 63–77, p95 131 ms |
| 2026-09-28 | same | V4.2 + Mac peer | B flood 5/s, echo ~19/s × 180 B | 127 KB | ≤ 5 ms / 563 ms | 0 % / 148 ms | 0 | 2626/2626 echoed, RTT p95 181 ms; no echo drops, no notify failures |
| 2026-09-28 | + host memory in PSRAM (e52aa4c) | V4.2 + Mac peer | B flood 5/s, echo ~19/s × 180 B | 138 KB | ≤ 5 ms / 517 ms | 0 % / 149 ms | 0 | NimBLE start −38.2 KB internal, −16.4 KB PSRAM; 2619/2619 echoed, RTT p95 151 ms |
| 2026-09-28 | same, Prns scan (200 ms / 1 s) | V4.2 + Mac peer | B flood 5/s, echo ~17.5/s × 180 B | 138 KB | ≤ 5 ms / 585 ms | 0 % / 146 ms | 0 | 2453/2453 echoed, RTT p95 346 ms, max 613 ms |
| 2026-09-28 | + WiFi power-save probe | V4.2 | boot | — | — | — | — | power save off with Bluetooth on: ESP-IDF aborts, boot loop |
| 2026-09-28 | Bluetooth interface (a4be90c…819784b) | V4.2 + Mac relay | `bench_ble_rns.py` | — | — | — | — | PASS: announces both ways ~1 s; Link 0.3 s; 10/10 echoes, RTT p50 149 ms (one fragment), 373 ms (peer fragments at 95 B), 421 ms (both at 100 B) |
| 2026-09-28 | same | V4.2 + Galaxy S23, Columba 2.2.6 | `bench_ble_columba.py` | 137 KB | ≤ 1 ms / 576 ms | — | 0 | PASS: path 1.24 s (never answered before b0c7a86), LXMF delivered; proof back over Bluetooth 126 ms after the message crossed; two peers at once, no drops or bad fragments |
| 2026-09-28 | + Prns native (04aa2a1), on by default (06efcfb) | V4.2 + prnsd 0.3.7 on the Mac | `bench_ble_rns.py --via prnsd` | 137 KB | — | — | 0 | PASS: Hello answered (Prns peer, MTU 517); announces both ways 0.84 s / 1.67 s (3 hops); Link 0.27 s; 10/10 echoes, RTT p50 392 ms; Columba on another slot throughout |
| 2026-09-28 | contract fixes (51b9262…5eb13db) | V4.2 + prnsd + stock RNode + stand-in backbone | contract soak, 30 min: WiFi↔BT echo 1/s + 20 kB/min, WAN↔WiFi echo 0.5/s + 20 kB/2 min, LoRa echoes to TCP and BT | 118 KB | ≤ 5 ms / 992 ms | — | 0 | all 5 flows PASS: 1643/1643 and 891/891 echoes, 43/43 resources, LoRa 88/90; no Bluetooth disconnects; heap flat |
| 2026-09-28 | + loop-time causes (376cb10) | V4.2 | idle and 20/s WAN announce flood | 118 KB | — / 1.2 s | — | 0 | per minute idle: `reticulum.loop()` ~25 s (0.24 ms × ~1,800 passes/s), display ~11 s (31 ms × ~6 pushes/s); flood: receive ~3 ms/packet, Transport jobs 100–470 ms per run, LAN→WAN echo p50 ~750 ms (~200 ms idle) |
