// Two boards: press the button on one board, and the other board's LED
// blinks, wherever the two boards are.
//
// Upload this same sketch to both boards and set each one up over USB with
// the Warblet setup page, under the same spec. Paste decoder.star (in this
// folder) into that spec, and pair.star into a routine scoped to it, with
// your two device ids in it. The routine runs on Warblet: the boards never
// talk to each other directly.
//
// Wiring on each board: a push button between pin 4 and GND.
//
// pair.star stops with an error until you have put your two device ids in it.

#include <Warblet.h>

// The board's own LED. Some boards name none (a plain ESP32 module, for
// one); pin 2 is the LED on many ESP32 boards, so it is used there. Change
// it to your LED's pin if yours is elsewhere.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// The level that lights the LED. Most boards light it on HIGH. Seeed's XIAO
// ESP32-S3 and S3 Plus (GPIO21), ESP32-C6 (GPIO15) and ESP32-C5 (GPIO27)
// light it on LOW, as Seeed's getting-started pages for each board show. On
// another board whose LED lights on LOW, set LOW.
#if defined(ARDUINO_XIAO_ESP32S3) || defined(ARDUINO_XIAO_ESP32S3_PLUS) || defined(ARDUINO_XIAO_ESP32C6) || \
    defined(ARDUINO_XIAO_ESP32C5)
const int LED_ON = LOW;
#else
const int LED_ON = HIGH;
#endif

const int BUTTON_PIN = 4;  // GPIO 4: the pad labelled D3 on a XIAO ESP32-S3

Warblet board;
int presses = 0;
bool wasDown = false;
bool saidHello = false;

void command(String text) {
  if (text == "led:blink") {
    for (int i = 0; i < 3; i++) {       // three blinks, then out
      digitalWrite(LED_BUILTIN, LED_ON);
      board.wait(200);
      digitalWrite(LED_BUILTIN, !LED_ON);
      board.wait(200);
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, !LED_ON);   // start with the LED out
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  board.begin();
  board.onCommand(command);
}

void loop() {
  // The first message adds the board to your account, so say hello once.
  if (!saidHello && board.connected()) {
    saidHello = board.sendNumber(0, "hello");
  }

  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down && !wasDown) {
    presses = presses + 1;
    board.sendNumber(presses, "button");  // the routine sees the "button" tag
  }
  wasDown = down;
  board.wait(20);
}
