# Wired Ethernet through a USB adapter: plan

Read with [CORE_PRINCIPLES.md](CORE_PRINCIPLES.md),
[PERFORMANCE_STRATEGY.md](PERFORMANCE_STRATEGY.md) and the workspace
`DESIGN_PRINCIPLES.md`.

Written 2026-10-25. This is a plan only: no firmware is changed yet. Work
starts after the v1.0.53 release and after the `behaviour-rewrite` branch is
merged. That branch barely touches the files below (it changes how
`TcpInterface.h` gates incoming announces, not when it connects).

## What it adds

On the Heltec V4 and V4 R8, the USB-C port can take a USB Ethernet adapter
instead of a computer. The cable then works the way WiFi does today, with
both sides on it: the backbone side (the node's connections out to its TCP
backbones) and the LAN side (local clients connecting to its TCP server).
WiFi can stay on alongside it. LoRa, Bluetooth and the firewall are
unchanged: the cable is another way into the IP network that `TcpInterface`
already uses.

One adapter protocol only: **CDC-ECM**, the USB standard for Ethernet. It
covers Realtek's RTL8152/8153/8156 and ASIX's AX88179A, which between them
are most adapters sold. Adapters that speak only their maker's own protocol
(the older AX88179 without the A, AX88772, Microchip LAN78xx, many cheap
100 Mbit sticks) won't work. SPI Ethernet chips such as the W5500 are ruled
out.

## What the proof of concept showed

Branch `poc/usb-ecm`, folder `poc/usb_ecm/`, whose README has the details.
On a V4.2, through James's USB-C dock (three hubs, then an AX88179A):

- The S3 runs the port as a full-speed USB host (12 Mbit/s) and finds the
  adapter through all three hubs.
- It picks the ECM configuration (the AX88179A offers it third, Realtek
  second) and gets an address from the router by DHCP.
- Pings both ways at up to 1472 bytes: no loss.
- TCP, 1 MB each way, five runs: 550–585 kB/s in, 460–490 kB/s out. That's
  about 4.5 Mbit/s, more than WiFi gave on the same board (325/360 kB/s) and
  far beyond what LoRa traffic needs.
- After a fix to the USB library's hub handling (below), the wired link came
  back within 10 s of every restart.

The USB host code is a subset of [CherryUSB](https://github.com/cherry-embedded/CherryUSB)
v1.6.1, Apache-2.0. Apache-2.0 code may go into GPL-3 firmware. Its notices
stay and it is credited in the README.

## Hardware

**Boards.** The V4.2 (schematic `WiFi_LoRa_32_V4.2`) and the V4 R8 (Heltec's
`HTIT-WBR8H_V4.3.2`) are wired the same way: USB-C data lines through 22-ohm
resistors straight to GPIO19/20, no USB-serial chip, 5.1K pull-downs on CC
(the port only takes power), VBUS through fuse F1 into the 5 V rail. So both
can be a USB host. The V3 has a CP2102 USB-serial chip in the way and can't.
Other V4 revisions: check the schematic when one arrives.

**Power.** The board can't power the adapter: its USB-C 5 V is an input only.
The adapter has to bring its own power, one of two ways:

1. An adapter or dock with a charging port. The charger powers the adapter,
   and the adapter passes power on to the board through the same cable
   (James's dock does this). This is the setup the README describes.
2. A plain adapter, with 5 V fed into the board's header (JP4 pin 2, which is
   wired to the USB-C 5 V line). Powers both. For people who build their own.

With option 1, unplugging the charger powers the board off unless it has a
battery.

## Decided (James, 2026-10-25)

1. **Off by default, switched on in the captive portal's WiFi settings.**
   With it on, the board checks at each boot whether a computer is on the USB
   port. If one is, the port stays USB serial for that boot, as today. If
   not, after a few seconds it becomes the adapter's port until the next
   restart. So to flash or use USB serial again, plug in the computer and
   press reset. The board can't notice a computer plugged in later, because
   two USB hosts can't see each other.
2. **WiFi and the cable together, and the cable follows WiFi's pattern.**
   WiFi carries both sides today: the backbones and the LAN clients. The
   cable carries both sides the same way, from the same settings:
   - The backbone slots and the local TCP server are not duplicated. They
     work over whichever network is up, or both.
   - Backbone connections go out over the cable when it's up and over WiFi
     when it isn't. A pulled cable moves them to WiFi; its return moves new
     connections back.
   - The local TCP server already listens on every address
     (`TcpInterface.h:149`, `WiFiServer` with no address), so LAN clients
     reach it over WiFi and over the cable at the same time. Replies leave by
     the network the client came in on: the proof of concept showed that,
     with traffic to the WiFi address staying on WiFi while the cable was up.
   - mDNS announces the node on both networks (see Unknowns).
   - The firewall's sides are Reticulum interfaces (local server = LAN,
     backbones = WAN), whichever network carries them, so a LAN client on the
     cable is LAN and a backbone on the cable is WAN, exactly as on WiFi.
3. **Experimental, promising nothing.** It goes out marked experimental in the
   README and the portal, with no list of supported adapters. The README says
   which adapter it was tested with, that it needs an ECM adapter that brings
   its own power, and that other adapters may or may not work.

## Firmware changes

In order of work. Line numbers are on main (02cd8c7).

### 1. The USB library

Bring the CherryUSB subset over from the proof of concept into `lib/CherryUSB/`,
with its `UPSTREAM` and `LICENSE` files. Only the V4 builds compile it (a
build flag, `RTNODE_ETH_USB=1`, in the two `rtnode_heltec_v4*` envs).

The changes it already carries, each marked `RTNode` in the source:

- The ESP32-S3 glue, rewritten for host only on IDF 4.4.
- Picking the ECM configuration by reading each one (the library's default
  takes the first, which is the maker's own protocol).
- The driver list as a plain array, since Arduino's linker scripts don't
  collect the library's linker section.
- A one-byte pad on frames that exactly fill their last USB packet, because
  the library can't send the empty packet that would end them. Linux pads ECM
  frames the same way.
- Hubs. Upstream stops polling a hub after one failed status read. Every
  other restart, the dock's hub sent its first report out of sequence, and
  the adapter was never seen. Now a failed read makes the library read every
  port of that hub once and carry on polling. A device already present when
  its hub comes up counts as newly connected, as Linux does. This reads the
  hub's state again; it doesn't resend anything.

Two new changes the proof of concept didn't need, both found while planning:

- **Sending must not wait forever.** `usbh_cdc_ecm_eth_output()` waits with no
  limit for the adapter to take a frame (`USB_OSAL_WAITING_FOREVER`,
  `usbh_cdc_ecm.c:305`). It runs on lwIP's thread, which WiFi shares, so a
  stuck adapter would freeze all networking. Give it a hard ceiling (one
  frame's worth of time at full speed, a few milliseconds). On timeout, drop
  the frame, count it and log it, as WiFi drops do. This is a ceiling on
  correct code, not a retry: TCP resends as it always does.
- **Follow the cable.** The glue marks the wired link up the moment the adapter
  enumerates (`usbh_net.c`, `usbh_cdc_ecm_run()`), and the ECM driver only
  reads the adapter's link reports until the first link-up. A pulled cable
  would leave the node holding a wired address, and preferring the dead wire
  over WiFi. Keep reading the link reports while the adapter is attached.
  Take the wired interface down on link loss, and up again (with a fresh
  DHCP) on link return.

Settings for the glue:

- The wired interface's route priority above WiFi station mode's 100, so
  connections prefer the cable.
- Name it the way IDF 4.4's mDNS expects its one Ethernet interface (the SDK
  has fixed slots for station, access point and Ethernet only, and no way to
  add another). This should make mDNS answer on the cable; it's unproven.
- The library's two tasks run on core 0 at priorities 24 and 23, above WiFi
  and lwIP (18). Set them below WiFi's task and NimBLE's host task, and check
  with Bluetooth peers connected.
- Library logs go to RTNode's log, not `printf`.

The proof of concept's test tools (HTTP endpoints, network log, speed-test
ports, port power switch) stay on the proof-of-concept branch.

### 2. Who owns the USB port at boot

New file `EthernetUsb.h`, in the style of `MdnsService.h`, holding everything
that follows.

- **First thing in `setup()`, always**, on every V4 build: give the USB port
  back to USB serial. Which side owns the port is kept in a register that
  survives a software restart. Without this, a node that ran as host, then had
  Ethernet turned off in the portal (which restarts it), would boot with a
  dead USB port.
- `setup()` already waits up to 2 s for USB serial and then another 2 s
  (`RNode_Firmware.ino:492-502`). If Ethernet is on and no computer has sent
  anything in that time (the port sees nothing from a host every
  millisecond), switch the port to host and start the library. Log the
  choice, on the header's UART pins (JP4 5/6), since USB serial is then gone.
- Never call `Serial.end()` before switching: it turns the USB pins into
  plain outputs and the host can't use them.
- With Ethernet off, nothing changes from today.

### 3. "The network is up" counts the cable

Add `network_is_up()` = WiFi connected, or the wired link up with an address.
Use it where WiFi alone is checked now:

| Where | Today |
|---|---|
| `RNode_Firmware.ino:1222` | TCP interfaces start only if WiFi is enabled and connected |
| `RNode_Firmware.ino:1235` | mDNS starts only on WiFi |
| `RNode_Firmware.ino:3121-3151` | the loop's late start and polling of TCP interfaces sit under `wifi_enabled` |
| `RNode_Firmware.ino:3259` | mDNS late start |
| `TcpInterface.h:205` | a backbone reconnects only if `WiFi.status() == WL_CONNECTED` |
| `MdnsService.h:59` | `start_sta()` refuses without WiFi |
| `Display.h:554`, `draw_cable_icon()` | the status icon shows WiFi only |

The TCP interfaces are created only when WiFi is enabled
(`RNode_Firmware.ino:1128`, the backbones and the local server). Create them when WiFi or Ethernet is
enabled. "WiFi off" then still means the radio is off, and an Ethernet-only
node is no longer a LoRa-only repeater.

Nothing in the firewall changes. Its LAN and WAN sides are Reticulum
interfaces (the local TCP server, the backbone clients), whichever cable or
radio carries them. `CORE_PRINCIPLES.md` applies as it is.

### 4. The WiFi watchdog

The watchdog restarts the node after 15 s without WiFi
(`RNode_Firmware.ino:3082-3117`). While the wired link is up, a WiFi loss
must not restart it: the backbones are on the cable and a restart would drop
them. The watchdog acts only when WiFi is the node's only network. WiFi keeps
trying to rejoin every 10 s, as it does today (`Remote.h`,
`wifi_update_status()`). The cost: if WiFi's driver ever stays stuck while
the cable is up, WiFi's LAN clients wait until the cable goes down or the
node restarts. The bench soak shows whether that happens. A pulled
cable doesn't restart anything: the wired interface goes down, connections
move to WiFi (if on), and it comes back on its own when the cable returns.

### 5. Portal, status and display

- `FirewallConfig.h`: in the "WiFi Network" section (line 269), under the
  WiFi switch, an "Ethernet (USB adapter, experimental)" switch, off by
  default. Its help text says the adapter must bring its own power, and that
  the USB port stops being a serial port unless a computer is attached at
  boot.
- The WiFi switch's "Disabled (LoRa-only repeater)" wording (line 279) is
  true only while Ethernet is off as well. Show plain "Disabled" when
  Ethernet is on.
- Saved at `ADDR_CONF_ETH_EN` = 0x23F, the first free byte after
  `ADDR_CONF_BLE_EN` (0x23E). 0x73 = on; 0x00 or 0xFF = off.
- Saving already restarts the node (`FirewallConfig.h:951-955`), so the port
  role is decided fresh.
- The status page and display: wired link up/down, address, adapter
  (vendor:product), frames and drops.
- `boundary_needs_config()` (`FirewallConfig.h:965`) needs no change: after
  the first save the firewall flag is set, so an Ethernet-only node with no
  SSID isn't sent back to the portal.

## Cost

PERFORMANCE_STRATEGY.md asks every change to state this up front. Measured
from the proof of concept's build of the library subset:

| | Cost | Note |
|---|---|---|
| Flash | about 37 KB | V4 app slots are 6.25 MB |
| Static internal RAM | about 21 KB as the library ships (16 KB zeroed, 5 KB initialised) | over budget as is; allocated at start only when Ethernet is on (below) |
| Task stacks | about 6 KB internal, only with Ethernet on | hub task 4 KB, receive task 2 KB |
| Frames | lwIP buffers per frame, as for WiFi | bounded by lwIP's pools |
| CPU | core 0, only while traffic flows | about 4.5 Mbit/s at most |

Static RAM is already at its budget: the V4 bench build of main uses 25.2 %
(82,412 of 327,680 bytes) against PERFORMANCE_STRATEGY's 25 %. So the
library's static buffers can't simply be added. Its larger buffers (the
frame buffers, the hub and class tables) are allocated at start, only when
Ethernet is on, which leaves the static cost with Ethernet off near zero.
Then check the other budgets (min free internal heap ≥ 48 KB, largest block
≥ 24 KB, loop p99 ≤ 5 ms) with Ethernet off and on.

## How it fits the design principles

- No retries are added. TCP's own resends, DHCP's own protocol and
  `TcpInterface`'s existing reconnect schedule are unchanged. The hub fix
  re-reads the hub's state, which USB hubs expect: a hub is polled for as
  long as it is attached.
- The one new timeout (sending a frame) is a hard ceiling on a call that
  should take microseconds. When it trips, the frame is dropped, counted and
  logged.
- Nothing waits in `loop()`. The library works on its own tasks. The send
  ceiling keeps lwIP's thread from waiting on the adapter.
- Every drop path reports itself: send timeouts, frames dropped with the link
  down, unsupported adapters ("adapter 0bda:8153 has no ECM configuration"),
  hub read errors.

## Tests

Each fix gets a test that fails before it and passes after.

**Bench, V4.2 and R8, on the dock (and any other ECM adapter at hand):**

1. Boot with a computer attached: USB serial as today, Ethernet off or on.
2. Boot from the adapter alone: wired address within 10 s; log on the UART
   header.
3. Restart while in use, 10 times (portal save, and a forced restart): the
   wired link is back every time. Fails on upstream CherryUSB's hub code.
4. Pull the Ethernet cable for 30 s and put it back. The wired interface
   goes down and up, backbones move to WiFi and back, and nothing restarts.
   Fails before the link-report change.
5. Hold the adapter busy, or unplug it mid-send, with a battery on the board.
   Sending never blocks for more than the ceiling, and WiFi keeps working.
   Fails before the send ceiling.
6. Unplug and replug the adapter (board on battery): it's found again.
7. Ethernet on, then off in the portal: USB serial works after the restart.
   Fails without the port hand-back at boot.
8. Unsupported adapter: a clear log line, no crash. WiFi carries on if on.
9. Bluetooth peers connected while the wired link carries traffic: no BLE
   drops, budgets met.
10. mDNS: the node answers `rtnode….local` over the cable and over WiFi.
11. Both sides on both networks: WiFi and the cable on, backbone up over the
    cable, one LAN client on WiFi and one on the cable. Both clients reach
    the backbone; the firewall's whitelists fill the same way as for WiFi
    clients; pulling the cable moves the backbone to WiFi and the WiFi client
    carries on.
12. Cable only, WiFi off: backbones and LAN clients over the cable; the
    watchdog never fires.

**Contract suites** (CONTRACT_TESTS.md): lan, wan and soak over the cable,
WiFi off. Then with WiFi on as well.

**Private staging network**, as for every release: the node's backbone over
the cable to the staging RPi, and a day-long soak with the serial log on the
UART header.

## Order of work and effort

1. Library in, with the two new fixes and its settings: 1–2 days.
2. Port role at boot, `network_is_up()`, watchdog, portal: 1–2 days.
3. Bench tests 1–12 on the dock: 1 day.
4. Contract suites and the staging soak: 1–2 days, mostly waiting.

About a week. Nothing here needs the RNode or Reticulum code changed.

## Unknowns

- Whether IDF 4.4's mDNS answers on the wired interface once it's named as
  its Ethernet slot. If not: advertise over WiFi only, and document it.
- How Realtek adapters report link changes over ECM (the AX88179A's reports
  were seen only at first link-up).
- Whether every adapter with a charging port passes power back to a board
  that only takes power. James's dock does; others may not, which is one
  reason it promises nothing.
- Whether the dock's out-of-sequence hub report is the dock's quirk or the
  library's. The fix covers both. The fix could also be offered to CherryUSB.
