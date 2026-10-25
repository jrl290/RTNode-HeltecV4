# Reticulum behaviour RTNode implements

RTNode is GPL-3, because it is built on the RNode firmware. The Python reference
implementation of Reticulum is under the Reticulum License, whose conditions the
GPL does not allow us to pass on. So nothing in RTNode is translated from the
Python code. Where RTNode needs to behave as a Reticulum node does, the behaviour
is described here, in our own words, and the code is written from these
descriptions.

The numbers below are either facts the network relies on (formats, constants
other nodes check) or parameters chosen so that RTNode behaves like other
Reticulum transport nodes. Sections name the code that implements them.

## 1. Ingress control: announces from a flooding source

*Code: `lib/microReticulum/src/AnnounceGate.h`; wired up in `Interface.cpp`,
`Transport.cpp` (inbound announces, `release_held_announces()`),
`TcpInterface.h` and `BleInterface.h`.*

A device that suddenly sends announces for many destinations the node has never
heard of, such as a new client replaying a big path table or a misbehaving app,
could keep an ESP32 busy checking signatures and fill its path table. Ingress
control holds such announces back and lets them in slowly once the flood is over.

**Sources.** Each interface is a source. A TCP server interface keeps a
separate source for each connected client slot, so that one flooding client does
not get every other client's announces held. A Bluetooth interface starts a new
source for each new peer. When a client or peer goes, its source, and
everything held for it, goes too.

**Arrivals.** Every announce that arrives from a source is recorded with its
arrival time. Only the 48 most recent arrivals are kept. An announce that is
being let back in after being held is not recorded again.

**Rate.** A source's announce rate is the number of recorded arrivals divided
by the seconds since the oldest of them. It counts as zero while there are two
or fewer arrivals, or while no time has passed since the oldest. Each time the
rate is measured, if the oldest arrival is more than 10 seconds old it is
forgotten afterwards. One is forgotten per measurement, so a quiet source's
rate drains away gradually.

**Flood threshold.** A source that is less than 2 hours old floods above
3 announces per second. An older source floods above 10 per second.

**Start of a flood.** When a source that is not flooding measures above its
threshold, it is flooding from then on. The first held announce may be let in
15 seconds later at the earliest. The node logs
`[INGRESS] <source> burst: <rate> announces/s > <threshold>, holding new destinations`.

**End of a flood.** While a source floods, every measurement at or above its
threshold counts as still flooding. The flood ends at the first measurement
that meets all three of these conditions:
- it is below the threshold;
- the source has not measured at or above the threshold for more than 15 seconds;
- the source still has at least two recorded arrivals.

The node then logs `[INGRESS] <source> burst over, <n> announces held`. A
measurement that ends a flood still counts as flooding for the announce that
caused it.

**Which announces are held.** An announce is held only when all of these are
true:
- it arrived on an interface and is not being let back in;
- its destination is not in the path table;
- no path request (the node's own, or one forwarded for someone else) is waiting for it;
- its source is flooding.

Re-announces of known destinations are not held. Announce rate limiting
handles those.

**Holding.** For each source, the node holds at most one announce per
destination. A newer announce for the same destination replaces the held one,
even when the store is full. An announce 127 or more hops away is never held.
A source holds at most 256 announces, and all sources together at most 512.
Boards without PSRAM allow 32 for each. An announce that cannot be held is
dropped and counted (the `ic_drop` telemetry), and the first drop is logged.
A held announce is kept exactly as it arrived, IFAC included, so it is checked
in full when it is let in.

**Letting held announces in.** Every 5 seconds, counted from the end of the
previous round, the node visits every source of every interface:
1. It measures the source's rate. This can forget an old arrival and can end
   a flood, as above.
2. If the source is not flooding, holds something, its next release time has
   come, and a second measurement is below its threshold, it takes out the
   held announce with the fewest hops. When several tie, the one held longest
   goes first. The source's next release is then 5 seconds later.

When the round is over, each announce taken out goes through inbound
processing as if it had just arrived on its interface. It is neither recorded
as an arrival nor held again.

## 2. Answering path requests the node forwarded

*Code: `Transport::path_request_handler()` and
`Transport::answer_forwarded_path_requests()` in `Transport.cpp`.*

When a device asks the node for a path it does not know, and the node passes
the request on to the rest of the network, it remembers who asked:
- the interface the request came in on;
- any later askers for the same destination, added to the same entry, each interface once;
- the entry lasts as long as a path request does (`PATH_REQUEST_TIMEOUT`);
- at most 64 destinations are remembered at once.

When an announce for that destination is accepted into the path table, every
remembered asker gets it at once as a path response, and the entry is
forgotten. The response is the announce's data, sent on the asker's interface
as a transport packet with this node as the transport, with the announce's hop
count and context flag kept.

The node does not also re-send every announce straight to its local clients.
RTNode treats all its LAN interfaces as local, LoRa included, and that would
double the airtime of every announce on LoRa.

## 3. Relay rules

*Code: `Transport::inbound()` in `Transport.cpp`.*

- **Proofs on an established link** are relayed only by link transport. A
  proof goes out on the side of the link opposite to the one it came in on,
  and nowhere else.
- **A packet with no transport header** names no next hop: its sender
  believes the destination is in range. If the node's path to that
  destination leads back out of the same interface, the two are in range of
  each other and the node does not relay the packet.
- **A packet travelling a link back towards its initiator** is relayed only
  when its hop count equals exactly the hop count recorded for that direction
  of the link.
- **The duplicate filter.** An accepted packet normally goes into the filter
  of packets already seen as soon as it arrives. Two kinds do not:
  - a packet addressed to a link this node carries goes in only once link
    transport has decided that this node is the one to relay it;
  - a link request proof does not go in on arrival.

  On a shared medium such as LoRa the node can hear a copy of such a packet
  before the copy it is meant to relay. Filtering the early copy would make
  it drop the one that matters.

## 4. When a client goes

*Code: `Transport::forget_interface_routes()`, called by `BleInterface.h`.*

When a peer that had its own interface slot leaves, the node forgets every
path, link and reverse entry that goes through that slot, so nothing is sent
into a connection that no longer exists.

## 5. Interface discovery announces

*Code: `Advertise.h`.*

The announce format follows the description in the Reticulum manual
(https://reticulum.network/manual/interfaces.html). It is sent on the
`rnstransport.discovery.interface` destination, with app data made of a flags
byte, a msgpack map of numbered fields, and a proof-of-work stamp. These
values are what other nodes check:

| What | Value |
|---|---|
| Stamp value | 16. A node on Reticulum 1.5 ignores a value-14 stamp three times in four. |
| Workblock expansion rounds | 20 |
| Stamp size | 32 bytes |

These are RTNode's own choices, made to match other nodes:

| What | Value |
|---|---|
| Default interval | 6 hours |
| First announce | 1 minute after boot |
| IFAC network name and key | published only when the user turns that on (off by default) |

## 6. The link MTU in a relayed link request

*Code: `fit_link_request_mtu()` in `Transport.cpp`.*

A link request may end with the MTU its sender wants for the link, together
with the link mode (`LINK_MTU_SIZE` bytes). When the node relays such a
request, it makes sure the request asks for no more than this node can carry:
- If the interface the request goes out on has no hardware MTU, or can
  neither configure its MTU nor has a fixed one, the node removes the MTU
  from the request. The link then uses the default MTU.
- Otherwise the link can have at most the smallest of: the MTU asked for, the
  hardware MTU of the interface the request goes out on, and the hardware MTU
  of the interface it came in on, if that one has one. When that is less than
  what was asked, the node rewrites the request to ask for it, keeping the
  link mode.

Without this, two TCP devices linking through a node with a smaller buffer on
the way (a 1064-byte serial or LoRa frame) agree on an MTU the node cannot
carry, and transfers stall part way.

## 7. The announce cap on slow interfaces

*Code: `Transport::announce_cap_allows()` and
`Transport::process_announce_queues()` in `Transport.cpp`; the LoRa
interface's bitrate and cap in `RNode_Firmware.ino`.*

Relayed announces could take all of a slow interface's airtime. So on an
interface slower than 50 kbps (in practice LoRa) they are kept to a share of
it, 2% (`Type::Reticulum::ANNOUNCE_CAP`).

**What is capped.** Only relayed announces sent to every interface. An
announce this node makes (0 hops), one sent to a single interface (a path
response), and anything on an interface whose bitrate is unknown (0) or at
least 50 kbps go out at once.

**When one may go.** Each interface keeps the time its next capped announce
may go. An announce goes at once if nothing is queued and that time has
passed. Sending it moves the time on by its airtime (its size in bits over
the bitrate) divided by the cap: at 2%, fifty times its airtime.

**The queue.** Any other capped announce waits in the interface's queue,
which keeps one announce per destination. A newer announce for a queued
destination (newer by the emission time carried in the announce) takes the
old one's place, with its own hops and a fresh queue time; an older or equal
one is let go. The queue holds at most 64. When it is full, an announce for a
new destination is dropped.

**Sending from the queue.** On each pass of the transport jobs, for each
interface whose time has come:
1. it forgets queued announces more than 3 hours old
   (`QUEUED_ANNOUNCE_LIFE`), counted from their queue time;
2. it sends the queued announce with the fewest hops, the earliest queued
   when several tie;
3. it moves the interface's time on as above, or not at all when the bitrate
   or the cap is unknown.

**Path requests.** A path request the node passes on for a device on the
backbone is not sent on an interface where the cap is in force (announces
queued, or its time not yet come), and sending one moves the time on like an
announce. One passed on for a LAN-side device always goes out, and leaves the
cap alone: the firewall protects LAN-side devices from the backbone, never
from each other.

Under heap pressure the node empties every announce queue.
