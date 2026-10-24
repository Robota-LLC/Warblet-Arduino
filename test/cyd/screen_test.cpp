// Runs the desk display's screen.cpp, unmodified, against a fake SPI bus with
// a screen chip behind it (SPI.h here), as each kind of Cheap Yellow Display
// answers: the one-USB board's ILI9341, an ST7789 that answers, and the
// two-USB ST7789 board whose data line from the screen is not connected.
//
// What this proves: the questions asked, the settings chosen for each answer
// (the same as the Warblet MicroPython desk display's screen.py, whose
// ILI9341 settings are proven on a board), and the letters' pixels. It does
// NOT prove the picture on a real screen.

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <SPI.h>

#include "screen.h"

unsigned long simNowMs = 1000;
int simPin[64];
int simAnalog = 0;
SimSerial Serial;
SimPanel simPanel;

namespace {

int pass = 0, fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    pass++;
  } else {
    fail++;
    printf("FAIL  %s\n", what.c_str());
  }
}

// The commands sent after the last software reset, each with its data bytes.
std::vector<std::vector<uint8_t>> afterReset() {
  size_t from = 0;
  for (size_t i = 0; i < simPanel.wire.size(); i++) {
    if (simPanel.wire[i].command && simPanel.wire[i].value == 0x01) from = i + 1;
  }
  std::vector<std::vector<uint8_t>> out;
  for (size_t i = from; i < simPanel.wire.size(); i++) {
    if (simPanel.wire[i].command) out.push_back({simPanel.wire[i].value});
    else if (!out.empty() && out.back().size() < 8) out.back().push_back(simPanel.wire[i].value);
  }
  return out;
}

std::vector<uint8_t> commandData(uint8_t code) {
  for (auto& c : afterReset()) {
    if (c[0] == code) return std::vector<uint8_t>(c.begin() + 1, c.end());
  }
  return {0xEE, 0xEE};  // not sent
}

bool sent(uint8_t code) { return commandData(code) != std::vector<uint8_t>{0xEE, 0xEE}; }

const char* startAs(const char* chip) {
  simPanel = SimPanel();
  simPanel.chip = chip;
  return screen::start();
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // ---- the one-USB board: ILI9341 ------------------------------------------
  std::string line = startAs("ILI9341");
  check(line == "screen: ILI9341 (answered 00 00 00 00 93 41)" && !screen::GUESSED,
        "ILI9341: found by its id register (D3, picked with D9), said on the console");
  bool slowReads = !simPanel.reads.empty();
  for (auto& r : simPanel.reads) slowReads = slowReads && r.clock == 4000000 && r.mode == 0;
  check(slowReads, "ILI9341: the id is read slowly (4 MHz), in SPI mode 0");
  check(commandData(0x36) == std::vector<uint8_t>{0xE8}, "ILI9341: sideways with 0xE8, the MicroPython desk display's setting");
  check(!sent(0xC0) && sent(0x20) && !sent(0x21), "ILI9341: no C0h, colours not inverted");
  check(commandData(0x3A) == std::vector<uint8_t>{0x55} && sent(0x11) && sent(0x29), "ILI9341: woken, 16 bits a pixel, on");
  bool fast = true;
  for (auto& b : simPanel.wire) fast = fast && (b.clock == 4000000 || (b.clock == 40000000 && b.mode == 0));
  check(fast, "ILI9341: drawn at 40 MHz in mode 0");
  check(simPanel.transactions == 0, "every SPI transaction is ended");

  // ---- an ST7789 that answers ----------------------------------------------
  line = startAs("ST7789");
  check(line == "screen: ST7789 (answered 85 85 52)" && !screen::GUESSED, "ST7789: found by DA DB DC = 85 85 52");
  check(commandData(0x36) == std::vector<uint8_t>{0xA8}, "ST7789: sideways with 0xA8 (the ILI9341's value, mirror bit flipped)");
  check(commandData(0xC0) == std::vector<uint8_t>{0x0C}, "ST7789: register C0h set to 0x0C");
  bool mode3 = false;
  for (auto& b : simPanel.wire) mode3 = mode3 || (b.clock == 40000000 && b.mode == 3);
  check(mode3, "ST7789: drawn in SPI mode 3");

  // ---- the two-USB ST7789 board: its data line from the screen is not wired --
  line = startAs("none");
  bool bothShapes = false, shape0 = false;
  for (auto& r : simPanel.reads) {
    shape0 = shape0 || r.mode == 0;
    bothShapes = bothShapes || (shape0 && r.mode == 3);
  }
  check(bothShapes, "no answer: asked in both clock shapes");
  check(screen::GUESSED && commandData(0x36) == std::vector<uint8_t>{0xA8} && commandData(0xC0) == std::vector<uint8_t>{0x0C},
        "no answer: the ST7789 settings, and GUESSED for the screen's note");
  check(line.find("no answer") != std::string::npos && line.find("set CHIP = \"ILI9341\" at the top of screen.h") != std::string::npos,
        "no answer: the console says how to choose the ILI9341");

  // ---- letters: MicroPython's 8x8 font, each byte a column, top bit first ----
  startAs("ILI9341");
  simPanel.wire.clear();
  screen::text(0, 0, "A", 1, 0xFFFF, 0x0000);
  std::vector<uint8_t> pixels;
  bool started = false;
  for (auto& b : simPanel.wire) {
    if (b.command) started = b.value == 0x2C;
    else if (started) pixels.push_back(b.value);
  }
  const uint8_t glyphA[8] = {0x00, 0x7c, 0x7e, 0x0b, 0x0b, 0x7e, 0x7c, 0x00};  // font_petme128_8x8, 65=A
  bool same = pixels.size() == 8 * 8 * 2;
  for (int r = 0; same && r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      bool on = (glyphA[c] >> r) & 1;
      same = same && pixels[(r * 8 + c) * 2] == (on ? 0xFF : 0x00);
    }
  }
  check(same, "\"A\" at size 1: 8 x 8 pixels, the same as MicroPython's framebuf draws it");
  simPanel.wire.clear();
  screen::text(0, 0, "\x7f", 2, 0xFFFF, 0x0000);
  pixels.clear();
  started = false;
  for (auto& b : simPanel.wire) {
    if (b.command) started = b.value == 0x2C;
    else if (started) pixels.push_back(b.value);
  }
  const uint8_t glyphQ[8] = {0x00, 0x02, 0x03, 0x51, 0x59, 0x0f, 0x06, 0x00};  // 63=?
  same = pixels.size() == 16 * 16 * 2;
  for (int r = 0; same && r < 16; r++) {
    for (int c = 0; c < 16; c++) {
      bool on = (glyphQ[c / 2] >> (r / 2)) & 1;
      same = same && pixels[(r * 16 + c) * 2] == (on ? 0xFF : 0x00);
    }
  }
  check(same, "a letter it has no shape for is '?', and size 2 doubles each pixel both ways");
  simPanel.wire.clear();
  screen::fill(10, 20, 30, 4, screen::rgb(255, 0, 0));
  size_t data = 0;
  for (auto& b : simPanel.wire) data += !b.command;
  check(data == 8 + 30 * 4 * 2 && screen::rgb(255, 0, 0) == 0xF800, "fill: the area's corners, then w x h pixels of 5-6-5 colour");
  check(simPanel.transactions == 0, "every SPI transaction is ended, drawing too");

  printf("\nscreen (the desk display's screen.cpp on the PC): %d passed, %d failed\n%s\n", pass, fail,
         fail ? "FAILED" : "PASSED");
  return fail ? 1 : 0;
}
