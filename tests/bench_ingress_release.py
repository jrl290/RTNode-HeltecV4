#!/usr/bin/env python3
"""Do announces held by ingress control come back out, on schedule?

A flooding client (F) announces five fresh destinations a second for 70 s,
then stays connected and silent. An observer (B), connected first on its own
TCP connection, records when it hears each of F's destinations.

In RTNode's ingress control (RETICULUM_BEHAVIOUR.md §1) the burst ends once
the rate is back under the threshold and has not reached it for 15 s; held
announces are released from then on, one every 5 s, fewest hops first (all
of F's have the same hops, so oldest first), by a job that runs every 5 s.
So B should hear:
  - during the flood, only the few destinations the node took before it
    detected the burst;
  - nothing more until the node logs "burst over";
  - then one destination about every 5 s, in the order F announced them.

    ../.venv/bin/python tests/bench_ingress_release.py --serial /dev/cu.usbmodem101 --host 192.168.2.125

Resets the node first, and floods only after the bench build's "backbones
off" mark (bench_load.py). Takes about three minutes.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_load import HERE, WORKSPACE_PYTHON, LineCapture, start_bench_node  # noqa: E402

FLOOD_RATE = 5.0
FLOOD_SECONDS = 70.0
BURST_HOLD = 15         # s at or over the threshold, then the flood may end (§1)
FREQ_DECAY = 10         # s before an arrival may be forgotten (§1)
RELEASE_INTERVAL = 5    # s between releases from one source (§1)
JOB_INTERVAL = 5        # s between release rounds (§1)
BURST_END_WITHIN = FREQ_DECAY + BURST_HOLD + 4 * JOB_INTERVAL  # after F falls silent
SLACK = 3               # release to B: the node's rebroadcast window, TCP
SKEW = 1                # serial lines reach the host up to 0.2 s after the event
RELEASES_TO_SEE = 3


class Observer:
    """First time B hears each destination announced."""
    aspect_filter = None

    def __init__(self):
        self.heard = {}
        self.lock = threading.Lock()

    def received_announce(self, destination_hash, announced_identity, app_data):
        with self.lock:
            self.heard.setdefault(destination_hash.hex(), time.time())


def write_config(directory, host, port):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "config"), "w") as config:
        config.write(
            "[reticulum]\n"
            "  enable_transport = No\n"
            "  share_instance = No\n"
            "[logging]\n"
            "  loglevel = 2\n"
            "[interfaces]\n"
            "  [[RTNode local TCP]]\n"
            "    type = TCPClientInterface\n"
            "    enabled = yes\n"
            # B must hear what the node sends when it sends it, not hold it
            # under its own ingress control.
            "    ingress_control = No\n"
            f"    target_host = {host}\n"
            f"    target_port = {port}\n"
        )


def wait_for_line(lines, pattern, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for t, text in list(lines):
            match = re.search(pattern, text)
            if match:
                return t, match
        time.sleep(0.2)
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--out", help="results directory (default tests/bench-results/<time>-ingress-release)")
    args = parser.parse_args()

    out = args.out or os.path.join(HERE, "bench-results", time.strftime("%Y%m%d-%H%M%S") + "-ingress-release")
    os.makedirs(out, exist_ok=True)
    capture = start_bench_node(args.serial, args.host, args.port, out, isolated=True)
    time.sleep(10)

    import RNS
    work = tempfile.mkdtemp(prefix="rtnode-ingress-release-")
    write_config(os.path.join(work, "observer"), args.host, args.port)
    RNS.Reticulum(os.path.join(work, "observer"))
    observer = Observer()
    RNS.Transport.register_announce_handler(observer)
    time.sleep(3)  # the TCP connection

    # F stays connected until B has seen RELEASES_TO_SEE releases, with room.
    linger = BURST_END_WITHIN + (RELEASES_TO_SEE + 1) * (RELEASE_INTERVAL + JOB_INTERVAL) + 20
    flood = subprocess.Popen(
        [WORKSPACE_PYTHON, os.path.join(HERE, "bench_flood.py"), args.host, str(args.port),
         str(FLOOD_RATE), str(FLOOD_SECONDS), os.path.join(out, "flood-rns"), str(linger)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    flood_capture = LineCapture(flood, os.path.join(out, "flood.log"))
    flood_capture.start()

    silent_at, _ = wait_for_line(flood_capture.lines, r"done announced=", FLOOD_SECONDS + 60)
    if silent_at is None:
        sys.exit("the flood client never finished")
    burst_at, _ = wait_for_line(capture.lines, r"\[INGRESS\] .* burst: ", 1)
    over_at, over = wait_for_line(capture.lines, r"\[INGRESS\] .* burst over, (\d+) announces held", BURST_END_WITHIN)
    if burst_at is None or over_at is None:
        sys.exit(f"no burst{'' if burst_at else ' start'}{'' if over_at else ' end'} in the node's log")
    print(f"flood {FLOOD_RATE:g}/s for {FLOOD_SECONDS:g} s; burst logged {burst_at - silent_at + FLOOD_SECONDS:.1f} s in, "
          f"over {over_at - silent_at:.1f} s after F fell silent with {over.group(1)} held", flush=True)

    # The job that ends the burst releases the first held announce.
    first_due = over_at
    deadline = first_due + RELEASES_TO_SEE * (RELEASE_INTERVAL + JOB_INTERVAL) + JOB_INTERVAL + SLACK
    while time.time() < deadline:
        time.sleep(1)

    announced = []  # F's destinations in the order it announced them
    for _, text in flood_capture.lines:
        match = re.match(r"[\d.]+ announce \d+ ([0-9a-f]{32}) ", text)
        if match:
            announced.append(match.group(1))
    with observer.lock:
        heard = {h: observer.heard[h] for h in announced if h in observer.heard}

    during = sorted((t, h) for h, t in heard.items() if t < silent_at + SLACK)
    early = sorted((t, h) for h, t in heard.items() if silent_at + SLACK <= t < first_due - SKEW)
    released = sorted((t, h) for h, t in heard.items() if t >= first_due - SKEW)
    print(f"B heard {len(during)} of F's {len(announced)} destinations during the flood "
          f"(taken before the burst was detected)")

    failures = []
    if early:
        failures.append(f"{len(early)} destination(s) heard during the hold, first "
                        f"{early[0][0] - over_at:.1f} s after the burst ended")
    if len(released) < RELEASES_TO_SEE:
        failures.append(f"only {len(released)} release(s) heard, wanted {RELEASES_TO_SEE}")
    previous = first_due - RELEASE_INTERVAL
    for n, (t, h) in enumerate(released[:RELEASES_TO_SEE]):
        gap = t - previous
        low, high = (-SKEW, SLACK) if n == 0 else (RELEASE_INTERVAL - SLACK, RELEASE_INTERVAL + JOB_INTERVAL + SLACK)
        offset = t - first_due if n == 0 else gap
        label = "after the burst ended" if n == 0 else "after the previous release"
        print(f"release {n + 1}: {h[:8]} {offset:.1f} s {label}")
        if not (low <= offset <= high):
            failures.append(f"release {n + 1} came {offset:.1f} s {label}, expected {low}–{high} s")
        previous = t
    taken = {h for _, h in during}
    held_order = [h for h in announced if h not in taken]
    released_order = [h for _, h in released[:RELEASES_TO_SEE]]
    if released_order and released_order != held_order[:len(released_order)]:
        failures.append("releases not in the order F announced them (oldest first)")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, flush=True)
        code = 1
    else:
        print(f"PASS: nothing leaked during the hold; {RELEASES_TO_SEE} releases on schedule, oldest first", flush=True)
        code = 0
    flood.kill()
    capture.stop.set()
    capture.join(timeout=5)
    print(f"results: {out}", flush=True)
    os._exit(code)


if __name__ == "__main__":
    main()
