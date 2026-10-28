/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Changed for RTNode, 2026: RTNODE.md at the library's root lists the changes.
 */
#include "usbh_core.h"
#include "usbh_cdc_ecm.h"
#include "esp_timer.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_cdc_ecm"
#include "usb_log.h"

#define DEV_FORMAT "/dev/cdc_ether"

/* general descriptor field offsets */
#define DESC_bLength            0 /** Length offset */
#define DESC_bDescriptorType    1 /** Descriptor type offset */
#define DESC_bDescriptorSubType 2 /** Descriptor subtype offset */

/* interface descriptor field offsets */
#define INTF_DESC_bInterfaceNumber  2 /** Interface number offset */
#define INTF_DESC_bAlternateSetting 3 /** Alternate setting offset */

#define CONFIG_USBHOST_CDC_ECM_PKT_FILTER   0x000C
#define CONFIG_USBHOST_CDC_ECM_ETH_MAX_SIZE 1514U

/* RTNode: the frame buffers are taken when the first adapter connects and
 * kept (usb_config.h says why they aren't static). */
#define CDC_ECM_BUFFER_SIZE USB_ALIGN_UP(CONFIG_USBHOST_CDC_ECM_ETH_MAX_SIZE, CONFIG_USB_ALIGN_SIZE)
static uint8_t *g_cdc_ecm_rx_buffer;
static uint8_t *g_cdc_ecm_tx_buffer;
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ecm_inttx_buffer[USB_ALIGN_UP(16, CONFIG_USB_ALIGN_SIZE)];

/* RTNode: allocated at the first connect and kept, like the buffers. */
static struct usbh_cdc_ecm *g_cdc_ecm_class;

/* RTNode: the receive thread. rx_done is given when a receive transfer ends;
 * rx_exit when the thread has stopped touching the adapter, which
 * disconnect waits for before the network interface goes. rx_running is set
 * before the thread is made and cleared as it ends; rx_stop asks it to end. */
static usb_osal_sem_t g_cdc_ecm_rx_done;
static usb_osal_sem_t g_cdc_ecm_rx_exit;
static volatile bool g_cdc_ecm_rx_running;
static volatile bool g_cdc_ecm_rx_stop;

static int usbh_cdc_ecm_set_eth_packet_filter(struct usbh_cdc_ecm *cdc_ecm_class, uint16_t filter_value)
{
    struct usb_setup_packet *setup;

    if (!cdc_ecm_class || !cdc_ecm_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = cdc_ecm_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_SET_ETHERNET_PACKET_FILTER;
    setup->wValue = filter_value;
    setup->wIndex = cdc_ecm_class->ctrl_intf;
    setup->wLength = 0;

    return usbh_control_transfer(cdc_ecm_class->hport, setup, NULL);
}

int usbh_cdc_ecm_get_connect_status(struct usbh_cdc_ecm *cdc_ecm_class)
{
    int ret;

    /* RTNode: a ceiling, not forever: an adapter with nothing to report
     * answers NAK at once, and the receive thread asks again later. */
    usbh_int_urb_fill(&cdc_ecm_class->intin_urb, cdc_ecm_class->hport, cdc_ecm_class->intin, g_cdc_ecm_inttx_buffer, 16, CONFIG_USBHOST_CDC_ECM_LINK_TIMEOUT_MS, NULL, NULL);
    ret = usbh_submit_urb(&cdc_ecm_class->intin_urb);
    if (ret < 0) {
        return ret;
    }

    /* RTNode: a notification starts with its 8-byte header, from the class
     * interface (0xA1). With 8-byte interrupt packets a speed report's
     * second half arrives on its own; it is data, not a notification. */
    if (cdc_ecm_class->intin_urb.actual_length < 8 || g_cdc_ecm_inttx_buffer[0] != 0xA1) {
        return 0;
    }

    if (g_cdc_ecm_inttx_buffer[1] == CDC_ECM_NOTIFY_CODE_NETWORK_CONNECTION) {
        if (g_cdc_ecm_inttx_buffer[2] == CDC_ECM_NET_CONNECTED) {
            cdc_ecm_class->connect_status = true;
        } else {
            cdc_ecm_class->connect_status = false;
        }
    } else if (g_cdc_ecm_inttx_buffer[1] == CDC_ECM_NOTIFY_CODE_CONNECTION_SPEED_CHANGE) {
        memcpy(cdc_ecm_class->speed, &g_cdc_ecm_inttx_buffer[8], 8);
    }
    return 0;
}

static int usbh_cdc_ecm_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usb_endpoint_descriptor *ep_desc;
    int ret;
    uint8_t altsetting = 0;
    char mac_buffer[12];
    uint8_t *p;
    uint8_t cur_iface = 0xff;
    uint8_t mac_str_idx = 0xff;

    struct usbh_cdc_ecm *cdc_ecm_class;

    if (g_cdc_ecm_rx_running) {
        /* RTNode: one adapter at a time; the buffers and thread are single. */
        USB_LOG_WRN("A second CDC ECM adapter is ignored\r\n");
        return -USB_ERR_BUSY;
    }
    if (!g_cdc_ecm_class) {
        g_cdc_ecm_class = USB_CPU_CALLOC(1, sizeof(struct usbh_cdc_ecm));
    }
    if (!g_cdc_ecm_rx_buffer) {
        g_cdc_ecm_rx_buffer = USB_DMA_CALLOC(1, CDC_ECM_BUFFER_SIZE);
    }
    if (!g_cdc_ecm_tx_buffer) {
        g_cdc_ecm_tx_buffer = USB_DMA_CALLOC(1, CDC_ECM_BUFFER_SIZE);
    }
    if (!g_cdc_ecm_rx_done) {
        g_cdc_ecm_rx_done = usb_osal_sem_create(0);
    }
    if (!g_cdc_ecm_rx_exit) {
        g_cdc_ecm_rx_exit = usb_osal_sem_create(0);
    }
    if (!g_cdc_ecm_class || !g_cdc_ecm_rx_buffer || !g_cdc_ecm_tx_buffer || !g_cdc_ecm_rx_done || !g_cdc_ecm_rx_exit) {
        USB_LOG_ERR("No memory for the CDC ECM adapter\r\n");
        return -USB_ERR_NOMEM;
    }

    cdc_ecm_class = g_cdc_ecm_class;
    memset(cdc_ecm_class, 0, sizeof(struct usbh_cdc_ecm));

    cdc_ecm_class->hport = hport;
    cdc_ecm_class->ctrl_intf = intf;
    cdc_ecm_class->data_intf = intf + 1;

    hport->config.intf[intf].priv = cdc_ecm_class;
    hport->config.intf[intf + 1].priv = NULL;

    p = hport->raw_config_desc;
    while (p[DESC_bLength]) {
        switch (p[DESC_bDescriptorType]) {
            case USB_DESCRIPTOR_TYPE_INTERFACE:
                cur_iface = p[INTF_DESC_bInterfaceNumber];
                //cur_alt_setting = p[INTF_DESC_bAlternateSetting];
                break;
            case CDC_CS_INTERFACE:
                if ((cur_iface == cdc_ecm_class->ctrl_intf) && p[DESC_bDescriptorSubType] == CDC_FUNC_DESC_ETHERNET_NETWORKING) {
                    struct cdc_eth_descriptor *desc = (struct cdc_eth_descriptor *)p;
                    mac_str_idx = desc->iMACAddress;
                    cdc_ecm_class->max_segment_size = desc->wMaxSegmentSize;
                    goto get_mac;
                }
                break;

            default:
                break;
        }
        /* skip to next descriptor */
        p += p[DESC_bLength];
    }

get_mac:
    if (mac_str_idx == 0xff) {
        USB_LOG_ERR("Do not find cdc ecm mac string\r\n");
        return -1;
    }

    memset(mac_buffer, 0, 12);
    ret = usbh_get_string_desc(cdc_ecm_class->hport, mac_str_idx, (uint8_t *)mac_buffer, 12);
    if (ret < 0) {
        return ret;
    }

    for (int i = 0, j = 0; i < 12; i += 2, j++) {
        char byte_str[3];
        byte_str[0] = mac_buffer[i];
        byte_str[1] = mac_buffer[i + 1];
        byte_str[2] = '\0';

        uint32_t byte = strtoul(byte_str, NULL, 16);
        cdc_ecm_class->mac[j] = (unsigned char)byte;
    }

    USB_LOG_INFO("CDC ECM MAC address %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                 cdc_ecm_class->mac[0],
                 cdc_ecm_class->mac[1],
                 cdc_ecm_class->mac[2],
                 cdc_ecm_class->mac[3],
                 cdc_ecm_class->mac[4],
                 cdc_ecm_class->mac[5]);

    if (cdc_ecm_class->max_segment_size > CONFIG_USBHOST_CDC_ECM_ETH_MAX_SIZE) {
        USB_LOG_ERR("CDC ECM Max Segment Size is overflow, default is %u, but now %u\r\n", CONFIG_USBHOST_CDC_ECM_ETH_MAX_SIZE, cdc_ecm_class->max_segment_size);
    } else {
        USB_LOG_INFO("CDC ECM Max Segment Size:%u\r\n", cdc_ecm_class->max_segment_size);
    }

    /* enable int ep */
    ep_desc = &hport->config.intf[intf].altsetting[0].ep[0].ep_desc;
    USBH_EP_INIT(cdc_ecm_class->intin, ep_desc);

    if (hport->config.intf[intf + 1].altsetting_num > 1) {
        altsetting = hport->config.intf[intf + 1].altsetting_num - 1;

        for (uint8_t i = 0; i < hport->config.intf[intf + 1].altsetting[altsetting].intf_desc.bNumEndpoints; i++) {
            ep_desc = &hport->config.intf[intf + 1].altsetting[altsetting].ep[i].ep_desc;

            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(cdc_ecm_class->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(cdc_ecm_class->bulkout, ep_desc);
            }
        }

        USB_LOG_INFO("Select cdc ecm altsetting: %d\r\n", altsetting);
        usbh_set_interface(cdc_ecm_class->hport, cdc_ecm_class->data_intf, altsetting);
    } else {
        for (uint8_t i = 0; i < hport->config.intf[intf + 1].altsetting[0].intf_desc.bNumEndpoints; i++) {
            ep_desc = &hport->config.intf[intf + 1].altsetting[0].ep[i].ep_desc;

            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(cdc_ecm_class->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(cdc_ecm_class->bulkout, ep_desc);
            }
        }
    }

    /* bit0 Promiscuous
    * bit1 ALL Multicast
    * bit2 Directed
    * bit3 Broadcast
    * bit4 Multicast
    */
    ret = usbh_cdc_ecm_set_eth_packet_filter(cdc_ecm_class, CONFIG_USBHOST_CDC_ECM_PKT_FILTER);
    if (ret < 0) {
        return ret;
    }
    USB_LOG_INFO("Set CDC ECM packet filter:%04x\r\n", CONFIG_USBHOST_CDC_ECM_PKT_FILTER);

    strncpy(hport->config.intf[intf].devname, DEV_FORMAT, CONFIG_USBHOST_DEV_NAMELEN);

    USB_LOG_INFO("Register CDC ECM Class:%s\r\n", hport->config.intf[intf].devname);

    g_cdc_ecm_rx_stop = false;
    g_cdc_ecm_rx_running = true;
    usbh_cdc_ecm_run(cdc_ecm_class);
    return ret;
}

static int usbh_cdc_ecm_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret = 0;

    struct usbh_cdc_ecm *cdc_ecm_class = (struct usbh_cdc_ecm *)hport->config.intf[intf].priv;

    if (cdc_ecm_class) {
        g_cdc_ecm_rx_stop = true;
        if (cdc_ecm_class->bulkin) {
            usbh_kill_urb(&cdc_ecm_class->bulkin_urb);
        }

        if (cdc_ecm_class->bulkout) {
            usbh_kill_urb(&cdc_ecm_class->bulkout_urb);
        }

        if (cdc_ecm_class->intin) {
            usbh_kill_urb(&cdc_ecm_class->intin_urb);
        }

        if (hport->config.intf[intf].devname[0] != '\0') {
            usb_osal_thread_schedule_other();
            /* RTNode: the receive thread may still be handing a frame to the
             * network interface that stop() destroys. It ends within one
             * link poll once its transfers are killed; this is the ceiling. */
            if (g_cdc_ecm_rx_running &&
                usb_osal_sem_take(g_cdc_ecm_rx_exit, 4 * CONFIG_USBHOST_CDC_ECM_LINK_POLL_MS) < 0) {
                USB_LOG_ERR("CDC ECM rx thread did not stop\r\n");
            }
            USB_LOG_INFO("Unregister CDC ECM Class:%s\r\n", hport->config.intf[intf].devname);
            usbh_cdc_ecm_stop(cdc_ecm_class);
        }

        memset(cdc_ecm_class, 0, sizeof(struct usbh_cdc_ecm));
    }

    return ret;
}

static void usbh_cdc_ecm_rx_complete(void *arg, int nbytes)
{
    (void)arg;
    (void)nbytes;
    usb_osal_sem_give(g_cdc_ecm_rx_done);
}

static int usbh_cdc_ecm_rx_submit(void)
{
    usbh_bulk_urb_fill(&g_cdc_ecm_class->bulkin_urb, g_cdc_ecm_class->hport, g_cdc_ecm_class->bulkin, g_cdc_ecm_rx_buffer, CONFIG_USBHOST_CDC_ECM_ETH_MAX_SIZE, 0, usbh_cdc_ecm_rx_complete, NULL);
    return usbh_submit_urb(&g_cdc_ecm_class->bulkin_urb);
}

/* RTNode: upstream read the adapter's link reports only until the first
 * link-up, then waited for frames forever, so a pulled cable went unnoticed
 * and the node kept a dead wired address. Here the receive transfer runs on
 * its own (it completes into rx_done) while this thread asks about the link
 * every LINK_POLL_MS, for as long as the adapter is attached, and tells the
 * network glue each time the link changes. */
void usbh_cdc_ecm_rx_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)
{
    uint32_t g_cdc_ecm_rx_length;
    uint32_t last_poll_ms = 0;
    uint32_t rx_errors_in_row = 0;
    bool rx_waiting = false;
    bool reported_link = false;
    int ret;

    (void)CONFIG_USB_OSAL_THREAD_GET_ARGV;
    USB_LOG_INFO("Create cdc ecm rx thread\r\n");

    usb_osal_sem_reset(g_cdc_ecm_rx_done);
    usb_osal_sem_reset(g_cdc_ecm_rx_exit);
    g_cdc_ecm_class->connect_status = false;
    if (usbh_find_class_instance("/dev/cdc_ether") == NULL) {
        goto delete;
    }

    ret = usbh_cdc_ecm_rx_submit();
    if (ret < 0) {
        USB_LOG_ERR("CDC ECM receive did not start: %d\r\n", ret);
        goto delete;
    }

    while (!g_cdc_ecm_rx_stop) {
        if (usb_osal_sem_take(g_cdc_ecm_rx_done, CONFIG_USBHOST_CDC_ECM_LINK_POLL_MS) == 0) {
            ret = g_cdc_ecm_class->bulkin_urb.errorcode;
            if (g_cdc_ecm_rx_stop || ret == -USB_ERR_SHUTDOWN || ret == -USB_ERR_NOTCONN) {
                goto delete; /* disconnect killed it */
            }
            if (ret < 0) {
                usbh_cdc_ecm_eth_rx_error(ret); /* a frame lost on the wire to the adapter */
                /* Twice in a row: try again at the next link poll, so an
                 * adapter that keeps failing can't spin this thread. */
                rx_waiting = ++rx_errors_in_row > 1;
            } else {
                rx_errors_in_row = 0;
                g_cdc_ecm_rx_length = g_cdc_ecm_class->bulkin_urb.actual_length;

                /* A transfer is complete because last packet is a short packet.
                 * Short packet is not zero, match g_cdc_ecm_rx_length % USB_GET_MAXPACKETSIZE(g_cdc_ecm_class->bulkin->wMaxPacketSize).
                 * Short packet is zero, check if g_cdc_ecm_class->bulkin_urb.actual_length < transfer_size, for example transfer is complete with size is 512 < 1514.
                 * This case is always true
                */
                if (g_cdc_ecm_rx_length % USB_GET_MAXPACKETSIZE(g_cdc_ecm_class->bulkin->wMaxPacketSize) ||
                    (g_cdc_ecm_class->bulkin_urb.actual_length < CONFIG_USBHOST_CDC_ECM_ETH_MAX_SIZE)) {
                    USB_LOG_DBG("rxlen:%d\r\n", g_cdc_ecm_rx_length);

                    usbh_cdc_ecm_eth_input(g_cdc_ecm_rx_buffer, g_cdc_ecm_rx_length);
                } else {
                    /* There's no way to run here. */
                }
            }
            if (!rx_waiting) {
                ret = usbh_cdc_ecm_rx_submit();
                if (ret < 0) {
                    USB_LOG_ERR("CDC ECM receive stopped: %d\r\n", ret);
                    goto delete;
                }
            }
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (!g_cdc_ecm_rx_stop && now_ms - last_poll_ms >= CONFIG_USBHOST_CDC_ECM_LINK_POLL_MS) {
            last_poll_ms = now_ms;
            ret = usbh_cdc_ecm_get_connect_status(g_cdc_ecm_class);
            if (g_cdc_ecm_rx_stop || ret == -USB_ERR_SHUTDOWN || ret == -USB_ERR_NOTCONN) {
                goto delete;
            }
            if (g_cdc_ecm_class->connect_status != reported_link) {
                reported_link = g_cdc_ecm_class->connect_status;
                usbh_cdc_ecm_link_changed(g_cdc_ecm_class);
            }
            if (rx_waiting) {
                rx_waiting = false;
                ret = usbh_cdc_ecm_rx_submit();
                if (ret < 0) {
                    USB_LOG_ERR("CDC ECM receive stopped: %d\r\n", ret);
                    goto delete;
                }
            }
        }
    }
    // clang-format off
delete:
    USB_LOG_INFO("Delete cdc ecm rx thread\r\n");
    if (reported_link && !g_cdc_ecm_rx_stop) {
        /* RTNode: the adapter is still attached but nothing reads it now:
         * take the wired interface down rather than leave a dead address. */
        g_cdc_ecm_class->connect_status = false;
        usbh_cdc_ecm_link_changed(g_cdc_ecm_class);
    }
    g_cdc_ecm_class->connect_status = false;
    g_cdc_ecm_rx_running = false;
    usb_osal_sem_give(g_cdc_ecm_rx_exit);
    usb_osal_thread_delete(NULL);
    // clang-format on
}

void usbh_cdc_ecm_rx_not_started(void)
{
    g_cdc_ecm_rx_running = false;
}

uint8_t *usbh_cdc_ecm_get_eth_txbuf(void)
{
    return g_cdc_ecm_tx_buffer;
}

int usbh_cdc_ecm_eth_output(uint32_t buflen)
{
    if (g_cdc_ecm_class->connect_status == false) {
        return -USB_ERR_NOTCONN;
    }

    USB_LOG_DBG("txlen:%d\r\n", buflen);

    /* RTNode: a ceiling (usb_config.h); the caller drops and counts on error. */
    usbh_bulk_urb_fill(&g_cdc_ecm_class->bulkout_urb, g_cdc_ecm_class->hport, g_cdc_ecm_class->bulkout, g_cdc_ecm_tx_buffer, buflen, CONFIG_USBHOST_CDC_ECM_TX_TIMEOUT_MS, NULL, NULL);
    return usbh_submit_urb(&g_cdc_ecm_class->bulkout_urb);
}

const struct usbh_class_driver cdc_ecm_class_driver = {
    .driver_name = "cdc_ecm",
    .connect = usbh_cdc_ecm_connect,
    .disconnect = usbh_cdc_ecm_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info cdc_ecm_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .bInterfaceClass = USB_DEVICE_CLASS_CDC,
    .bInterfaceSubClass = CDC_ETHERNET_NETWORKING_CONTROL_MODEL,
    .bInterfaceProtocol = CDC_COMMON_PROTOCOL_NONE,
    .id_table = NULL,
    .class_driver = &cdc_ecm_class_driver
};