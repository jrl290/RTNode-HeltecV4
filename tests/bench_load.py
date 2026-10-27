#!/usr/bin/env python3
"""Bench load harness for RTNode (PERFORMANCE_STRATEGY.md, "The load harness").

Resets the node, waits for the bench build's boot marks, lets it settle, then
runs one scenario while capturing the serial log, a 5-per-second ping, and —
for lan-flood — an announce stream into the local TCP server. Everything goes
under --out, followed by a summary that lines each run of lost pings up
against the node's [STALL], LoRa TX and [FLASH] marks.

    .venv/bin/python tests/bench_load.py --serial /dev/cu.usbmodem101 \\
        --host 192.168.2.125 --scenario idle --duration 600

    .venv/bin/python tests/bench_load.py --serial /dev/cu.usbmodem101 \\
        --host 192.168.2.125 --scenario lan-flood --rate 1 --duration 600

lan-flood refuses to start unless the node prints the bench build's
"backbones off" mark, so the flood never reaches a public backbone.
"""
import argparse
import ipaddress
import json
import os
import re
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
WORKSPACE_PYTHON = os.path.abspath(os.path.join(HERE, "..", "..", ".venv", "bin", "python"))
PING_INTERVAL = 0.2


class SerialCapture(threading.Thread):
    """Timestamps every serial line; reopens the port across resets."""

    def __init__(self, port, path):
        super().__init__(daemon=True)
        self.port, self.path = port, path
        self.lines = []  # (host_time, text)
        self.stop = threading.Event()
        self.lock = threading.Lock()

    def wait_for_port(self, timeout=10):
        # After a flash or watchdog reset the USB-Serial/JTAG port re-enumerates;
        # it is briefly absent from /dev.
        deadline = time.time() + timeout
        while not os.path.exists(self.port):
            if time.time() > deadline:
                sys.exit(f"{self.port} did not appear within {timeout} s")
            time.sleep(0.1)

    def reset_node(self):
        self.wait_for_port()
        with serial.Serial(self.port, 115200, timeout=0.2) as s:
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False

    def watchdog_reset_node(self):
        # On the V4's USB-Serial/JTAG port an RTS reset can leave the chip in
        # the ROM bootloader (silent, no WiFi), notably with a battery keeping
        # it powered. esptool's watchdog reset always starts the app.
        esptool = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
        python = os.path.expanduser("~/.platformio/penv/bin/python")
        subprocess.run([python, esptool, "--chip", "esp32s3", "-p", self.port,
                        "--before", "default_reset", "--after", "watchdog_reset", "chip_id"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)

    def run(self):
        buffer, link = b"", None
        with open(self.path, "w") as out:
            while not self.stop.is_set():
                if link is None:
                    try:
                        link = serial.Serial(self.port, 115200, timeout=0.2)
                    except (serial.SerialException, OSError):
                        time.sleep(0.05)
                        continue
                try:
                    chunk = link.read(4096)
                except (serial.SerialException, OSError):
                    link = None
                    continue
                buffer += chunk
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    now, text = time.time(), raw.decode("utf-8", "replace").rstrip()
                    out.write(f"{now:.3f} {text}\n")
                    out.flush()
                    with self.lock:
                        self.lines.append((now, text))
        if link:
            link.close()

    def wait_for(self, needle, since, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if any(t >= since and needle in text for t, text in self.lines):
                    return True
            time.sleep(0.1)
        return False


class LineCapture(threading.Thread):
    """Timestamps every stdout line of a subprocess."""

    def __init__(self, process, path):
        super().__init__(daemon=True)
        self.process, self.path, self.lines = process, path, []

    def run(self):
        with open(self.path, "w") as out:
            for raw in self.process.stdout:
                now, text = time.time(), raw.rstrip()
                out.write(f"{now:.3f} {text}\n")
                out.flush()
                self.lines.append((now, text))


def tcp_ready(host, port, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2):
                return True
        except OSError:
            time.sleep(1)
    return False


def start_bench_node(serial_port, host, port, out, isolated):
    """Resets the node and captures its serial log into out/serial.log.
    With isolated, insists on the bench build's "backbones off" mark, so no
    test traffic reaches a public backbone, and on the local TCP server
    answering."""
    capture = SerialCapture(serial_port, os.path.join(out, "serial.log"))
    reset_at = time.time()
    capture.reset_node()
    capture.start()
    if not capture.wait_for("[RES] boot", reset_at, 30):
        print("no boot mark after an RTS reset; trying esptool's watchdog reset", flush=True)
        capture.stop.set()
        capture.join(timeout=5)
        capture = SerialCapture(serial_port, os.path.join(out, "serial.log"))
        reset_at = time.time()
        capture.watchdog_reset_node()
        capture.start()
        if not capture.wait_for("[RES] boot", reset_at, 60):
            sys.exit("node did not print its [RES] boot mark — is the bench build flashed?")
    if isolated:
        if not capture.wait_for("[BENCH] overrides: backbones off", reset_at, 5):
            sys.exit("no 'backbones off' mark: refusing to test a node that may be connected to a public backbone")
        # A WAN contract build (RTNODE_BENCH_WAN_HOST) turns one backbone back
        # on, to a stand-in on the bench; it must stay on the bench network.
        stand_in = "[BENCH] overrides: backbone 1 -> "
        if capture.wait_for(stand_in, reset_at, 1):
            with capture.lock:
                line = next(text for t, text in capture.lines if t >= reset_at and stand_in in text)
            host = line.split(stand_in, 1)[1].rsplit(":", 1)[0]
            try:
                private = ipaddress.ip_address(host).is_private
            except ValueError:
                private = False
            if not private:
                sys.exit(f"backbone 1 goes to {host}: refusing to test through a backbone off the bench network")
        if not tcp_ready(host, port, 60):
            sys.exit(f"local TCP server {host}:{port} not reachable")
    return capture


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def summarise(args, serial_lines, ping_lines, flood_lines, window_start, window_end, ping_start):
    summary = {"scenario": args.scenario, "rate": args.rate, "duration_s": args.duration,
               "window_start": round(window_start, 3), "window_end": round(window_end, 3)}

    # Ping: every sequence number gets a reply or a timeout line on macOS, so
    # the highest one seen says how many were sent (ping runs a little slower
    # than its nominal interval, so elapsed time over-counts).
    replies, rtts, highest = set(), [], -1
    for _, text in ping_lines:
        match = re.search(r"icmp_seq=(\d+) .*time=([\d.]+) ms", text)
        if match:
            replies.add(int(match.group(1)))
            rtts.append(float(match.group(2)))
            highest = max(highest, int(match.group(1)))
        match = re.search(r"Request timeout for icmp_seq (\d+)", text)
        if match:
            highest = max(highest, int(match.group(1)))
    sent = highest + 1
    lost = [seq for seq in range(sent) if seq not in replies]
    summary["ping"] = {
        "sent": sent,
        "lost": len(lost),
        "loss_pct": round(100.0 * len(lost) / sent, 2) if sent > 0 else None,
        "rtt_p50_ms": percentile(rtts, 0.50),
        "rtt_p95_ms": percentile(rtts, 0.95),
        "rtt_max_ms": max(rtts) if rtts else None,
    }
    bursts = []
    for seq in lost:
        if bursts and seq == bursts[-1][1] + 1:
            bursts[-1][1] = seq
        else:
            bursts.append([seq, seq])

    # Node marks inside the measurement window.
    stalls, tx_windows, flashes, res_lines, boots, drops, panics = [], [], [], [], 0, 0, 0
    tx_start = None
    for t, text in serial_lines:
        if t < window_start - 2 or t > window_end + 2:
            continue
        match = re.search(r"\[STALL\] t=\d+ (\d+)ms lora_tx=(\d+)ms/(\d+) flash=(\d+)ms/(\d+) tcp=(\d+)ms/(\d+)(?: other=(\d+)ms)?", text)
        if match:
            duration = int(match.group(1)) / 1000.0
            stalls.append({"start": t - duration, "end": t, "ms": int(match.group(1)),
                           "lora_tx_ms": int(match.group(2)), "lora_tx_packets": int(match.group(3)),
                           "flash_ms": int(match.group(4)), "flash_ops": int(match.group(5)),
                           "tcp_ms": int(match.group(6)), "tcp_ops": int(match.group(7)),
                           "other_ms": int(match.group(8) or 0)})
        elif "[RTNode] TXCFG" in text:
            tx_start = t
        elif "[RTNode] TXDONE" in text and tx_start is not None:
            tx_windows.append((tx_start, t))
            tx_start = None
        elif text.startswith("[FLASH]"):
            flashes.append((t, text))
        elif text.startswith("[RES] boot") and t > window_start:
            boots += 1
        elif text.startswith("[RES]"):
            res_lines.append(text)
        if "TX DROP" in text:
            drops += 1
        if "Guru Meditation" in text or "task_wdt" in text or "abort()" in text:
            panics += 1

    summary["node"] = {
        "stalls": len(stalls),
        "stall_max_ms": max((s["ms"] for s in stalls), default=0),
        "stall_total_ms": sum(s["ms"] for s in stalls),
        "lora_tx_packets": len(tx_windows),
        "lora_tx_airtime_s": round(sum(end - start for start, end in tx_windows), 1),
        "slow_flash_ops": len(flashes),
        "lora_queue_drops": drops,
        "reboots_during_run": boots,
        "panic_lines": panics,
        "res_lines": res_lines,
    }

    offline = [t for t, text in flood_lines if text.endswith("iface offline") and window_start <= t <= window_end]
    announced = sum(1 for _, text in flood_lines if re.match(r"\d+\.\d+ announce \d+ ", text))
    summary["flood"] = {"announces": announced, "tcp_offline_events": len(offline)}

    # Which node marks overlap each run of lost pings.
    correlated = []
    for first, last in bursts:
        start = ping_start + first * PING_INTERVAL
        end = ping_start + (last + 1) * PING_INTERVAL + 1.0  # a reply may arrive up to ~1 s late
        overlap = lambda a, b: a < end and b > start
        correlated.append({
            "at": round(start - window_start, 1),
            "lost": last - first + 1,
            "stalls": [s for s in stalls if overlap(s["start"], s["end"])],
            "lora_tx": sum(1 for a, b in tx_windows if overlap(a, b)),
            "slow_flash": sum(1 for t, _ in flashes if start - 0.5 <= t <= end + 0.5),
        })
    summary["loss_bursts"] = correlated
    return summary


def print_summary(summary):
    ping, node, flood = summary["ping"], summary["node"], summary["flood"]
    print(f"\n== {summary['scenario']} rate={summary['rate']} duration={summary['duration_s']}s")
    print(f"ping: {ping['lost']}/{ping['sent']} lost ({ping['loss_pct']}%), "
          f"rtt p50 {ping['rtt_p50_ms']} p95 {ping['rtt_p95_ms']} max {ping['rtt_max_ms']} ms")
    print(f"node: {node['stalls']} stalls (max {node['stall_max_ms']} ms, total {node['stall_total_ms']} ms), "
          f"{node['lora_tx_packets']} LoRa TX ({node['lora_tx_airtime_s']} s airtime), "
          f"{node['slow_flash_ops']} slow flash ops, {node['lora_queue_drops']} queue drops, "
          f"{node['reboots_during_run']} reboots, {node['panic_lines']} panic lines")
    print(f"flood: {flood['announces']} announces, {flood['tcp_offline_events']} TCP offline events")
    for line in node["res_lines"]:
        print("  " + line)
    bursts = summary["loss_bursts"]
    if bursts:
        print(f"loss bursts ({len(bursts)}):")
        for burst in bursts[:40]:
            causes = ", ".join(
                f"stall {s['ms']}ms (tx {s['lora_tx_ms']}/flash {s['flash_ms']}/tcp {s['tcp_ms']}/other {s['other_ms']})"
                for s in burst["stalls"]) or "no stall"
            print(f"  +{burst['at']}s lost {burst['lost']}: {causes}; "
                  f"lora_tx {burst['lora_tx']}; slow_flash {burst['slow_flash']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=4242)
    parser.add_argument("--scenario", choices=["idle", "lan-flood"], required=True)
    parser.add_argument("--rate", type=float, default=1.0, help="announces per second (lan-flood)")
    parser.add_argument("--duration", type=float, default=600.0, help="measurement window, seconds")
    parser.add_argument("--settle", type=float, default=30.0, help="seconds after boot before measuring")
    parser.add_argument("--out", help="results directory (default tests/bench-results/<time>-<scenario>)")
    args = parser.parse_args()

    out = args.out or os.path.join(HERE, "bench-results", time.strftime("%Y%m%d-%H%M%S") + f"-{args.scenario}")
    os.makedirs(out, exist_ok=True)

    capture = start_bench_node(args.serial, args.host, args.port, out, isolated=args.scenario == "lan-flood")
    time.sleep(args.settle)

    ping = subprocess.Popen(["ping", "-i", str(PING_INTERVAL), args.host],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    ping_start = time.time()
    ping_capture = LineCapture(ping, os.path.join(out, "ping.log"))
    ping_capture.start()

    flood_capture = None
    if args.scenario == "lan-flood":
        flood = subprocess.Popen(
            [WORKSPACE_PYTHON, os.path.join(HERE, "bench_flood.py"), args.host, str(args.port),
             str(args.rate), str(args.duration), os.path.join(out, "flood-rns")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        flood_capture = LineCapture(flood, os.path.join(out, "flood.log"))
        flood_capture.start()

    window_start = ping_start
    time.sleep(args.duration)
    window_end = time.time()

    ping.send_signal(signal.SIGINT)
    ping.wait(timeout=10)
    ping_capture.join(timeout=10)  # ping block-buffers into the pipe; read to EOF first
    if flood_capture:
        flood_capture.process.wait(timeout=30)
        flood_capture.join(timeout=10)
    time.sleep(1)
    capture.stop.set()
    capture.join(timeout=5)

    summary = summarise(args, capture.lines, ping_capture.lines,
                        flood_capture.lines if flood_capture else [], window_start, window_end, ping_start)
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print_summary(summary)
    print(f"\nresults: {out}")


if __name__ == "__main__":
    main()
