/*
 * Copyright (c) 2025, Loogg
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Changed for RTNode, 2026: RTNODE.md at the library's root lists the changes.
 */
#include <stdlib.h>
#include <inttypes.h>
#include "lwip/netif.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "usbh_core.h"

#if TCPIP_THREAD_STACKSIZE < 1024
#error TCPIP_THREAD_STACKSIZE must be >= 1024
#endif

// #define CONFIG_USBHOST_PLATFORM_CDC_ECM
// #define CONFIG_USBHOST_PLATFORM_CDC_RNDIS
// #define CONFIG_USBHOST_PLATFORM_CDC_NCM
// #define CONFIG_USBHOST_PLATFORM_ASIX
// #define CONFIG_USBHOST_PLATFORM_RTL8152

struct usbh_net_netif_glue {
    esp_netif_driver_base_t base;
    esp_event_handler_instance_t ins_got_ip;
    void *usbh_class;
    esp_err_t (*transmit)(void *h, void *buffer, size_t len);
};

/* RTNode: returns false if the frame was dropped (no memory for its copy). */
static bool usbh_net_input_common(struct usbh_net_netif_glue *netif_glue, uint8_t *buf, uint32_t len)
{
    uint8_t *input_buf = buf;

#if !LWIP_TCPIP_CORE_LOCKING_INPUT
    /* RTNode: lwIP keeps the frame until it is done with it, so it gets a
     * copy. In PSRAM when the board has it, as this SDK's WiFi does with
     * lwIP's buffers (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP): internal RAM is
     * for WiFi, Bluetooth and DMA. Upstream logged each failure; the caller
     * counts them instead. */
    input_buf = heap_caps_malloc_prefer(len, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (input_buf == NULL) {
        return false;
    }
    usb_memcpy(input_buf, buf, len);
#endif

    esp_netif_receive(netif_glue->base.netif, input_buf, len, NULL);
    return true;
}

static void usbh_net_free(void *h, void *buffer)
{
#if !LWIP_TCPIP_CORE_LOCKING_INPUT
    usb_osal_free(buffer);
#endif
}

static esp_err_t usbh_net_post_attach(esp_netif_t *esp_netif, void *args)
{
    struct usbh_net_netif_glue *netif_glue = (struct usbh_net_netif_glue *)args;

    netif_glue->base.netif = esp_netif;

    // set driver related config to esp-netif
    esp_netif_driver_ifconfig_t driver_ifconfig = {
        .handle = netif_glue,
        .transmit = netif_glue->transmit,
        .driver_free_rx_buffer = usbh_net_free,
    };

    ESP_ERROR_CHECK(esp_netif_set_driver_config(esp_netif, &driver_ifconfig));

    return ESP_OK;
}

/* RTNode: records the address for RTNode's status (the ECM section below). */
static void usbh_net_got_ip(struct usbh_net_netif_glue *netif_glue, const esp_netif_ip_info_t *ip_info);

static void usbh_net_action_got_ip(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    struct usbh_net_netif_glue *netif_glue = (struct usbh_net_netif_glue *)handler_args;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;
    if (event->esp_netif != netif_glue->base.netif) {
        return; /* RTNode: another Ethernet interface's address */
    }
    USB_LOG_INFO("NETIF %s Got IP Address\r\n", esp_netif_get_ifkey(netif_glue->base.netif));
    USB_LOG_INFO("IP:" IPSTR "\r\n", IP2STR(&ip_info->ip));
    USB_LOG_INFO("MASK:" IPSTR "\r\n", IP2STR(&ip_info->netmask));
    USB_LOG_INFO("GW:" IPSTR "\r\n\r\n", IP2STR(&ip_info->gw));
    usbh_net_got_ip(netif_glue, ip_info);
}

static esp_err_t usbh_net_netif_glue_init_common(struct usbh_net_netif_glue *netif_glue, void *usbh_class, esp_err_t (*transmit)(void *h, void *buffer, size_t len))
{
    netif_glue->usbh_class = usbh_class;
    netif_glue->transmit = transmit;
    netif_glue->base.post_attach = usbh_net_post_attach;

    return esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, usbh_net_action_got_ip, netif_glue, &netif_glue->ins_got_ip);
}

/* RTNode: unused by ECM, whose interface is kept (usbh_cdc_ecm_netif()). */
__attribute__((unused)) static esp_err_t usbh_net_netif_glue_deinit_common(struct usbh_net_netif_glue *netif_glue)
{
    return esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, netif_glue->ins_got_ip);
}

#ifdef CONFIG_USBHOST_PLATFORM_CDC_ECM
#include "usbh_cdc_ecm.h"

#include "rtnode/eth_usb_state.h"

struct usbh_net_netif_glue g_cdc_ecm_netif_glue;

/* RTNode: the wired interface's state and counters, for RTNode's status. */
struct eth_usb_state *g_eth_usb_state;

/* RTNode: every dropped frame is counted; the log says so at most every 10 s,
 * so a stuck adapter can't flood it. */
static uint32_t ecm_drops_logged_ms;

static void ecm_log_drops(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (ecm_drops_logged_ms && now - ecm_drops_logged_ms < 10000) {
        return;
    }
    ecm_drops_logged_ms = now ? now : 1;
    USB_LOG_WRN("Wired frames dropped so far: %u sending (last error %d), %u receiving (last error %d)\r\n",
                (unsigned)g_eth_usb_state->tx_dropped, (int)g_eth_usb_state->tx_last_error,
                (unsigned)g_eth_usb_state->rx_dropped, (int)g_eth_usb_state->rx_last_error);
}

static esp_err_t usbh_cdc_ecm_transmit(void *h, void *buffer, size_t len)
{
    int ret;
    struct usbh_cdc_ecm *cdc_ecm_class = (struct usbh_cdc_ecm *)((struct usbh_net_netif_glue *)h)->usbh_class;
    uint8_t *txbuf = usbh_cdc_ecm_get_eth_txbuf();
    uint16_t mps = cdc_ecm_class->bulkout ? USB_GET_MAXPACKETSIZE(cdc_ecm_class->bulkout->wMaxPacketSize) : 0;

    usb_memcpy(txbuf, buffer, len);
    /* RTNode: ECM ends a frame on a short packet, so a frame that
     * fills its last packet exactly would run into the next one. Nothing here
     * sends a zero-length packet, so add one byte, as Linux's usbnet does for
     * ECM; the receiver goes by the IP length and ignores it. The largest
     * such frame, 1472 bytes at 64-byte packets, still fits the buffer. */
    if (mps && (len % mps) == 0) {
        txbuf[len++] = 0;
        g_eth_usb_state->tx_padded++;
    }
    ret = usbh_cdc_ecm_eth_output(len);
    if (ret < 0) {
        /* RTNode: dropped (the adapter didn't take it within the ceiling, or
         * the link is down); TCP resends as it does for any lost frame. */
        g_eth_usb_state->tx_dropped++;
        g_eth_usb_state->tx_last_error = ret;
        ecm_log_drops();
        return ESP_FAIL;
    }
    g_eth_usb_state->tx_frames++;
    g_eth_usb_state->tx_bytes += len;

    return ESP_OK;
}

void usbh_cdc_ecm_eth_input(uint8_t *buf, uint32_t buflen)
{
    if (!usbh_net_input_common(&g_cdc_ecm_netif_glue, buf, buflen)) {
        g_eth_usb_state->rx_dropped++;
        g_eth_usb_state->rx_last_error = -USB_ERR_NOMEM;
        ecm_log_drops();
        return;
    }
    g_eth_usb_state->rx_frames++;
    g_eth_usb_state->rx_bytes += buflen;
}

static void usbh_net_got_ip(struct usbh_net_netif_glue *netif_glue, const esp_netif_ip_info_t *ip_info)
{
    if (netif_glue != &g_cdc_ecm_netif_glue) {
        return;
    }
    g_eth_usb_state->netmask = ip_info->netmask.addr;
    g_eth_usb_state->gw = ip_info->gw.addr;
    g_eth_usb_state->ipv4 = ip_info->ip.addr;
}

void usbh_cdc_ecm_eth_rx_error(int error)
{
    g_eth_usb_state->rx_dropped++;
    g_eth_usb_state->rx_last_error = error;
    ecm_log_drops();
}

/* RTNode: from the receive thread when the adapter reports its cable in or
 * out. The interface comes up only on a cable-in report (upstream raised it
 * as soon as the adapter enumerated), and each cable-in asks DHCP afresh,
 * as IDF's own Ethernet glue does. */
void usbh_cdc_ecm_link_changed(struct usbh_cdc_ecm *cdc_ecm_class)
{
    esp_netif_t *esp_netif = g_cdc_ecm_netif_glue.base.netif;

    g_eth_usb_state->link_changes++;
    if (cdc_ecm_class->connect_status) {
        USB_LOG_INFO("Wired link up, asking for an address\r\n");
        g_eth_usb_state->link = true;
        esp_netif_action_connected(esp_netif, NULL, 0, NULL);
    } else {
        USB_LOG_INFO("Wired link down\r\n");
        g_eth_usb_state->link = false;
        g_eth_usb_state->ipv4 = 0;
        g_eth_usb_state->netmask = 0;
        g_eth_usb_state->gw = 0;
        esp_netif_action_disconnected(esp_netif, NULL, 0, NULL);
    }
}

/* RTNode: the network interface is made for the first adapter and kept for
 * every later one (upstream made and destroyed one per attach). IDF 4.4's
 * mDNS keeps the handle of its Ethernet interface, so a destroyed one would
 * leave it pointing at freed memory after a replug; keeping it also spares
 * lwIP an interface torn down under it. A later adapter gets the same
 * interface with its own MAC. */
static esp_netif_t *usbh_cdc_ecm_netif(struct usbh_cdc_ecm *cdc_ecm_class)
{
    if (g_cdc_ecm_netif_glue.base.netif) {
        return g_cdc_ecm_netif_glue.base.netif;
    }

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    /* RTNode: "ETH_DEF" is the key IDF 4.4's mDNS looks up for its one
     * Ethernet slot (it has no way to add another interface), so mDNS can
     * answer on the cable. Route priority above WiFi station's 100: with
     * both up, connections take the cable. */
    base_cfg.if_key = "ETH_DEF";
    base_cfg.if_desc = "usb ecm";
    base_cfg.route_prio = 128;

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_cfg.base = &base_cfg;
    esp_netif_t *esp_netif = esp_netif_new(&netif_cfg);
    if (!esp_netif) {
        return NULL;
    }
    if (g_eth_usb_state->hostname[0]) {
        esp_netif_set_hostname(esp_netif, g_eth_usb_state->hostname);
    }

    usbh_net_netif_glue_init_common(&g_cdc_ecm_netif_glue, cdc_ecm_class, usbh_cdc_ecm_transmit);
    esp_netif_attach(esp_netif, &g_cdc_ecm_netif_glue.base); /* sets base.netif */
    return esp_netif;
}

void usbh_cdc_ecm_run(struct usbh_cdc_ecm *cdc_ecm_class)
{
    esp_netif_t *esp_netif = usbh_cdc_ecm_netif(cdc_ecm_class);

    if (!esp_netif) {
        USB_LOG_ERR("No memory for the wired network interface\r\n");
        usbh_cdc_ecm_rx_not_started();
        return;
    }
    g_cdc_ecm_netif_glue.usbh_class = cdc_ecm_class;

    esp_netif_set_mac(esp_netif, cdc_ecm_class->mac);

    /* RTNode: started, but up only when the adapter reports its cable in
     * (usbh_cdc_ecm_link_changed()). */
    esp_netif_action_start(esp_netif, NULL, 0, NULL);

    g_eth_usb_state->vid = cdc_ecm_class->hport->device_desc.idVendor;
    g_eth_usb_state->pid = cdc_ecm_class->hport->device_desc.idProduct;
    for (int i = 0; i < 6; i++) {
        g_eth_usb_state->mac[i] = cdc_ecm_class->mac[i];
    }
    g_eth_usb_state->link = false;
    g_eth_usb_state->adapter = true;

    /* RTNode: 3 KB, not upstream's 2: the thread now also follows the link,
     * which calls into esp_netif and the log. */
    if (!usb_osal_thread_create("usbh_cdc_ecm_rx", 3072, CONFIG_USBHOST_PSC_PRIO + 1, usbh_cdc_ecm_rx_thread, NULL)) {
        USB_LOG_ERR("No memory for the CDC ECM rx thread\r\n");
        usbh_cdc_ecm_rx_not_started();
    }
}

void usbh_cdc_ecm_stop(struct usbh_cdc_ecm *cdc_ecm_class)
{
    (void)cdc_ecm_class;

    g_eth_usb_state->adapter = false;
    g_eth_usb_state->link = false;
    g_eth_usb_state->ipv4 = 0;
    g_eth_usb_state->netmask = 0;
    g_eth_usb_state->gw = 0;
    /* RTNode: stopped, not destroyed (usbh_cdc_ecm_netif()). */
    if (g_cdc_ecm_netif_glue.base.netif) {
        esp_netif_action_stop(g_cdc_ecm_netif_glue.base.netif, NULL, 0, NULL);
    }
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_CDC_RNDIS
#include "usbh_rndis.h"

struct usb_osal_timer *timer_handle;

static void rndis_dev_keepalive_timeout(void *arg)
{
    struct usbh_rndis *rndis_class = (struct usbh_rndis *)arg;
    usbh_rndis_keepalive(rndis_class);
}

void timer_init(struct usbh_rndis *rndis_class)
{
    timer_handle = usb_osal_timer_create("rndis_keepalive", 5000, rndis_dev_keepalive_timeout, rndis_class, true);
    if (NULL != timer_handle) {
        usb_osal_timer_start(timer_handle);
    } else {
        USB_LOG_ERR("timer creation failed! \r\n");
        for (;;) {
            ;
        }
    }
}

struct usbh_net_netif_glue g_rndis_netif_glue;

static esp_err_t usbh_rndis_transmit(void *h, void *buffer, size_t len)
{
    int ret;
    (void)h;

    usb_memcpy(usbh_rndis_get_eth_txbuf(), buffer, len);
    ret = usbh_rndis_eth_output(len);
    if (ret < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void usbh_rndis_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_net_input_common(&g_rndis_netif_glue, buf, buflen);
}

void usbh_rndis_run(struct usbh_rndis *rndis_class)
{
    memset(&g_rndis_netif_glue, 0, sizeof(struct usbh_net_netif_glue));

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key = "u2";
    base_cfg.if_desc = "usbh rndis";

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_cfg.base = &base_cfg;
    esp_netif_t *esp_netif = esp_netif_new(&netif_cfg);

    usbh_net_netif_glue_init_common(&g_rndis_netif_glue, rndis_class, usbh_rndis_transmit);
    esp_netif_attach(esp_netif, &g_rndis_netif_glue.base);

    esp_netif_set_mac(esp_netif, rndis_class->mac);

    esp_netif_action_start(esp_netif, NULL, 0, NULL);
    esp_netif_action_connected(esp_netif, NULL, 0, NULL);

    usb_osal_thread_create("usbh_rndis_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rndis_rx_thread, NULL);

    //timer_init(rndis_class);
}

void usbh_rndis_stop(struct usbh_rndis *rndis_class)
{
    (void)rndis_class;

    usbh_net_netif_glue_deinit_common(&g_rndis_netif_glue);
    esp_netif_action_stop(g_rndis_netif_glue.base.netif, NULL, 0, NULL);
    esp_netif_destroy(g_rndis_netif_glue.base.netif);

    // xTimerStop(timer_handle, 0);
    // xTimerDelete(timer_handle, 0);
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_CDC_NCM
#include "usbh_cdc_ncm.h"

struct usbh_net_netif_glue g_cdc_ncm_netif_glue;

static esp_err_t usbh_cdc_ncm_transmit(void *h, void *buffer, size_t len)
{
    int ret;
    (void)h;

    usb_memcpy(usbh_cdc_ncm_get_eth_txbuf(), buffer, len);
    ret = usbh_cdc_ncm_eth_output(len);
    if (ret < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void usbh_cdc_ncm_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_net_input_common(&g_cdc_ncm_netif_glue, buf, buflen);
}

void usbh_cdc_ncm_run(struct usbh_cdc_ncm *cdc_ncm_class)
{
    memset(&g_cdc_ncm_netif_glue, 0, sizeof(struct usbh_net_netif_glue));

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key = "u1";
    base_cfg.if_desc = "usbh cdc ncm";

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_cfg.base = &base_cfg;
    esp_netif_t *esp_netif = esp_netif_new(&netif_cfg);

    usbh_net_netif_glue_init_common(&g_cdc_ncm_netif_glue, cdc_ncm_class, usbh_cdc_ncm_transmit);
    esp_netif_attach(esp_netif, &g_cdc_ncm_netif_glue.base);

    esp_netif_set_mac(esp_netif, cdc_ncm_class->mac);

    esp_netif_action_start(esp_netif, NULL, 0, NULL);
    esp_netif_action_connected(esp_netif, NULL, 0, NULL);

    usb_osal_thread_create("usbh_cdc_ncm_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_cdc_ncm_rx_thread, NULL);
}

void usbh_cdc_ncm_stop(struct usbh_cdc_ncm *cdc_ncm_class)
{
    (void)cdc_ncm_class;

    usbh_net_netif_glue_deinit_common(&g_cdc_ncm_netif_glue);
    esp_netif_action_stop(g_cdc_ncm_netif_glue.base.netif, NULL, 0, NULL);
    esp_netif_destroy(g_cdc_ncm_netif_glue.base.netif);
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_ASIX
#include "usbh_asix.h"

struct usbh_net_netif_glue g_asix_netif_glue;

static esp_err_t usbh_asix_transmit(void *h, void *buffer, size_t len)
{
    int ret;
    (void)h;

    usb_memcpy(usbh_asix_get_eth_txbuf(), buffer, len);
    ret = usbh_asix_eth_output(len);
    if (ret < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void usbh_asix_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_net_input_common(&g_asix_netif_glue, buf, buflen);
}

void usbh_asix_run(struct usbh_asix *asix_class)
{
    memset(&g_asix_netif_glue, 0, sizeof(struct usbh_net_netif_glue));

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key = "u3";
    base_cfg.if_desc = "usbh asix";

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_cfg.base = &base_cfg;
    esp_netif_t *esp_netif = esp_netif_new(&netif_cfg);

    usbh_net_netif_glue_init_common(&g_asix_netif_glue, asix_class, usbh_asix_transmit);
    esp_netif_attach(esp_netif, &g_asix_netif_glue.base);

    esp_netif_set_mac(esp_netif, asix_class->mac);

    esp_netif_action_start(esp_netif, NULL, 0, NULL);
    esp_netif_action_connected(esp_netif, NULL, 0, NULL);

    usb_osal_thread_create("usbh_asix_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_asix_rx_thread, NULL);
}

void usbh_asix_stop(struct usbh_asix *asix_class)
{
    (void)asix_class;

    usbh_net_netif_glue_deinit_common(&g_asix_netif_glue);
    esp_netif_action_stop(g_asix_netif_glue.base.netif, NULL, 0, NULL);
    esp_netif_destroy(g_asix_netif_glue.base.netif);
}
#endif

#ifdef CONFIG_USBHOST_PLATFORM_RTL8152
#include "usbh_rtl8152.h"

struct usbh_net_netif_glue g_rtl8152_netif_glue;

static esp_err_t usbh_rtl8152_transmit(void *h, void *buffer, size_t len)
{
    int ret;
    (void)h;

    usb_memcpy(usbh_rtl8152_get_eth_txbuf(), buffer, len);
    ret = usbh_rtl8152_eth_output(len);
    if (ret < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void usbh_rtl8152_eth_input(uint8_t *buf, uint32_t buflen)
{
    usbh_net_input_common(&g_rtl8152_netif_glue, buf, buflen);
}

void usbh_rtl8152_run(struct usbh_rtl8152 *rtl8152_class)
{
    memset(&g_rtl8152_netif_glue, 0, sizeof(struct usbh_net_netif_glue));

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key = "u4";
    base_cfg.if_desc = "usbh rtl8152";

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    netif_cfg.base = &base_cfg;
    esp_netif_t *esp_netif = esp_netif_new(&netif_cfg);

    usbh_net_netif_glue_init_common(&g_rtl8152_netif_glue, rtl8152_class, usbh_rtl8152_transmit);
    esp_netif_attach(esp_netif, &g_rtl8152_netif_glue.base);

    esp_netif_set_mac(esp_netif, rtl8152_class->mac);

    esp_netif_action_start(esp_netif, NULL, 0, NULL);
    esp_netif_action_connected(esp_netif, NULL, 0, NULL);

    usb_osal_thread_create("usbh_rtl8152_rx", 2048, CONFIG_USBHOST_PSC_PRIO + 1, usbh_rtl8152_rx_thread, NULL);
}

void usbh_rtl8152_stop(struct usbh_rtl8152 *rtl8152_class)
{
    (void)rtl8152_class;

    usbh_net_netif_glue_deinit_common(&g_rtl8152_netif_glue);
    esp_netif_action_stop(g_rtl8152_netif_glue.base.netif, NULL, 0, NULL);
    esp_netif_destroy(g_rtl8152_netif_glue.base.netif);
}
#endif
