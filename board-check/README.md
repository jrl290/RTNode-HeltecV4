# Board check

A small firmware, separate from RTNode, that finds out which LoRa board it is
running on, and the web page that drives it (`docs/board-check/`). The plan
and the reasons for each step are in `../BOARD_CHECK_PLAN.md`.

ESP32-S3 boards only for now.

## Files

| Path | What it is |
|---|---|
| `src/main.cpp` | Talks to the page; runs the probe; scores the boards |
| `src/radio_probe.cpp` | Reads a radio's ID and proves its IRQ line (SX126x, SX127x, SX128x, LR11xx) |
| `src/power.cpp` | Power switch pins, and the AXP2101 power chip |
| `src/screen.cpp` | The status, the X, and the arrow with UP in eight orientations |
| `src/touch.cpp`, `src/buttons.cpp` | Touches and button presses |
| `src/board_library.h` | Generated board list. Don't edit it by hand |
| `tools/gen_board_library.py` | Makes the board list from Meshtastic's board definitions |
| `tools/update_page.py` | Copies a build into `docs/board-check/firmware/` and puts its SHA-256s in the page |
| `tools/bench_check.py` | Drives the image over a serial port like the page does, for bench runs |

## Build

```bash
~/.platformio/penv/bin/pio run -d board-check
python3 board-check/tools/update_page.py
```

## Refresh the board list

Check out Meshtastic's firmware somewhere of its own (only `variants` and
`boards` are needed), then:

```bash
python3 -I board-check/tools/gen_board_library.py --meshtastic <checkout> --chip esp32s3 \
  --framework ~/.platformio/packages/framework-arduinoespressif32 \
  --pio-boards ~/.platformio/platforms/espressif32/boards \
  --header board-check/src/board_library.h --json docs/board-check/boards.json \
  --log board-check/tools/board_library.log
```

The log lists every definition that couldn't be read, rather than guessing.

## How the page and the image talk

JSON, one object per line, 115200 baud, over the chip's own USB port or UART0.

| Page sends | Board answers |
|---|---|
| `{"cmd":"hello"}` | `{"event":"hello","fw":...,"probed":false}` |
| `{"cmd":"probe","psram":"2MB","flash_mb":16,"usb":"native"}` | `step` lines, then `buttons`, then `found` |
| `{"cmd":"wireless"}` | `{"event":"wireless","wifi_networks":7,...,"ble_devices":26}` |
| `{"cmd":"show","what":"x"\|"arrow"\|"status"}` | `orientation` after `arrow` |
| `{"cmd":"rotate"}` | `orientation` |
| `{"cmd":"dump"}` | the monochrome screen's picture, for bench checks |

Unprompted: `boot` at start-up, `button` and `touch` whenever they happen.
While the arrow shows, any press or touch turns it.

## Writing it to a board

The page writes the bootloader, partition table, `boot_app0` and the image
separately, so the board's saved settings (0x9000 to 0xe000) are left alone.
Putting RTNode back needs a First install, as the partition table changes.

## Bench

```bash
python3 bench_check.py /dev/cu.usbmodemXXXX --psram 2MB --flash-mb 16 --usb native --dumps out.json
```

Results on 2026-11-03: the V4.2 was named Heltec V4, the staging R8 was named
Heltec V4 R8, the only match each time, in about 5 s. On both, the radio's ID
read back, its oscillator ran, and its IRQ line rose in 15 ms. The OLED was
found and drawn on.
