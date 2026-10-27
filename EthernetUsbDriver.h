// EthernetUsbDriver.h — the C side of wired Ethernet through a USB adapter
// (CDC-ECM), implemented in lib/CherryUSB/rtnode/eth_usb.c. EthernetUsb.h is
// what the firmware uses; this is its interface to the USB library.
//
// It is a project header, not one of the library's, on purpose: PlatformIO's
// dependency finder would otherwise build the library for every board that
// includes EthernetUsb.h. Only the V4 environments list CherryUSB.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct eth_usb_info {
    bool started;  // the USB port is a host
    bool adapter;  // an ECM adapter is attached
    bool link;     // its cable is in
    uint32_t ipv4, netmask, gw;  // lwIP byte order; 0 without an address
    uint16_t vid, pid;
    uint8_t mac[6];
    uint32_t link_changes;
    uint32_t tx_frames, tx_dropped, rx_frames, rx_dropped;
    int32_t tx_last_error, rx_last_error;
    uint32_t hub_errors;  // hub reports lost to a bad transfer (each re-read)
    uint32_t log_lost;    // library log lines that found the queue full
    uint32_t uart_lost;   // log lines that found the UART0 ring full
};

// Gives the USB port (the S3's one PHY) to USB serial. Which side owns it is
// an RTC register that a software restart keeps.
void eth_usb_port_to_serial(void);

// Takes the port from USB serial and starts the host. hostname is the DHCP
// name for the wired interface (NULL for the SDK's). 0, or a negative error.
int eth_usb_start(const char* hostname);

// The wired IPv4 address (lwIP byte order), 0 without one.
uint32_t eth_usb_ipv4(void);

void eth_usb_get_info(struct eth_usb_info* out);

// The library's next log line ("I/usbh_hub: ...", no newline) into buf;
// false when none is waiting. Lines are queued because the library's tasks
// must not call RTNode's logger, which isn't thread-safe.
bool eth_usb_next_log_line(char* buf, size_t n);

// The log on UART0 (the console UART, GPIO43/44, 115200) while USB serial is
// gone. Lines wait in a ring (taken by eth_usb_uart_start(), false without
// memory) and eth_usb_uart_pump() feeds the UART's FIFO from it; nothing
// waits on the UART. A line that doesn't fit is dropped and counted
// (uart_lost). Loop task only.
bool eth_usb_uart_start(void);
void eth_usb_uart_line(const char* text, size_t len);
size_t eth_usb_uart_free(void);
void eth_usb_uart_pump(void);

#ifdef __cplusplus
}
#endif
