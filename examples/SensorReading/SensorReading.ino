// Sensor reading: send a light level to your Warblet dashboard once a minute.
//
// Wire a light sensor (or a knob) to pin A0. Upload this sketch, close the
// Serial Monitor, then open the Warblet setup page and connect over USB: it
// gives the board your Wi-Fi and its device settings.
//
// This example sends with sendTenths(): two bytes, the value times ten, the
// same compact format and decoder as the public Warblet temperature demos.
// Paste decoder.star (in this folder) into the device's spec, so the
// dashboard shows "value" as a number. For values outside -3276.8 to 3276.7,
// or more than one decimal, use sendNumber() and the text decoder instead.
//
// It takes no commands, so the board never asks for any: between readings
// it sends nothing.

#include <Warblet.h>

Warblet board;

void setup() {
  board.begin();                        // settings come from the setup page
  analogReadResolution(10);             // readings from 0 to 1023 on every board
}

void loop() {
  float percent = analogRead(A0) * 100.0 / 1023;
  board.sendTenths(percent);            // arrives as 0.0 to 100.0
  board.wait(60000);                    // a minute, still answering the setup page
}
