// Button: tell your Warblet dashboard each time a button is pressed.
//
// Wire a push button between pin 4 and GND. Each press sends the number of
// presses so far, as text, tagged "button", so a routine can react to it.
// Paste decoder.star (in this folder) into the device's spec.
// Set up the board over USB with the Warblet setup page, as in SensorReading.
//
// The board sends nothing until the first press, and that press is what adds
// it to your account. It takes no commands, so it never stops to ask for any
// between presses.

#include <Warblet.h>

const int BUTTON_PIN = 4;  // GPIO 4: the pad labelled D3 on a XIAO ESP32-S3

Warblet board;
int presses = 0;
bool wasDown = false;

void setup() {
  board.begin();
  pinMode(BUTTON_PIN, INPUT_PULLUP);    // reads LOW while pressed
}

void loop() {
  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down && !wasDown) {               // the moment it goes down
    presses = presses + 1;
    board.sendNumber(presses, "button");
  }
  wasDown = down;
  board.wait(20);                       // ignore the bounce of the contacts
}
