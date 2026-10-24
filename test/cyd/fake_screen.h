// What the fake screen (fake_screen.cpp) shows, for test/cyd's tests.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

struct SimText {
  int x, y, size;
  std::string words;
  uint16_t colour;
};
extern std::vector<SimText> simScreen;  // the text on the screen now
extern std::vector<std::string> simStrip;  // every status strip drawn, in order
extern bool simTouch;                   // a finger on the screen
extern std::string simStartLine;        // what screen::start() says

const SimText* simTextAt(int y);         // the text drawn at height y, or nullptr
std::string simWordsAt(int y);           // its words, "" when none
std::string simMiddle(int top, int bottom);  // the lines between, top to bottom, joined with "|"
bool simShows(const std::string& words);     // any text on the screen holds these words
