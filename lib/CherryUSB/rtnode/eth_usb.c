/*
 * RTNode's side of CherryUSB: starting the host on the V4's USB-C port, the
 * class driver table, choosing the adapter configuration that offers ECM,
 * the library's log, the log on UART0, and the state EthernetUsbDriver.h
 * reports.
 *
 * Part of RTNode, not of CherryUSB: GNU GPL v3, as LICENSE at the repository
 * root.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "hal/uart_ll.h"
#include "hal/usb_phy_ll.h"
#include "soc/usb_serial_jtag_struct.h"

#include "usbh_core.h"
#include "usbh_hub.h"
#include "usbh_cdc_ecm.h"
#include "rtnode/eth_usb_state.h"

#include "EthernetUsbDriver.h"

extern const struct usbh_class_driver hub_class_driver;
extern const struct usbh_class_driver cdc_ecm_class_driver;

extern volatile uint32_t usbh_hub_int_errors;
extern uint8_t (*ep0_request_buffer)[USB_ALIGN_UP(CONFIG_USBHOST_REQUEST_BUFFER_LEN, CONFIG_USB_ALIGN_SIZE)];

/* usbh_core.c walks the class drivers between __usbh_class_info_start__ and
 * __usbh_class_info_end__. Upstream fills that range from a linker section
 * that ESP-IDF's linker fragments collect; Arduino's prebuilt linker scripts
 * don't, so the range is this plain array: hubs, and ECM only. */
__attribute__((used)) const struct usbh_class_info eth_usb_class_table[2] = {
    {
        .match_flags = USB_CLASS_MATCH_INTF_CLASS,
        .bInterfaceClass = USB_DEVICE_CLASS_HUB,
        .class_driver = &hub_class_driver,
    },
    {
        .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
        .bInterfaceClass = USB_DEVICE_CLASS_CDC,
        .bInterfaceSubClass = CDC_ETHERNET_NETWORKING_CONTROL_MODEL,
        .bInterfaceProtocol = CDC_COMMON_PROTOCOL_NONE,
        .class_driver = &cdc_ecm_class_driver,
    },
};
_Static_assert(sizeof(eth_usb_class_table) == 32, "update the end symbol below");
__asm__(".global __usbh_class_info_start__\n"
        ".set __usbh_class_info_start__, eth_usb_class_table\n"
        ".global __usbh_class_info_end__\n"
        ".set __usbh_class_info_end__, eth_usb_class_table + 32\n");

/* ---- the library's log ------------------------------------------------- */

/* Lines wait here for RTNode's loop (eth_usb_next_log_line()). In PSRAM when
 * the board has it, taken when the host starts. */
#define LOG_RING_BYTES 4096
#define LOG_LINE_MAX 160

static RingbufHandle_t log_ring;
static volatile uint32_t log_lost;

static void log_push(const char *line)
{
    /* No interrupt path logs at the level built in; if one ever does, it is
     * counted rather than formatted and queued from the interrupt. */
    if (!log_ring || xPortInIsrContext() ||
        xRingbufferSend(log_ring, line, strlen(line) + 1, 0) != pdTRUE) {
        log_lost++;
    }
}

static void log_vformat(const char *prefix, const char *fmt, va_list ap)
{
    char line[LOG_LINE_MAX];
    int o = snprintf(line, sizeof(line), "%s", prefix);

    vsnprintf(line + o, sizeof(line) - o, fmt, ap);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = 0;
    }
    if (n) {
        log_push(line);
    }
}

/* CONFIG_USB_LOG_LINE (usb_config.h): one whole line per call. */
int eth_usb_log_line(const char *level, const char *tag, const char *fmt, ...)
{
    char prefix[32];
    va_list ap;

    snprintf(prefix, sizeof(prefix), "%s/%s: ", level, tag);
    va_start(ap, fmt);
    log_vformat(prefix, fmt, ap);
    va_end(ap);
    return 0;
}

/* CONFIG_USB_PRINTF: raw prints (hex dumps), each its own line. */
int eth_usb_log_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_vformat("", fmt, ap);
    va_end(ap);
    return 0;
}

bool eth_usb_next_log_line(char *buf, size_t n)
{
    size_t len;
    char *item;

    if (!log_ring || n == 0) {
        return false;
    }
    item = xRingbufferReceive(log_ring, &len, 0);
    if (!item) {
        return false;
    }
    if (len > n) {
        len = n;
    }
    memcpy(buf, item, len);
    buf[len - 1] = 0;
    vRingbufferReturnItem(log_ring, item);
    return true;
}

/* A ring in PSRAM when the board has it. Its control block holds a spinlock,
 * so that stays internal. */
static RingbufHandle_t ring_create(size_t bytes, RingbufferType_t type)
{
    StaticRingbuffer_t *ctrl = heap_caps_calloc(1, sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *storage = heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    RingbufHandle_t ring = NULL;

    if (ctrl && storage) {
        ring = xRingbufferCreateStatic(bytes, type, storage, ctrl);
    }
    if (!ring) {
        free(ctrl);
        free(storage);
    }
    return ring;
}

/* ---- the log on UART0 --------------------------------------------------- */

/* UART0 is the console UART, running at 115200 on GPIO43/44 from boot, so it
 * needs no driver. Lines wait in this ring and eth_usb_uart_pump() moves them
 * into the UART's 128-byte FIFO as it empties: nothing waits on the UART. A
 * line that doesn't fit the ring is dropped and counted. Used from RTNode's
 * loop task only. */
#define UART_RING_BYTES 4096

static RingbufHandle_t uart_ring;
static volatile uint32_t uart_lost;

bool eth_usb_uart_start(void)
{
    if (!uart_ring) {
        uart_ring = ring_create(UART_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    }
    return uart_ring != NULL;
}

size_t eth_usb_uart_free(void)
{
    return uart_ring ? xRingbufferGetCurFreeSize(uart_ring) : 0;
}

void eth_usb_uart_pump(void)
{
    if (!uart_ring) {
        return;
    }
    /* Two pieces at most: the waiting bytes may wrap round the ring's end. */
    for (int i = 0; i < 2; i++) {
        uint32_t room = uart_ll_get_txfifo_len(&UART0);
        size_t n = 0;
        uint8_t *p;

        if (room == 0) {
            return;
        }
        p = xRingbufferReceiveUpTo(uart_ring, &n, 0, room);
        if (!p) {
            return;
        }
        uart_ll_write_txfifo(&UART0, p, n);
        vRingbufferReturnItem(uart_ring, p);
    }
}

void eth_usb_uart_line(const char *text, size_t len)
{
    bool crlf = len == 0 || text[len - 1] != '\n';

    if (!uart_ring || xRingbufferGetCurFreeSize(uart_ring) < len + 2) {
        uart_lost++;
        return;
    }
    if (len) {
        xRingbufferSend(uart_ring, text, len, 0);
    }
    if (crlf) {
        xRingbufferSend(uart_ring, "\r\n", 2, 0);
    }
    eth_usb_uart_pump();
}

/* ---- choosing the configuration ---------------------------------------- */

/* Adapters keep ECM in a later configuration: Realtek's 8152/8153 in the
 * second, ASIX's AX88179A in the third, after their own protocol (and NCM).
 * CherryUSB enumerates whichever index this returns, so read each one and
 * take the first with an ECM control interface (class 02, subclass 06). It
 * runs before CherryUSB reads the configuration itself, so the control
 * buffer is free to use. */
uint8_t usbh_get_hport_active_config_index(struct usbh_hubport *hport)
{
    uint8_t *buf = ep0_request_buffer[hport->bus->busid];
    uint8_t count = hport->device_desc.bNumConfigurations;
    bool known = false; /* a hub, or ECM */
    int pick = -1;

    USB_LOG_INFO("Device %04x:%04x has %u configuration(s)\r\n",
                 hport->device_desc.idVendor, hport->device_desc.idProduct, count);
    for (uint8_t i = 0; i < count && i < 8; i++) {
        struct usb_setup_packet *setup = hport->setup;
        setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
        setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
        setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8) | i);
        setup->wIndex = 0;
        setup->wLength = CONFIG_USBHOST_REQUEST_BUFFER_LEN;
        int len = usbh_control_transfer(hport, setup, buf);
        if (len < USB_SIZEOF_CONFIG_DESC) {
            USB_LOG_WRN("Configuration index %u: read failed (%d)\r\n", i, len);
            continue;
        }
        int total = buf[2] | (buf[3] << 8);
        if (total > len) {
            total = len;
        }
        char line[120];
        int o = snprintf(line, sizeof(line), "index %u:", i);
        for (int p = 0; p + 2 <= total && buf[p] >= 2; p += buf[p]) {
            if (buf[p + 1] != USB_DESCRIPTOR_TYPE_INTERFACE || p + 8 > total) {
                continue;
            }
            uint8_t cls = buf[p + 5], sub = buf[p + 6], proto = buf[p + 7];
            if (o < (int)sizeof(line) - 16) {
                o += snprintf(line + o, sizeof(line) - o, " if%u.%u %02x/%02x/%02x",
                              buf[p + 2], buf[p + 3], cls, sub, proto);
            }
            if (cls == USB_DEVICE_CLASS_HUB) {
                known = true;
            }
            if (pick < 0 && cls == USB_DEVICE_CLASS_CDC && sub == CDC_ETHERNET_NETWORKING_CONTROL_MODEL) {
                pick = i;
                known = true;
            }
        }
        USB_LOG_INFO("%s\r\n", line);
    }
    if (pick < 0) {
        if (!known) {
            USB_LOG_WRN("Device %04x:%04x offers no CDC-ECM (USB Ethernet) configuration: not supported\r\n",
                        hport->device_desc.idVendor, hport->device_desc.idProduct);
        }
        return 0;
    }
    USB_LOG_INFO("Using configuration index %d (CDC-ECM)\r\n", pick);
    return pick;
}

/* ---- EthernetUsbDriver.h ------------------------------------------------ */

static volatile bool started;

/* The header's inline register helpers don't compile as C++, hence C here. */
void eth_usb_port_to_serial(void)
{
    usb_phy_ll_int_jtag_enable(&USB_SERIAL_JTAG);
}

int eth_usb_start(const char *hostname)
{
    if (started) {
        return 0;
    }
    if (!g_eth_usb_state) {
        g_eth_usb_state = USB_CPU_CALLOC(1, sizeof(*g_eth_usb_state));
    }
    if (!log_ring) {
        log_ring = ring_create(LOG_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    }
    if (!g_eth_usb_state || !log_ring) {
        return -USB_ERR_NOMEM;
    }
    if (hostname) {
        strncpy(g_eth_usb_state->hostname, hostname, sizeof(g_eth_usb_state->hostname) - 1);
    }
    int ret = usbh_initialize(0, ESP_USBH_BASE, NULL);
    if (ret == 0) {
        started = true;
    }
    return ret;
}

uint32_t eth_usb_ipv4(void)
{
    return g_eth_usb_state ? g_eth_usb_state->ipv4 : 0;
}

void eth_usb_get_info(struct eth_usb_info *out)
{
    struct eth_usb_state *st = g_eth_usb_state;

    memset(out, 0, sizeof(*out));
    out->started = started;
    out->uart_lost = uart_lost;
    out->log_lost = log_lost;
    if (!st) {
        return;
    }
    out->adapter = st->adapter;
    out->link = st->link;
    out->ipv4 = st->ipv4;
    out->netmask = st->netmask;
    out->gw = st->gw;
    out->vid = st->vid;
    out->pid = st->pid;
    for (int i = 0; i < 6; i++) {
        out->mac[i] = st->mac[i];
    }
    out->link_changes = st->link_changes;
    out->tx_frames = st->tx_frames;
    out->tx_dropped = st->tx_dropped;
    out->tx_last_error = st->tx_last_error;
    out->rx_frames = st->rx_frames;
    out->rx_dropped = st->rx_dropped;
    out->rx_last_error = st->rx_last_error;
    out->hub_errors = usbh_hub_int_errors;
}
