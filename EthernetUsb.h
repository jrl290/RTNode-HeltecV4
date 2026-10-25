// EthernetUsb.h — wired Ethernet through a USB adapter (CDC-ECM) on the V4's
// USB-C port. Experimental, and off unless turned on in the portal (James,
// 2026-10-25). The plan, the proof of concept's results and the tests are in
// USB_ETHERNET_PLAN.md; the USB host is CherryUSB, in lib/CherryUSB.
//
// The S3 has one USB port, so it is either USB serial or the adapter's host,
// chosen at boot: with Ethernet on and no computer on the port by the end of
// setup()'s serial wait, it becomes the host, and USB serial is gone until
// the next restart. The log then also goes out on UART0, the header's TX and
// RX pins (JP4 5 and 6, 115200 baud).
//
// The cable carries what WiFi carries, from the same settings: the backbones
// (the WAN side) and the local TCP server (the LAN side). Nothing in the
// firewall changes.
//
// Header-only, like MdnsService.h. Without RTNODE_ETH_USB (every board but
// the V4s) the queries below say "no cable", so call sites need no #if.

#pragma once

#include <Arduino.h>

#ifdef RTNODE_ETH_USB
#include "EthernetUsbDriver.h"

// Arduino's WiFiGeneric.cpp: starts esp_netif, the default event loop and
// Arduino's network event task, once. WiFi does it when it starts; with WiFi
// off the cable needs it too.
bool tcpipInit();
#endif

namespace ethernet_usb {

#ifdef RTNODE_ETH_USB

struct State {
    bool host = false;           // the port became the adapter's host this boot
    uint32_t reported_ip = 0;    // the address last logged
};

inline State& state() {
    static State s;
    return s;
}

// The port is the adapter's host this boot (Ethernet on, no computer).
inline bool running() { return state().host; }

// The cable is up with an address from DHCP.
inline bool has_address() { return state().host && eth_usb_ipv4() != 0; }

// A log line on UART0 while the port is a host. Never waits: a line that
// doesn't fit is dropped and counted, so the log can't hold up loop().
inline void uart_line(const char* text, size_t len) {
    if (state().host) eth_usb_uart_line(text, len);
}

// RTNode's own log lines (on_log()), mirrored to UART0 while USB serial is
// gone.
inline void mirror_log(const String& line) { uart_line(line.c_str(), line.length()); }

inline void log_line(const char* text) {
    Serial.println(text);
    uart_line(text, strlen(text));
}

// First thing in setup(), on every V4 build. Which side owns the port is an
// RTC register that a software restart keeps: a node that ran as host and
// then had Ethernet turned off in the portal (which restarts it) would
// otherwise boot with a dead USB port.
inline void boot_port_to_serial() { eth_usb_port_to_serial(); }

// Once in setup(), after the serial wait, once WiFi is decided and before
// the TCP interfaces are made. enabled is the portal setting; hostname the
// node's DHCP name.
inline void begin(bool enabled, const char* hostname) {
    if (!enabled) return;
    if (HWCDC::isPlugged()) {
        Serial.println("[ETH] Ethernet is on, but a computer is on the USB port: it stays USB serial this boot");
        return;
    }
    // Never Serial.end() first: that makes the USB pins plain outputs, and
    // the host could not use them. With no computer, USB serial drops writes.
    // UART0 needs no setup: it is the console UART, running since boot.
    eth_usb_uart_start();
    state().host = true;
    log_line("[ETH] Ethernet is on and no computer is on the USB port: the port becomes a USB host for the adapter. "
             "The log continues here, on UART0, until the next restart.");
    tcpipInit();
    int r = eth_usb_start(hostname);
    if (r != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "[ETH] USB host did not start (%d): no Ethernet this boot", r);
        log_line(msg);
        state().host = false;
    }
}

// Every loop(): the library's log lines into the log, a few at a time and
// only as fast as UART0 takes them, and the wired address coming and going.
inline void loop() {
    State& s = state();
    if (!s.host) return;

    eth_usb_uart_pump();
    char line[168];
    for (int i = 0; i < 4; i++) {
        if (eth_usb_uart_free() < sizeof(line) + 2) break;
        if (!eth_usb_next_log_line(line + 6, sizeof(line) - 6)) break;
        memcpy(line, "[ETH] ", 6);
        log_line(line);
    }

    uint32_t ip = eth_usb_ipv4();
    if (ip != s.reported_ip) {
        s.reported_ip = ip;
        char msg[64];
        if (ip) {
            snprintf(msg, sizeof(msg), "[ETH] wired address %u.%u.%u.%u", (unsigned)(ip & 0xff),
                     (unsigned)((ip >> 8) & 0xff), (unsigned)((ip >> 16) & 0xff), (unsigned)(ip >> 24));
        } else {
            snprintf(msg, sizeof(msg), "[ETH] wired address lost");
        }
        log_line(msg);
    }
}

inline void get_info(eth_usb_info* out) { eth_usb_get_info(out); }

#else  // !RTNODE_ETH_USB

inline bool running() { return false; }
inline bool has_address() { return false; }
inline void mirror_log(const String&) {}
inline void log_line(const char* text) { Serial.println(text); }
inline void boot_port_to_serial() {}
inline void begin(bool, const char*) {}
inline void loop() {}

#endif

}  // namespace ethernet_usb
