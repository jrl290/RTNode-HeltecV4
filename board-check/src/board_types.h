// board_types.h — the shape of a board library entry, as
// tools/gen_board_library.py writes it into board_library.h.
#pragma once
#include <stdint.h>

enum RadioType : uint8_t { RADIO_NONE, RADIO_SX126X, RADIO_SX127X, RADIO_SX128X, RADIO_LR11XX, RADIO_LR2021 };
enum PsramKind : uint8_t { PSRAM_ANY, PSRAM_NONE, PSRAM_QUAD, PSRAM_OCTAL };
enum UsbKind : uint8_t { USB_ANY, USB_NATIVE, USB_BRIDGE };
enum PmuType : uint8_t { PMU_NONE, PMU_AXP192, PMU_AXP2101 };
enum ScreenType : uint8_t {
  SCREEN_NONE, SCREEN_OLED_MAYBE, SCREEN_SSD1306, SCREEN_SH1106, SCREEN_SH1107,
  SCREEN_ST7789, SCREEN_ST7735, SCREEN_EINK, SCREEN_TFT_OTHER
};

struct RadioDef {
  uint8_t type;
  int8_t cs, rst, irq, busy;
  uint8_t tcxo_dv;       // TCXO voltage on DIO3 in tenths of a volt; 0 = crystal
  bool rf_switch;        // DIO2 drives the antenna switch
  int8_t txen, rxen;
  int8_t sck, miso, mosi;
};

struct PowerPin {
  int8_t pin;
  uint8_t on;            // level that switches the supply on
};

struct BoardDef {
  const char *id;
  const char *name;
  uint8_t psram;
  uint8_t flash_mb;
  uint8_t usb;
  RadioDef radio[3];     // several when the board's firmware tries each type
  PowerPin power[4];
  uint8_t pmu;
  uint8_t pmu_bus;       // 0: i2c[0..1], 1: i2c[2..3]
  int8_t i2c[4];         // sda, scl, sda1, scl1
  uint8_t screen;
  int8_t tft[8];         // sck, mosi, miso, cs, dc, rst, backlight, power; OLEDs use only rst
  uint8_t tft_bl_on, tft_power_on;
  uint16_t tft_w, tft_h;
  bool touch;
  int8_t touch_int, touch_rst;
  uint8_t touch_bus, touch_addr;
  int8_t buttons[6];
  int8_t led;
  uint8_t led_on;
  int8_t battery;
  uint32_t adc_mult_milli;
  int8_t adc_ctrl;
  uint8_t adc_ctrl_on;
  int8_t other_pins[24]; // GPS, SD card, amplifier and other parts: never read as buttons
};

enum TftPin { TFT_SCK, TFT_MOSI, TFT_MISO, TFT_CS, TFT_DC, TFT_RST, TFT_BL, TFT_POWER };
