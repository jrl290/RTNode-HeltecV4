// screen.cpp — see screen.h.
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_ST7735.h>
#include "screen.h"

static Adafruit_GFX *gfx = nullptr;
static Adafruit_SSD1306 *ssd1306 = nullptr;
static Adafruit_SH110X *sh110x = nullptr;
static SPIClass *tft_spi = nullptr;
static bool colour = false;
static uint16_t white = 1, black = 0;

static void show() {
  if (ssd1306) ssd1306->display();
  if (sh110x) sh110x->display();
}

bool screen_begin(const BoardDef &b, uint8_t type, TwoWire *bus, uint8_t addr) {
  int8_t rst = b.tft[TFT_RST];
  switch (type) {
    case SCREEN_SSD1306:
      ssd1306 = new Adafruit_SSD1306(128, 64, bus, rst);
      if (!ssd1306->begin(SSD1306_SWITCHCAPVCC, addr, true, false)) return false;
      gfx = ssd1306;
      break;
    case SCREEN_SH1106: {
      Adafruit_SH1106G *d = new Adafruit_SH1106G(128, 64, bus, rst);
      if (!d->begin(addr, true)) return false;
      sh110x = d;
      gfx = d;
      break;
    }
    case SCREEN_SH1107: {
      Adafruit_SH1107 *d = new Adafruit_SH1107(64, 128, bus, rst);
      if (!d->begin(addr, true)) return false;
      sh110x = d;
      gfx = d;
      break;
    }
    case SCREEN_ST7789:
    case SCREEN_ST7735: {
      const int8_t *t = b.tft;
      if (t[TFT_SCK] < 0 || t[TFT_MOSI] < 0 || t[TFT_CS] < 0 || t[TFT_DC] < 0) return false;
      if (t[TFT_POWER] >= 0) {
        pinMode(t[TFT_POWER], OUTPUT);
        digitalWrite(t[TFT_POWER], b.tft_power_on ? HIGH : LOW);
        delay(20);
      }
      tft_spi = new SPIClass(HSPI);
      tft_spi->begin(t[TFT_SCK], t[TFT_MISO], t[TFT_MOSI], -1);
      uint16_t w = b.tft_w ? b.tft_w : 240, h = b.tft_h ? b.tft_h : 320;
      if (type == SCREEN_ST7789) {
        Adafruit_ST7789 *d = new Adafruit_ST7789(tft_spi, t[TFT_CS], t[TFT_DC], rst);
        d->init(w, h);
        d->setSPISpeed(40000000);
        gfx = d;
      } else {
        Adafruit_ST7735 *d = new Adafruit_ST7735(tft_spi, t[TFT_CS], t[TFT_DC], rst);
        d->initR(w == 80 ? INITR_MINI160x80 : w == 128 && h == 128 ? INITR_144GREENTAB : INITR_BLACKTAB);
        d->setSPISpeed(27000000);
        gfx = d;
      }
      if (t[TFT_BL] >= 0) {
        pinMode(t[TFT_BL], OUTPUT);
        digitalWrite(t[TFT_BL], b.tft_bl_on ? HIGH : LOW);
      }
      colour = true;
      white = 0xFFFF;
      break;
    }
    default:
      return false;
  }
  gfx->setRotation(0);
  return true;
}

bool screen_ready() { return gfx != nullptr; }
bool screen_is_colour() { return colour; }

uint16_t screen_width() {
  if (!gfx) return 0;
  gfx->setRotation(0);
  return gfx->width();
}

uint16_t screen_height() {
  if (!gfx) return 0;
  gfx->setRotation(0);
  return gfx->height();
}

void screen_status(const char *line1, const char *line2) {
  if (!gfx) return;
  gfx->setRotation(0);
  gfx->fillScreen(black);
  uint8_t size = gfx->width() >= 200 ? 2 : 1;
  gfx->setTextSize(size);
  gfx->setTextColor(white);
  gfx->setTextWrap(true);
  gfx->setCursor(2, 2);
  gfx->println("Board check");
  gfx->println();
  if (line1) gfx->println(line1);
  if (line2) gfx->println(line2);
  show();
}

void screen_show_x() {
  if (!gfx) return;
  gfx->setRotation(0);
  gfx->fillScreen(black);
  int16_t s = min(gfx->width(), gfx->height());
  int16_t q = colour ? s / 3 : s / 2;     // the X's size; in the top left corner
  int16_t m = q / 6;
  int16_t thick = max(2, q / 10);
  for (int16_t d = -thick / 2; d <= thick / 2; d++) {
    gfx->drawLine(m + d, m, m + q + d, m + q, white);
    gfx->drawLine(m + q + d, m, m + d, m + q, white);
  }
  show();
}

// The arrow and UP are drawn once into a square picture, then copied to the
// screen turned and, for orientations 4-7, mirrored.
void screen_show_arrow(uint8_t o) {
  if (!gfx) return;
  gfx->setRotation(0);
  int16_t s = min(gfx->width(), gfx->height());
  s = (s * 7 / 8) & ~7;
  GFXcanvas1 pic(s, s);
  pic.fillScreen(0);
  uint8_t k = max(1, s / 28);             // text size: UP is 8k pixels tall
  int16_t text_h = 8 * k;
  int16_t arrow_bottom = s - text_h - s / 12;
  int16_t head_bottom = arrow_bottom * 11 / 20;
  pic.fillTriangle(s / 2, s / 20, s / 2 - s * 3 / 10, head_bottom, s / 2 + s * 3 / 10, head_bottom, 1);
  pic.fillRect(s / 2 - s / 12, head_bottom - 1, s / 6, arrow_bottom - head_bottom, 1);
  pic.setTextSize(k);
  pic.setTextColor(1);
  pic.setCursor(s / 2 - (12 * k - k) / 2, s - text_h - 1);
  pic.print("UP");

  GFXcanvas1 out(s, s);
  for (int16_t y = 0; y < s; y++)
    for (int16_t x = 0; x < s; x++)
      out.drawPixel(x, y, pic.getPixel(o >= 4 ? s - 1 - x : x, y));

  gfx->setRotation(o & 3);
  gfx->fillScreen(black);
  gfx->drawBitmap((gfx->width() - s) / 2, (gfx->height() - s) / 2, out.getBuffer(), s, s, white, black);
  show();
}

size_t screen_dump(const uint8_t **buf) {
  if (ssd1306) { *buf = ssd1306->getBuffer(); return 128 * 64 / 8; }
  if (sh110x) { *buf = sh110x->getBuffer(); return (size_t)sh110x->width() * sh110x->height() / 8; }
  return 0;
}
