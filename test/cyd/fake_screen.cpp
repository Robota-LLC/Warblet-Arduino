// A fake Cheap Yellow Display behind examples/DeskDisplay/screen.h: it keeps
// what text is on the screen where, in which colour and size, so a test can
// read the screen's words. A fill wipes the text it covers. PC only.
#include <string.h>

#include <string>
#include <vector>

#include "fake_screen.h"
#include "screen.h"

std::vector<SimText> simScreen;
std::vector<std::string> simStrip;  // every status strip drawn, in order
bool simTouch = false;
std::string simStartLine = "screen: ILI9341 (simulated)";

namespace screen {

bool GUESSED = false;

const char* start() {
  simScreen.clear();
  return simStartLine.c_str();
}

void backlight(bool) {}

void fill(int x, int y, int w, int h, uint16_t) {
  std::vector<SimText> kept;
  for (const SimText& t : simScreen) {
    int tw = (int)t.words.size() * 8 * t.size, th = 8 * t.size;
    bool covered = t.x < x + w && x < t.x + tw && t.y < y + h && y < t.y + th;
    if (!covered) kept.push_back(t);
  }
  simScreen = kept;
}

void text(int x, int y, const char* words, int size, uint16_t colour, uint16_t background) {
  std::string shown;
  for (const char* p = words; *p; p++) shown += (*p >= ' ' && *p <= '~') ? *p : '?';
  if (shown.empty()) return;
  fill(x, y, (int)shown.size() * 8 * size, 8 * size, background);
  simScreen.push_back(SimText{x, y, size, shown, colour});
  if (y == 1 && (simStrip.empty() || simStrip.back() != shown)) simStrip.push_back(shown);
}

bool touched() { return simTouch; }

}  // namespace screen

const SimText* simTextAt(int y) {
  for (const SimText& t : simScreen) {
    if (t.y == y) return &t;
  }
  return nullptr;
}

std::string simWordsAt(int y) {
  const SimText* t = simTextAt(y);
  return t ? t->words : "";
}

std::string simMiddle(int top, int bottom) {
  std::vector<const SimText*> lines;
  for (const SimText& t : simScreen) {
    if (t.y >= top && t.y < bottom) lines.push_back(&t);
  }
  std::string out;
  for (int y = top; y < bottom; y++) {
    for (const SimText* t : lines) {
      if (t->y == y) out += (out.empty() ? "" : "|") + t->words;
    }
  }
  return out;
}

bool simShows(const std::string& words) {
  for (const SimText& t : simScreen) {
    if (t.words.find(words) != std::string::npos) return true;
  }
  return false;
}
