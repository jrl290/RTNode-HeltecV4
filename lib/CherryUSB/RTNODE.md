# CherryUSB in RTNode

This folder holds part of [CherryUSB](https://github.com/cherry-embedded/CherryUSB)
v1.6.1 (upstream commit `c9625ffa773ad10b8824d1b5361bca2ccc1f3d1e`, 2026-04-28),
the USB host behind wired Ethernet through a USB adapter on the Heltec V4 and
V4 R8 (`EthernetUsb.h`, `USB_ETHERNET_PLAN.md` at the repository root).

CherryUSB is licensed under the Apache License 2.0, kept in [LICENSE](LICENSE).
Its files keep their copyright notices:

- sakumisu: the host core, the hub and CDC-ECM class drivers, the DWC2 host
  controller driver and the FreeRTOS layer;
- Espressif Systems (Shanghai) CO LTD: `osal/idf/usb_config.h` and
  `port/dwc2/usb_glue_esp.c`;
- Loogg: `platform/idf/usbh_net.c`.

Each file RTNode changed says so in its header. The changes are listed
below. `rtnode/` is RTNode's own code, under RTNode's licence (GNU GPL v3,
`LICENSE` at the repository root), and so is `library.json`.

## What is included

Only the host side, and only what one CDC-ECM adapter needs, on or behind
hubs: `core/usbh_core.c`, `class/hub/usbh_hub.c`, `class/cdc/usbh_cdc_ecm.c`,
`port/dwc2/usb_hc_dwc2.c`, `port/dwc2/usb_glue_esp.c`,
`osal/idf/usb_osal_idf.c` and `platform/idf/usbh_net.c`, with the headers they
use. The device side, the other classes and the other ports are left out.

The V4 environments list the library in `platformio.ini`. The root
`build_src_filter` keeps every other environment from compiling it.

## RTNode's changes

The aims are the ones `USB_ETHERNET_PLAN.md` sets out:

- run on Arduino-ESP32 2.0.17, which is ESP-IDF 4.4;
- take no memory with Ethernet off;
- never hold up WiFi or the TCP/IP thread;
- count every frame dropped.

### `osal/idf/usb_config.h`

- Buffers the controller reads or writes by DMA come from internal
  DMA-capable memory (`USB_DMA_CALLOC`). State only the processor touches
  prefers PSRAM (`USB_CPU_CALLOC`).
- The library's log goes to RTNode's queue (`CONFIG_USB_PRINTF`,
  `CONFIG_USB_LOG_LINE`), without colour codes.
- Up to four hubs (upstream: one), so an adapter behind a powered hub works.
- The host's tasks get priority 4 in CherryUSB's terms. That puts them below
  WiFi, the system timer and Bluetooth, and above TCP/IP.
- Sending a frame has a 20 ms ceiling (upstream: none). The cable's state is
  polled every 128 ms, and each poll waits at most 50 ms.

### `osal/idf/usb_osal_idf.c`

- Defines the system-timer task's core for ESP-IDF 4.4, which lacks the
  setting.

### `common/usb_log.h`

- With `CONFIG_USB_LOG_LINE` defined, each log call hands over one whole
  line, so lines from different tasks can't interleave.

### `core/usbh_core.c`, `core/usbh_core.h`

- The bus table, the control-transfer buffer and the setup packets were
  static arrays. `usbh_initialize()` now allocates them, keeps them, and
  returns `-USB_ERR_NOMEM` when it can't.
- The hub structure gains `rescan_ports` (see the hub driver below).

### `class/hub/usbh_hub.c`

- The hub table is allocated at the first hub instead of being static.
- Upstream stops polling a hub after one failed interrupt transfer. A data
  toggle error, for example, means a device plugged in afterwards is never
  seen. RTNode counts the error (`usbh_hub_int_errors`), reads every port
  instead, and polling starts again.
- A port with a device connected but not enabled is read again.
- If the controller can't get its memory, the hub thread logs it and ends.

### `class/cdc/usbh_cdc_ecm.c`, `class/cdc/usbh_cdc_ecm.h`

- The class state and the frame buffers are allocated at the first adapter
  instead of being static. One adapter at a time; a second one is refused
  and logged.
- The receive thread waits for each transfer with a semaphore and checks
  the cable at the poll interval. It tells the network glue when the cable
  comes or goes (`usbh_cdc_ecm_link_changed()`). Upstream reads the cable
  once and waits forever.
- Receive errors are counted. After two in a row, the thread waits for the
  next poll instead of resubmitting at once.
- On unplug, disconnect stops the receive thread and waits for it, at most
  four poll intervals, before the network interface goes.
- The cable-status reply is used only if it is a whole CDC notification.

### `platform/idf/usbh_net.c`

- Received frames are copied into PSRAM when the board has it.
- Every frame sent, received or dropped is counted, with the last error, for
  RTNode's status. Drops are logged at most every 10 seconds.
- When the cable comes or goes, the interface starts or ends DHCP. The
  address and its netmask and gateway are recorded.
- The network interface is made once, with RTNode's DHCP host name, and kept
  across unplugging. It is never destroyed, because mDNS keeps its handle.
- The interface key is `ETH_DEF`, the slot ESP-IDF 4.4's mDNS uses for
  Ethernet.

### `port/dwc2/usb_hc_dwc2.c`

- The controller state is allocated by `usb_hc_init()` instead of being
  static.
- A channel's semaphore is reset when the channel is taken. A give left over
  from the channel's last transfer could otherwise end the next transfer
  early.

### `port/dwc2/usb_glue_esp.c`

- Cut down to the ESP32-S3's host side on ESP-IDF 4.4: one controller and
  its internal PHY. The device-mode code, the other chips and the cache
  maintenance are gone.

## RTNode's own files (`rtnode/`)

- `eth_usb.c` covers four things:
  - starting the host;
  - the class driver table;
  - choosing the adapter configuration that offers ECM. Adapters keep it in
    their second or third configuration, after their own protocol.
  - the library's log queue, and the log on UART0 while USB serial is gone.
- `eth_usb_state.h` is the state the network glue records for RTNode's
  status.
