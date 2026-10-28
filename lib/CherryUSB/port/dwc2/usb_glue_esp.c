/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Changed for RTNode, 2026: RTNODE.md at the library's root lists the changes.
 */
/*
 * RTNode: host-only, ESP32-S3 only, cut down from CherryUSB
 * v1.6.1's port/dwc2/usb_glue_esp.c for Arduino-ESP32 2.0.17 (ESP-IDF 4.4),
 * whose usb_phy_config_t has gpio_conf instead of ext_io_conf/otg_io_conf
 * and whose soc_caps.h has no SOC_USB_OTG_PERIPH_NUM. The device-mode
 * functions are gone: RTNode never runs the OTG controller as a device.
 */
#include "sdkconfig.h"
#include "esp_intr_alloc.h"
#include "esp_private/usb_phy.h"
#include "soc/periph_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usbh_core.h"
#include "usb_dwc2_param.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "this glue is for the ESP32-S3 only"
#endif

#define DEFAULT_CPU_FREQ_MHZ CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ

uint32_t SystemCoreClock = (DEFAULT_CPU_FREQ_MHZ * 1000 * 1000);
static usb_phy_handle_t s_phy_handle = NULL;
static intr_handle_t s_interrupt_handle = NULL;

static const struct dwc2_user_params param_fs = {
    .phy_type = DWC2_PHY_TYPE_PARAM_FS,
    .device_dma_enable = true,
    .device_dma_desc_enable = false,
    .device_rx_fifo_size = (200 - 16 * 7),
    .device_tx_fifo_size = {
        [0] = 16,
        [1] = 16,
        [2] = 16,
        [3] = 16,
        [4] = 16,
        [5] = 16,
        [6] = 16,
    },

    .host_dma_desc_enable = false,
    .host_rx_fifo_size = 80,
    .host_nperio_tx_fifo_size = 60, // 240 byte
    .host_perio_tx_fifo_size = 60,  // 240 byte
};

static void usb_hc_interrupt_cb(void *arg_pv)
{
    extern void USBH_IRQHandler(uint8_t busid);
    uint8_t busid = (uintptr_t)arg_pv;
    USBH_IRQHandler(busid);
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
    // Takes the internal PHY from USB Serial/JTAG and gives it to the OTG
    // controller as a host.
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED, // In Host mode, the speed is determined by the connected device
        .gpio_conf = NULL,
    };

    esp_err_t ret = usb_new_phy(&phy_config, &s_phy_handle);
    if (ret != ESP_OK) {
        USB_LOG_ERR("USB Phy Init Failed!\r\n");
        return;
    }

    uintptr_t arg = bus->busid;
    ret = esp_intr_alloc(ETS_USB_INTR_SOURCE, ESP_INTR_FLAG_LOWMED, usb_hc_interrupt_cb, (void *)(arg), &s_interrupt_handle);
    if (ret != ESP_OK) {
        USB_LOG_ERR("USB Interrupt Init Failed!\r\n");
        return;
    }
    USB_LOG_INFO("cherryusb, version: " CHERRYUSB_VERSION_STR "\r\n");
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
    (void)bus;
    if (s_interrupt_handle) {
        esp_intr_free(s_interrupt_handle);
        s_interrupt_handle = NULL;
    }
    if (s_phy_handle) {
        usb_del_phy(s_phy_handle);
        s_phy_handle = NULL;
    }
}

void dwc2_get_user_params(uint32_t reg_base, struct dwc2_user_params *params)
{
    (void)reg_base;
    memcpy(params, &param_fs, sizeof(struct dwc2_user_params));
}

void usbd_dwc2_delay_ms(uint8_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t usbd_dwc2_get_system_clock(void)
{
    return SystemCoreClock;
}
