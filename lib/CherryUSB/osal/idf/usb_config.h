/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Changed for RTNode, 2026: RTNODE.md at the library's root lists the changes.
 */

#pragma once

#include "sdkconfig.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

/* RTNode: RTNode's V4 builds carry this library whether or not Ethernet is on,
 * so its large buffers and state come from the heap when the host starts,
 * not from static RAM. Memory the USB controller reads and writes itself
 * must be internal; state only the processor touches goes to PSRAM when the
 * board has it. Both are zeroed. */
#define USB_DMA_CALLOC(n, size) heap_caps_calloc((n), (size), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
#define USB_CPU_CALLOC(n, size) heap_caps_calloc_prefer((n), (size), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL)

/* ================ USB common Configuration ================ */

/* RTNode: CherryUSB's log goes into RTNode's log, a whole line per call
 * (rtnode/eth_usb.c, usb_log.h). */
int eth_usb_log_printf(const char *fmt, ...);
int eth_usb_log_line(const char *level, const char *tag, const char *fmt, ...);
#define CONFIG_USB_PRINTF(...) eth_usb_log_printf(__VA_ARGS__)
#define CONFIG_USB_LOG_LINE(level, tag, ...) eth_usb_log_line(level, tag, __VA_ARGS__)

#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

/* Enable print with color */
// #define CONFIG_USB_PRINTF_COLOR_ENABLE

/* data align size when use dma or use dcache */
#ifndef CONFIG_USB_ALIGN_SIZE
#define CONFIG_USB_ALIGN_SIZE 4
#endif

// #define CONFIG_USB_DCACHE_ENABLE

/* attribute data into no cache ram
* DRAM_DMA_ALIGNED_ATTR was introduced in IDF 5.3. If not defined, it falls back to DMA_ATTR
*/
#ifndef DRAM_DMA_ALIGNED_ATTR
#define DRAM_DMA_ALIGNED_ATTR DMA_ATTR
#endif
#define USB_NOCACHE_RAM_SECTION DRAM_DMA_ALIGNED_ATTR

/* use usb_memcpy default for high performance but cost more flash memory.
 * And, arm libc has a bug that memcpy() may cause data misalignment when the size is not a multiple of 4.
*/
// #define CONFIG_USB_MEMCPY_DISABLE

/* ================= USB Device Stack Configuration ================ */

/* Ep0 in and out transfer buffer */
#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

/* Setup packet log for debug */
// #define CONFIG_USBDEV_SETUP_LOG_PRINT

/* Send ep0 in data from user buffer instead of copying into ep0 reqdata
 * Please note that user buffer must be aligned with CONFIG_USB_ALIGN_SIZE
*/
// #define CONFIG_USBDEV_EP0_INDATA_NO_COPY

/* Check if the input descriptor is correct */
// #define CONFIG_USBDEV_DESC_CHECK

/* Enable test mode */
// #define CONFIG_USBDEV_TEST_MODE

/* enable advance desc register api */
#define CONFIG_USBDEV_ADVANCE_DESC

/* move ep0 setup handler from isr to thread */
// #define CONFIG_USBDEV_EP0_THREAD

#ifndef CONFIG_USBDEV_EP0_PRIO
#define CONFIG_USBDEV_EP0_PRIO 4
#endif

#ifndef CONFIG_USBDEV_EP0_STACKSIZE
#define CONFIG_USBDEV_EP0_STACKSIZE 2048
#endif

#ifndef CONFIG_USBDEV_MSC_MAX_LUN
#define CONFIG_USBDEV_MSC_MAX_LUN 1
#endif

#ifndef CONFIG_USBDEV_MSC_MAX_BUFSIZE
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE 512
#endif

#ifndef CONFIG_USBDEV_MSC_MANUFACTURER_STRING
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING ""
#endif

#ifndef CONFIG_USBDEV_MSC_PRODUCT_STRING
#define CONFIG_USBDEV_MSC_PRODUCT_STRING ""
#endif

#ifndef CONFIG_USBDEV_MSC_VERSION_STRING
#define CONFIG_USBDEV_MSC_VERSION_STRING "0.01"
#endif

/* move msc read & write from isr to while(1), you should call usbd_msc_polling in while(1) */
// #define CONFIG_USBDEV_MSC_POLLING

/* move msc read & write from isr to thread */
#define CONFIG_USBDEV_MSC_THREAD

#ifndef CONFIG_USBDEV_MSC_PRIO
#define CONFIG_USBDEV_MSC_PRIO 4
#endif

#ifndef CONFIG_USBDEV_MSC_STACKSIZE
#define CONFIG_USBDEV_MSC_STACKSIZE 2048
#endif

#ifndef CONFIG_USBDEV_MTP_MAX_BUFSIZE
#define CONFIG_USBDEV_MTP_MAX_BUFSIZE 2048
#endif

#ifndef CONFIG_USBDEV_MTP_MAX_OBJECTS
#define CONFIG_USBDEV_MTP_MAX_OBJECTS 256
#endif

#ifndef CONFIG_USBDEV_MTP_MAX_PATHNAME
#define CONFIG_USBDEV_MTP_MAX_PATHNAME 256
#endif

#define CONFIG_USBDEV_MTP_THREAD

#ifndef CONFIG_USBDEV_MTP_PRIO
#define CONFIG_USBDEV_MTP_PRIO 4
#endif

#ifndef CONFIG_USBDEV_MTP_STACKSIZE
#define CONFIG_USBDEV_MTP_STACKSIZE 4096
#endif

#ifndef CONFIG_USBDEV_RNDIS_RESP_BUFFER_SIZE
#define CONFIG_USBDEV_RNDIS_RESP_BUFFER_SIZE 156
#endif

/* rndis transfer buffer size, must be a multiple of (1536 + 44)*/
#ifndef CONFIG_USBDEV_RNDIS_ETH_MAX_FRAME_SIZE
#define CONFIG_USBDEV_RNDIS_ETH_MAX_FRAME_SIZE 1580
#endif

#ifndef CONFIG_USBDEV_RNDIS_VENDOR_ID
#define CONFIG_USBDEV_RNDIS_VENDOR_ID 0x0000ffff
#endif

#ifndef CONFIG_USBDEV_RNDIS_VENDOR_DESC
#define CONFIG_USBDEV_RNDIS_VENDOR_DESC "CherryUSB"
#endif

/* ================ USB HOST Stack Configuration ================== */

#define CONFIG_USBHOST_MAX_RHPORTS          1
#define CONFIG_USBHOST_MAX_EXTHUBS          4 /* adapters with a power port often hold a hub; the M1 showed three above an AX88179A */
#define CONFIG_USBHOST_MAX_EHPORTS          4
#define CONFIG_USBHOST_MAX_INTERFACES       8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#define CONFIG_USBHOST_MAX_ENDPOINTS        4

#define CONFIG_USBHOST_MAX_SERIAL_CLASS  4
#define CONFIG_USBHOST_MAX_HID_CLASS     4
#define CONFIG_USBHOST_MAX_MSC_CLASS     2
#define CONFIG_USBHOST_MAX_AUDIO_CLASS   1
#define CONFIG_USBHOST_MAX_VIDEO_CLASS   1

#define CONFIG_USBHOST_DEV_NAMELEN 16

/* RTNode: the hub thread runs at FreeRTOS priority 24 - this (20) and the
 * adapter's receive thread one below (19): under WiFi (23), the timer task
 * (22) and NimBLE's host (21), above lwIP's tcpip thread (18) that takes the
 * frames. Upstream's 0 put both above WiFi. */
#ifndef CONFIG_USBHOST_PSC_PRIO
#define CONFIG_USBHOST_PSC_PRIO 4
#endif
#ifndef CONFIG_USBHOST_PSC_STACKSIZE
#define CONFIG_USBHOST_PSC_STACKSIZE 4096
#endif

//#define CONFIG_USBHOST_GET_STRING_DESC

// #define CONFIG_USBHOST_MSOS_ENABLE
#ifndef CONFIG_USBHOST_MSOS_VENDOR_CODE
#define CONFIG_USBHOST_MSOS_VENDOR_CODE 0x00
#endif

/* Ep0 max transfer buffer */
#ifndef CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN 2048
#endif

#ifndef CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 500
#endif

#ifndef CONFIG_USBHOST_MSC_TIMEOUT
#define CONFIG_USBHOST_MSC_TIMEOUT 5000
#endif

/* RTNode: the longest a frame may hold lwIP's tcpip thread while the adapter
 * takes it. A frame takes about 3 ms at full speed; past this it is dropped
 * and counted (usbh_net.c), so a stuck adapter can't stall WiFi. */
#ifndef CONFIG_USBHOST_CDC_ECM_TX_TIMEOUT_MS
#define CONFIG_USBHOST_CDC_ECM_TX_TIMEOUT_MS 20
#endif

/* RTNode: how often the adapter is asked whether its cable is in (the ECM
 * interrupt endpoint, read by the receive thread), and how long one ask may
 * take. */
#ifndef CONFIG_USBHOST_CDC_ECM_LINK_POLL_MS
#define CONFIG_USBHOST_CDC_ECM_LINK_POLL_MS 128
#endif
#ifndef CONFIG_USBHOST_CDC_ECM_LINK_TIMEOUT_MS
#define CONFIG_USBHOST_CDC_ECM_LINK_TIMEOUT_MS 50
#endif

#ifndef CONFIG_USBHOST_SERIAL_RX_SIZE
#define CONFIG_USBHOST_SERIAL_RX_SIZE 2048
#endif

/* This parameter affects usb performance, and depends on (TCP_WND)tcp eceive windows size,
 * you can change to 2K ~ 16K and must be larger than TCP RX windows size in order to avoid being overflow.
 */
#ifndef CONFIG_USBHOST_RNDIS_ETH_MAX_RX_SIZE
#define CONFIG_USBHOST_RNDIS_ETH_MAX_RX_SIZE (2048)
#endif

/* Because lwip do not support multi pbuf at a time, so increasing this variable has no performance improvement */
#ifndef CONFIG_USBHOST_RNDIS_ETH_MAX_TX_SIZE
#define CONFIG_USBHOST_RNDIS_ETH_MAX_TX_SIZE (2048)
#endif

/* This parameter affects usb performance, and depends on (TCP_WND)tcp eceive windows size,
 * you can change to 2K ~ 16K and must be larger than TCP RX windows size in order to avoid being overflow.
 */
#ifndef CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE
#define CONFIG_USBHOST_CDC_NCM_ETH_MAX_RX_SIZE (2048)
#endif
/* Because lwip do not support multi pbuf at a time, so increasing this variable has no performance improvement */
#ifndef CONFIG_USBHOST_CDC_NCM_ETH_MAX_TX_SIZE
#define CONFIG_USBHOST_CDC_NCM_ETH_MAX_TX_SIZE (2048)
#endif

/* This parameter affects usb performance, and depends on (TCP_WND)tcp eceive windows size,
 * you can change to 2K ~ 16K and must be larger than TCP RX windows size in order to avoid being overflow.
 */
#ifndef CONFIG_USBHOST_ASIX_ETH_MAX_RX_SIZE
#define CONFIG_USBHOST_ASIX_ETH_MAX_RX_SIZE (2048)
#endif
/* Because lwip do not support multi pbuf at a time, so increasing this variable has no performance improvement */
#ifndef CONFIG_USBHOST_ASIX_ETH_MAX_TX_SIZE
#define CONFIG_USBHOST_ASIX_ETH_MAX_TX_SIZE (2048)
#endif

/* This parameter affects usb performance, and depends on (TCP_WND)tcp eceive windows size,
 * you can change to 2K ~ 16K and must be larger than TCP RX windows size in order to avoid being overflow.
 */
#ifndef CONFIG_USBHOST_RTL8152_ETH_MAX_RX_SIZE
#define CONFIG_USBHOST_RTL8152_ETH_MAX_RX_SIZE (2048)
#endif
/* Because lwip do not support multi pbuf at a time, so increasing this variable has no performance improvement */
#ifndef CONFIG_USBHOST_RTL8152_ETH_MAX_TX_SIZE
#define CONFIG_USBHOST_RTL8152_ETH_MAX_TX_SIZE (2048)
#endif

#define CONFIG_USBHOST_BLUETOOTH_HCI_H4
// #define CONFIG_USBHOST_BLUETOOTH_HCI_LOG

#ifndef CONFIG_USBHOST_BLUETOOTH_TX_SIZE
#define CONFIG_USBHOST_BLUETOOTH_TX_SIZE 2048
#endif
#ifndef CONFIG_USBHOST_BLUETOOTH_RX_SIZE
#define CONFIG_USBHOST_BLUETOOTH_RX_SIZE 2048
#endif

#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
#define ESP_USB_FS0_BASE            0x60080000
#define ESP_USBD_BASE               ESP_USB_FS0_BASE
#define ESP_USBH_BASE               ESP_USB_FS0_BASE
#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBHOST_MAX_BUS 1
#elif CONFIG_IDF_TARGET_ESP32P4
#define ESP_USB_HS0_BASE            0x50000000
#define ESP_USB_FS0_BASE            0x50040000
#define ESP_USBD_BASE               ESP_USB_HS0_BASE
#define ESP_USBH_BASE               ESP_USB_HS0_BASE
#define CONFIG_USBDEV_MAX_BUS 2
#define CONFIG_USBHOST_MAX_BUS 2
#define CONFIG_USB_HS
#else
#error "Unsupported SoC"
#endif

#if CONFIG_IDF_TARGET_ESP32P4
#define CONFIG_USB_DCACHE_ENABLE

#undef CONFIG_USB_ALIGN_SIZE
#define CONFIG_USB_ALIGN_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE

#endif