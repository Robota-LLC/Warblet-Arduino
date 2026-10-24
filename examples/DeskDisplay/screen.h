// The Cheap Yellow Display's screen and touch, for the desk display.
//
// The pins are the same on the one-USB and the two-USB boards:
//   screen: SPI clock 14, data to the screen 13, data from the screen 12,
//           select 15, command/data 2, backlight 21 (high = on). The screen
//           resets with the board, so it has no reset pin of its own.
//   touch:  XPT2046 chip on clock 25, data in 32, data out 39, select 33, and
//           a "pen" line on 36 that goes low while the screen is pressed.
//
// Boards come with one of two screen chips: the one-USB board has an ILI9341,
// most two-USB boards an ST7789 (some an ILI9341). screen::start() asks the
// chip for its id and sets itself up for whichever answers. If the picture
// is wrong, the settings below fix it; the console line at start says what
// was chosen.
#pragma once

#include <Arduino.h>

namespace screen {

constexpr const char* CHIP = "";    // "": ask the screen. Or "ILI9341" or "ST7789" to skip asking.
const bool INVERT = false;          // true if the colours look like a photo negative.
const bool FLIP = false;            // true if the picture is upside down.
const bool MIRROR = false;          // true if the writing reads backwards; if it is then upside down, set FLIP too.
const bool SWAP_RED_BLUE = false;   // true if red shows as blue and blue as red.

// Any other CHIP stops the build here, rather than using the ST7789's
// settings without a word.
constexpr bool sameText(const char* a, const char* b) { return *a == *b && (*a == '\0' || sameText(a + 1, b + 1)); }
static_assert(sameText(CHIP, "") || sameText(CHIP, "ILI9341") || sameText(CHIP, "ST7789"),
              "screen.h: CHIP must be \"\" (ask the screen), \"ILI9341\" or \"ST7789\"");

const int WIDTH = 320;
const int HEIGHT = 240;

// Set by start(): true when the screen did not answer and the ST7789
// settings were used.
extern bool GUESSED;

// Find the chip, wake the screen, clear it and turn the light on. Returns a
// line saying what was found, for the console.
const char* start();

void backlight(bool on);

// A colour from red, green and blue (each 0-255), as the screen takes it:
// 5 bits red, 6 green, 5 blue.
constexpr uint16_t rgb(uint8_t red, uint8_t green, uint8_t blue) {
  return (uint16_t)((red & 0xF8) << 8 | (green & 0xFC) << 3 | blue >> 3);
}

// Paint a rectangle one colour.
void fill(int x, int y, int w, int h, uint16_t colour);

// Write words at (x, y) in 8x8 letters, each letter 8 x size pixels square.
// Letters it has no shape for show as '?'.
void text(int x, int y, const char* words, int size, uint16_t colour, uint16_t background);

// True while a finger presses the screen.
bool touched();

}  // namespace screen
