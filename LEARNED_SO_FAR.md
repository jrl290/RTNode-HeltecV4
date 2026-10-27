# Learned So Far

## Current Hardware

- Current attached standard-RNode devices are:
  - Heltec V4.3-class board, serial `00:00:00:0b`, at `/dev/cu.usbmodem11101`
  - Heltec V4.2-class board, serial `00:00:00:0a`, at `/dev/cu.usbmodem114401`
- The V3 has been removed from the active test path.
- The current two-board baseline is V4.2 RNode <-> V4.3 RNode, not V3-based.
- RTNode does not expose KISS serial access in FIREWALL_MODE. Serial is useful for boot/runtime logs only.
- Use PlatformIO (`pio`) for flashing RTNode changes. Do not rely on interactive terminal prompts.

## Firmware/Test Baseline

- Built target: `rtnode_heltec_v4`.
- Flash to RTNode succeeded with `pio run -e rtnode_heltec_v4 -t upload --upload-port /dev/cu.usbmodem114401`.
- Post-upload `rnodeconf` provisioning/hash steps failed to detect the device. This is expected with RTNode FIREWALL_MODE serial behavior and did not indicate a failed flash.
- Monitor needed explicit inactive control lines to see boot output:
  - `pio device monitor -p /dev/cu.usbmodem114401 -b 115200 --filter direct --dtr 0 --rts 0`
- A diagnostics firmware build has now been flashed. It adds visible `LOG_VERBOSE` markers for link and proof decisions:
  - `[LINK] XPORT`, `[LINK] FWD`, `[LINK] DROP`
  - `[LRPROOF] IN`, `[LRPROOF] FWD`, `[LRPROOF] DROP ...`, `[LRPROOF] LOCAL-CHECK`
  - `[PROOF] XPORT`, `[PROOF] DROP ...`, `[PROOF] LOCAL`
- Diagnostics build and flash both succeeded. The same expected post-upload `rnodeconf` detect/hash failures occurred.

## RTNode Boot State Observed

- RTNode boots successfully.
- Active LoRa channel from EEPROM:
  - Frequency: `914875000`
  - Bandwidth: `125000`
  - Spreading factor: `10`
  - Coding rate: `5`
  - TX power: `28`
- LittleFS mounted and `destination_table` read as `{}` after flash/test boot.
- RNS transport starts and loads transport identity.
- LoRa startup announce is queued and reaches actual TX start:
  - `[LoRa] TX 167 bytes`
  - `[LoRa] TXSTART 167 bytes`
- Local TCP server starts on port `4242` and mDNS advertises `mynode.local`.
- Boot check after flashing diagnostics also succeeded with the same channel settings and startup `TXSTART` behavior.

## Hardware Test Results

Command used:

```sh
.venv/bin/python -m pytest tests/lora_test.py \
  --rtnode-port /dev/cu.usbmodem114401 \
  --rnode-port /dev/cu.usbmodem11201 \
  --lora-freq 914875000 \
  --lora-bw 125000 \
  --lora-sf 10 \
  --lora-cr 5 \
  --lora-txp 28 \
  --rx-timeout 20 \
  --announce-timeout 90 \
  -v
```

Results:

- PASS: RTNode alive/log output.
- PASS: RNS active.
- PASS: No error logs during quiet observation.
- PASS: RTNode channel config matches test parameters.
- PASS: RNode -> RTNode single receive test.
- PASS: RNode -> RTNode multiple receive test.
- FAIL: RTNode -> RNode transmit decode test.

Important interpretation:

- RNode -> RTNode passing means the RTNode receive path and shared channel parameters are good enough for over-the-air reception.
- RTNode logs `TXSTART`, so RTNode queues and begins RF transmission.
- The RNode probe did not decode the RTNode startup packet within the test window. That points at RTNode transmit-side framing, radio TX behavior, test/probe receive assumptions, or RNode promiscuous decode compatibility.

## Current User-Reported Problem

The practical failure is not just the startup TX probe test.

Scenario:

- A LoRa client sends an LXMF message through RTNode to a TCP client on the other side.
- The TCP client actually receives the LXMF message.
- The LoRa client does not receive or does not recognize the proof.
- Messages sent from the TCP client back toward the LoRa client are not received.
- Other functionality is currently unknown.

Working hypothesis from the symptom:

- LoRa -> RTNode -> TCP forwarding is at least partially working for LXMF data.
- The broken direction is TCP/local-client -> RTNode -> LoRa, or the proof/link-return path is being dropped, malformed, filtered, routed to the wrong interface, or transmitted in a way the LoRa client cannot decode.
- This aligns with the hardware test asymmetry: RTNode can receive LoRa packets, but RTNode-originated/forwarded LoRa transmissions are not confirmed as decoded by the RNode probe.

## Code Areas Already Identified As Relevant

- `RNode_Firmware.ino`
  - `LoRaInterface::send_outgoing()` queues RNS packets for LoRa TX.
  - `transmit(uint16_t size)` adds RNode LoRa framing and writes to the SX126x.
  - FIREWALL_MODE adds RX raw-shift handling and verbose `[LoRa] RX/TX/TXSTART` diagnostics.
- `lib/microReticulum/src/Transport.cpp`
  - `Transport::outbound()` returns false when no OUT interface accepts a packet, causing `No interfaces could process the outbound packet`.
  - `path_request()` and immediate PATH_RESPONSE logic are relevant to proof/link return behavior.
  - FIREWALL_MODE whitelists and local-client interface logic are relevant to TCP -> LoRa return traffic.
- `tests/lora_test.py`
  - Current hardware tests prove RTNode RX and expose the current RTNode TX decode failure.

## Next Investigation Steps

- Reproduce the actual LXMF/TCP proof failure with logs showing packet type/context/interface on both directions.
- Determine why RTNode LoRa TX reaches `TXSTART` but is not decoded by the standard RNode.
- Inspect low-level SX1262 TX parameters, FIFO/base-address state, packet header mode, CRC/IQ settings, and timing against upstream RNode firmware.

## 2026-05-09 Radio TX Diagnostic Update

- RTNode Heltec V4 auto-detects the front-end module as KCT8103L:
  - `[RTNode] PA detect: model=KCT8103L`
- Restored runtime KCT/GC FEM detection under `FIREWALL_MODE`; the earlier forced-GC1109 diagnostic was wrong for this physical board.
- Removed the temporary 14 dBm cap and confirmed the full-power mapping:
  - `[RTNode] TXP: requested=28 effective=28 modem=17 pa=2`
- Added a TX completion diagnostic in `sx126x::endPacket()`:
  - Startup announce and LoRa path-response transmissions both reported `[RTNode] TXDONE irq=0001 timeout=0 pa=2`.
  - This means the SX1262 reports TX_DONE; RTNode is not just entering `transmit()` and hanging.
- During a LoRa path request, the standard RNode reported strong interference around the RTNode response window (`-27 dBm`) but still did not decode a packet.
  - This strongly suggests RF is being emitted, but the RNode cannot demodulate the RTNode transmission.
- Added a non-invasive SX1262 TX profile diagnostic for the next flash:
  - `[RTNode] SXPA ...` logs PA config, OCP, and SX1262 TX parameter bytes when TX power is set.
  - `[RTNode] TXCFG ...` logs the active frequency, SF, BW code, CR code, LDRO, preamble, header mode, payload length, CRC, IQ mode, and modem TX power at each transmit.
- First capture with this diagnostic found a real TX-power application bug:
  - Boot showed `[RTNode] TXP: requested=28 effective=28 modem=17 pa=2`, but transmit showed `[RTNode] TXCFG ... txp=2` and only `[RTNode] SXPA ... tx=0202`.
  - Cause: `FIREWALL_MODE` bypassed RNode provisioning and left `model = 0x00`; `Utilities.h::setTXPower()` calculates the mapped power but only applies it in model-specific branches such as `MODEL_C8`.
  - Fixed by setting `model = MODEL_C8` during the Heltec V4 `FIREWALL_MODE` provisioning bypass before `startRadio()`.
  - Verification after flash showed `[RTNode] SXPA ... tx=1102` and `[RTNode] TXCFG ... txp=17`, so the SX1262 now applies the mapped modem output power.
  - A fresh promiscuous RNode sniff after this fix still decoded no RTNode startup packet, so TX power application was a real bug but not the complete demodulation failure.
- Added another read-only PHY diagnostic for the next flash:
  - `[RTNode] DIO2RF=1` confirms the SX1262 DIO2 RF-switch opcode is enabled.
  - `[RTNode] SXFREQ ...` logs the exact RF frequency opcode bytes.
  - `[RTNode] SXSYNC ...` reads back the SX1262 sync word registers after programming.
- Tried disabling the SX1262 DIO2 RF-switch opcode only for Heltec V4 `FIREWALL_MODE`, while retaining manual KCT CSD/CTX control.
  - Result: `[RTNode] DIO2RF=0 diagnostic`, valid TXCFG/TXDONE, but the RNode still decoded nothing.
  - Reverted to documented `DIO2RF=1` behavior after the failed diagnostic.
- Tested these KCT/PA variants with an armed promiscuous RNode sniffer during RTNode boot TX; all still decoded nothing:
  - Correct KCT path, CSD high, CTX high for TX.
  - Forced/inverted CTX low during TX.
  - CSD low diagnostic.
  - GPIO46/CPS forced high diagnostic.
  - Full power (`28 dBm`) and very low diagnostic cap (`2 dBm`).
  - SX126x IQ register write to `0x0736` re-enabled.
  - Explicit inverted IQ packet parameter (`buf[5] = 0x01`).

Current interpretation:

- The remaining failure is most consistent with a LoRa modem parameter/PHY mismatch, not RNS transport, packet bytes, path/proof logic, or absence of RF TX.
- Next cheap discriminator is explicit IQ inversion in packet params (`buf[5]`) and then deeper SX1262 TX/RX register/status inspection.

## 2026-05-09 Proof Probe Findings

Added `tests/proof_probe.py`, a noninteractive minimal Reticulum proof probe using local source imports:

```sh
PYTHONPATH=/Users/james/Offline/Reticulum/Reticulum-master \
  /Users/james/Offline/Reticulum/RTNode-HeltecV4/.venv/bin/python \
  /Users/james/Offline/Reticulum/RTNode-HeltecV4/tests/proof_probe.py ...
```

LoRa client -> TCP server probe:

- TCP server announces destination `2fcd02e9...` through RTNode TCP.
- RTNode receives/stores the TCP announce and repeatedly queues LoRa transmissions for it.
- LoRa client sends a path request; RTNode logs:
  - `[PATH] REQ dst=2fcd02e9 from=Interface[LoRaInterface]`
  - `[PATH] RESP dst=2fcd02e9 hops=0 to=Interface[LoRaInterface]`
  - `[LoRa] TXSTART 183 bytes`
- LoRa client still fails with `CLIENT_FAIL no path after 75.0s`.

TCP client -> LoRa server probe:

- LoRa server announces destination `1607e2d5...` through the standard RNode.
- RTNode receives/stores LoRa announce from `Interface[LoRaInterface]`.
- TCP client learns path via RTNode, sends data, and RTNode logs TCP packet forwarded to LoRa:
  - `[PKT] IN iface=Interface[LocalTcpInterface] ... dst=1607e2d5 tid=11a63924`
  - `[LoRa] TX 131/147 bytes`
  - `[LoRa] TXSTART 131/147 bytes`
- LoRa server never logs `SERVER_RX`; TCP client times out waiting for proof.

Interpretation:

- The minimal proof failure reproduces without LXMF complexity.
- Transport path/proof forwarding is no longer the primary suspect for this minimal repro.
- Current failure is below or at the RTNode LoRa TX layer: RTNode-originated/forwarded LoRa frames produce `TXSTART` but are not decoded by the standard RNode.

## 2026-05-09 Radio Diagnostics Tried

## 2026-05-09 Three-Device Cross-Listen Discriminator

- All three devices were attached simultaneously:
  - V4 RTNode: `/dev/cu.usbmodem11101`
  - V3 RTNode: `/dev/cu.usbserial-0001`
  - Standard RNode probe: `/dev/cu.usbmodem11201`
- Port mapping was confirmed from boot diagnostics:
  - V3 reports `txp=22`, `pa=0`, `ocp=18`, `tx=1602`.
  - V4 reports `txp=28`, `PA detect: model=KCT8103L`, `pa=2`, `ocp=28`, `tx=1102`.
- Cross-listen results:
  - V4 TX -> V3 listener: V4 logged one `[LoRa] TXSTART 167 bytes` and `TXDONE`; V3 logged `[LoRa] RX 167 bytes`, a valid LoRaInterface packet, valid announce, and path storage.
  - V3 TX -> V4 listener: V3 logged `[LoRa] TXSTART 167 bytes` and `TXDONE`; V4 logged `[LoRa] RX 167 bytes`, a valid LoRaInterface packet, valid announce, and path storage.
- Three-device side-by-side result while V4 transmitted:
  - V4 source: `v4_txstart=1`.
  - V3 listener: `v3_lora_rx=1` with valid announce/path storage.
  - Standard RNode probe: `rnode_packets=0`.
- Capturing every KISS frame from the standard RNode during the V4 TX window showed only `CMD_STAT_CHTM` telemetry/config frames and no `CMD_DATA`, `CMD_STAT_RSSI`, or `CMD_STAT_SNR` receive indication for the RTNode packet.
- New interpretation:
  - RTNode-originated SX1262 LoRa frames are valid enough for another RTNode/SX1262 receiver to demodulate and hand to RNS.
  - The failure is now narrowed away from generic RTNode TX absence, V4-only KCT PA behavior, and RNS transport/path/proof logic.
  - The remaining suspect is an interoperability mismatch between RTNode/SX1262 TX behavior and the current standard RNode probe's receive modem/firmware behavior, or a configuration/receive-side issue specific to that standard RNode probe.
  - This explains the original symptom: RTNode sends the LoRa return/proof path, but the LoRa client behind the standard RNode never decodes it.

## 2026-05-09 Heltec V3 A/B Test

- Heltec V3 RTNode was attached at `/dev/cu.usbserial-0001` and flashed with target `rtnode_heltec_v3`.
- The `FIREWALL_MODE` provisioning-bypass `model = MODEL_C8` fix was extended to Heltec V3 as well as V4, because V3 otherwise leaves `model = 0x00` and `setTXPower()` would calculate but not apply SX1262 TX power.
- V3 boot/TX diagnostics after flash:
  - `[RTNode] LoRa: freq=914875000 bw=125000 sf=10 cr=5 txp=22`
  - `[RTNode] SXSYNC requested=1424 reg=1424`
  - `[RTNode] DIO2RF=1`
  - `[RTNode] SXFREQ hz=914875000 reg=392e0000`
  - `[RTNode] TXP: requested=22 effective=22 modem=22 pa=0`
  - `[RTNode] SXPA pa=04070001 ocp=18 tx=1602`
  - `[RTNode] TXCFG ... txp=22`
  - `[RTNode] TXDONE irq=0001 timeout=0 pa=0`
- Hardware test against the standard RNode at `/dev/cu.usbmodem11201`:
  - PASS: V3 RTNode alive/RNS/channel config.
  - PASS: RNode -> V3 RTNode receive, including multiple packet receive test.
  - FAIL: V3 RTNode -> RNode receive; V3 logs `[LoRa] TXSTART`, but the RNode receives no decoded packet.
- Interpretation:
  - The same asymmetric result occurs on V3 and V4: RTNode can receive LoRa from the standard RNode, but the standard RNode does not decode RTNode-originated LoRa TX.
  - This weakens the earlier V4.3/KCT8103L-specific hypothesis and points toward common SX1262 TX setup/framing, common RTNode firmware behavior, or an unverified receive-side issue on the standard RNode probe.
  - A stronger discriminator would require either the V4 and V3 attached simultaneously, or a second known-good standard RNode receiver, to determine whether RTNode-originated packets are undecodable by all receivers or only by the current standard RNode probe.

- TX power cap diagnostic in `Utilities.h::setTXPower()`:
  - EEPROM requested `txp=28`.
  - Firmware capped effective output to `14` dBm for `BOARD_HELTEC32_V4` + `FIREWALL_MODE`.
  - Boot log: `[RTNode] TXP: requested=28 effective=14 modem=1 pa=2` for auto-detected KCT8103L.
  - Result: no improvement; LoRa client still did not learn the path.
- Forced GC1109 FEM path diagnostic in `sx126x.cpp`:
  - Boot log: `[RTNode] PA diagnostic: forcing GC1109 path` and `pa=1`.
  - Result: no improvement; LoRa client still did not learn the path.
- Disabled the fork's SX1262 IQ-register write after `SetPacketParams`:
  - The fork wrote register `0x0736` after every packet-params update; upstream RNode firmware does not.
  - Result: no improvement; LoRa client still did not learn the path.
- Disabled the fork's SX1262 `optimizeModemSensitivity()` register write:
  - The fork wrote register `0x0889` for non-500 kHz bandwidths; upstream RNode firmware leaves this function empty.
  - Result: no improvement; LoRa client still did not learn the path.

Current diagnostic firmware state:

- RTNode is flashed with the restored diagnostic baseline, not the failed TX power cap or forced-GC1109 experiments.
- Current baseline keeps normal TX power, runtime KCT8103L PA/FEM detection, normal IQ packet params, documented `DIO2RF=1`, and the `model = MODEL_C8` fix so `setTXPower()` actually applies the mapped SX1262 TX power.
- Useful guarded diagnostics remain: DIO2RF, sync word, RF frequency opcode bytes, PA/TX parameter bytes, TXCFG, TXDONE, and LoRa RX/TX/TXSTART logs.
- Post-upload PlatformIO `rnodeconf` provisioning/hash steps are disabled because RTNode FIREWALL_MODE serial is logs-only, not KISS, and these checks only add expected noise after a successful flash.

2026-05-12 follow-up diagnostics:

- Standard RNode probe identity:
  - Port: `/dev/cu.usbmodem11201`
  - Product: Wio Tracker L1 862-930 MHz
  - Firmware: 1.85
  - Modem: SX1262
  - Max TX power: 22 dBm
- Forcing the probe to explicit LoRa header mode with `CMD_IMPLICIT=0` did not make it decode RTNode-originated frames.
- Reconfiguring the probe with a full radio restart (`RADIO_STATE_OFF`, channel config, `CMD_IMPLICIT=0`, `RADIO_STATE_ON`, promisc) also did not make it decode V3-originated frames.
- Three-device discriminator after proper probe restart:
  - V3 RTNode TX logged `[LoRa] TXSTART 167 bytes`, TXCFG explicit header, CRC on, IQ normal, TXDONE.
  - V4 RTNode listener decoded the V3 announce and stored the path.
  - Wio Tracker L1 probe emitted channel telemetry only and no KISS DATA frame.
- Important test-helper lesson:
  - Standard RNode config setters can run while the radio is already online and do not force continuous RX afterward.
  - Hardware tests should force `RADIO_STATE_OFF` before channel setup, then `RADIO_STATE_ON` after setup, to avoid false-negative receive tests.
- Current interpretation:
  - The standard probe sees RF/channel activity during RTNode TX but does not produce a valid decoded packet.
  - Because V3 and V4 RTNodes decode each other, the RTNode TX is not generally malformed, but it is still not accepted by the Wio Tracker L1 receiver.
  - Next strong discriminator: flash a Heltec V3 with plain/non-FIREWALL RNode firmware and test whether the Wio can decode that upstream-style Heltec TX.

## 2026-05-12 v1.0.29 A/B Test

- Created/used a `v1.0.29` worktree at `/Users/james/Offline/Reticulum/RTNode-HeltecV4-v1029`.
- Built and uploaded `v1.0.29` target `rtnode_heltec_v3` to `/dev/cu.usbserial-0001`.
  - PlatformIO reported success; old `extra_script.py` still ran noisy `rnodeconf` post-upload checks afterward.
  - Wio Tracker L1 probe was configured OFF -> channel params -> `CMD_IMPLICIT=0` -> ON -> promisc.
  - Result: no KISS `CMD_DATA` frames decoded by the Wio during the old V3 boot/TX window.
- Full-flashed exact mirrored release image `docs/firmware/v1.0.29/rtnode_heltec_v3_merged.bin` to `/dev/cu.usbserial-0001` at offset `0x0`.
  - Hash verified, but after reset the device entered setup portal mode:
    - `[RTNode] RTNode app marker missing — previous firmware was not RTNode or config is unclaimed`
    - `[RTNode] Starting config portal to migrate settings into RTNode`
    - `[Config] AP started: RTNode-Setup`
  - Result: invalid as a LoRa decode test because normal RNS/LoRa did not start.
- App-flashed exact mirrored release image `docs/firmware/v1.0.29/rtnode_heltec_v4.bin` to V4 at offset `0x10000`, preserving the V4 config/filesystem.
  - V4 booted normally and loaded LoRa config from EEPROM.
  - RNS transport started, TCP server listened on port `4242`, and counters showed one outbound packet (`pout: 1`).
  - Wio Tracker L1 probe again saw only config/channel telemetry frames (`0x25`/`0x26`) and no KISS `CMD_DATA` frames during the old V4 boot/TX window.
- Restored V4 to the current diagnostic firmware with `pio run -e rtnode_heltec_v4 -t upload --upload-port /dev/cu.usbmodem11101`; upload succeeded and current post-upload `rnodeconf` steps were skipped.
- Restored V3 to the current diagnostic firmware with `pio run -e rtnode_heltec_v3 -t upload --upload-port /dev/cu.usbserial-0001`; upload succeeded and current post-upload `rnodeconf` steps were skipped.
  - Follow-up reset/boot check showed V3 is not stuck in setup portal mode.
  - V3 loaded LoRa config from EEPROM (`914875000`, `125000`, SF10, CR5, TXP22), started RNS, queued/transmitted a 167-byte LoRa startup packet, and decoded inbound LoRa frames during the check.
- Interpretation:
  - The configured V4 app-only A/B test is the cleanest old-release result so far: `v1.0.29` ran normally, transmitted at least one RNS packet, and the Wio still decoded no LoRa DATA.
  - Therefore the Wio no-decode behavior is not yet proven to be a current-code regression from `v1.0.29`.
  - The full-flashed V3 exact image cannot answer the question until it is configured out of setup portal mode.
  - The V3 app/boot/partition images have been restored to current firmware after the full-flash test, and a boot check confirmed it starts normal RNS/LoRa operation again.

## 2026-05-09 v1.0.28 RTNode-Only A/B Test

- User requested testing an earlier release without using the standard RNode/Wio probe.
- App-flashed `docs/firmware/v1.0.28/rtnode_heltec_v4.bin` to V4 at offset `0x10000`, preserving the V4 config/filesystem.
- First reset attempt was invalid because serial was opened before `esptool.py run`, leaving the V4 in ESP32 download mode (`waiting for download`). Recovered with `esptool.py --chip esp32s3 --port /dev/cu.usbmodem11101 run` and reran with a safer reset/listen order.
- Valid RTNode-only test setup:
  - Current diagnostic V3 at `/dev/cu.usbserial-0001` was reset first and used as the listener.
  - `v1.0.28` V4 at `/dev/cu.usbmodem11101` was reset as the source.
  - No standard RNode/Wio probe was used.
- Result:
  - V4 `v1.0.28` booted normally, loaded LoRa config, started RNS and TCP, and counters showed `pout: 1`.
  - V3 current diagnostic firmware decoded the V4 transmission:
    - `[LoRa] RX 167 bytes`
    - `[PKT] IN iface=Interface[LoRaInterface] sz=167 type=1 ctx=0 hdr=0 dstt=0 hops=1 dst=6e89720e tid=none`
- Restored V4 to current diagnostic firmware with `pio run -e rtnode_heltec_v4 -t upload --upload-port /dev/cu.usbmodem11101`; upload succeeded and post-upload `rnodeconf` steps were skipped.
- Interpretation:
  - Earlier release `v1.0.28` can transmit a startup LoRa announce that another RTNode decodes.
  - This RTNode-only test does not exercise the standard RNode/Wio decode path and therefore should not be used as evidence that the Wio interoperability problem is fixed.
  - It does reinforce the current pattern: RTNode SX1262-to-RTNode SX1262 decode works, while standard Wio/RNode decode remains the failing interoperability path.

## 2026-05-10 Standard RNode Baseline Attempt

- User clarified that RTNode-to-RTNode success is not sufficient: RNode compatibility is the standard to follow.
- Built/flashed the fork's non-FIREWALL `heltec_wifi_lora_32_V3` target to `/dev/cu.usbserial-0001`.
  - Initial build failed because FIREWALL-only RTC node-hash cache symbols were referenced in the standard build.
  - Patched the RTC node-hash cache block under `#ifdef FIREWALL_MODE`; standard V3 build/upload then succeeded.
- Standard V3 initially accepted KISS channel config but kept reporting `CMD_RADIO_STATE=00`.
  - Cause 1: missing standard RNode EEPROM provisioning. Fixed with `rnodeconf --rom --product c1 --model ca --hwrev 1 /dev/cu.usbserial-0001`.
  - Cause 2: missing saved TNC config. Fixed with `rnodeconf --tnc --freq 914875000 --bw 125000 --txp 17 --sf 10 --cr 5 /dev/cu.usbserial-0001`.
  - Cause 3: local standard firmware hash not stored after post-upload provisioning/hash hooks were disabled. Fixed by computing the appended ESP32 partition hash from `.pio/build/heltec_wifi_lora_32_V3/rtnode_heltec32v3.bin` and setting it with `rnodeconf --firmware-hash`.
  - After those steps, the fork standard V3 reported `CMD_RADIO_STATE=01`.
- The fork's standard target still did not exchange LoRa DATA with the Wio Tracker L1 in either direction, despite both sides reporting radio online.
- Flashed stock/official RNode firmware `1.86` to the V3 with `rnodeconf --update /dev/cu.usbserial-0001` to remove fork build flags and local edits from the baseline.
  - Stock V3 reported TNC mode, frequency `914.875 MHz`, bandwidth `125 KHz`, TX power `22 dBm` in `rnodeconf --info`.
  - Stock V3 and Wio both reported `CMD_RADIO_STATE=01` during tests.
  - Stock V3 <-> Wio still produced no KISS `CMD_DATA` in either direction.
  - Retested with long payloads after noticing short arbitrary test payloads can be below RNode's queue minimum and create false negatives.
  - Disabled interference avoidance on both devices with `rnodeconf --ia-disable`; no change.
  - Tested with upstream `RNode.py` host interface rather than the local `tests/kiss_serial.py` helper; no packets arrived.
- Sanity check: Wio still transmits real RF. With the same channel settings, the RTNode V4 at `/dev/cu.usbmodem11101` logged `[LoRa] RX ...` immediately after Wio `CMD_DATA` TX.
- Restored V3 to current RTNode FIREWALL_MODE firmware with `pio run -e rtnode_heltec_v3 -t upload --upload-port /dev/cu.usbserial-0001`.
  - Follow-up sanity check: restored V3 RTNode again decoded Wio TX and logged `[LoRa] RX ...`.
- Current interpretation:
  - The Heltec V3 hardware is not dead; it receives Wio packets in RTNode/FIREWALL_MODE.
  - Stock/standard RNode firmware on this V3, even when provisioned and radio-online, did not receive from or transmit decodable packets to the Wio in these bench tests.
  - This means a V3 standard-firmware baseline is not currently a reliable oracle for fixing RTNode-to-Wio TX.
  - Wio-to-RTNode RX remains confirmed, while RTNode-to-Wio TX remains the standard-compatibility failure to solve.
- Current hardware state after user request: V3 at `/dev/cu.usbserial-0001` was directly flashed back to official RNode `1.86` from the cached `rnode_firmware_heltec32v3` package and verified with `rnodeconf --info` as Heltec LoRa32 V3 high-band TNC on `914.875 MHz`, BW `125 KHz`, SF10, CR5, TXP22.
- Follow-up user request: V3 official RNode was changed away from startup TNC operation with `rnodeconf --normal --bluetooth-on --wifi OFF /dev/cu.usbserial-0001`. Verification with `rnodeconf --info` reports `Device mode: Normal (host-controlled)`. Bluetooth was enabled and WiFi disabled; USB serial still exists for configuration, but the device is no longer configured as an auto-start USB TNC.
- Pairing screen follow-up: user reported the screen blanks when entering Bluetooth pairing mode. Applied `rnodeconf --display 255 --timeout 0 /dev/cu.usbserial-0001` to set maximum display intensity and disable display blanking. Then started `rnodeconf --bluetooth-pair /dev/cu.usbserial-0001`, confirmed it reached pairing mode, and exited it cleanly because the user was not available to pair live.

## 2026-05-10 Direct RTNode to Official RNode Test

- Wio was removed from the test path after user reported the Wio can send but cannot receive.
- Live devices for this test:
  - V4 RTNode: `/dev/cu.usbmodem11101`
  - V3 official RNode 1.86: `/dev/cu.usbserial-0001`
- Verified V3 official RNode with `rnodeconf --info`; it reports Heltec LoRa32 V3 high-band, firmware `1.86`, SX1262, device mode `Normal (host-controlled)`.
- Ran focused pytest hardware checks on channel `914875000 / 125000 / SF10 / CR5` with the V3 RNode configured at TXP22.
- Result 1: official RNode -> RTNode receive path works. Single receive passed, and repeat receive passed `3/3` with RTNode logging `[LoRa] RX` for each packet.
- Result 2: RTNode -> official RNode receive path does not work. RTNode logs `[LoRa] TX 167 bytes` and `[LoRa] TXSTART 167 bytes`, but the official RNode receives no KISS `CMD_DATA` packet.
- Current direct-RNode interpretation: the failure no longer requires the Wio. RTNode can receive from official RNode, but official RNode does not decode RTNode-originated frames in the current setup.

## 2026-05-10 Additional V4.2 Standard Probe

- User connected another device identified as `/dev/cu.usbmodem11201` to rule out bad RX on multiple existing devices.
- Initial state: it was running RTNode setup portal firmware and booted into `RTNode-Setup`; it did not answer `rnodeconf` as a standard RNode.
- Flashed non-FIREWALL `heltec_wifi_lora_32_V4` firmware to `/dev/cu.usbmodem11201`; upload succeeded.
- Bootstrapped EEPROM with `rnodeconf --rom --product c3 --model c8 --hwrev 1 /dev/cu.usbmodem11201`.
- First KISS checks showed accepted config but radio stayed OFF. Boot log said `RNS is inoperable because hardware is not ready! Check firmware signature and eeprom provisioning`.
- Computed the ESP32 app partition hash from `.pio/build/heltec_wifi_lora_32_V4/rtnode_heltec32v4.bin` and applied it with `rnodeconf --firmware-hash ... /dev/cu.usbmodem11201`.
- After firmware hash provisioning, `rnodeconf --info` reports Heltec LoRa32 V4 high-band (`c3:c8:3f`), firmware `1.85`, signature validated, SX1262, Normal mode. KISS config now brings radio state to `ON`.
- V4.2 probe test on `914875000 / 125000 / SF10 / CR5`:
  - V4.2 standard RNode -> RTNode: PASS, including `3/3` at `14 dBm` and `3/3` at `2 dBm` after hash provisioning.
  - RTNode -> V4.2 standard RNode: FAIL. RTNode logs `[LoRa] TX 167 bytes` and `[LoRa] TXSTART 167 bytes`, but the V4.2 standard RNode receives no KISS `CMD_DATA`.
- Near-field note: devices are within about one meter. Standard probes can use `--lora-txp 2` for receive-path tests; RTNode persistent config still logs `txp=28`, so RTNode-originated near-field results should consider possible overload until RTNode TX power is lowered persistently or devices are separated/attenuated.

## 2026-05-10 Fresh Official RNode Download Check

- Tested the "maybe the flashed package is corrupted" hypothesis directly.
- GitHub latest release for `markqvist/RNode_Firmware` is still `1.86`.
- Fresh download checksum checks:
  - `rnode_firmware_heltec32v3.zip` from GitHub `1.86` matched the cached `~/.config/rnodeconf/update/1.86/rnode_firmware_heltec32v3.zip` exactly.
  - `rnode_firmware_heltec32v4pa.zip` from GitHub `1.85` matched the cached `~/.config/rnodeconf/update/1.85/rnode_firmware_heltec32v4pa.zip` exactly.
- Official `1.86` release assets do include `rnode_firmware_heltec32v4pa.zip`, so the V4.2 probe was reflashed directly from a fresh GitHub-downloaded official `1.86` package, not from the local repo build.
- After the direct official flash, the V4.2 firmware hash was updated from the freshly downloaded `rnode_firmware_heltec32v4pa.bin`, and `rnodeconf --info /dev/cu.usbmodem11201` reported:
  - Firmware `1.86`
  - Product `Heltec LoRa32 v4 850 - 950 MHz (c3:c8:3f)`
  - Signature validated
  - Normal mode
- Retest after fresh official `1.86` flash:
  - V4.2 standard RNode -> RTNode: PASS `3/3` at `14 dBm`.
  - RTNode -> V4.2 standard RNode: FAIL unchanged. RTNode still logs `[LoRa] TXSTART 167 bytes`, but the freshly downloaded official `1.86` V4.2 receives no packet.
- Current interpretation: the failure is not explained by a corrupted cached firmware package or by the V4.2 running a stale local standard build. The direct incompatibility reproduces with freshly downloaded official upstream RNode firmware.

## 2026-05-10 Python Reticulum rnsd/rnprobe Check

- Earlier direct RNode-to-RNode smoke tests used the local raw KISS helper in `tests/kiss_serial.py` and `CMD_DATA` frames, not Python Reticulum.
- To test the user's requested Python path, the KCT8103L Heltec V4.3 board at `/dev/cu.usbmodem11101` was reflashed from the fresh official `1.86` `rnode_firmware_heltec32v4pa` package, provisioned as `c3:c8:3f`, set to Normal mode, and used as a standard RNode alongside the V4.2 at `/dev/cu.usbmodem11201`.
- Created isolated `rnsd` configs under:
  - `tests/rnsd_v43/config`
  - `tests/rnsd_v42/config`
- Important Python Reticulum startup quirk:
  - `RNS.Interfaces.RNodeInterface.validateRadioState()` validates after only `0.25 s`.
  - On these boards, the first `CMD_TXPOWER` echo can transiently report the previous value before settling to the requested value.
  - Priming both radios to the target channel and `14 dBm` before starting `rnsd` avoided the startup mismatch and allowed both daemons to power up cleanly.
- To avoid attaching to an unrelated local shared Reticulum instance on macOS, the configs were switched to:
  - `share_instance = Yes`
  - `shared_instance_type = tcp`
  - unique `shared_instance_port` / `instance_control_port` values per config.
- Valid Python-stack result:
  - V4.3 `rnsd` came up on `/dev/cu.usbmodem11101` and exposed probe destination `rnstransport.probe ... :12d871251e253efbc9403bc568364219`.
  - V4.2 `rnsd` came up on `/dev/cu.usbmodem11201` and exposed probe destination `rnstransport.probe ... :9e3b27a5e0e03720ac16623bc4549f43`.
  - `rnprobe` from V4.2 config to the V4.3 probe destination: `Path request timed out`.
  - `rnprobe` from V4.3 config to the V4.2 probe destination: `Path request timed out`.
  - The live `rnsd` logs showed no subsequent path-request or receive activity during these probe attempts.
- Current interpretation at that point: the direct V4.3 standard RNode <-> V4.2 standard RNode path appeared to fail when using Python Reticulum (`rnsd` + `rnprobe`), not only when using the raw KISS test helper.

## 2026-05-10 User-Confirmed V4.2 <-> V4.3 Standard RNode Success

- User has now successfully sent from Meshchat to Sideband and back using the V4.2 and V4.3, both running standard RNode firmware.
- The V3 has been removed from the equation.
- Current attached ports were rechecked after this update:
  - `/dev/cu.usbmodem11101` reports serial `00:00:00:0b`
  - `/dev/cu.usbmodem114401` reports serial `00:00:00:0a`
- Both attached devices report official RNode firmware `1.86`, validated signatures, product `Heltec LoRa32 v4 850 - 950 MHz (c3:c8:3f)`, SX1262, and Normal mode.
- This user-confirmed application-layer round trip supersedes the earlier assumption that the standard-RNode bench baseline itself was broken.
- Updated interpretation:
  - Standard RNode operation between the V4.2 and V4.3 is now confirmed at the application level.
  - Earlier raw KISS and Python `rnsd`/`rnprobe` failures were therefore not a reliable representation of the current real-world standard-RNode baseline.
  - With the V3 removed and V4.2 <-> V4.3 standard RNode confirmed working, the active interoperability question should be narrowed back toward RTNode versus the now-working standard-RNode pair when RTNode is reintroduced.

## 2026-05-10 Independent Reticulum Proof Round Trip Confirmed

- Independently verified the correct protocol path with `tests/proof_probe.py`, not just with raw KISS and not only from user-observed Meshchat/Sideband behavior.
- Active standard-RNode ports during this verification:
  - V4.3-side standard RNode: `/dev/cu.usbmodem11101`
  - V4.2-side standard RNode: `/dev/cu.usbmodem114401`
- Shared LoRa settings during the proof test:
  - Frequency `914875000`
  - Bandwidth `125000`
  - SF `10`
  - CR `5`
  - TXP `14`

Forward direction, V4.2 -> V4.3:

- Started `proof_probe.py server --side lora` on the V4.3-side RNode at `/dev/cu.usbmodem11101` with work dir `tests/proof_probe_v43_server`.
- Server destination hash: `539f534769778629b697b0b401327282`.
- Started `proof_probe.py client --side lora` on the V4.2-side RNode at `/dev/cu.usbmodem114401` with work dir `tests/proof_probe_v42_client`.
- Client result:
  - `CLIENT_PATH_READY elapsed=4.6s`
  - `CLIENT_SENT ... len=21`
  - `CLIENT_DELIVERED rtt=4.245s`
- Server result:
  - answered the path request for the local destination
  - `SERVER_RX side=lora len=21 ... from=RNodeInterface[RNode LoRa]`

Reverse direction, V4.3 -> V4.2:

- Started `proof_probe.py server --side lora` on the V4.2-side RNode at `/dev/cu.usbmodem114401` with work dir `tests/proof_probe_v42_server`.
- Server destination hash: `ef78a46ee16a1e5a08e627f53ac27beb`.
- Started `proof_probe.py client --side lora` on the V4.3-side RNode at `/dev/cu.usbmodem11101` with work dir `tests/proof_probe_v43_client`.
- Client result:
  - `CLIENT_PATH_READY elapsed=3.0s`
  - `CLIENT_SENT ... len=21`
  - `CLIENT_DELIVERED rtt=4.345s`
- Server result:
  - answered the path request for the local destination
  - `SERVER_RX side=lora len=21 ... from=RNodeInterface[RNode LoRa]`

- Current interpretation:
  - Standard RNode V4.2 <-> V4.3 is now independently confirmed with full Reticulum delivery and proof return in both directions.
  - This is a stronger confirmation than earlier raw KISS packet tests or the earlier `rnsd`/`rnprobe` path-discovery attempts.
  - The current known-good reference protocol path is therefore the bidirectional proof round trip between the V4.2 and V4.3 standard-RNode pair.

## 2026-05-10 Mixed Baseline Restored: RTNode + Standard RNode

- User preference was to stop keeping both current boards on standard firmware and instead return one of the two now-proven-good boards to RTNode firmware.
- Selected split:
  - `/dev/cu.usbmodem11101` restored to RTNode firmware via `pio run -e rtnode_heltec_v4 -t upload --upload-port /dev/cu.usbmodem11101`
  - `/dev/cu.usbmodem114401` kept as the standard-RNode control board
- PlatformIO upload to `/dev/cu.usbmodem11101` completed cleanly:
  - flash/write succeeded
  - `Hash of data verified`
  - `Hard resetting via RTS pin`
  - `SUCCESS`
- Post-upload note:
  - `extra_script.py` still skips post-upload `rnodeconf` provisioning/hash steps, which is expected and appropriate for RTNode `FIREWALL_MODE`
- Short boot/runtime capture from `/dev/cu.usbmodem11101` confirmed RTNode is running again:
  - `[RTNode] Provisioning check bypassed, modem installed`
  - `[RTNode] No LoRa config in EEPROM, using defaults`
  - `[RTNode] LoRa: freq=914875000 bw=125000 sf=10 cr=5 txp=28`
  - `Starting RNS...`
  - `Transport mode is enabled`
  - `[TcpIF] Server listening on port 4242`
  - `[mDNS] STA up: mynode.local (_reticulum._tcp port 4242)`
  - `[LoRa] TXSTART 167 bytes`
- Control-board verification on `/dev/cu.usbmodem114401` confirmed it remains a standard RNode:
  - firmware `1.86`
  - product `Heltec LoRa32 v4 850 - 950 MHz (c3:c8:3f)`
  - serial `00:00:00:0a`
  - `Device mode: Normal (host-controlled)`
- Current active baseline after this change:
  - RTNode under test: `/dev/cu.usbmodem11101`
  - standard-RNode control: `/dev/cu.usbmodem114401`
- Updated interpretation:
  - The two-board known-good standard baseline has now served its purpose.
  - The active next debugging target should be RTNode versus the still-known-good standard RNode on the same V4-class hardware pair.

## 2026-05-10 Mixed Proof Test: LoRa Client -> RTNode TCP Server PASS

- Exact RTNode firmware baseline used for this test:
  - flashed from the current local `RTNode-HeltecV4` working tree, not from an older mirrored release image
  - local repo identity at flash/test time: `v1.0.32-1-g39a5230-dirty`
  - internal firmware version macros in `Config.h` remain `MAJ_VERS 0x01` / `MIN_VERS 0x55` (reported protocol version `1.85`)
- Active devices during this test:
  - RTNode under test on `/dev/cu.usbmodem11101`
  - standard RNode control on `/dev/cu.usbmodem114401`
- Test direction run:
  - TCP-side proof server connected through RTNode local TCP on `mynode.local:4242`
  - standard-RNode LoRa-side proof client on `/dev/cu.usbmodem114401`
- Command pattern used:
  - server: `tests/proof_probe.py server --side tcp --work-dir tests/proof_probe_mixed_lora_to_tcp --duration 120 --announce-interval 15 --debug`
  - client: `tests/proof_probe.py client --side lora --work-dir tests/proof_probe_mixed_lora_to_tcp --rnode-port /dev/cu.usbmodem114401 --frequency 914875000 --bandwidth 125000 --spreadingfactor 10 --codingrate 5 --txpower 14 --path-timeout 60 --timeout 45 --payload 'mixed baseline lora->tcp' --debug`
- Result:
  - LoRa client learned path to the TCP-side destination in `3.3s`
  - LoRa client sent payload length `24`
  - LoRa client received delivery proof with `CLIENT_DELIVERED rtt=2.807s`
  - TCP-side server logged `SERVER_RX side=tcp len=24 ... from=TCPInterface[RTNode TCP/mynode.local:4242]`
- RTNode serial capture during the pass showed the expected firewall/proof path:
  - stored the TCP-side announce locally: `[PATH] STORED dst=6d9b8639 hops=0 iface=Interface[LocalTcpInterface]`
  - answered the incoming LoRa path request: `[PATH] REQ dst=6d9b8639 from=Interface[LoRaInterface]` followed by `[PATH] RESP dst=6d9b8639 hops=0 to=Interface[LoRaInterface] local=0`
  - received the LoRa-side payload over LoRa: `[LoRa] RX 131 bytes`
  - transported the generated proof back toward LoRa: `[PROOF] XPORT dst=84c8b837 data=64 hops=0 recv=Interface[LocalTcpInterface] out=Interface[LoRaInterface]`
  - actually transmitted the proof over LoRa: `[LoRa] TXSTART 83 bytes`
- Updated interpretation:
  - On the restored mixed baseline, the LoRa-client -> RTNode -> TCP-server path now works with proof return to the LoRa client.
  - This directly contradicts the earlier failing mixed-baseline symptom for this direction.
  - The remaining proof-style check to run is the reverse direction: TCP client -> RTNode -> LoRa server.

## 2026-05-10 Mixed Proof Test: TCP Client -> RTNode -> LoRa Server PASS

- Active devices during this test:
  - RTNode under test on `/dev/cu.usbmodem11101`
  - standard RNode control on `/dev/cu.usbmodem114401`
- Test direction run:
  - WAN-side LoRa proof server on the standard RNode with work dir `tests/proof_probe_mixed_tcp_to_lora`
  - LAN-side TCP proof client through RTNode local TCP with payload `mixed baseline tcp->lora`
- LoRa-side server result:
  - `SERVER_READY side=lora hash=13f35cd143398c24eeb499c5c775b16e`
  - answered the incoming path request for the local destination
  - `SERVER_RX side=lora len=24 hash=ae08e296f9b13f18b0066e8ed83a9db8dc3f082dd20d96bc6faff175c990a814 from=RNodeInterface[RNode LoRa]`
- The TCP-side client run completed successfully with exit code `0`; the reverse proof path did not time out.
- RTNode serial capture during the pass showed the expected reverse-path behavior:
  - received the LAN-side path request locally: `[PATH] REQ dst=13f35cd1 from=Interface[LocalTcpInterface] local=1 sz=32`
  - learned the LoRa-side announce: `[ANNC] IN dst=13f35cd1 valid=1 ctx=11 hops=1 iface=Interface[LoRaInterface]` and `[PATH] STORED dst=13f35cd1 hops=1 iface=Interface[LoRaInterface]`
  - forwarded the LAN TCP payload toward LoRa: `[PKT] IN iface=Interface[LocalTcpInterface] sz=147 type=0 ctx=0 hdr=1 dstt=0 hops=1 dst=13f35cd1 tid=11a63924` followed by `[LoRa] TXSTART 131 bytes`
  - transported the returning proof back from LoRa to the LAN TCP client: `[PROOF] XPORT dst=ae08e296 data=64 hops=1 recv=Interface[LoRaInterface] out=Interface[LocalTcpInterface]`
- Updated interpretation:
  - The mixed announce/access path is now confirmed in both directions on the restored RTNode + standard-RNode split.
  - The earlier proof failure between the LoRa and TCP sides is no longer reproducing in either direction with `tests/proof_probe.py`.

## 2026-05-10 Pure LAN TCP -> TCP by Direct IP PASS

- The first LAN-only attempt using `mynode.local` was confounded by TCP connection timeouts and an unsafe concurrent raw serial capture.
- Important serial-access lesson from that failed attempt:
  - direct `pyserial` capture on `/dev/cu.usbmodem11101` is unsafe for RTNode in this setup
  - the partial capture file `tests/proof_probe_lan_tcp_to_tcp_rtnode_serial.log` later showed `boot:0x1 (DOWNLOAD(USB/UART0))`, meaning the ESP32 had been knocked into ROM download mode instead of normal runtime
  - safer RTNode monitoring is `pio device monitor -p /dev/cu.usbmodem11101 -b 115200 --filter direct --dtr 0 --rts 0`
- Retested the LAN-only proof using RTNode's direct IP `192.168.2.122` to remove mDNS/name-resolution noise:
  - TCP-side client destination hash: `935208f1f301a3c75058783de9fb79b8`
  - `CLIENT_PATH_READY elapsed=15.7s`
  - `CLIENT_SENT packet_hash=726ed709d1861e10980c1f293a0dca5c9900b76e3d189cc0c37a9e6155735424 len=21`
  - `CLIENT_DELIVERED rtt=1.984s`
- Updated interpretation:
  - Pure LAN-side local TCP clients can announce and access each other through RTNode when RTNode is healthy and addressed directly by IP.
  - The earlier LAN-only failure was a test artifact driven by mDNS/connection timing and unsafe serial-port access, not a proof of broken local TCP forwarding.

## 2026-05-10 WAN Flood Postcheck: LAN Reachability PASS, Unsolicited WAN Announces NOT Blocked

- Flood setup:
  - persistent LAN-side TCP proof server on `192.168.2.122:4242`
  - LAN server destination hash: `3ee511e6579fb20cc57906e8c55f7bbd`
  - WAN flood script sent four unsolicited LoRa announces for hashes:
    - `c35fc03cb99e0629367b302c6294f231`
    - `ace624d1ea08b19a3bf381118a3043b3`
    - `04c0b7bc075a0cc37890ece246257011`
    - `4f0501d0fe9669610d37f5e163eba3cc`
  - WAN flood script also sent twenty random path requests over LoRa
- Important filter verdict from RTNode's own monitor log:
  - there were no `BLOCKED unsolicited backbone announce` lines in `tests/filter_stress_rtnode_serial.log`
  - instead, RTNode accepted and stored all four unsolicited WAN announces, for example:
    - `[ANNC] IN dst=c35fc03c valid=1 ctx=0 hops=1 iface=Interface[LoRaInterface]`
    - `[PATH] STORED dst=c35fc03c hops=1 iface=Interface[LoRaInterface]`
    - repeated similarly for `ace624d1`, `04c0b7bc`, and `4f0501d0`
- The LAN-side TCP server also learned those four unsolicited WAN destinations through RTNode:
  - `Destination <c35fc03cb99e0629367b302c6294f231> is now 2 hops away via <11a6392444a36824dd8a0e7b9caba59d> on TCPInterface[RTNode TCP/192.168.2.122:4242]`
  - likewise for `ace624d1...`, `04c0b7bc...`, and `4f0501d0...`
- The random WAN path-request flood also reached the LAN-side TCP server, which logged each one and ignored it because no path was known.
- Despite the WAN-side noise, valid LAN reachability still survived:
  - post-flood LoRa client result:
    - `CLIENT_PATH_READY elapsed=33.5s`
    - `CLIENT_SENT packet_hash=0e27b5d98041e2575e61cd851544eedd8cfe66a170c53bf0b61fde39134b8c91 len=26`
    - `CLIENT_DELIVERED rtt=13.027s`
  - LAN-side TCP server result:
    - `SERVER_RX side=tcp len=26 hash=0e27b5d98041e2575e61cd851544eedd8cfe66a170c53bf0b61fde39134b8c91 from=TCPInterface[RTNode TCP/192.168.2.122:4242]`
  - RTNode serial log showed the valid path/proof sequence after the flood:
    - `[PATH] REQ dst=3ee511e6 from=Interface[LoRaInterface] local=0 sz=32`
    - `[PATH] RESP dst=3ee511e6 hops=0 to=Interface[LoRaInterface] local=0`
    - `[PROOF] XPORT dst=0e27b5d9 data=64 hops=0 recv=Interface[LocalTcpInterface] out=Interface[LoRaInterface]`
- Updated interpretation:
  - The current firmware preserves communication with the LAN-side TCP destination even after a WAN-side flood, though path discovery is slower under load.
  - The current firmware does not appear to enforce the intended unsolicited-WAN announce block yet; the flood announces were accepted into RTNode state and surfaced to the LAN TCP endpoint.
  - WAN-side random path requests were tolerated rather than blocked at the firewall, so the filter behavior is still weaker than the intended design in `CORE_PRINCIPLES.md`.

## 2026-05-10 Firewall Root Cause, Two-Step Fix, and Final Hardware Validation PASS

- Root cause found in `lib/microReticulum/src/Transport.cpp`:
  - RTNode firmware config marks LoRa as `MODE_GATEWAY`, not `is_backbone(true)`.
  - The old firewall logic treated "non-backbone" as trusted, so LoRa/WAN traffic could grow the whitelist and store unsolicited announces.
  - `Transport::path_request()` also allowed unknown path requests from untrusted LoRa ingress to search and leak toward local TCP clients.
- First firmware fix that was built, flashed, and validated on hardware:
  - introduced trusted/untrusted firewall helpers based on `Transport::is_local_client_interface()`
  - applied the inbound whitelist gate to untrusted ingress, which includes LoRa in this runtime setup
  - restricted whitelist growth and unknown-path discovery so unsolicited LoRa announces were no longer stored and random unknown LoRa path requests no longer reached the LAN TCP proof server
  - preserved valid post-flood access: LoRa postcheck client still reached the LAN TCP server and received proof
- Intermediate validation result after that first fix:
  - unsolicited LoRa flood announces no longer produced `[ANNC] IN` / `[PATH] STORED` for the flood hashes on RTNode
  - LAN TCP proof server no longer learned those WAN flood destinations and no longer logged the random path requests
  - however, `bma` still climbed during the random WAN path-request flood because the "mentioned" set was still learning non-address identifiers such as the control destination and proof hashes
- Final refinement requested by user and validated on hardware:
  - tightened whitelist growth so WAN flood control hashes were no longer entering the whitelists
  - that intermediate implementation used a per-packet helper to avoid learning plain control destinations and proof packet hashes from WAN flood traffic
  - this helper-based narrowing was later superseded by the simpler destination-centric contract documented below: every destination the LAN mentions is whitelisted, and path requests whitelist their payload target rather than the `path.request` wrapper hash
- Final hardware validation on the refined firmware:
  - LAN TCP proof server hash: `703d29c1883e35f48c25beef9ee00b89`
  - WAN flood script again sent four unsolicited LoRa announces and twenty random LoRa path requests
  - RTNode monitor during the flood showed `bma=0` throughout the unsolicited announce flood and throughout all twenty random WAN path requests
  - valid post-flood path request for `703d29c1...` produced:
    - `[PATH] REQ dst=703d29c1 from=Interface[LoRaInterface] local=0 sz=32`
    - `[PATH] RESP dst=703d29c1 hops=0 to=Interface[LoRaInterface] local=0`
    - `bma` remained `0` after the path response
  - valid post-flood LoRa client then delivered successfully:
    - `CLIENT_PATH_READY elapsed=3.3s`
    - `CLIENT_SENT packet_hash=c90cc690f2f2703ddd50b6f8e6ae6bd3981c10e854da38c43d295abca285a237 len=29`
    - `CLIENT_DELIVERED rtt=2.860s`
  - LAN TCP server received the payload:
    - `SERVER_RX side=tcp len=29 hash=c90cc690f2f2703ddd50b6f8e6ae6bd3981c10e854da38c43d295abca285a237 from=TCPInterface[RTNode TCP/192.168.2.122:4242]`
  - RTNode monitor showed the valid inbound data packet raised `bma` to `1` for the real destination address, and the returning proof did not increase `bma` further
- Final interpretation:
  - unsolicited LoRa/WAN announces are no longer being admitted into RTNode path state
  - random unknown LoRa/WAN path requests no longer leak to LAN TCP clients
  - the firewall whitelist no longer grows from WAN flood control/proof identifiers
  - valid LoRa-to-LAN access still works after the flood, which is the required behavior for the two-whitelist policy

## 2026-05-10 Firewall Whitelist Rule Simplified And Hardened

- The durable rule is now: every destination the LAN mentions goes in the whitelist.
- The firewall must key that rule to the real destination referenced by the packet, not blindly to `packet.destination_hash()` when the packet is only a wrapper around another destination.
- The critical special case is a path request:
  - the wrapper packet is sent to the shared `path.request` control destination
  - the actual destination the LAN mentioned is the first 16 bytes of the payload
  - whitelist admission and learning must therefore use that payload hash
- For other trusted LAN packets, the destination the LAN mentioned is simply `packet.destination_hash()`. That includes announces, proofs, link traffic, and ordinary messages.
- WAN-side admission is now aligned to the same rule:
  - whitelisted destinations are allowed through regardless of whether they arrive as announces, path requests, proofs, or messages
  - reverse-table and link-table state still allow established return traffic for active flows
  - unsolicited WAN announces may still be buffered in bounded `_held_announces`, but they are not promoted into live path state unless the destination is already whitelisted or a waiting LAN discovery request asks for it
- Do not reintroduce helper logic that narrows whitelist learning to a subset of packet types. The invariant is destination-centric: if the LAN mentioned that destination, it belongs in the whitelist.

## 2026-05-10 Proof Harness Orchestrator Added; Current Execution State

- What was added:
  - `tests/proof_probe_harness.py` now supports orchestrated `run` and `run-all` commands.
  - The orchestrator starts the server side, waits for readiness, runs the client side, captures per-scenario logs, and stops the server.
  - Each scenario now passes an explicit per-scenario `--hash-file` under its own work directory.
- Important harness lesson:
  - `tests/proof_probe.py` does not derive the hash file from `--work-dir`; its default hash path is the shared `tests/proof_probe_state/server_hash.txt`.
  - Without an explicit `--hash-file`, multiple scenarios can accidentally share readiness state.
- Command used for the live suite:

```bash
.venv/bin/python tests/proof_probe_harness.py run-all \
  --tcp-host 192.168.2.122 \
  --rnode-port /dev/cu.usbmodem114401 \
  --frequency 914875000 \
  --bandwidth 125000 \
  --spreadingfactor 10 \
  --codingrate 5 \
  --txpower 14 \
  --debug
```

- Actual results from this run:
  - `local-tcp-to-local-tcp`: PASS
    - `CLIENT_PATH_READY elapsed=13.9s`
    - `CLIENT_SENT ... len=30`
    - `CLIENT_DELIVERED rtt=1.777s`
  - `lora-to-local-tcp`: FAIL in current environment
  - `local-tcp-to-wan`: FAIL in current environment
  - `wan-to-local-tcp`: FAIL in current environment
- Root cause of the three LoRa-involved failures:
  - the expected LoRa peer device `/dev/cu.usbmodem114401` was not present during execution
  - the current host only exposed `/dev/cu.usbmodem11101`
  - LoRa-side client logs repeatedly showed `could not open port /dev/cu.usbmodem114401: [Errno 2] No such file or directory`
- Interpretation:
  - the orchestrator itself is functioning; it successfully ran the pure LAN TCP scenario end to end
  - the remaining failures are not evidence of a new Reticulum regression in RTNode path handling
  - to complete the mixed and WAN scenario execution, the standard RNode peer must be reattached or the harness must be rerun with the current correct LoRa device path

## 2026-05-10 Final Orchestrated Pass After Reattach and Firewall Response Fixes

- Reattached hardware state:
  - RTNode remained on `/dev/cu.usbmodem11101`
  - reattached standard RNode peer appeared as `/dev/cu.usbmodem11301`
- First retest after reattach:
  - `local-tcp-to-local-tcp`: PASS
  - `lora-to-local-tcp`: PASS
  - `wan-to-local-tcp`: PASS
  - `local-tcp-to-wan`: still failing
- Root cause 1 for `local-tcp-to-wan`:
  - the firewall treated a WAN `PATH_RESPONSE` as solicited only if it arrived as `HEADER_2` and addressed to the transport identity
  - real Reticulum destinations answer path requests with a normal announce packet shape plus `context=PATH_RESPONSE`
  - fix: allow any `PATH_RESPONSE` whose destination hash matches an outstanding `_discovery_path_requests` entry
- Root cause 2 for repeated `local-tcp-to-wan` lookups:
  - after the first successful lookup, RTNode already knew the WAN path and answered later LAN requests from the known-path branch
  - that branch sent an immediate local `PATH_RESPONSE`, but a fresh TCP client could still miss that response and then never receive a queued retry under `FIREWALL_MODE`
  - fix:
    - stop requiring `Identity::recall()` success before synthesizing a known-path local `PATH_RESPONSE`
    - keep a queued retry for trusted local interfaces one announce tick later instead of suppressing all queued path responses under firewall mode
- Firmware actions:
  - built and flashed `pio run -e rtnode_heltec_v4 -t upload --upload-port /dev/cu.usbmodem11101`
  - safe RTNode runtime trace via `pio device monitor -p /dev/cu.usbmodem11101 -b 115200 --filter direct --dtr 0 --rts 0`
- Discriminating runtime evidence from RTNode:
  - second-run `local-tcp-to-wan` showed RTNode receiving the LAN path request and sending `[PATH] RESP dst=6c4eec98 hops=1 to=Interface[LocalTcpInterface] local=1`
  - that proved the remaining bug was reliability of local response delivery for repeated lookups, not WAN discovery itself
- Final repeated-request validation:
  - `local-tcp-to-wan` PASS on first run
  - `local-tcp-to-wan` PASS again immediately on the second run
- Final orchestrated suite command:

```bash
.venv/bin/python tests/proof_probe_harness.py run-all \
  --tcp-host 192.168.2.122 \
  --rnode-port /dev/cu.usbmodem11301 \
  --frequency 914875000 \
  --bandwidth 125000 \
  --spreadingfactor 10 \
  --codingrate 5 \
  --txpower 14 \
  --debug
```

- Final results:
  - `local-tcp-to-local-tcp`: PASS
    - `CLIENT_PATH_READY elapsed=2.0s`
    - `CLIENT_DELIVERED rtt=0.045s`
  - `lora-to-local-tcp`: PASS
    - `CLIENT_PATH_READY elapsed=3.3s`
    - `CLIENT_DELIVERED rtt=3.039s`
  - `local-tcp-to-wan`: PASS
    - `CLIENT_PATH_READY elapsed=1.3s`
    - `CLIENT_DELIVERED rtt=3.086s`
  - `wan-to-local-tcp`: PASS
    - `CLIENT_PATH_READY elapsed=3.1s`
    - `CLIENT_DELIVERED rtt=2.952s`
- Final interpretation:
  - the orchestrator is now valid for all four requested scenarios on current hardware
  - strict firewall filtering still holds
  - both WAN discovery from LAN and WAN access into LAN are now working on the patched RTNode firmware