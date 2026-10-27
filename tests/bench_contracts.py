#!/usr/bin/env python3
"""RTNode's transport contracts, end to end (CORE_PRINCIPLES.md).

Starts one contract_agent.py per medium and checks, for every ordered pair:

  announce  A's announce reaches B (LAN announces cross every interface)
  path      A finds B's never-announced destination: the path request goes
            out through RTNode and B's answer comes back
  link      A opens a Link to B, echoes packets over it, and sends B a
            resource that B receives intact (SHA-256 checked)

LAN media: tcp (RTNode's local TCP server), prnsd (Prns Bluetooth, through a
prnsd on this Mac: tests/prnsd/config) and lora (an RNode on a serial port).
A trailing digit starts another agent on the same medium (tcp2).

    ../.venv/bin/python tests/bench_contracts.py lan --host 192.168.2.125 \\
        --media tcp,prnsd,lora --rnode-port /dev/cu.usbserial-0001

The wan suite checks the firewall (CORE_PRINCIPLES.md) against a stand-in
backbone on this machine (tests/wan-backbone/config), which a bench build
with RTNODE_BENCH_WAN_HOST connects backbone 1 to:

  wan-path-unmentioned  a WAN path request for a LAN destination the LAN has
                        never mentioned dies at the firewall
  wan-announce-held     an unsolicited WAN announce does not reach the LAN
  lan-announce-out      LAN announces reach the WAN
  lan-path-wan(-silent) the LAN finds WAN destinations, announced or not
  wan-path-mentioned    once the LAN has mentioned a destination, a WAN path
                        request for it is answered
  lan-link-wan, wan-link-lan
                        links both ways carry echoes and an intact resource
  lan-packet-wan, wan-packet-lan
                        single packets both ways arrive and their proofs
                        come back
  wan-announce-known    a WAN announce for a destination the LAN asked about
                        crosses
  wan-link-survives-churn, wan-reaches-quiet-lan
                        a link across the firewall, and a LAN destination
                        that stays quiet, survive the LAN mentioning more new
                        addresses than the whitelists hold
  wan-flood             a WAN flood of announces for unknown destinations
                        reaches no LAN agent, while links on both sides keep
                        working and the node stays healthy (--node-log)

    ../.venv/bin/rnsd --config tests/wan-backbone      # separately
    ../.venv/bin/python tests/bench_contracts.py wan --media tcp,tcp2 \\
        --node-log <serial log of the node>

The soak suite runs WiFi, Bluetooth and LoRa at once for --soak-s: echoes
every second between tcp and prnsd with a resource a minute, LoRa echoes to
tcp and prnsd, and WAN echoes and resources when the stand-in backbone is
connected; each flow's loss and latency, and the node's own reports, decide.

LoRa pairs use smaller payloads (SF10/125 kHz is about 1 kbps). Results go to
tests/bench-results/<time>-contracts-<suite>/results.json.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PYTHON = os.path.abspath(os.path.join(HERE, "..", "..", ".venv", "bin", "python"))


class Agent:
    def __init__(self, name, medium, args, extra=()):
        self.name, self.medium, self.label = name, re.sub(r"\d+$", "", medium), medium
        medium = self.medium
        command = [PYTHON, os.path.join(HERE, "contract_agent.py"), "--name", name, "--medium", medium]
        if medium == "tcp":
            command += ["--host", args.host, "--port", str(args.port)]
        elif medium == "prnsd":
            command += ["--host", "127.0.0.1", "--port", str(args.prnsd_port)]
        elif medium == "lora":
            command += ["--rnode-port", args.rnode_port, "--txpower", str(args.lora_txpower)]
        elif medium == "wan":
            command += ["--host", args.wan_host, "--port", str(args.wan_port)]
        command += list(extra)
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.events, self.lock = [], threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()
        ready = self.wait(lambda e: e["event"] == "ready", 60)
        if not ready:
            raise SystemExit(f"agent {name} ({medium}) did not start")
        self.dest, self.silent = ready["dest"], ready["silent"]

    def _read(self):
        for line in self.process.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                continue  # RNS log lines
            with self.lock:
                self.events.append(event)

    def mark(self):
        with self.lock:
            return len(self.events)

    def wait(self, predicate, timeout, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for event in self.events[since:]:
                    if predicate(event):
                        return event
            time.sleep(0.05)
        return None

    def call(self, reply, wait_s, **command):
        since = self.mark()
        self.process.stdin.write(json.dumps(command) + "\n")
        self.process.stdin.flush()
        # A command about a destination waits for the reply about that one;
        # any other (announce, silent, flood) for the first reply of its kind.
        want = command.get("dest")
        return self.wait(lambda e: e["event"] == reply and (want is None or e.get("dest") == want),
                         wait_s, since)

    def quit(self):
        try:
            self.process.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
            self.process.stdin.flush()
        except OSError:
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()


def slow(*agents):
    return any(a.medium == "lora" for a in agents)


def lan(args):
    media = args.media.split(",")
    agents = [Agent(f"{re.sub(r'[0-9]+$', '', m)}{i}x{int(time.time()) % 10000}", m, args) for i, m in enumerate(media)]
    print("agents: " + ", ".join(f"{a.label} {a.dest[:8]} (silent {a.silent[:8]})" for a in agents), flush=True)
    results, failures = [], []

    record = recorder(results, failures)

    try:
        print("announces:", flush=True)
        for a in agents:
            marks = {b.name: b.mark() for b in agents if b is not a}
            sent = time.time()
            a.call("announced", 5, cmd="announce")
            for b in agents:
                if b is a:
                    continue
                heard = b.wait(lambda e: e["event"] == "heard" and e["dest"] == a.dest,
                               45 if slow(a, b) else 15, marks[b.name])
                record("announce", a, b, heard, s=round(heard["t"] - sent, 2) if heard else None,
                       hops=heard.get("hops") if heard else None)

        print("paths to never-announced destinations:", flush=True)
        for a in agents:
            for b in agents:
                if b is a:
                    continue
                if "prnsd" in (a.medium, b.medium) and not args.prnsd_gateway:
                    # prnsd is a transport node; it passes requests for unknown
                    # paths on only from gateway-mode interfaces.
                    print(f"  SKIP path      {a.label:>5} -> {b.label:<5} prnsd not in gateway mode "
                          "(tests/prnsd/config; then --prnsd-gateway)", flush=True)
                    continue
                # A destination of b's that nobody has asked for yet: a
                # gateway-mode transport node (prnsd) that passed on another
                # agent's request for the same one holds this request for
                # 45 s (RNS 1.5.2 path-request gate) and never hears the answer.
                fresh = b.call("silent", 10, cmd="silent")
                path = a.call("path", 60, cmd="path", dest=fresh["dest"], timeout=45 if slow(a, b) else 15)
                record("path", a, b, path and path["ok"], s=path.get("s") if path else None,
                       hops=path.get("hops") if path else None)
                if slow(a, b):
                    time.sleep(args.lora_settle)

        print("links, echoes and resources:", flush=True)
        for a in agents:
            for b in agents:
                if b is not a:
                    link_check(record, "link", a, b, args)

        print("single packets and their proofs:", flush=True)
        for a in agents:
            for b in agents:
                if b is not a:
                    packet_check(record, "packet", a, b, args)
    finally:
        for agent in agents:
            agent.quit()
    return results, failures


def recorder(results, failures):
    def record(check, a, b, ok, **detail):
        row = {"check": check, "from": a.label, "to": b.label, "ok": bool(ok), **detail}
        results.append(row)
        print(f"  {'PASS' if ok else 'FAIL'} {check:9} {a.label:>5} -> {b.label:<5} " +
              " ".join(f"{k}={v}" for k, v in detail.items()), flush=True)
        if not ok:
            failures.append(row)
    return record


def link_check(record, check, a, b, args):
    """a opens a link to b, echoes over it and sends b a resource."""
    lora = slow(a, b)
    link = a.call("link", 90, cmd="link", dest=b.dest, aspects=f"contract.{b.name}", timeout=60 if lora else 20)
    if not (link and link["ok"]):
        record(check, a, b, False, reason=link.get("reason") if link else "no reply")
        return
    echo = a.call("echo", 200, cmd="echo", dest=b.dest, n=3, size=200 if lora else 400, timeout=40 if lora else 10)
    size = args.lora_resource if lora else args.resource
    since = b.mark()
    resource = a.call("resource", 600, cmd="resource", dest=b.dest, size=size, timeout=540 if lora else 120)
    received = b.wait(lambda e: e["event"] == "resource_in" and e["dest"] == b.dest, 10, since)
    intact = bool(resource and resource["ok"] and received and received.get("sha") == resource["sha"])
    record(check, a, b, echo and echo["ok"] == echo["n"] and intact,
           link_s=link["s"], echo=f"{echo['ok']}/{echo['n']}" if echo else None,
           echo_p50_ms=echo.get("rtt_p50_ms") if echo else None,
           resource=f"{size}B in {resource['s']}s" if resource else None, intact=intact)
    a.call("closed", 10, cmd="close", dest=b.dest)
    if lora:
        time.sleep(args.lora_settle)


def packet_check(record, check, a, b, args):
    """a sends b single packets (no link); b proves each, and the proofs come
    back through the node's reverse table."""
    lora = slow(a, b)
    since = b.mark()
    # One at a time on LoRa, as an app sends messages: back to back, the
    # next packet goes on air while the first one's proof is coming back.
    sent = a.call("send", 180, cmd="send", dest=b.dest, aspects=f"contract.{b.name}", n=3,
                  size=100 if lora else 300, gap=10 if lora else 0.2, wait=30 if lora else 5)
    arrived = sum(1 for e in b.events[since:] if e["event"] == "packet_in" and e["dest"] == b.dest)
    record(check, a, b, sent and sent.get("proved") == sent.get("n") and arrived == sent.get("n"),
           arrived=f"{arrived}/{sent.get('n') if sent else '?'}",
           proved=f"{sent.get('proved') if sent else '?'}/{sent.get('n') if sent else '?'}",
           reason=sent.get("reason") if sent and sent.get("reason") else None)
    if lora:
        time.sleep(args.lora_settle)


def node_log(path, since, until=None):
    """(host time, text) lines of the node's serial log in a window."""
    lines = []
    if not path:
        return lines
    try:
        with open(path, errors="replace") as f:
            for line in f:
                stamp, _, text = line.partition(" ")
                try:
                    t = float(stamp)
                except ValueError:
                    continue
                if t >= since and (until is None or t <= until):
                    lines.append((t, text.rstrip()))
    except OSError:
        pass
    return lines


def node_health(lines):
    """What the node's own reports say about a window."""
    health = {"reboots": 0, "panics": 0, "stall_max_ms": 0, "heap_min": None, "paths_max": 0,
              "wl1_max": 0, "wl2_max": 0, "res_lines": 0}
    for _, text in lines:
        if text.startswith("[RES] boot"):
            health["reboots"] += 1
        elif text.startswith("[RES]"):
            health["res_lines"] += 1
            match = re.search(r"heap=(\d+)/(\d+)/(\d+)", text)
            if match:
                low = int(match.group(2))
                health["heap_min"] = low if health["heap_min"] is None else min(health["heap_min"], low)
        if "Guru Meditation" in text or "abort()" in text or "task_wdt" in text:
            health["panics"] += 1
        match = re.search(r"\[STALL\] t=\d+ (\d+)ms", text)
        if match:
            health["stall_max_ms"] = max(health["stall_max_ms"], int(match.group(1)))
        match = re.search(r"paths: (\d+)", text)
        if match:
            health["paths_max"] = max(health["paths_max"], int(match.group(1)))
        match = re.search(r"bla: (\d+) bma: (\d+)", text)
        if match:
            health["wl1_max"] = max(health["wl1_max"], int(match.group(1)))
            health["wl2_max"] = max(health["wl2_max"], int(match.group(2)))
    return health


def backbone_connected(node_host, port):
    """Whether the node holds a connection to the stand-in backbone here."""
    listing = subprocess.run(["lsof", "-nP", f"-iTCP:{port}", "-sTCP:ESTABLISHED"],
                             capture_output=True, text=True).stdout
    return f":{port}->{node_host}:" in listing


def wan(args):
    if not backbone_connected(args.host, args.wan_port):
        raise SystemExit(f"the node ({args.host}) holds no connection to a stand-in backbone on port "
                         f"{args.wan_port}: start ../.venv/bin/rnsd --config tests/wan-backbone and flash a "
                         "bench build with RTNODE_BENCH_WAN_HOST")
    stamp = int(time.time()) % 10000
    lan_agents = [Agent(f"{re.sub(r'[0-9]+$', '', m)}{i}x{stamp}", m, args) for i, m in enumerate(args.media.split(","))]
    w = Agent(f"wan0x{stamp}", "wan", args)
    agents = lan_agents + [w]
    print("agents: " + ", ".join(f"{a.label} {a.dest[:8]} (silent {a.silent[:8]})" for a in agents), flush=True)
    results, failures = [], []
    record = recorder(results, failures)
    a = lan_agents[0]
    b = lan_agents[1] if len(lan_agents) > 1 else None

    def wait_s(*pair):
        return 45 if slow(*pair) else 15

    try:
        print("the firewall holds:", flush=True)
        path = w.call("path", 60, cmd="path", dest=a.silent, timeout=15)
        record("wan-path-unmentioned", w, a, path and not path["ok"],
               outcome="no path" if path and not path["ok"] else f"path in {path.get('s') if path else '?'} s")

        marks = {x.name: x.mark() for x in lan_agents}
        w.call("announced", 5, cmd="announce")
        for x in lan_agents:
            heard = x.wait(lambda e: e["event"] == "heard" and e["dest"] == w.dest, wait_s(x), marks[x.name])
            record("wan-announce-held", w, x, not heard,
                   outcome="not heard" if not heard else f"heard, {heard.get('hops')} hops")

        print("the LAN reaches out:", flush=True)
        for x in lan_agents:
            since = w.mark()
            sent = time.time()
            x.call("announced", 5, cmd="announce")
            heard = w.wait(lambda e: e["event"] == "heard" and e["dest"] == x.dest, wait_s(x), since)
            record("lan-announce-out", x, w, heard, s=round(heard["t"] - sent, 2) if heard else None,
                   hops=heard.get("hops") if heard else None)
        path = a.call("path", 60, cmd="path", dest=w.dest, timeout=wait_s(a))
        record("lan-path-wan", a, w, path and path["ok"], s=path.get("s") if path else None,
               hops=path.get("hops") if path else None)
        path = a.call("path", 60, cmd="path", dest=w.silent, timeout=wait_s(a))
        record("lan-path-wan-silent", a, w, path and path["ok"], s=path.get("s") if path else None,
               hops=path.get("hops") if path else None)
        mentioned_at = None
        if b:
            # b's silent destination, not a's: the WAN side asked for a's at
            # the start. The LAN's request crosses to the WAN too (LAN packets
            # go everywhere), so the WAN side asks later: a transport node
            # holds repeat requests for one destination for 45 s (RNS
            # PATH_REQUEST_GATE_TIMEOUT), and the node answers only the LAN
            # requester, so the stand-in's gate stays shut until then (45 s,
            # plus up to one 5 s cull).
            path = a.call("path", 60, cmd="path", dest=b.silent, timeout=wait_s(a, b))
            record("lan-path-lan", a, b, path and path["ok"], s=path.get("s") if path else None)
            mentioned_at = time.time()

        print("links and packets across the firewall:", flush=True)
        link_check(record, "lan-link-wan", a, w, args)
        link_check(record, "wan-link-lan", w, a, args)
        packet_check(record, "lan-packet-wan", a, w, args)
        packet_check(record, "wan-packet-lan", w, a, args)

        if mentioned_at:
            # The gate entry goes at the stand-in's next table cull after
            # 45 s, and it culls every 5 s (RNS tables_cull_interval).
            time.sleep(max(0, mentioned_at + 55 - time.time()))
            path = w.call("path", 60, cmd="path", dest=b.silent, timeout=15)
            record("wan-path-mentioned", w, b, path and path["ok"], s=path.get("s") if path else None,
                   hops=path.get("hops") if path else None)

        since = a.mark()
        w.call("announced", 5, cmd="announce")
        heard = a.wait(lambda e: e["event"] == "heard" and e["dest"] == w.dest, wait_s(a), since)
        record("wan-announce-known", w, a, heard, outcome="heard" if heard else "not heard")

        if b and args.churn:
            # The whitelists hold a bounded number of addresses. Links that
            # cross the firewall, and a LAN destination that stays quiet,
            # must survive the LAN mentioning many new ones - including when
            # the WAN side is the first to speak afterwards, before any LAN
            # packet on the link has named it again.
            print(f"whitelist churn: {args.churn} fresh LAN announces at {args.churn_rate}/s:", flush=True)
            a_link = a.call("link", 90, cmd="link", dest=w.dest, aspects=f"contract.{w.name}", timeout=20)
            w_link = w.call("link", 90, cmd="link", dest=a.dest, aspects=f"contract.{a.name}", timeout=20)
            if not (a_link and a_link["ok"] and w_link and w_link["ok"]):
                record("wan-link-survives-churn", w, a, False, reason="links did not open")
            else:
                w_before = w.call("echo", 60, cmd="echo", dest=a.dest, n=3, size=200, timeout=10)
                churn_s = args.churn / args.churn_rate
                b.call("flood", churn_s + 60, cmd="flood", rate=args.churn_rate, duration=churn_s)
                time.sleep(15)  # the node's table culls run
                w_after = w.call("echo", 60, cmd="echo", dest=a.dest, n=3, size=200, timeout=10)
                record("wan-link-survives-churn", w, a, w_after and w_after["ok"] == w_after["n"],
                       first_to_speak="wan", before=f"{w_before['ok']}/{w_before['n']}" if w_before else None,
                       after=f"{w_after['ok']}/{w_after['n']}" if w_after else None)
                a_after = a.call("echo", 60, cmd="echo", dest=w.dest, n=3, size=200, timeout=10)
                record("wan-link-survives-churn", a, w, a_after and a_after["ok"] == a_after["n"],
                       first_to_speak="lan", after=f"{a_after['ok']}/{a_after['n']}" if a_after else None)
            for x, y in ((a, w), (w, a)):
                x.call("closed", 10, cmd="close", dest=y.dest)
            link = w.call("link", 90, cmd="link", dest=a.dest, aspects=f"contract.{a.name}", timeout=20)
            record("wan-reaches-quiet-lan", w, a, link and link["ok"],
                   link_s=link.get("s") if link else None, reason=None if link and link["ok"] else "no link")
            if link and link["ok"]:
                w.call("closed", 10, cmd="close", dest=a.dest)

        print(f"WAN flood: {args.flood_rate}/s announces for unknown destinations for {args.flood_s} s:", flush=True)
        f = Agent(f"flood0x{stamp}", "wan", args)
        a_link = a.call("link", 90, cmd="link", dest=w.dest, aspects=f"contract.{w.name}", timeout=20)
        b_link = b.call("link", 90, cmd="link", dest=a.dest, aspects=f"contract.{a.name}", timeout=20) if b else None
        marks = {x.name: x.mark() for x in lan_agents}
        started = time.time()
        f.process.stdin.write(json.dumps({"cmd": "flood", "rate": args.flood_rate, "duration": args.flood_s}) + "\n")
        f.process.stdin.flush()
        echoes = {}

        def echo_during(x, dest, key):
            got, sent_n, rtts = 0, 0, []
            while time.time() - started < args.flood_s:
                echo = x.call("echo", 30, cmd="echo", dest=dest, n=1, size=200, timeout=10)
                sent_n += 1
                if echo and echo["ok"]:
                    got += 1
                    rtts.append(echo["rtt_p50_ms"])
                time.sleep(1)
            echoes[key] = {"ok": got, "n": sent_n, "p50_ms": sorted(rtts)[len(rtts) // 2] if rtts else None,
                           "max_ms": max(rtts) if rtts else None}

        threads = []
        if a_link and a_link["ok"]:
            threads.append(threading.Thread(target=echo_during, args=(a, w.dest, "lan->wan")))
        if b_link and b_link["ok"]:
            threads.append(threading.Thread(target=echo_during, args=(b, a.dest, "lan->lan")))
        for thread in threads:
            thread.start()
        flood = f.wait(lambda e: e["event"] == "flood", args.flood_s + 60)
        for thread in threads:
            thread.join()
        time.sleep(10)  # stragglers
        ended = time.time()
        flooded = set(flood["hashes"]) if flood else set()
        leaked = {x.label: sum(1 for e in x.events[marks[x.name]:] if e["event"] == "heard" and e["dest"] in flooded)
                  for x in lan_agents}
        health = node_health(node_log(args.node_log, started, ended))
        links_up = bool(a_link and a_link["ok"]) and (not b or bool(b_link and b_link["ok"]))
        echo_ok = links_up and all(v["n"] and v["ok"] >= 0.9 * v["n"] for v in echoes.values()) \
            and len(echoes) == len(threads)
        record("wan-flood", f, a, flood and not any(leaked.values()) and echo_ok and not health["reboots"]
               and not health["panics"],
               sent=flood["n"] if flood else None, leaked_to_lan=leaked, echoes=echoes,
               links_up=links_up,
               **({"node": health} if args.node_log else {}))
        if a_link and a_link["ok"]:
            a.call("closed", 10, cmd="close", dest=w.dest)
        if b_link and b_link["ok"]:
            b.call("closed", 10, cmd="close", dest=a.dest)
        f.quit()
    finally:
        for agent in agents:
            agent.quit()
    return results, failures


def soak(args):
    """WiFi, Bluetooth and LoRa carrying traffic at once, for --soak-s."""
    stamp = int(time.time()) % 10000
    media = args.media.split(",")
    agents = {m: Agent(f"{re.sub(r'[0-9]+$', '', m)}{i}x{stamp}", m, args) for i, m in enumerate(media)}
    w = Agent(f"wan0x{stamp}", "wan", args) if backbone_connected(args.host, args.wan_port) else None
    everyone = list(agents.values()) + ([w] if w else [])
    print("agents: " + ", ".join(f"{a.label} {a.dest[:8]}" for a in everyone), flush=True)
    results, failures = [], []
    record = recorder(results, failures)
    tcp, prnsd, lora = agents.get("tcp"), agents.get("prnsd"), agents.get("lora")
    # (name, initiator, responder, echo every s, echo bytes, resource every s, resource bytes, loss allowed)
    flows = []
    if tcp and prnsd:
        flows.append(("wifi<->bt", tcp, prnsd, 1, 400, 60, args.resource, 0.01))
    # LoRa at SF10 carries an echo round (request, proof, reply) in about 5 s
    # of airtime; these two keep the channel about a quarter busy.
    if tcp and lora:
        flows.append(("wifi<->lora", tcp, lora, 30, 100, 0, 0, 0.10))
    if prnsd and lora:
        flows.append(("bt<->lora", prnsd, lora, 60, 100, 0, 0, 0.10))
    if w and tcp:
        flows.append(("wan<->wifi", w, tcp, 2, 400, 120, args.resource, 0.01))
    try:
        for a in everyone:
            a.call("announced", 5, cmd="announce")
        time.sleep(20 if lora else 5)  # announces across LoRa
        for a in everyone:
            a.call("announced", 5, cmd="announce")
        time.sleep(20 if lora else 5)
        live = []
        for name, a, b, *_ in flows:
            link = a.call("link", 90, cmd="link", dest=b.dest, aspects=f"contract.{b.name}",
                          timeout=60 if slow(a, b) else 20)
            if not (link and link["ok"]):
                record("soak-link", a, b, False, flow=name, reason=link.get("reason") if link else "no reply")
            else:
                live.append(name)
            if slow(a, b):
                time.sleep(args.lora_settle)
        started = time.time()
        stats = {}

        def run_flow(name, a, b, every, size, resource_every, resource_size, _):
            st = stats[name] = {"sent": 0, "ok": 0, "rtts": [], "resources": 0, "resources_ok": 0,
                                "worst_minute_loss": 0}
            minute, minute_sent, minute_ok = 0, 0, 0
            next_resource = started + resource_every if resource_every else None
            while time.time() - started < args.soak_s:
                tick = time.time()
                echo = a.call("echo", 60, cmd="echo", dest=b.dest, n=1, size=size, timeout=min(40, 2 * every + 5))
                st["sent"] += 1
                minute_sent += 1
                if echo and echo["ok"]:
                    st["ok"] += 1
                    minute_ok += 1
                    st["rtts"].append(echo["rtt_p50_ms"])
                if int((time.time() - started) // 60) != minute:
                    if minute_sent:
                        st["worst_minute_loss"] = max(st["worst_minute_loss"], 1 - minute_ok / minute_sent)
                    minute, minute_sent, minute_ok = int((time.time() - started) // 60), 0, 0
                if next_resource and time.time() >= next_resource:
                    next_resource += resource_every
                    since = b.mark()
                    resource = a.call("resource", 300, cmd="resource", dest=b.dest, size=resource_size, timeout=240)
                    got = b.wait(lambda e: e["event"] == "resource_in" and e["dest"] == b.dest, 10, since)
                    st["resources"] += 1
                    if resource and resource["ok"] and got and got.get("sha") == resource["sha"]:
                        st["resources_ok"] += 1
                time.sleep(max(0, every - (time.time() - tick)))

        threads = [threading.Thread(target=run_flow, args=flow) for flow in flows if flow[0] in live]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        ended = time.time()
        health = node_health(node_log(args.node_log, started, ended))
        for name, a, b, every, size, resource_every, resource_size, allowed in flows:
            if name not in stats:
                continue
            st = stats[name]
            loss = 1 - st["ok"] / st["sent"] if st["sent"] else 1
            rtts = sorted(st["rtts"])
            record("soak", a, b, loss <= allowed and st["resources_ok"] == st["resources"],
                   flow=name, echoes=f"{st['ok']}/{st['sent']}", loss=round(loss, 3),
                   worst_minute_loss=round(st["worst_minute_loss"], 2),
                   rtt_p50_ms=rtts[len(rtts) // 2] if rtts else None,
                   rtt_p95_ms=rtts[int(len(rtts) * 0.95)] if rtts else None, rtt_max_ms=rtts[-1] if rtts else None,
                   resources=f"{st['resources_ok']}/{st['resources']}")
        if args.node_log:
            ok = not health["reboots"] and not health["panics"]
            row = {"check": "soak-node", "from": "node", "to": "node", "ok": ok, **health}
            results.append(row)
            print(f"  {'PASS' if ok else 'FAIL'} soak-node " + " ".join(f"{k}={v}" for k, v in health.items()),
                  flush=True)
            if not ok:
                failures.append(row)
        for name, a, b, *_ in flows:
            if name in live:
                a.call("closed", 10, cmd="close", dest=b.dest)
    finally:
        for agent in everyone:
            agent.quit()
    return results, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("suite", choices=["lan", "wan", "soak"])
    parser.add_argument("--host", default="192.168.2.125")
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--prnsd-port", type=int, default=4270)
    parser.add_argument("--rnode-port", default="/dev/cu.usbserial-0001")
    parser.add_argument("--lora-txpower", type=int, default=2)
    parser.add_argument("--wan-host", default="127.0.0.1")
    parser.add_argument("--wan-port", type=int, default=4290)
    parser.add_argument("--media", help="LAN agents (lan default tcp,prnsd,lora; wan default tcp,tcp2)")
    parser.add_argument("--node-log", help="the node's serial log (host-timestamped lines), for wan-flood health")
    parser.add_argument("--flood-rate", type=float, default=20, help="wan-flood announces per second")
    parser.add_argument("--flood-s", type=float, default=60, help="wan-flood duration")
    parser.add_argument("--soak-s", type=float, default=900, help="soak duration")
    parser.add_argument("--churn", type=int, default=260, help="fresh LAN announces for the whitelist churn check (0: skip)")
    parser.add_argument("--churn-rate", type=float, default=10)
    parser.add_argument("--prnsd-gateway", action="store_true",
                        help="prnsd runs with mode = gateway, so path discovery through it can be checked")
    parser.add_argument("--resource", type=int, default=20000, help="resource bytes between fast media")
    parser.add_argument("--lora-resource", type=int, default=1500, help="resource bytes when LoRa is involved")
    parser.add_argument("--lora-settle", type=float, default=5, help="seconds of quiet after each LoRa step")
    parser.add_argument("--out")
    args = parser.parse_args()

    if not args.media:
        args.media = "tcp,tcp2" if args.suite == "wan" else "tcp,prnsd,lora"
    out = args.out or os.path.join(HERE, "bench-results", time.strftime("%Y%m%d-%H%M%S") + f"-contracts-{args.suite}")
    os.makedirs(out, exist_ok=True)
    started = time.time()
    results, failures = {"lan": lan, "wan": wan, "soak": soak}[args.suite](args)
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump({"suite": args.suite, "media": args.media, "started": started, "results": results}, f, indent=2)
    print(f"\n{len(results) - len(failures)}/{len(results)} passed in {time.time() - started:.0f} s; results: {out}",
          flush=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
