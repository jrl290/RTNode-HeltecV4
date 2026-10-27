# RTNode Contract Tests

Read with [CORE_PRINCIPLES.md](CORE_PRINCIPLES.md) (the firewall rules),
[FIREWALL_ADDRESSES.md](FIREWALL_ADDRESSES.md) and
[PERFORMANCE_STRATEGY.md](PERFORMANCE_STRATEGY.md) (the budgets). This file
covers whether the node does what the apps on either side of it rely on:

- **The LAN routes freely.** Announces, path discovery, links and resources
  pass between every pair of LAN media: RTNode's local TCP server, Bluetooth
  (Prns native; Columba's protocol is covered by `tests/bench_ble_columba.py`)
  and LoRa.
- **The WAN firewall holds.** WAN packets without a whitelisted address die
  at reception; unsolicited WAN announces never reach the LAN; LAN traffic
  goes everywhere and teaches the whitelists; the replies it asked for get
  back in.
- **Links and resources work** across every crossing above, both ways, and
  keep working while the LAN churns.
- **WiFi, Bluetooth and LoRa coexist**: all three carry traffic at once and
  the node stays healthy.

Everything runs from `tests/bench_contracts.py`, which starts one
`tests/contract_agent.py` per endpoint. An agent is a Reticulum instance on
exactly one medium, driven over stdin with JSON commands; it owns an
announced destination and a *silent* one that is never announced, so the
only way to find the silent one is a path request that the agent answers.

## The bench

| Piece | What | Notes |
|---|---|---|
| Node | V4.2, `rtnode_heltec_v4_bench` | backbones off, local TCP server on 4242, TX power 2 dBm; Bluetooth on (the default). For the WAN suite, build with `PLATFORMIO_BUILD_FLAGS="'-DRTNODE_BENCH_WAN_HOST=\"<Mac IP>\"' -DRTNODE_BENCH_WAN_PORT=4290"` |
| Node log | host-timestamped serial log (`<time> <line>` per line) | `--node-log`; the WAN flood and the soak read the node's own `[RES]`, `[STALL]`, table and whitelist reports from it |
| TCP | agents on RTNode's local TCP server | `tcp`, `tcp2`, … (a trailing digit starts another agent on the same medium) |
| Bluetooth | prnsd 0.3.7 on the Mac, `tests/prnsd/config`; agent on its loopback port 4270 | start prnsd from Terminal: macOS gives Bluetooth only to apps it can ask |
| LoRa | stock RNode (Heltec V3, fw 1.86) on a serial port | 914.875 MHz, SF10, 125 kHz, CR 4/5, 2 dBm |
| WAN | stand-in backbone: `../.venv/bin/rnsd --config tests/wan-backbone` | one TCP server on 4290 (gateway mode, no ingress control) and nothing else, so no test traffic leaves the bench; the WAN agents connect to it too |

```
../.venv/bin/python tests/bench_contracts.py lan  --media tcp,prnsd,lora --rnode-port /dev/cu.usbserial-0001
../.venv/bin/python tests/bench_contracts.py wan  --media tcp,tcp2 --node-log <serial log>
../.venv/bin/python tests/bench_contracts.py soak --media tcp,prnsd,lora --rnode-port /dev/cu.usbserial-0001 \
    --node-log <serial log> --soak-s 1800
../.venv/bin/python tests/bench_lora_neighbour.py --node-log <serial log>
```

`bench_lora_neighbour.py` plays two LoRa devices in range of each other with
the one RNode: first as a Reticulum endpoint that announces, then raw over
KISS, sending what its neighbour would (a packet, a link request, a packet on
that link — none naming a next hop) and listening for RTNode sending any of
them again.

Results go to `tests/bench-results/<time>-contracts-<suite>/results.json`.

## The checks

**lan**, for every ordered pair of agents:

| Check | Passes when |
|---|---|
| announce | A's announce reaches B |
| path | A finds B's silent destination (skipped through prnsd unless `--prnsd-gateway`: see below) |
| link | A's Link to B carries 3 echoes (400 B; 200 B with LoRa) and a resource (20 kB; 1.5 kB with LoRa) that B receives intact (SHA-256) |
| packet | A sends B 3 single packets (no link; 10 s apart on LoRa, as an app sends messages); all arrive and all 3 proofs come back |

**wan**, with LAN agents `a`, `b` and a WAN agent `w`:

| Check | Passes when |
|---|---|
| wan-path-unmentioned | `w` finds no path to `a`'s silent destination: the request dies at the firewall |
| wan-announce-held | `w`'s announce reaches no LAN agent |
| lan-announce-out | each LAN agent's announce reaches `w` |
| lan-path-wan, lan-path-wan-silent | `a` finds `w`'s destinations, announced and silent |
| lan-path-lan | `a` finds `b`'s silent destination, which puts it on the whitelist |
| lan-link-wan, wan-link-lan | links both ways carry echoes and an intact resource |
| lan-packet-wan, wan-packet-lan | single packets both ways arrive and their proofs come back (through the reverse table) |
| wan-path-mentioned | `w` finds `b`'s silent destination once the LAN has mentioned it |
| wan-announce-known | `w`'s next announce reaches `a`, which asked for it |
| wan-link-survives-churn | after `b` announces 260 fresh destinations (more than the whitelists hold), open links still carry echoes, the WAN side speaking first as well as the LAN side |
| wan-reaches-quiet-lan | after the churn, `w` can still open a link to `a`, which has stayed quiet |
| wan-flood | a WAN agent announces fresh destinations at 20/s for 60 s: no LAN agent hears one, echoes on a LAN–WAN and a LAN–LAN link keep coming back (≥ 90 %), the node neither reboots nor panics |

**announce cap** (`tests/bench_announce_cap.py`), a LAN agent on the local TCP
server and a WAN agent on the stand-in backbone, with the node's serial log.
LoRa (under 50 kbps) sends a relayed announce only once the previous one has
had tx_time / 2 % to clear; the rest queue. LAN-side path requests are never
held back; backbone ones wait for the cap:

| Check | Passes when |
|---|---|
| cap-spacing | the LAN's five fresh announces leave LoRa at least a cap window apart (`--window`; 90 s for 183 B at 814 bps) |
| lan-preq-out | LAN path requests for unknown destinations go out on LoRa within 5 s while those announces are queued |
| wan-preq-held | a backbone path request for a whitelisted unknown destination is searched for (DISCOVERY-EXPIRED) but not sent on LoRa in that window |
| wan-uncapped | the backbone hears every flooded announce |
| node-stayed-up | the node neither panics nor restarts during the run |

**soak**, all at once for `--soak-s`:

| Flow | Traffic | Passes when |
|---|---|---|
| wifi<->bt | TCP ↔ Prns: echo every 1 s (400 B), resource every 60 s (20 kB) | loss ≤ 1 %, every resource intact |
| wifi<->lora | TCP ↔ LoRa: echo every 30 s (100 B) | loss ≤ 10 % |
| bt<->lora | Prns ↔ LoRa: echo every 60 s (100 B) | loss ≤ 10 % |
| wan<->wifi | WAN ↔ TCP: echo every 2 s (400 B), resource every 120 s (20 kB) | loss ≤ 1 %, every resource intact |
| soak-node | the node's own reports | no reboot, no panic |

At SF10 an echo round over LoRa (request, proof, reply) is about 5 s of
airtime; the two LoRa flows keep the channel about a quarter busy.

## What is not an RTNode fault

- **prnsd does not look for unknown paths in full mode.** A transport node
  forwards a request for a destination it does not know only when it arrived
  on a gateway-mode interface (RNS `DISCOVER_PATHS_FOR`; Prns
  `recursively_forwards_unknown_paths`). `tests/prnsd/config` sets `mode =
  gateway`; a prnsd started with it can run the path checks through Bluetooth
  (`--prnsd-gateway`). The prnsd on the bench on 2026-09-28 ran an older copy
  of the config, so those checks were skipped. Bluetooth path discovery
  itself was shown with Columba phones (b0c7a86).
- **RNS 1.5.2 holds repeat path requests for 45 s.** A transport node that
  has passed on a request for a destination batches further requests for it
  until an answer comes or `PATH_REQUEST_GATE_TIMEOUT` (45 s) passes. LAN
  path requests reach the stand-in too (LAN packets go everywhere) and the
  node answers only the LAN requester, so a WAN request for the same
  destination within 45 s — up to 50 s, as the gate entry goes at the next
  5-second table cull — never reaches the node. `wan-path-mentioned` waits
  55 s.
- **LoRa is half duplex.** A step that starts while the last one's frames
  (a link close, a late proof) are still on air collides with them. The
  harness leaves `--lora-settle` (5 s) of quiet after each LoRa step; a lost
  single frame still fails a check, as it would for an app.

## Found and fixed, 2026-09-28

All on `bench/instrumentation`, found by these suites:

1. **Link proofs sent back the way they came** (51b9262). A firewall-mode
   block sent every proof on a link to the link's outbound interface, in
   either direction, before link transport forwarded it properly. On LoRa a
   destination's delivery proof went back on air, colliding with its next
   frame; TCP → LoRa lost 2 of 3 echoes.
2. **Link requests forwarded twice** (21292d6). A firewall-mode block sent
   every LAN link request again, raw, and replaced the link entry forwarding
   had made. Over LoRa the copy went on air while the destination was
   sending its link proof: TCP → LoRa links failed outright.
3. **Split LoRa packets glued to other packets' bytes** (a6d2e37). A second
   half with no first half was taken as a first half, and `loop()` staged
   every received packet through the reassembly buffer. A 275-byte echo
   arrived with its first 21 bytes from the proof before it, typed as a
   proof. `[RES]` now counts discarded halves (`split_drop`).
4. **In-use addresses evicted from the whitelists** (7b682b2). The lists
   were culled oldest-added first at 200 and a hit never refreshed an entry:
   after 260 fresh LAN announces, WAN-to-LAN echoes on an open link went
   from 3/3 to 0/3. Hits now refresh an entry, and link ids in the link
   table count as known.
5. **Busy paths evicted from a 24-entry table** (9afcd4e). Eviction went by
   bitrate over hops, so LoRa destinations went first and multi-hop WAN ones
   next; the churn evicted the path to a WAN endpoint in use and the next
   link to it failed. Eviction now goes by last use, and with Reticulum's
   heap in PSRAM (V4) the table holds 256.

6. **Backbone proofs skipped the whitelist** (5eb13db). WAN proofs
   were exempt, so an unrelated one reached transport processing and the
   packet hashlist, and its destination was added to WL#2 — a WAN peer could
   churn the whitelist with junk proofs. A WAN proof now passes only when it
   answers this node's traffic (a link in the link table, a packet in the
   reverse table, one of its own receipts) and seeds nothing.
   `lan-packet-wan` covers the reverse-table case.

7. **Traffic between direct LoRa neighbours repeated** (62747ca). Reticulum
   names the next hop in every packet but the last; on the last hop the
   destination is in range and nobody should relay. FWD-CHECK (v1.0.41)
   relayed such packets whenever the node had a path — back out of the
   interface they came in on. Two LoRa devices talking directly had every
   packet and link request sent again, and every packet of their link.
   `bench_lora_neighbour.py` showed all three on air, and passes now.

## Found and fixed, 2026-10-24

**The temperature read hung the radio** (`init_pmu()` in `Power.h`). Every
ESP32-S3 build read the chip's temperature sensor once a second
(`update_pmu()` → Arduino's `temperatureRead()`), although firewall mode
sends no KISS frames and so never reports it. The radio code (the PHY) reads
the same sensor on core 0, about 1.5 times a second on the bench, for WiFi's
TX power control and Bluetooth's clock tracking; it holds `rtc_spinlock`
with interrupts off while it waits for the conversion. `temperatureRead()`
configures and restarts the sensor on every call, and `temp_sensor_set_config()`
and `temp_sensor_start()` rewrite the sensor's control register without that
lock. One landing inside the PHY's wait clears the conversion bit; the
conversion never finishes and the interrupt watchdog reboots the node. The
V4.2 bench build rebooted this way once in about 50 minutes with WiFi on.
Firewall builds no longer start the sensor; RNode builds are unchanged.

The panic shows it: core 0 in `temperature_sensor_get_raw_value()` polling
the ready bit, with `SENS_SAR_TCTRL` = `0x00c19083` (sensor powered, ready
clear, conversion bit cleared), and core 1 in `temperatureRead()` waiting
for the lock. A test build reproduced it (local branch `scratch/temp-read-hammer`, not pushed). It
counts raw reads per core (`-Wl,--wrap=temp_sensor_get_raw_value`) and runs
`temperatureRead()`'s unlocked calls back to back on core 1, about 30,000 a
second. It hung four times in 11 minutes of running (5 to 526 s after each
boot's start) with the same register value, two through WiFi and two
through the Bluetooth controller, so either radio alone can hit it. Reading
the temperature on every loop pass (~1,270 a second) did not reproduce it in
53 minutes: the window is a few instructions wide.

## Open

- **Loop time.** `[STALL]` and `[RES]` now attribute time to `rx` (Reticulum
  processing a received packet), `jobs` (`reticulum.loop()`: Transport jobs,
  every interface's `loop()`, the filesystem), `ble` and `display` (measured
  2026-09-28 on the V4.2 bench build):
  - the display costs 31 ms per push, about 6 pushes a second: ~11 s of
    every minute on the loop core, and any packet can wait up to 31 ms
    behind it;
  - `reticulum.loop()` takes ~25 s of every minute even idle — ~0.24 ms on
    each of ~1,800 passes a second, with Transport's periodic jobs on top
    (100–470 ms per run under churn, most of the once-a-minute stall
    alongside ~0.5 s of path-persistence flash writes);
  - under a 20/s WAN announce flood, receive processing is ~3 ms per packet
    and the loop's p50 LAN → WAN echo RTT rises from ~200 ms to ~750 ms.
  Nothing is lost and the soak passes, but the loop is far over its 50 ms
  budget. Slow passes now say where they went — `[RLOOP]` (a
  `reticulum.loop()` pass of 100 ms or more: housekeeping, interface loops,
  filesystem, Transport), `[JOBS]` (a Transport jobs run of 100 ms or more,
  by section) and `[RJOBS]` (cache clean or persist of 50 ms or more) — and
  under the WAN suite they show three things:
  - the TCP interfaces processed a whole burst of received packets in one
    pass (up to 360 ms under the flood) while the other radios waited; a
    budget of four frames per client per pass (f666ba6) brought the worst
    pass to ~210 ms — what is left is single packets that are expensive on
    their own;
  - releasing one held announce costs ~100 ms (verification and path work),
    every 5 s during a burst;
  - the once-a-minute stall was `persist_data()` (0.3–1.2 s) plus cache
    clean (up to 0.2 s) plus the table cull (0.1–0.17 s). Paths are now
    saved an hour after start and then daily, the clock offset hourly
    (ba3219e): no flash at all after the startup minute, and the idle loop
    maximum is ~150 ms — the table cull — instead of 1–2.6 s.
  The display question stands too: does it need 6 pushes a second?
- **A quiet backbone is re-dialled every 2 minutes.** The backbone
  connection drops itself after 120 s without receiving anything
  (`TCP_IF_READ_TIMEOUT`) and reconnects. Busy public backbones never go
  quiet that long, but the bench stand-in does, and so could a small
  community one; anything crossing at that moment is lost. Dead peers are
  already caught by TCP keepalive on that socket (10 s idle, 3 probes 5 s
  apart; lwIP has `LWIP_TCP_KEEPALIVE`), which is all Python RNS uses. A
  different value would break DESIGN_PRINCIPLES.md §4; the choice is whether
  to rely on keepalive alone — for James, as this area has had half-open
  regressions before.
- **The Prns peer dropped and did not come back** (2026-09-28 17:55):
  `left (reason 0x208)` — an HCI connection timeout, the radio link to the
  Mac silent — after 571 s, and prnsd had not reconnected minutes later
  while the node advertised a free slot. The earlier prnsd stayed connected
  for hours of tests. Not yet known whether the Mac, prnsd or the node.
- **Whitelists are 200 entries with a linear search.** In use they no longer
  evict, but a LAN with more than 200 addresses active at once would push
  idle ones out, and the search cost grows with the cap. A set would allow a
  larger cap on PSRAM boards.
- **README's interface modes are stale**: it says LoRa is `MODE_GATEWAY`
  and the backbone `MODE_BACKBONE`; the firmware sets every interface to
  `MODE_FULL`.
- **A flood fills LoRa's announce queue.** The queue keeps the newest
  announce per destination, 64 destinations at most, sent fewest hops then
  oldest first, one per cap window (90 s at SF10). After a LAN flood (the wan
  suite's 260-announce churn) a real LAN device's announce can wait for LoRa
  behind up to ~96 minutes of stale ones, or miss LoRa while the queue is
  full; other interfaces still get it at once. The
  reference does the same with a larger queue; nothing seen in use needs
  more.

## Results log

| Date | Firmware | Suite | Result | Notes |
|---|---|---|---|---|
| 2026-09-28 | cc6371e | lan tcp,prnsd | 4/6 | paths through prnsd unanswered: prnsd in full mode (above) |
| 2026-09-28 | cc6371e | lan tcp,prnsd,lora | 12/14 | TCP → LoRa echoes 1/3, LoRa → prnsd 0/3: fixes 1 and 3 |
| 2026-09-28 | + 51b9262, a6d2e37 | lan tcp,prnsd,lora | 11/14 | TCP → LoRa and LoRa → TCP links failed: fix 2, and the harness's own collision after a link close (settle time) |
| 2026-09-28 | + 21292d6 | lan tcp,prnsd,lora | **14/14** | LoRa echo RTT p50 8.1–8.8 s (SF10), 1.5 kB resources in 21–23 s |
| 2026-09-28 | 21292d6 + stand-in backbone | wan tcp,tcp2 | 14/15 | wan-path-mentioned: the RNS path gate (above); flood PASS, 1201 announces, none leaked |
| 2026-09-28 | same | wan tcp,tcp2 | 14/16 | wan-link-survives-churn (WAN first) 0/3: fix 4; flood phase's LAN–WAN link failed to open: fix 5 |
| 2026-09-28 | + 7b682b2, 9afcd4e | wan tcp,tcp2 | **16/16** | flood: 1201 announces, 0 leaked; echoes LAN–LAN 41/41 (p50 310 ms), LAN–WAN 33/33 (p50 753 ms); heap min 118 KB; stall max 2.0 s |
| 2026-09-28 | same | soak tcp,prnsd,lora + wan, 30 min | **5/5** | WiFi↔BT 1643/1643 echoes (p50 380, p95 614 ms), 29/29 resources; WAN↔WiFi 891/891 (p50 236, p95 602 ms), 14/14 resources; WiFi↔LoRa 59/60 and BT↔LoRa 29/30 (p50 5.4 s); no reboot or panic, heap min 118 KB, stall max 992 ms, no Bluetooth disconnect, no LoRa queue or split drops |
| 2026-09-28 | + backbone proof filter (5eb13db), loop-time causes | wan tcp,tcp2 | **18/18** | with lan-packet-wan and wan-packet-lan (3/3 arrived, 3/3 proved each way); flood: 1201 announces, 0 leaked, LAN–WAN echo p50 752 ms |
| 2026-09-28 | same | lan tcp,prnsd,lora | **20/20** | with single packets on every pair (3/3 arrived, 3/3 proved, LoRa included); LoRa echo RTT p50 7.8–9.8 s |
| 2026-09-28 | + slow-pass lines (202ea41), TCP frame budget (f666ba6) | wan tcp,tcp2 | **18/18** | flood: LAN–LAN echo p50 262 ms (was ~300); LAN–WAN p50 ~860 ms, dominated by the WAN test agent processing the whole flood itself |
| 2026-09-28 | f666ba6 | lan tcp,prnsd,lora | **20/20** | final regression |
| 2026-09-28 | 935b9e6 | lora neighbour | FAIL | RTNode sent again a data packet, a link request and a link packet between direct neighbours |
| 2026-09-28 | + 62747ca, ba3219e | lora neighbour | **PASS** | none repeated; no link entry made |
| 2026-09-28 | same | lan tcp,prnsd,lora `--prnsd-gateway` | 22/24 | Bluetooth paths through prnsd pass (tcp→prnsd 0.45 s, prnsd→tcp 0.29 s, lora→prnsd 4.4 s); prnsd→lora held by prnsd's path gate after another agent's request (checks now use a fresh destination); lora→prnsd packets lost with the Prns peer, which dropped (0x208) |
| 2026-09-28 | same | wan tcp,tcp2 | **18/18** | flood LAN–LAN echo p50 199 ms |
| 2026-10-19 | 8b29734, R8 bench build (`rtnode_heltec_v4_r8` + RTNODE_BENCH flags) on a V4 R8 (ESP32-S3R8, 8 MB octal PSRAM) | wan tcp,tcp2 | **18/18** | first run on an R8; flood: 1201 announces, 0 leaked, LAN–LAN echo p50 186 ms, LAN–WAN p50 495 ms; no reboot or panic, heap min 157 KB, stall max 255 ms. Before 8b29734 the R8 was detected as a GC1109 (its CSD reads LOW) |
| 2026-10-19 | same | lan tcp,tcp2,tcp3 | **24/24** | no LoRa radio or prnsd on the bench this time; stalls up to 3.0 s in TCP writes during the 20 kB resources (as on the V4.2) |
| 2026-10-19 | 8b29734 release R8 build, saved settings 921.125 MHz SF9 CR 4/7 10 dBm | LoRa probe to a second RTNode (rnstransport.probe, through the R8's TCP server) | **20/20** | proofs back over LoRa, RTT p50 2.1 s, max 4.2 s (first, just after the far node restarted); neighbour heard at −61 dBm (nodes close together, so range is not measured); CSMA held one send while the channel was busy; loop max 143 ms, no queue drops |
| 2026-10-19 | v1.0.52 release image (`Release/v1.0.52/rtnode_heltec_v4_r8.bin`, app-only update) on the R8 | LoRa probe to the second RTNode | **10/10** | RTT p50 2.1 s, max 2.3 s; KCT8103L selected, Bluetooth up, local TCP server on 4242, port 7633 closed |
| 2026-10-24 | ce1232a (LoRa bitrate + 2 % announce cap) alone, on main 7ceb826; bench build on the V4.2, saved LoRa 914.875 MHz SF10 125 kHz CR 4/6 (814 bps) | announce cap (scratch version of the script) | LAN held back | relayed announces 90 s apart, but both LAN path requests made while announces were queued never reached LoRa (the one before the flood did) |
| 2026-10-24 | + 4cfa5d4 (LAN-side path requests skip the cap; 3 h queue life) | announce cap | 4/5 | the node panicked 244 s in (the temperature read, see Found and fixed, 2026-10-24), so the spacing check saw its boot announce; the path request checks passed before it |
| 2026-10-24 | same | announce cap | **5/5** | announces left LoRa 90.3, 90.3, 89.3 s apart; LAN path requests out on LoRa in 0.4–0.5 s while announces were queued; the backbone's request for a whitelisted destination was searched for and kept off LoRa; the backbone heard 5/5 flooded announces (spread over 30 s by ingress control) |
| 2026-10-24 | same | wan tcp,tcp2 | **18/18** | flood: 1201 announces, 0 leaked, LAN–LAN echo p50 223 ms, LAN–WAN p50 493 ms; no reboot or panic, heap min 126 KB, stall max 292 ms; during the 260-announce churn LoRa sent three relayed announces 90 s apart and dropped nothing |
| 2026-10-24 | same | lan tcp,tcp2,tcp3 | **24/24** | no LoRa agent or prnsd on the bench |
| 2026-10-24 | same, R8 bench build (`rtnode_heltec_v4_r8` + RTNODE_BENCH flags) on the staging V4 R8 (ESP32-S3R8, MAC 14:c1:9f:ce:1b:e8) on the M5, saved LoRa 914.875 MHz SF10 125 kHz CR 4/6 | announce cap | **5/5** | KCT8103L detected, 8184 KB PSRAM, Bluetooth up; announces left LoRa 90.5, 90.5, 89.6 s apart; LAN path requests out on LoRa in 0.9–1.1 s; the backbone's request held and searched for; backbone heard 5/5; serial log streamed over ssh from the M5 |
| 2026-10-24 | same | wan tcp,tcp2 | **18/18** | flood: 1201 announces, 0 leaked, LAN–LAN echo p50 181 ms, LAN–WAN p50 423 ms; no reboot or panic, heap min 151 KB, stall max 340 ms |
| 2026-10-24 | same | lan tcp,tcp2,tcp3 | **24/24** | |
| 2026-10-24 | same | announce cap, again | 4/5, not a cap fault | the M5 lost its USB link to the R8 151 s in (the M5 on battery at 7 %); up to then announces 89.6 s apart, LAN path requests out in 0.5 s, the backbone's held; cap-spacing saw only the logged part |
| 2026-10-24 | same, after the M5 was back on power | announce cap, again | **5/5** | announces 90.6, 89.5, 90.5 s apart; LAN path requests out in 0.1 s; the backbone's held and searched for; backbone heard 5/5. The R8 was then put back on this branch's plain `rtnode_heltec_v4_r8` build (saved settings, backbone to the staging Pi) |
| 2026-10-24 | 7ceb826 + temperature read on every loop pass (`scratch/temp-read-hammer` 1731158), V4.2 bench build, WiFi on | lan tcp,tcp2 back to back for 20 min, then 20 min idle | 48/48 runs 8/8, **no panic** in 53 min | the race's window is a few instructions wide; not reproduced this way |
| 2026-10-24 | + 21ce5ea: `temperatureRead()`'s unlocked calls back to back on core 1, raw reads counted per core | idle on WiFi, 13 min | **4 panics** (5–526 s after each start) | the stock build's signature: core 0 in the PHY's sensor read, `SENS_SAR_TCTRL` 0x00c1908x; two through WiFi TX power control, two through Bluetooth's clock tracking; the PHY read ~1.5/s |
| 2026-10-24 | the fix (ab3d2cc; 9a077fd before its merge), V4.2 bench build, WiFi on | lan tcp,tcp2 back to back for 20 min, then 20 min idle | 47/47 runs 8/8, **no panic** | |
| 2026-10-24 | same | wan tcp,tcp2 | **18/18** | flood: 1201 announces, 0 leaked, LAN–LAN echo p50 190 ms, stall max 270 ms, heap min 105 KB |
| 2026-10-24 | same | lan tcp,tcp2,tcp3 | **24/24** | 47 min up, no restart |
| 2026-10-24 | bb4794b (main with the announce cap rework and the temperature fix), V4.2 bench build | announce cap | **5/5** | announces 90.4, 89.4, 90.4 s apart; LAN path requests out in ~1 s; the backbone's held and searched for; backbone heard 5/5 |
| 2026-10-24 | same | wan tcp,tcp2 | **18/18** | flood: 1201 announces, 0 leaked, LAN–LAN echo p50 201 ms, LAN–WAN p50 414 ms; no reboot or panic, heap min 124 KB |
| 2026-10-24 | same | lan tcp,tcp2,tcp3 | **24/24** | |
