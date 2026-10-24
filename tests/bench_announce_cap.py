#!/usr/bin/env python3
"""Does the LoRa announce cap rate-limit announces without holding back the LAN?

The firewall keeps the backbone's flood off LAN-side devices; LAN-side traffic
is never blocked from them, but slow interfaces may rate-limit it. On LoRa
(under 50 kbps) a relayed announce may go out only once the previous one has
had tx_time / 2% to clear; the rest wait in a queue. A path request from a
LAN-side device always goes out, and one from the backbone waits for the cap.

Two contract agents (contract_agent.py), one on RTNode's local TCP server and
one on the stand-in backbone (tests/wan-backbone), with a bench build whose
backbone 1 is that stand-in:

  W  the LAN asks for unknown R3 (so R3 is whitelisted for the backbone);
     100 s of quiet then lets R3's discovery entry and the stand-in's 45 s
     path request gate expire
  A  the LAN announces five fresh destinations at once
  B  the LAN asks for unknown R1 while those announces are queued
  C  the backbone asks for R3, same window
  D  the LAN asks for unknown R2, same window
  E  wait three cap windows for the queue to drain

    ../.venv/bin/python tests/bench_announce_cap.py --host <node> --node-log <serial log>

PASS: announces leave LoRa a cap window apart; B and D go out on LoRa at
once; C is searched for (DISCOVERY-EXPIRED) but not sent on LoRa; the
backbone hears every flooded announce; the node neither panics nor restarts. The serial log needs host timestamps
("<epoch> <line>") and the node's [LoRa] TX lines (VERBOSE).
"""
import argparse
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PY = os.path.join(HERE, "..", "..", ".venv", "bin", "python")
AGENT = os.path.join(HERE, "contract_agent.py")
ANNOUNCE_MIN, PREQ_RANGE = 120, (60, 90)    # bytes on LoRa
timeline = []


def note(**kv):
    kv.setdefault("t", round(time.time(), 3))
    timeline.append(kv)
    print(json.dumps(kv), flush=True)


class Agent:
    def __init__(self, name, medium, host, port):
        self.name = name
        self.events = queue.Queue()
        self.p = subprocess.Popen([PY, "-I", AGENT, "--name", name, "--medium", medium, "--host", host,
                                   "--port", str(port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.p.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            ev["agent"] = self.name
            timeline.append(ev)
            self.events.put(ev)

    def send(self, **cmd):
        note(agent=self.name, cmd=cmd)
        self.p.stdin.write(json.dumps(cmd) + "\n")
        self.p.stdin.flush()

    def wait(self, event, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                ev = self.events.get(timeout=max(0.1, deadline - time.time()))
            except queue.Empty:
                break
            if ev.get("event") == event:
                return ev
        return None


def run(args):
    lan = Agent("lan", "tcp", args.host, args.port)
    wan = Agent("wan", "wan", args.wan_host, args.wan_port)
    for a in (lan, wan):
        if not a.wait("ready", 30):
            raise SystemExit(f"{a.name} not ready")
    steps = {}

    def step(key, text, **kv):
        steps[key] = time.time()
        note(step=f"{key}: {text}", **kv)

    r3 = os.urandom(16).hex()
    step("W", "LAN path request for R3", dest=r3)
    lan.send(cmd="path", dest=r3, timeout=3)
    lan.wait("path", 10)
    time.sleep(100)

    step("A", "LAN floods fresh announces")
    lan.send(cmd="flood", rate=10, duration=0.4)
    flood = lan.wait("flood", 10)
    time.sleep(8)
    r1 = os.urandom(16).hex()
    step("B", "LAN path request for R1 while announces are queued", dest=r1)
    lan.send(cmd="path", dest=r1, timeout=3)
    lan.wait("path", 10)
    time.sleep(3)
    step("C", "WAN path request for R3, same window", dest=r3)
    wan.send(cmd="path", dest=r3, timeout=3)
    wan.wait("path", 10)
    time.sleep(3)
    r2 = os.urandom(16).hex()
    step("D", "LAN path request for R2, same window", dest=r2)
    lan.send(cmd="path", dest=r2, timeout=3)
    lan.wait("path", 10)
    step("E", f"wait {3 * args.window + 30:.0f} s for the queue to drain")
    time.sleep(3 * args.window + 30)
    steps["end"] = time.time()
    for a in (lan, wan):
        a.send(cmd="quit")
    time.sleep(2)
    return steps, flood["hashes"] if flood else [], {"R1": r1, "R2": r2, "R3": r3}


def report(steps, hashes, dests, node_log, window):
    tx, searched, boots = [], [], []
    for line in open(node_log, errors="replace"):
        m = re.match(r"(\d+\.\d+) .*(rst:0x|Guru Meditation)", line)
        if m and float(m.group(1)) >= steps["W"]:
            boots.append(float(m.group(1)))
        m = re.match(r"(\d+\.\d+) .*\[LoRa\] TX (\d+) bytes", line)
        if m:
            tx.append((float(m.group(1)), int(m.group(2))))
        m = re.match(r"(\d+\.\d+) .*DISCOVERY-EXPIRED: path request for (\w+)", line)
        if m:
            searched.append((float(m.group(1)), m.group(2)))
    t_a, t_end = steps["A"], steps["end"]
    announces = [t for t, n in tx if t_a - 1 <= t <= t_end and n >= ANNOUNCE_MIN]
    gaps = [round(b - a, 1) for a, b in zip(announces, announces[1:])]

    def preq_out(key):
        return [round(t - steps[key], 2) for t, n in tx
                if steps[key] <= t <= steps[key] + 5 and PREQ_RANGE[0] <= n <= PREQ_RANGE[1]]

    out = {key: preq_out(key) for key in "WBCD"}
    r3_searched = any(h == dests["R3"][:8] and t > steps["C"] for t, h in searched)
    heard = {ev["dest"] for ev in timeline if ev.get("agent") == "wan" and ev.get("event") == "heard"
             and ev["dest"] in hashes}
    print(f"announces on LoRa after A: {len(announces)}, gaps {gaps} s")
    for key in "WBCD":
        print(f"{key} path request on LoRa within 5 s: {out[key] or 'none'}")
    if boots:
        print(f"node restarted or panicked at {[round(t - t_a, 1) for t in boots]} s from A")
    print(f"C searched for by the node: {r3_searched}; backbone heard {len(heard)}/{len(hashes)} flooded announces")
    checks = {
        "cap-spacing": len(gaps) >= 2 and all(g >= 0.9 * window for g in gaps),
        "lan-preq-out": bool(out["B"]) and bool(out["D"]),
        "wan-preq-held": not out["C"] and r3_searched,
        "wan-uncapped": len(heard) == len(hashes) > 0,
        "node-stayed-up": not boots,
    }
    for name, ok in checks.items():
        print(("PASS " if ok else "FAIL ") + name)
    return all(checks.values()), checks


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="the node (its local TCP server)")
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--wan-host", default="127.0.0.1")
    parser.add_argument("--wan-port", type=int, default=4290)
    parser.add_argument("--node-log", required=True)
    parser.add_argument("--window", type=float, default=90,
                        help="cap window for one relayed announce, s (183 B at 814 bps: 90)")
    parser.add_argument("--out")
    args = parser.parse_args()
    steps, hashes, dests = run(args)
    ok, checks = report(steps, hashes, dests, args.node_log, args.window)
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"checks": checks, "steps": steps, "dests": dests, "timeline": timeline}, f, indent=1)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
