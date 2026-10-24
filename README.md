# Warblet for Arduino

Send readings from an Arduino sketch to your [Warblet](https://warbletiot.com) dashboard, and act on the commands its buttons send. The library looks after Wi-Fi, the device credential, signed messages and the settings that arrive over USB. Your sketch does the part you want to build.

## What happens

Your sketch calls `board.sendNumber()` with a reading, and it appears on the board's device page. A button on the dashboard sends a command, such as `led:on`, and the function your sketch named with `board.onCommand()` runs with it. The Warblet setup page gives the board its Wi-Fi and its device settings over the USB cable, and the board keeps them.

```cpp
#include <Warblet.h>

Warblet board;

void setup() {
  board.begin();                     // Wi-Fi and device settings come from the setup page over USB
}

void loop() {
  board.sendNumber(analogRead(A0));  // a reading, sent as text
  board.wait(60000);                 // wait a minute, still listening to the setup page
}
```

Use `board.wait()` wherever you would use `delay()`: while it waits, the board keeps answering the setup page, keeps Wi-Fi up and collects commands.

## What you need

- An ESP32 board, with Espressif's Arduino core for ESP32 3.x installed from the Boards Manager. The library has run on a Seeed XIAO ESP32-S3 with core 3.3.12, and builds for a plain ESP32 and the Arduino Nano ESP32.
- A USB data cable.
- A 2.4 GHz Wi-Fi network that reaches the internet, including network time (NTP, UDP port 123). The board sends nothing until its clock is set.
- A Warblet account, and Chrome or Edge on a computer for the setup page.

On a board with native USB (ESP32-S3, C3, C6), turn on **Tools → USB CDC On Boot**, so `Serial` is the USB port the setup page talks to.

## Install

In the Arduino IDE, choose **Sketch → Include Library → Add .ZIP Library** and pick a ZIP of this repository. The examples then appear under **File → Examples → Warblet**.

## Get a board running

1. Open an example, such as **LightByCommand**. Its comments say what to wire.
2. Upload it, then close the Serial Monitor: the setup page needs the port.
3. Use https://warbletiot.com/flash to send your Wi-Fi and the device's settings over USB. The board restarts once a new device id or token is stored, so your sketch starts afresh under it; new Wi-Fi alone is used without a restart.
4. The board's first message adds it to your account. Open its device page to see what it sends.
5. Paste the example's `decoder.star` into the device's spec, so the dashboard shows the reading as a number.

## The examples

**Button** and **TwoBoards** compile for the XIAO ESP32-S3 and plain ESP32 but are untested on hardware. Check the button and LED behaviour on your boards before building on these examples.

| Example | What it does | Wiring |
|---|---|---|
| **SensorReading** | Sends a light level once a minute, in the compact two-byte format the public temperature demos use. | A light sensor or a knob on A0 |
| **Button** | Sends the number of presses so far, tagged `button`, each time the button goes down. | A push button between GPIO 4 and GND (the pad labelled D3 on a XIAO ESP32-S3) |
| **LightByCommand** | Turns the board's LED on and off, or blinks it, on `led:on`, `led:off` and `led:blink`, and reports what it set. | None: the board's own LED |
| **LightByCommandMqtt** | The same over MQTT: a command arrives as soon as it is sent. | None |
| **TwoBoards** | A press on one board blinks the other board's LED, through a routine on Warblet (`pair.star`). | A push button on each board |
| **DeskDisplay** | The Warblet desk display on a Cheap Yellow Display (ESP32-2432S028R): a message typed on the dashboard fills the screen, a tap says it was seen, and a routine shows how full your computers' disks are. | None: the board's own screen |

The LED examples have an `LED_ON` constant: the level that lights the LED. It is `LOW` on Seeed's XIAO ESP32-S3, ESP32-S3 Plus, ESP32-C6 and ESP32-C5, whose LEDs light when their pin is low, and `HIGH` elsewhere. Change it if your board's LED lights on `LOW`.

SensorReading and Button set no command function, so they never ask for commands. The others ask every 10 seconds over HTTPS; DeskDisplay asks every 5.

DeskDisplay's screen tab has the settings for a picture that comes out mirrored, upside down or in the wrong colours; it needs no other library. Choose the board **ESP32-2432S028R CYD** for it.

## HTTPS or MQTT

`Warblet board;` talks HTTPS. It asks for commands every 10 seconds, and works on any network that lets web traffic and network time through.

`Warblet board(Warblet::MQTT);` keeps one MQTT session open over TLS, so a command arrives as soon as it is sent; on a XIAO ESP32-S3, in well under a second. It uses Espressif's MQTT client, which comes with the Arduino core for ESP32 3.x, so there is nothing else to install. On an older core the library builds without it, and the board says on the console that MQTT is not available.

What MQTT costs:
- **Flash and memory.** About 48 KB more flash, in sketches that use it only. While the session is open the board has about 63 KB less free memory: 173 KB free on a XIAO ESP32-S3, against 236 KB with HTTPS.
- **Port 8883**, which some networks close.
- **No "stored" answer.** A report's `true` means the broker took it, not that Warblet stored it. Warblet drops a report it refuses, without a word.
- **No nonce.** A signed report carries no nonce, so Warblet cannot refuse a copy sent again.

## What the board says

The library prints its own lines on `Serial`, each starting `warblet:`. They say what it is doing and why something failed: waiting for settings, Wi-Fi joined, a message not stored and why, a command received (its size, never its words). A sketch can show the same on a screen or a light with `board.state()`. Each problem is said once, not on every try. Over MQTT, while the broker cannot be reached, Espressif's TLS layer also prints its own `E (...)` lines; the library leaves them, because quieting them would hide your sketch's own TLS errors too.

When Warblet refuses a message or does not answer, the board waits before it sends again: a minute after a refusal (401, 403, 404), doubling while it lasts, up to ten; 20 seconds after "too many" (429), doubling up to ten minutes, or longer if Warblet asks; 20 seconds when nothing answers or Warblet has a fault, doubling up to a minute. Until then `sendNumber()` returns `false` at once. A new setup over USB ends the wait. Command checks slow down the same way.

## If Warblet does not know the board

If its device was deleted, or its setup code was never used, Warblet answers the board's next message with `unclaimed`. Over MQTT, Warblet refuses its connection because no device has its id. Nothing the board sends would reach an account, so it stops:
- it sends no messages and asks for no commands;
- it leaves its Wi-Fi network (the radio stays on, so the setup page can still reach it over USB);
- it says why on the console, once.

It stays stopped after a restart. To start it again, set it up over USB at warbletiot.com/flash; changing only the Wi-Fi does not count. A board whose settings are typed into `board.begin()` starts again with new settings typed in and uploaded. The board keeps a short fingerprint of the refused setup, never the code itself.

## The library uses `Serial`

`board.begin()` starts `Serial` at 115200 baud, with a 2 KB receive buffer on ESP32 boards, so call it before any `Serial.begin()`. The library reads every byte that arrives on `Serial`: it answers the setup page's lines, which start with `CHIRP?` and `CHIRP+`, and discards everything else. A sketch that reads `Serial` itself will not see that input.

## Credentials and security

Every connection is TLS: HTTPS, or MQTT on port 8883. The board checks that the server's certificate chains to one of four pinned Let's Encrypt roots and names the host. The device token says which device is talking.

With a signing key, each message also carries an HMAC-SHA256 signature over its tag and its bytes, and over HTTPS a timestamp nonce too. Warblet refuses a message that was altered, and over HTTPS a copy sent again. Signing is not encryption; TLS is what keeps the bytes unreadable on the way. Over HTTPS, a sketch with a command function can also receive its signing key from Warblet over the air, because it asks Warblet for commands. Every other board gets its key only over USB, from the setup page: a sketch with no command function never asks, and over MQTT a key never comes over the air.

The settings, including Wi-Fi passwords, the token and the key, are stored unencrypted in the board's flash, and the board accepts new settings from anyone at its USB port. Physical access to a board is full access to it.

The reference, with every call, what goes on the wire and the limits, is [docs/REFERENCE.md](docs/REFERENCE.md). For the platform itself, see the [Warblet docs](https://warbletiot.com/docs).

## License

MIT. See [LICENSE](LICENSE).
