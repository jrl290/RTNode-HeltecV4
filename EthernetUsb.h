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
//
// Bench builds (RTNODE_BENCH) also send that log to TCP port 4243, for a
// bench with no serial adapter on UART0 (netlog below).

#pragma once

#include <Arduino.h>
#include <stdarg.h>

#ifdef RTNODE_ETH_USB
#include "EthernetUsbDriver.h"
#ifdef RTNODE_BENCH
#include <WiFi.h>
#include <fcntl.h>
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#endif

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

#ifdef RTNODE_BENCH
// Bench builds only: the mirrored log also goes to TCP port 4243, one client
// at a time (the newest), as "<seconds since boot> <line>". The last 16 KB
// wait in PSRAM, so a client that connects late gets what came before. A
// client that falls more than that behind loses the oldest bytes, and is told
// how many. Never waits: sockets are non-blocking. Loop task only.
//
// The client may send one command per line, for the restart tests:
//   restart         restart as a portal save does
//   restart nowifi  the same, with WiFi off for the next boot only
namespace netlog {

constexpr uint16_t kPort = 4243;
constexpr uint32_t kBytes = 16384;
constexpr uint32_t kNoWifiMagic = 0x4E6F5746;  // "NoWF"

struct Log {
    uint8_t* buf = nullptr;
    uint32_t wpos = 0;  // bytes written since boot
    uint32_t rpos = 0;  // the client's next byte
    int listen_fd = -1;
    int client_fd = -1;
    uint32_t next_listen_ms = 0;
    char cmd[24] = {};
    uint8_t cmd_len = 0;
    uint32_t restart_at_ms = 0;  // nonzero: restart once millis() reaches it
};

inline Log& log() {
    static Log l;
    return l;
}

// Kept across a software restart (RTC memory isn't cleared by one).
inline uint32_t& nowifi_flag() {
    static RTC_NOINIT_ATTR uint32_t flag;
    return flag;
}

// At boot: true once after "restart nowifi".
inline bool take_nowifi() {
    bool set = nowifi_flag() == kNoWifiMagic;
    nowifi_flag() = 0;
    return set;
}

inline void put_bytes(const char* p, size_t n) {
    Log& l = log();
    for (size_t i = 0; i < n; i++) l.buf[(l.wpos + i) % kBytes] = (uint8_t)p[i];
    l.wpos += n;
}

inline void put(const char* text, size_t len) {
    Log& l = log();
    if (!l.buf) {
        l.buf = (uint8_t*)heap_caps_malloc_prefer(kBytes, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (!l.buf) return;
    }
    char stamp[16];
    uint32_t ms = millis();
    int n = snprintf(stamp, sizeof(stamp), "%lu.%03lu ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    put_bytes(stamp, n);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    put_bytes(text, len);
    put_bytes("\r\n", 2);
}

inline void command(const char* cmd) {
    bool nowifi = strcmp(cmd, "restart nowifi") == 0;
    if (!nowifi && strcmp(cmd, "restart") != 0) {
        char note[64];
        int n = snprintf(note, sizeof(note), "[NETLOG] unknown command: %s", cmd);
        put(note, n);
        return;
    }
    nowifi_flag() = nowifi ? kNoWifiMagic : 0;
    const char* note = nowifi ? "[NETLOG] restarting, WiFi off for the next boot" : "[NETLOG] restarting";
    put(note, strlen(note));
    log().restart_at_ms = millis() + 300;  // time to send the note
    if (log().restart_at_ms == 0) log().restart_at_ms = 1;
}

// Reads the client's command lines. Returns false once the client has gone.
inline bool read_commands() {
    Log& l = log();
    char in[32];
    int got = recv(l.client_fd, in, sizeof(in), MSG_DONTWAIT);
    if (got == 0) return false;
    if (got < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
    for (int i = 0; i < got; i++) {
        if (in[i] == '\n' || in[i] == '\r') {
            if (l.cmd_len) {
                l.cmd[l.cmd_len] = 0;
                command(l.cmd);
                l.cmd_len = 0;
            }
        } else if (l.cmd_len < sizeof(l.cmd) - 1) {
            l.cmd[l.cmd_len++] = in[i];
        }
    }
    return true;
}

// Every loop(): opens the port once the network is up, takes a new client,
// reads its commands, and sends what the socket will take now.
inline void pump() {
    Log& l = log();
    uint32_t now = millis();
    if (l.restart_at_ms && (int32_t)(now - l.restart_at_ms) >= 0) ESP.restart();
    if (l.listen_fd < 0) {
        if ((int32_t)(now - l.next_listen_ms) < 0) return;
        l.next_listen_ms = now + 5000;
        // lwIP must be running: WiFi up, or the cable's address.
        if (WiFi.status() != WL_CONNECTED && !has_address()) return;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(kPort);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
            close(fd);
            return;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        l.listen_fd = fd;
        Serial.printf("[ETH] bench: log on TCP port %u\r\n", (unsigned)kPort);
    }
    int c = accept(l.listen_fd, nullptr, nullptr);
    if (c >= 0) {
        if (l.client_fd >= 0) close(l.client_fd);
        fcntl(c, F_SETFL, O_NONBLOCK);
        l.client_fd = c;
        l.rpos = l.wpos > kBytes ? l.wpos - kBytes : 0;
        l.cmd_len = 0;
    }
    if (l.client_fd < 0) return;
    if (!read_commands()) {
        close(l.client_fd);
        l.client_fd = -1;
        return;
    }
    if (!l.buf) return;
    if (l.wpos - l.rpos > kBytes) {
        char note[48];
        int n = snprintf(note, sizeof(note), "[NETLOG] %lu bytes lost\r\n", (unsigned long)(l.wpos - l.rpos - kBytes));
        send(l.client_fd, note, n, MSG_DONTWAIT);
        l.rpos = l.wpos - kBytes;
    }
    // Two pieces at most: the waiting bytes may wrap round the buffer's end.
    for (int i = 0; i < 2 && l.rpos != l.wpos; i++) {
        uint32_t off = l.rpos % kBytes;
        uint32_t n = min(l.wpos - l.rpos, kBytes - off);
        int sent = send(l.client_fd, l.buf + off, n, MSG_DONTWAIT);
        if (sent > 0) {
            l.rpos += sent;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close(l.client_fd);
        l.client_fd = -1;
        break;
    }
}

}  // namespace netlog
#endif

// A log line on UART0 while the port is a host (and, on bench builds, on the
// network log). Never waits: a line that doesn't fit is dropped and counted,
// so the log can't hold up loop().
inline void uart_line(const char* text, size_t len) {
    if (state().host) eth_usb_uart_line(text, len);
#ifdef RTNODE_BENCH
    netlog::put(text, len);
#endif
}

// RTNode's own log lines (on_log()), mirrored to UART0 while USB serial is
// gone.
inline void mirror_log(const String& line) { uart_line(line.c_str(), line.length()); }

inline void log_line(const char* text) {
    Serial.println(text);
    uart_line(text, strlen(text));
}

// log_line() with printf formatting, for the lines that matter with USB
// serial gone (the TCP interfaces'). A trailing newline is dropped.
inline void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void logf(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    log_line(buf);
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
        log_line("[ETH] Ethernet is on, but a computer is on the USB port: it stays USB serial this boot");
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
#ifdef RTNODE_BENCH
    netlog::pump();
#endif
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
inline void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void logf(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    Serial.println(buf);
}
inline void boot_port_to_serial() {}
inline void begin(bool, const char*) {}
inline void loop() {}

#endif

}  // namespace ethernet_usb
