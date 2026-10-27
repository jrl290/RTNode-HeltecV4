"""
RTNode LoRa integration test suite.

FIREWALL_MODE architecture note
---------------------------------
The RTNode runs in FIREWALL_MODE, which means serial_write() is a
compile-time no-op (Utilities.h:#ifdef FIREWALL_MODE … return;).
No KISS frame responses will ever arrive from the RTNode.  All
RTNode self-tests therefore parse the device's ASCII RNS debug log
output via KissSerial.wait_for_log_line() and .log_lines.

The RNode probe IS a standard KISS TNC and responds normally.

Test categories
----------------
  CATEGORY 1 — RTNode alive tests (RTNode serial log, no RNode needed)
    Verifies the device is running, RNS is active, and no error-level
    log entries appear during a quiet observation window.

  CATEGORY 2 — LoRa receive path  (RNode → RTNode)
    RNode sends a KISS DATA packet.  We verify the RTNode's RNS stack
    logs new activity, indicating the LoRa packet was received and
    handed to the Reticulum transport layer.

  CATEGORY 3 — LoRa transmit path  (RTNode → RNode)
    RTNode periodically transmits RNS announces and transport packets
    over LoRa.  The RNode (in promiscuous mode) watches for any
    inbound LoRa packet.

Quick start
-----------
  cd tests
  # Self-tests only (no RNode):
  pytest lora_test.py --rtnode-port /dev/cu.usbmodem114401 -v

  # Full suite:
  pytest lora_test.py \\
      --rtnode-port /dev/cu.usbmodem114401 \\
      --rnode-port  /dev/cu.usbmodem11201  \\
      --lora-freq   869525000              \\
      --lora-bw     250000                 \\
      --lora-sf     8                      \\
      --lora-cr     5                      \\
      -v
"""

import threading
import time

import pytest

from kiss_serial import (
    CMD_DATA,
    CMD_ERROR,
    KissFrame,
    KissSerial,
    RadioConfig,
)


# ── helpers ───────────────────────────────────────────────────────────────────

def _requires_rnode(rnode: "KissSerial | None") -> None:
    if rnode is None:
        pytest.skip("Requires --rnode-port (no RNode probe attached)")


# ─────────────────────────────────────────────────────────────────────────────
# CATEGORY 1 — RTNode alive tests
# ─────────────────────────────────────────────────────────────────────────────

class TestRTNodeAlive:
    """
    RTNode self-tests that require only the RTNode serial port.

    In FIREWALL_MODE the serial port carries ASCII RNS log lines only.
    These tests parse that output rather than KISS frames.
    """

    def test_alive(self, rtnode: KissSerial):
        """
        RTNode produces ASCII log output — confirms the firmware is running
        and the USB CDC serial connection is healthy.

        We send a CMD_DETECT query (serial_callback processes the bytes even
        though the KISS response is suppressed) to trigger RNS activity, then
        wait up to 10 s for any log line to appear.
        """
        rtnode.query_detect()
        line = rtnode.wait_for_log_line("", timeout=10.0)
        assert line is not None, (
            "No ASCII log output from RTNode within 10 s after CMD_DETECT.\n"
            "  Check:\n"
            "    1. --rtnode-port points to the Heltec V4 running FIREWALL_MODE\n"
            "    2. The device is powered and the USB cable is data-capable\n"
            "    3. The firmware was compiled with ARDUINO_USB_CDC_ON_BOOT=1"
        )
        print(f"\n  First log line: {line[:100]!r}")

    def test_rns_active(self, rtnode: KissSerial):
        """
        RNS stack is running and producing verbose log output.

        Looks for a [VRB] VERBOSE or [TRC] TRACE log entry — the format
        that the RNS heap-telemetry logger and packet handlers emit.
        """
        # Trigger activity so the RNS loop produces logs
        rtnode.query_detect()
        rtnode.query_radio_state()

        line = rtnode.wait_for_log_line("[VRB]", timeout=10.0)
        if line is None:
            line = rtnode.wait_for_log_line("[TRC]", timeout=5.0)
        if line is None:
            line = rtnode.wait_for_log_line("RNS", timeout=5.0)

        assert line is not None, (
            "No RNS verbose log output detected within 20 s.\n"
            "  Expected lines containing '[VRB]', '[TRC]', or 'RNS'.\n"
            "  Ensure the firmware was compiled with HAS_RNS and the RNS "
            "log level is set to LOG_VERBOSE or higher."
        )
        print(f"\n  RNS log sample: {line[:100]!r}")

    def test_no_error_logs(self, rtnode: KissSerial):
        """
        No ERROR or CRITICAL log lines during a 5 s observation window.

        [ERR] lines indicate hardware faults such as SX126x SPI errors,
        EEPROM lock failures, or RNS stack exceptions.  [CRT] lines are
        fatal failures.
        """
        error_lines: list[str] = []

        def _capture(line: str) -> None:
            if "[ERR]" in line or "[CRT]" in line:
                error_lines.append(line)

        rtnode.on_log_line(_capture)
        time.sleep(5.0)
        try:
            rtnode._log_callbacks.remove(_capture)
        except ValueError:
            pass

        if error_lines:
            formatted = "\n".join(f"    {l}" for l in error_lines[:10])
            pytest.fail(
                f"Error-level log lines detected from RTNode:\n{formatted}\n"
                "  [ERR] often indicates ERROR_INITRADIO (SX126x SPI problem), "
                "TX failure, or EEPROM lock.  Check hardware connections."
            )
        print(f"\n  No [ERR] or [CRT] lines in 5 s observation window")

    def test_channel_config(
        self,
        rtnode: KissSerial,
        channel_config: RadioConfig,
    ):
        """
        Verify that the RTNode's active channel matches the test parameters.

        The updated firmware emits ``[RTNode] LoRa: freq=... bw=... sf=...
        cr=... txp=...`` immediately after loading its EEPROM config.  If
        this line is present in the RTNode's recent log (i.e. the device
        booted while the test session was running), we parse it and compare
        to the CLI channel parameters.

        If the line is absent (device booted before the test session), the
        test is skipped with a hint to reflash or restart the RTNode so we
        can capture the startup log.
        """
        # Look for the startup channel log in recently received lines
        lora_line: str | None = None
        for line, _ts in rtnode.log_lines:
            if "[RTNode] LoRa:" in line:
                lora_line = line
                break

        if lora_line is None:
            # Also wait a short time in case we just connected mid-boot
            lora_line = rtnode.wait_for_log_line("[RTNode] LoRa:", timeout=3.0)

        if lora_line is None:
            pytest.skip(
                "RTNode did not emit '[RTNode] LoRa:' during this session.\n"
                "  The device booted before the test run started.\n"
                "  To verify channel: reset the RTNode, then re-run the tests."
            )

        print(f"\n  RTNode startup channel line: {lora_line.strip()!r}")

        # Parse: [RTNode] LoRa: freq=869525000 bw=250000 sf=8 cr=5 txp=14
        import re
        m = re.search(
            r"freq=(\d+)\s+bw=(\d+)\s+sf=(\d+)\s+cr=(\d+)\s+txp=(\d+)",
            lora_line,
        )
        assert m is not None, (
            f"Could not parse channel values from: {lora_line!r}\n"
            "  Expected format: freq=N bw=N sf=N cr=N txp=N"
        )

        rtnode_freq = int(m.group(1))
        rtnode_bw   = int(m.group(2))
        rtnode_sf   = int(m.group(3))
        rtnode_cr   = int(m.group(4))

        assert rtnode_freq == channel_config.frequency, (
            f"RTNode freq {rtnode_freq} Hz ≠ test --lora-freq {channel_config.frequency} Hz\n"
            "  Update --lora-freq to match the RTNode's EEPROM config, or\n"
            "  change the channel via the RTNode web portal."
        )
        assert rtnode_bw == channel_config.bandwidth, (
            f"RTNode bw {rtnode_bw} Hz ≠ test --lora-bw {channel_config.bandwidth} Hz"
        )
        assert rtnode_sf == channel_config.sf, (
            f"RTNode sf {rtnode_sf} ≠ test --lora-sf {channel_config.sf}"
        )
        assert rtnode_cr == channel_config.cr, (
            f"RTNode cr {rtnode_cr} ≠ test --lora-cr {channel_config.cr}"
        )
        print(
            f"  Channel matches: freq={rtnode_freq} Hz, bw={rtnode_bw} Hz, "
            f"sf={rtnode_sf}, cr={rtnode_cr}"
        )


# ─────────────────────────────────────────────────────────────────────────────
# CATEGORY 2 — LoRa receive path  (RNode TX → RTNode RX)
# ─────────────────────────────────────────────────────────────────────────────

class TestLoRaReceive:
    """
    Requires an attached RNode probe (--rnode-port).

    The RNode transmits a test packet on the configured channel.  The
    firmware now emits ``[VRB] [LoRa] RX N bytes`` at LOG_VERBOSE level
    whenever a LoRa frame arrives at the physical layer.  We wait for
    exactly that log pattern — any match is a definitive confirmation
    that the RTNode received the packet over the air.

    Previous tests matched *any* log line which was a false positive
    because the periodic ``[HEAP-TEL] firewall:`` telemetry fires every
    ~2 s regardless of LoRa activity.
    """

    def test_rnode_tx_rtnode_receives(
        self,
        rtnode: KissSerial,
        rnode: "KissSerial | None",
        channel_config: RadioConfig,
        tx_payload: bytes,
        rx_timeout: float,
    ):
        """
        Basic LoRa receive path.

        1. RNode transmits *tx_payload* via CMD_DATA on the configured channel.
        2. We wait up to *rx_timeout* seconds for the RTNode to emit
           ``[LoRa] RX`` in its log — the VERBOSE-level line added to
           ``LoRaInterface::handle_incoming`` in the firmware.
        3. A match is unambiguous proof the LoRa frame was received.

        Failure means the RTNode did not receive the LoRa packet.
        Common causes:
          - Wrong channel parameters (--lora-freq/bw/sf/cr)
          - Missing antenna on either device
          - RTNode radio failed to initialise (look for [ERR] in test_no_error_logs)
          - Devices out of range
          - Old firmware build (rebuild + reflash to get [LoRa] RX log)
        """
        _requires_rnode(rnode)

        print(f"\n  Channel: {channel_config}")
        print(f"  Sending {tx_payload!r} from RNode → RTNode …")

        rnode.send_packet(tx_payload)
        line = rtnode.wait_for_log_line("[LoRa] RX", timeout=rx_timeout)

        assert line is not None, (
            f"RTNode did not log '[LoRa] RX' within {rx_timeout} s after RNode TX.\n"
            f"  Channel: {channel_config}\n"
            "  Check:\n"
            "    1. --lora-freq/bw/sf/cr match the RTNode's EEPROM config\n"
            "       (see test_channel_config or '[RTNode] LoRa:' in startup log)\n"
            "    2. Antennas are attached to both devices\n"
            "    3. Devices are within LoRa range\n"
            "    4. Firmware has the [LoRa] RX log (rebuild + reflash if not)"
        )

        print(f"\n  RTNode logged: {line[:100]!r}")

    def test_rnode_tx_rtnode_receives_multiple(
        self,
        rtnode: KissSerial,
        rnode: "KissSerial | None",
        channel_config: RadioConfig,
        tx_payload: bytes,
        rx_timeout: float,
    ):
        """
        Reliability check: send 3 packets and verify the RTNode logs
        ``[LoRa] RX`` for at least 2 of them.

        A consistent receive rate of 0/3 indicates a channel mismatch or
        hardware problem.  Occasional misses (1/3) can indicate interference
        or range issues.
        """
        _requires_rnode(rnode)

        print(f"\n  Channel: {channel_config}")
        received = 0

        for i in range(3):
            rnode.send_packet(tx_payload + f" #{i+1}".encode())
            line = rtnode.wait_for_log_line("[LoRa] RX", timeout=rx_timeout)

            if line is not None:
                received += 1
                print(f"  Packet {i+1}/3: RTNode logged '[LoRa] RX' ✓")
            else:
                print(f"  Packet {i+1}/3: no '[LoRa] RX' in RTNode log ✗")

            time.sleep(0.5)

        assert received >= 2, (
            f"RTNode only logged '[LoRa] RX' for {received}/3 transmitted packets.\n"
            f"  Channel: {channel_config}\n"
            "  Expected ≥2/3.  Persistent failure suggests wrong channel config."
        )
        print(f"\n  Receive rate: {received}/3")


# ─────────────────────────────────────────────────────────────────────────────
# CATEGORY 3 — LoRa transmit path  (RTNode TX → RNode RX)
# ─────────────────────────────────────────────────────────────────────────────

class TestLoRaTransmit:
    """
    Requires an attached RNode probe (--rnode-port).

    The RTNode's firmware emits ``[VRB] [LoRa] TX N bytes`` at
    LOG_VERBOSE level every time it queues a frame for LoRa transmission.
    We trigger transmission by resetting the RTNode via its serial port's
    DTR/RTS lines — the resulting reboot causes the RNS transport layer to
    send its startup ``probe_destination.announce()`` packet over LoRa.

    The RNode in promiscuous mode simultaneously listens for any received
    LoRa frame to confirm the transmission actually reached the air.
    """

    @staticmethod
    def _reset_via_dtr(ser: "serial.Serial") -> bool:
        """
        Attempt to reset the ESP32-S3 via DTR/RTS serial control lines.

        Standard Arduino auto-reset sequence (works on most USB-to-UART
        adapters and on the ESP32-S3 native USB CDC boot bridge):
          DTR=False, RTS=True  → holds RESET low
          DTR=False, RTS=False → releases RESET → device boots

        Returns True if the DTR attribute exists, False otherwise.
        """
        try:
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setDTR(False)
            ser.setRTS(False)
            time.sleep(0.1)
            return True
        except Exception:
            return False

    def test_rnode_receives_rtnode_packet(
        self,
        rtnode: KissSerial,
        rnode: "KissSerial | None",
        channel_config: RadioConfig,
        announce_timeout: float,
    ):
        """
        Verify the RTNode's LoRa transmitter is working end-to-end.

        Strategy
        --------
        1. Reset the RTNode via DTR/RTS so its RNS stack restarts and
           sends the startup ``probe_destination.announce()`` over LoRa.
        2. Watch the RTNode's serial log for ``[LoRa] TXSTART`` — the
           firmware log that fires inside ``transmit()`` immediately before
           the SX126x packet is written, confirming actual RF transmission.
           (``[LoRa] TX`` fires earlier at packet-queue time and is shown
           for information only; TXSTART proves the RF actually started.)
        3. Simultaneously watch the RNode (promiscuous) for any LoRa frame.
        4. Both must succeed: RTNode logs TXSTART *and* RNode receives a packet.

        If the DTR reset has no effect (native USB CDC on some hardware),
        the test falls back to waiting the full ``announce_timeout`` for a
        spontaneous transmission (e.g., a deferred announce from the RNS
        jobs loop).

        Failure indicates:
          - RTNode LoRa TX hardware issue (check ``[ERR]`` in alive tests)
          - Channel mismatch — RTNode EEPROM freq ≠ RNode config
            (run ``test_channel_config`` to diagnose)
          - CSMA blocking TX: check for ``[LoRa] TX BLOCKED`` messages
          - announce_timeout too short; try ``--announce-timeout 300``
          - Old firmware — rebuild + reflash to get ``[LoRa] TXSTART`` log
        """
        _requires_rnode(rnode)

        print(f"\n  Channel: {channel_config}")

        # ── Step 1: register all listeners BEFORE triggering any reset ────
        # [LoRa] TXSTART fires when transmit() is called (actual RF start).
        # Register both the RTNode TX watcher and the RNode packet listener
        # now, before anything is triggered.
        rnode_packet: list = []
        rnode_evt = threading.Event()

        def _bg_rnode_wait() -> None:
            result = rnode.wait_for_packet(timeout=announce_timeout + 30.0)
            if result:
                rnode_packet.append(result)
            rnode_evt.set()

        txstart_lines: list[str] = []
        txstart_evt = threading.Event()
        tx_queue_lines: list[str] = []   # [LoRa] TX (queue event, informational)
        tx_blocked_lines: list[str] = [] # [LoRa] TX BLOCKED (CSMA diagnostics)

        def _tx_cb(line: str) -> None:
            if "[LoRa] TXSTART" in line and not txstart_lines:
                txstart_lines.append(line)
                txstart_evt.set()
            elif "[LoRa] TX BLOCKED" in line:
                tx_blocked_lines.append(line)
            elif "[LoRa] TX " in line and not txstart_lines and not tx_queue_lines:
                tx_queue_lines.append(line)

        bg = threading.Thread(target=_bg_rnode_wait, daemon=True)
        bg.start()
        rtnode.on_log_line(_tx_cb)
        time.sleep(0.1)  # let callbacks register before we trigger TX

        try:
            # ── Step 2: attempt DTR reset to trigger startup announce ─────
            reset_attempted = self._reset_via_dtr(rtnode._ser)
            if reset_attempted:
                print("  DTR/RTS reset sent — waiting for RTNode to reboot …")
            else:
                print("  DTR reset not available — waiting for spontaneous TX …")

            # ── Step 3: wait for [LoRa] TXSTART in RTNode log ─────────────
            # TXSTART fires inside transmit() — actual SX126x write starts.
            print(f"  Waiting for '[LoRa] TXSTART' in RTNode log (up to {announce_timeout} s) …")
            txstart_evt.wait(timeout=announce_timeout)
            txstart_log = txstart_lines[0] if txstart_lines else None

            if tx_queue_lines:
                print(f"  TX queued : {tx_queue_lines[0][:100]!r}")
            if tx_blocked_lines:
                print(f"  TX BLOCKED ({len(tx_blocked_lines)} times): {tx_blocked_lines[0][:120]!r}")

            assert txstart_log is not None, (
                f"RTNode did not log '[LoRa] TXSTART' within {announce_timeout} s.\n"
                f"  Channel: {channel_config}\n"
                + (
                    f"  TX was queued ({tx_queue_lines[0].strip()!r}) but CSMA blocked actual RF TX.\n"
                    f"  CSMA blocks: {len(tx_blocked_lines)} × '[LoRa] TX BLOCKED'\n"
                    + (f"  Last BLOCKED: {tx_blocked_lines[-1].strip()!r}\n" if tx_blocked_lines else "")
                    if tx_queue_lines else
                    "  Possible causes:\n"
                    "    1. DTR reset had no effect and no spontaneous TX occurred\n"
                    "    2. Old firmware — rebuild + reflash to get '[LoRa] TXSTART' log\n"
                    "    3. TX hardware fault (see test_no_error_logs)\n"
                    "    4. Try --announce-timeout 300 if announce interval is long\n"
                )
            )
            print(f"  TXSTART    : {txstart_log[:100]!r}")

            # ── Step 4: verify the RNode received the frame ────────────────
            # TXSTART fired — the SX126x is writing the packet.  Give the
            # air-time (SF10 BW125 ≈ 1.3 s for 167 bytes) plus margin.
            rnode_evt.wait(timeout=15.0)
            result = rnode_packet[0] if rnode_packet else None

        finally:
            try:
                rtnode._log_callbacks.remove(_tx_cb)
            except ValueError:
                pass

        assert result is not None, (
            "RTNode logged '[LoRa] TXSTART' but RNode received nothing within 15 s.\n"
            "  The SX126x started transmitting but the RNode did not decode the frame.\n"
            "  Check:\n"
            "    1. Antennas attached to both devices\n"
            "    2. Devices within LoRa range\n"
            "    3. Channel config matches (--lora-freq/bw/sf/cr)"
        )

        data, rssi, _ts = result
        rssi_str = f"{rssi} dBm" if rssi is not None else "unknown"
        print(f"\n  RNode received {len(data)} bytes, RSSI={rssi_str}")
        print(f"  Payload (hex): {data.hex()[:64]}{'…' if len(data) > 32 else ''}")

        assert len(data) > 0, "Received empty packet (0 bytes)"

        if rssi is not None:
            assert -130 <= rssi <= -10, (
                f"RSSI {rssi} dBm is outside the plausible range [-130, -10].\n"
                "  This may indicate phantom reception or a hardware issue."
            )
