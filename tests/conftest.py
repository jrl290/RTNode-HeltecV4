"""
pytest configuration for the RTNode LoRa test suite.

RTNode (FIREWALL_MODE) specifics
---------------------------------
In FIREWALL_MODE the RTNode serial port is used for ASCII RNS debug
logging only.  serial_write() is a compile-time no-op (Utilities.h,
#ifdef FIREWALL_MODE … return;).  No KISS frame responses will ever
arrive from the RTNode; all self-tests use log-line parsing instead.

The RNode probe (--rnode-port) IS a standard KISS TNC and responds
normally to KISS commands.

Channel configuration
----------------------
Because the RTNode does not answer KISS config queries, the LoRa channel
must be supplied via CLI options.  Defaults are the Reticulum EU868
medium-fast preset; override them to match your device's EEPROM config.

Usage
-----
Self-tests only (no RNode needed):
    pytest tests/ --rtnode-port /dev/cu.usbmodem114401 -v

Full TX/RX tests with an RNode probe:
    pytest tests/ \
        --rtnode-port /dev/cu.usbmodem114401 \
        --rnode-port  /dev/cu.usbmodem11201  \
        --lora-freq   869525000              \
        --lora-bw     250000                 \
        --lora-sf     8                      \
        --lora-cr     5                      \
        -v
"""

import struct
import glob
import time

import pytest
import serial

from kiss_serial import (
    CMD_BANDWIDTH,
    CMD_CR,
    CMD_FREQUENCY,
    CMD_IMPLICIT,
    CMD_RADIO_STATE,
    CMD_SF,
    CMD_TXPOWER,
    RADIO_STATE_OFF,
    RADIO_STATE_ON,
    KissSerial,
    RadioConfig,
)


# ── command-line options ──────────────────────────────────────────────────────

def pytest_addoption(parser):
    parser.addoption(
        "--rtnode-port",
        default=None,
        help="Serial port for the RTNode (Heltec V4 running FIREWALL_MODE firmware)",
    )
    parser.addoption(
        "--rnode-port",
        default=None,
        help="Serial port for the RNode probe used as LoRa TX/RX reference",
    )
    parser.addoption(
        "--baud",
        default=115200,
        type=int,
        help="Serial baud rate (default: 115200)",
    )
    parser.addoption(
        "--rtnode-host",
        default="mynode.local",
        help="RTNode TCP server hostname (default: mynode.local)",
    )
    parser.addoption(
        "--rtnode-tcp-port",
        default=4242,
        type=int,
        help="RTNode TCP server port (default: 4242)",
    )
    parser.addoption(
        "--rx-timeout",
        default=15.0,
        type=float,
        help="Seconds to wait for a LoRa receive event (default: 15)",
    )
    parser.addoption(
        "--announce-timeout",
        default=120.0,
        type=float,
        help="Seconds to wait for an RTNode LoRa transmission to be picked up "
             "by the RNode probe (default: 120).  RTNode announces periodically; "
             "increase if your device has a long announce interval.",
    )
    parser.addoption(
        "--tx-payload",
        default="RTNode-test",
        help="ASCII payload sent by the RNode in RX-path tests",
    )
    # ── LoRa channel parameters ────────────────────────────────────────────────
    # Defaults: Reticulum EU868 medium-fast preset
    # Change these to match your device's EEPROM-provisioned channel config.
    parser.addoption(
        "--lora-freq",
        default=869_525_000,
        type=int,
        help="LoRa centre frequency in Hz (default: 869525000, EU868 medium preset)",
    )
    parser.addoption(
        "--lora-bw",
        default=250_000,
        type=int,
        help="LoRa bandwidth in Hz (default: 250000)",
    )
    parser.addoption(
        "--lora-sf",
        default=8,
        type=int,
        help="LoRa spreading factor 5-12 (default: 8)",
    )
    parser.addoption(
        "--lora-cr",
        default=5,
        type=int,
        help="LoRa coding rate 5-8, where N means 4/N (default: 5 = 4/5)",
    )
    parser.addoption(
        "--lora-txp",
        default=14,
        type=int,
        help="LoRa TX power in dBm (default: 14)",
    )


# ── auto-discovery helpers ────────────────────────────────────────────────────

def _auto_detect_port(exclude: str | None = None) -> str | None:
    """Return the first plausible serial port that is not *exclude*."""
    candidates: list[str] = []
    candidates += glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
    candidates += glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    for p in candidates:
        if p == exclude:
            continue
        try:
            s = serial.Serial(p, 115200, timeout=0.1)
            s.close()
            return p
        except serial.SerialException:
            pass
    return None


# ── fixtures ──────────────────────────────────────────────────────────────────

@pytest.fixture(scope="session")
def rtnode_port(request) -> str:
    port = request.config.getoption("--rtnode-port")
    if port is None:
        port = _auto_detect_port()
    if port is None:
        pytest.skip("No RTNode port found; pass --rtnode-port /dev/cu.XXX")
    return port


@pytest.fixture(scope="session")
def rnode_port(request) -> str | None:
    return request.config.getoption("--rnode-port")


@pytest.fixture(scope="session")
def baud(request) -> int:
    return request.config.getoption("--baud")


@pytest.fixture(scope="session")
def rtnode_tcp_endpoint(request) -> tuple[str, int]:
    return (
        request.config.getoption("--rtnode-host"),
        request.config.getoption("--rtnode-tcp-port"),
    )


@pytest.fixture(scope="session")
def rx_timeout(request) -> float:
    return request.config.getoption("--rx-timeout")


@pytest.fixture(scope="session")
def announce_timeout(request) -> float:
    return request.config.getoption("--announce-timeout")


@pytest.fixture(scope="session")
def tx_payload(request) -> bytes:
    return request.config.getoption("--tx-payload").encode()


@pytest.fixture(scope="session")
def channel_config(request) -> RadioConfig:
    """
    LoRa channel configuration used for the whole test session.

    Sourced from CLI options; defaults to the Reticulum EU868 medium-fast
    preset.  Pass --lora-freq / --lora-bw / --lora-sf / --lora-cr to
    match the RTNode's EEPROM-provisioned channel.
    """
    return RadioConfig(
        frequency=request.config.getoption("--lora-freq"),
        bandwidth=request.config.getoption("--lora-bw"),
        sf=request.config.getoption("--lora-sf"),
        cr=request.config.getoption("--lora-cr"),
        txpower=request.config.getoption("--lora-txp"),
        state=RADIO_STATE_ON,
    )


@pytest.fixture(scope="session")
def rtnode_config(channel_config) -> RadioConfig:
    """
    Alias for channel_config.

    The RTNode does not answer KISS config queries (serial_write() is a
    no-op in FIREWALL_MODE), so CLI-supplied values are the authoritative
    source of the device's channel configuration.
    """
    return channel_config


@pytest.fixture(scope="session")
def rtnode(rtnode_port, baud) -> KissSerial:
    """
    Open a connection to the RTNode for the whole test session.

    Resets the device via DTR/RTS at session start so the startup log
    (including ``[RTNode] LoRa: freq=... bw=... sf=... cr=... txp=...``)
    is always captured.  This makes ``test_channel_config`` reliable rather
    than dependent on the device having been rebooted externally.
    """
    ks = KissSerial(port=rtnode_port, baud=baud)
    ks.start()
    # Brief settle before toggling control lines
    time.sleep(0.5)
    # Standard Arduino/ESP32 auto-reset: DTR=F, RTS=T → RESET=low; then both=F → boot
    try:
        ks._ser.setDTR(False)
        ks._ser.setRTS(True)
        time.sleep(0.1)
        ks._ser.setDTR(False)
        ks._ser.setRTS(False)
    except Exception:
        pass  # DTR not available on all adapters — continue without reset
    # Wait for ESP32-S3 to boot and for RNS to initialise (typically 3-4 s)
    time.sleep(5.0)
    yield ks
    ks.stop()


@pytest.fixture(scope="session")
def rnode(rnode_port, baud, channel_config) -> "KissSerial | None":
    """
    Open a connection to the RNode probe (if --rnode-port is provided).

    Configures the RNode with channel_config parameters so it is on the
    same LoRa channel as the RTNode, enables promiscuous mode, and powers
    the radio on.
    """
    if rnode_port is None:
        return None

    ks = KissSerial(port=rnode_port, baud=baud)
    ks.start()
    time.sleep(1.5)

    cfg = channel_config
    # Some RNode config setters leave the SX126x out of continuous RX when
    # applied while the radio is already online. Force a clean restart so the
    # final RADIO_STATE_ON path always calls LoRa->receive().
    ks._send_frame(CMD_RADIO_STATE, bytes([RADIO_STATE_OFF]))
    time.sleep(0.2)
    ks._send_frame(CMD_FREQUENCY, struct.pack(">I", cfg.frequency))
    time.sleep(0.05)
    ks._send_frame(CMD_BANDWIDTH, struct.pack(">I", cfg.bandwidth))
    time.sleep(0.05)
    ks._send_frame(CMD_TXPOWER, bytes([cfg.txpower]))
    time.sleep(0.05)
    ks._send_frame(CMD_SF, bytes([cfg.sf]))
    time.sleep(0.05)
    ks._send_frame(CMD_CR, bytes([cfg.cr]))
    time.sleep(0.05)
    ks._send_frame(CMD_IMPLICIT, bytes([0x00]))
    time.sleep(0.1)
    ks._send_frame(CMD_RADIO_STATE, bytes([RADIO_STATE_ON]))
    time.sleep(0.3)
    ks.enable_promisc()
    time.sleep(0.2)

    yield ks
    ks.stop()
