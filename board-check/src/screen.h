// screen.h — draws the board check's pictures on whatever screen the board
// has: a short status, an X to press, and an arrow with UP under it in any of
// eight orientations (four quarter turns, then the same four mirrored).
#pragma once
#include <Wire.h>
#include "board_types.h"

// Starts the screen; false when its type is one this check can't draw on.
// bus and addr are used by I2C screens only.
bool screen_begin(const BoardDef &b, uint8_t type, TwoWire *bus, uint8_t addr);
bool screen_ready();
bool screen_is_colour();
uint16_t screen_width();   // at rotation 0
uint16_t screen_height();
void screen_status(const char *line1, const char *line2);
void screen_show_x();
void screen_show_arrow(uint8_t orientation);  // 0-3: quarter turns; 4-7: the same, mirrored
// A monochrome screen's picture, for checking the drawings on the bench:
// one bit per pixel in the driver's page order. 0 bytes on colour screens.
size_t screen_dump(const uint8_t **buf);
