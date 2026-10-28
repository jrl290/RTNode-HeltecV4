/*
 * RTNode: what the network glue (platform/idf/usbh_net.c) records about the
 * wired interface, for RTNode's side (eth_usb.c). Each counter is one word or
 * less, written by one task and read without locks.
 *
 * Part of RTNode, not of CherryUSB: GNU GPL v3, as LICENSE at the repository
 * root.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct eth_usb_state {
    volatile bool adapter; /* an ECM adapter is attached and its network interface made */
    volatile bool link;    /* the adapter last reported its cable connected */
    volatile uint32_t ipv4, netmask, gw; /* from DHCP; 0 without an address */
    volatile uint16_t vid, pid;
    volatile uint8_t mac[6];
    volatile uint32_t link_changes;
    volatile uint32_t tx_frames, tx_bytes, tx_padded, tx_dropped;
    volatile int32_t tx_last_error;
    volatile uint32_t rx_frames, rx_bytes, rx_dropped;
    volatile int32_t rx_last_error;
    char hostname[33]; /* the DHCP host name, set before the host starts; empty for the SDK's */
};

/* Allocated by eth_usb_start() before the host starts, so it costs nothing
 * with Ethernet off; NULL until then. */
extern struct eth_usb_state *g_eth_usb_state;
