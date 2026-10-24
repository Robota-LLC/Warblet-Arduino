# Host tests

These tests run on a PC and need no board, no network and no Arduino install:
- the portable core (`src/warblet/core`), checked against the Warblet API contract's signing rule and against the public Warblet demos' own code (Warblet-IoT v1.0.2), compiled or run unmodified;
- the sketch-facing class (`src/Warblet.cpp`, `src/WarbletMqtt.cpp`) in a simulation;
- the ESP32 section of `src/WarbletBoard.cpp`, unmodified, against stand-ins of the ESP32 core and of Espressif's MQTT client;
- every example, run as it is, with its own decoder reading what it sends;
- the DeskDisplay example's screen driver against a fake SPI bus with each kind of screen chip behind it, and the sketch with a fake screen and touch.

**They do not prove that anything compiles for a board or works on one.**

## Run

From the repository root, with the reference demos cloned next to this repository:

```sh
git clone --branch v1.0.2 https://github.com/Robota-LLC/Warblet-IoT.git ../wiot-ref
REF=../wiot-ref sh test/run_tests.sh
```

Exit code 0 means every check passed. Without the clone, the `demo-c` and `demo-cam` groups are skipped, and the committed `test/vectors.txt` and `test/prov_vectors.txt` are used.

The suite is run with GCC 12 (MinGW-w64) and Python 3.10 under Git Bash on Windows; any C++11 compiler and Python 3 should do.

## The signing rule, and which oracle can judge it

The library signs the contract's v0.31 shape only: `<nonce> "\n" "tag=" <tag> "\n" <body>`, both lines always present, empty fields left empty.

The public demos at v1.0.2 still sign the older v0.29 shape. In that shape both lines were optional, so the two rules give the same bytes **only when a message carries both a nonce and a tag**. So the demos can no longer be the reference for the signed input. They remain the reference for the HMAC, for the nonce-and-tag layout, and for the poll challenge, which v0.31 does not change.

The reference for the v0.31 input is:
- the contract's **three worked examples**, checked byte for byte here;
- an independent Python transcription of the rule, which refuses to write vectors unless it reproduces those examples.

## What each group proves

| Group | Checks | Proves | Does not prove |
|---|---|---|---|
| `sha256` | FIPS 180-4 examples, including one million `a` fed in 997-byte pieces | the hash | — |
| `hmac` | RFC 4231 cases 1, 2, 3, 4, 6, 7 | HMAC for short, block-sized and longer-than-block keys | — |
| `demo-c` | Random keys and bodies with every tag and nonce shape, plus 20 poll challenges. The bare-metal demo's own `chirp_sign()` (compiled unmodified) signs the bytes this library's `signedInput()` builds, and the library's streaming signer signs the same message. | the library's HMAC equals the shipped firmware's; its streaming signer equals its own `signedInput()`; the demo's example frame is signed differently under v0.31 than at v1.0.2 | that `signedInput()` is right (`wire` and `vectors` cover that) |
| `demo-cam` | The **shipped HTTPS camera demo's own code** (`chirp_send.c`, `chirp_sign.c`, compiled unmodified with ESP-IDF stand-ins, hashed by the bare-metal demo's SHA-256) over 40 rounds × 4 tag cases, a body that looks like a nonce and tag line, and 40 poll challenges | with a tag, its signatures equal the library's (where both rules agree), an independent check of the nonce-and-tag layout and HMAC; without a tag they **differ**, so the v0.31 cut is real; the poll challenge equals the library's | the untagged v0.31 shape (only `wire` and `vectors` judge that) |
| `vectors` | `test/vectors.txt`, written by `test/oracle.py`: header, envelope and poll vectors from Python's own `hmac` over the v0.31 rule; `hmac` vectors from the demos' own `chirp_sign.hmac_sha256()` (run on CPython, unmodified) | the v0.31 signed input as transcribed in `oracle.py`, which reproduces the contract's three worked examples before writing anything; the HMAC primitive equals the demos' | anything the transcription and the worked examples both get wrong |
| `wire` | The contract's three worked examples (input hex and HMAC) byte for byte; v0.31 inputs for every field combination, including the empty message; the forgery v0.31 closes (nonce and tag moved into the body) failing; request heads (the longest fit 836 and 580 of 1,024 bytes); tags, ids, control characters, nonces; numbers as text (`21.53`, `21.53f`, `-5.0`, `101325`, the most negative 64-bit value, a GPS coordinate, NaN and infinity refused); the demos' README two-byte readings; base64; key frames; the unclaimed answer; back-off; the response reader's edge cases | the framing matches the contract and the demos' published examples | that the platform's decoder accepts the exponent form (`1e+20`); only `21.53`, `-5.0` and `101325` were dry-run on the platform |
| `prov` | the USB setup protocol, with expectations written by hand | the parser's own rules | that they match the demos (`prov-demo` checks that) |
| `prov-demo` | `test/prov_vectors.txt`: 49 pushes decided by the public demos' **`chirp_prov.py`, run unmodified on CPython** by `test/prov_oracle.py` | in 37 cases the library gives the same answer and saves the same config; the 12 documented stricter cases are refused with `bad-json` | — |

## The sketch-facing layer, simulated (`test/sim`)

`run_tests.sh` builds `src/Warblet.cpp`, `src/WarbletMqtt.cpp` and the LightByCommand example with:
- stand-in Arduino headers, whose serial line delivers 64 bytes per millisecond into a receive buffer that drops on overflow;
- a simulated board in place of `src/WarbletBoard.cpp`, whose requests can take time and whose Wi-Fi joins only a named network;
- a fake Warblet server, which answers a command check only as a `POST`;
- a fake MQTT broker that knows only `mqtt.warbletiot.com`. Like Espressif's client on the board, it ends every failed attempt with a disconnect.

`sim_test.cpp` (130 checks) covers:
- **setup:** waiting for settings; the setup page's probe and push; settings loaded after a restart; typed settings; a push that arrives while the board is busy (nothing is sent for 10 s after the probe, and a push over 256 bytes survives a 1.5 s request with the 2 KB buffer, but is lost at 256 bytes); a corrected network tried at once.
- **messages:** the first message carrying the setup code; `led:on` reaching the sketch; `led:blink`, whose command function waits with `board.wait(200)`, starting no second command check; a key over USB, after which every signature verifies and nonces always increase; a key frame over the air; refusals named; numbers as text and as two bytes.
- **waiting:** ten minutes of 401s (4 command checks, 1 console line); a 429 that waits for `Retry-After`; a rate 429 told from a budget 429; a 45-minute outage (a queued command arrives 50 s after the server is back); no command checks without a command function; a reply cut short at close, neither handed over nor counted as stored.
- **`board.wait(60000)`:** a setup push arriving 5 s in is stored; a command queued before the wait arrives; the wait takes 60 s; a 2,000-character command arrives whole, and one past 8 KiB is dropped and said.
- **stays connected (simulated):** a board reporting once a minute stores again after each fault ends: Wi-Fi gone for 3 and for 20 minutes (61 s), TLS refused or DNS failing (1 s), a server that never answers (1 s), junk on the USB port (1 s), a restart (1 s). Twenty minutes without Wi-Fi are 45 tries and 3 console lines, not two lines a try.
- **not on an account:** after `202 {"status":"unclaimed"}`, nothing is sent for 20 minutes, Wi-Fi is off, the console says so once, the board keeps a fingerprint and never the code, a restart stays stopped, a Wi-Fi-only push does not count, and a new setup brings it back; a board with typed settings is told to change them in the sketch.
- **for a sketch with a screen:** the demo's slug in the announce; the device id; commands sent together taken in one round (ten of fifteen); `checkCommandsEvery()` (6 checks a minute by default, 12 at 5 s, no faster); every change of `state()` handed to `onState()`, in order; `restartWhenStuck()` (a restart once nothing has answered for its time, never on refusals, a lost network, an answer in time, or once stopped); a command said with its size, never its words.
- **MQTT:** the session's fields; a report acknowledged, tagged, signed or not; a command at once with no HTTPS request; a dropped session that comes back; a refused CONNECT that backs off and is said once; new credentials during that back-off; a wrong host said once; with no session, reports that return at once, said once; a CONNECT refused with code 4 (no device has this id), which stops the board as `unclaimed` does.

It proves the logic of `Warblet.cpp` and `WarbletMqtt.cpp`. **It cannot see real serial drivers, Wi-Fi, TLS, the clock or storage.**

## The ESP32 board layer (`test/esp32`)

`board_test.cpp` compiles the ESP32 section of `src/WarbletBoard.cpp`, unmodified, against stand-ins of the ESP32 core and of Espressif's MQTT client with the core's own names (33 checks). The stand-in client follows ESP-IDF v5.5.5's `mqtt_client.c` where the library depends on it: a failed attempt ends in an error and a disconnect; a long message arrives in pieces; an unacknowledged QoS 1 report is sent again after `message_retransmit_timeout`; the outbox drops a report after 30 s. It checks:
- the serial receive buffer is 2 KB when `begin()` runs;
- the TLS limits and the pinned roots;
- the MQTT configuration, and that only the MQTT client's own log tag is quieted;
- what the client's events mean: a failed attempt, a refusal (code 5), no device (code 4), a dropped session;
- a report acknowledged after 2.5 s, or never, goes on the wire once;
- commands: three before `loop()` takes one are all kept; two cut off mid-transfer, or one cut off by a dropped session, are said and do not block the next; 48 KB arriving at once are kept or said dropped, none silently, and exactly 32 KB wait at most (four of 8,000 bytes kept, two said dropped), with room again once `loop()` has taken them; a command waiting when the session restarts is still handed over.

## The examples (`test/sim/example_run.cpp`, `test/run_decoders.py`)

Each example is built and run as it is on the simulated board:
- **record:** its inputs (a reading on A0, presses of the button) and the commands its dashboard sends. Every message is written down with the value it should decode to, and the LED's pin is checked against the sketch's `LED_ON`. The three LED examples run again built as for each Seeed XIAO whose LED lights on LOW (the ESP32-S3, S3 Plus, C6 and C5 board macros) and with no board macro, and `LED_ON` is held to the level that board needs, so a misspelt macro fails.
- **decoders:** `run_decoders.py` runs the example's own `decoder.star` on each message with CPython, with a payload that behaves as the platform's sandbox treats bytes (`str(payload)` is the text, `int(payload[0])` one byte), and checks the value.
- **flood:** ten minutes of 401s, 403s or 500s at 600 ms a request. The examples that say hello until it is stored send at most 11 messages and one "not stored" line (LightByCommand: 4 for 401 and 403, 11 for 500).
- **corrected:** for LightByCommand, LightByCommandMqtt and TwoBoards, 10 and then 20 minutes of 401s, then a setup push with another refused token (said again, and messages wait 60 s), then one Warblet accepts: the hello is stored at once, not when the old back-off would have ended (303 s before the fix).

## The desk display (`test/cyd`)

- `screen_test.cpp` (18 checks) runs `examples/DeskDisplay/screen.cpp`, unmodified, against `SPI.h` here, a fake bus with a screen chip behind it. As the one-USB board's ILI9341, an ST7789 that answers, and the two-USB ST7789 whose data line from the screen is not connected, it checks: the questions asked (in both clock shapes, at 4 MHz); the settings chosen for each answer (`0xE8`; `0xA8` with C0h `0x0C` in mode 3; the ST7789 when nothing answers, and the guess said); the console line; and the letters' pixels against MicroPython's own font.
- `desk_display_test.cpp` (55 checks) runs the sketch on the simulated board with `fake_screen.cpp`, which keeps each line of text on the screen. It covers: not set up; the demo named in the announce; the strip from Joining Wi-Fi to Connected; the waiting text with the id; messages at each size, with letters outside plain English, and with a newline or a tab between words; one "seen" per message, and a tap while offline sent later; each message sent said on the console with the free memory; one disk line per computer, and a fourth replacing the oldest; Clear; a check every 5 s and a report every 30 s; 401, 429 and 403 (refusals), 503 (a fault: no answer) and no answer on the strip; the self-restart; the not-on-an-account screen and stop; a setup push as a new start (each restart boots a fresh copy of the sketch, so the first setup, the self-restart and a setup after the stop each start afresh, the last with a hello under the new id), and a Wi-Fi-only push that keeps the message; and that the routine and decoder are the MicroPython desk display's, byte for byte.
- `screen.h`'s `CHIP` is checked at build time: the suite builds it with `""`, `ILI9341` and `ST7789`, and must fail to build with `ST7798` or `ili9341`.
- 14 planted faults are each caught: the words on the console, a second seen, the id left on screen, no self-restart, the 10 s pace, no demo slug, a 401 shown as Connected, one row per disk line, the strip one letter wider, the ST7789 turned like the ILI9341, the panel read at 40 MHz, the ILI9341 as the fallback, no C0h, the letters upside down.

## Stack, measured on the PC build

`g++ -O2 -fstack-usage`, x86-64, in bytes. An ARM build will differ and has not been measured.

| Function | Stack |
|---|---|
| `Warblet::post` | 304 |
| `Warblet::poll` | 272 |
| `Warblet::serviceMqtt` | 384 |
| `Warblet::publishMqtt` | 336 |
| `signHeader` | 272 |
| `HmacSha256::finish` | 176 |
| `Sha256::finish` | 96 |
| `Sha256::compress` | 272 |

The deepest signing path is 1,120 bytes on x86-64 (`post` → `signHeader` → `HmacSha256::finish` → `Sha256::finish` → `compress`), plus the small `sendNumber` frame on top. That is more than the UNO R4's 1 KB main stack, which is one reason that board is not supported yet.

## Faults put in on purpose, to check that the tests catch them

Each was reverted afterwards, and the suite passed again.

| Fault | Checks that fail |
|---|---|
| the older signing rule restored (optional nonce and tag lines) | 513: `vectors` 462, `demo-cam` 40, `wire` 10, `demo-c` 1 |
| the empty tag line left out | 502: `demo-c` 251, `vectors` 210, `demo-cam` 40, `wire` 1 |
| the envelope given a nonce field | 419: `demo-c` 250, `vectors` 168, `wire` 1 |
| the newline after `chirp-down` dropped | 72: `demo-cam` 40, `demo-c` 20, `vectors` 12 |
| `board.wait()`'s re-entry guard removed | 1 simulation check |
| a single `ssid` push no longer replacing the stored list | 2: `prov` 1, `prov-demo` 1 |
| the 2 KB serial receive buffer removed | 1 board check |
| MQTT's retransmit timeout left unset | 3 board checks (3 and 30 copies on the wire) |
| a reply cut short at close accepted | 2 simulation checks |
| the 10-minute ceiling kept for no answer | 1 simulation check (a command 510 s late) |
| SensorReading switched to `sendNumber()` with its two-byte decoder kept | its decoder check (50.0 read as 1361.6) |
| every answer `ANSWERED`, a 5xx `REFUSED`, or a 5xx without its code | the state sequence and the per-status table (simulation) |
| an MQTT refusal leaving `answer()` as it was (D12) | 1 simulation check |
| a refusal not counting as an answer for `restartWhenStuck` (D8) | 1 simulation check |
| DeskDisplay ignoring `REFUSED`, or calling a 429 refused | 1 desk display check each |
| a new setup leaving the report hold in place | 2 checks per corrected run (the hello 303 s late) |
| a new setup clearing the hold but keeping its back-off | 1 check per corrected run (a refusal of the new token not said, and a 10-minute wait) |
| an LED example's board macro misspelt | its board-macro run (`LED_ON` HIGH on a XIAO) |
| no cap on the commands waiting for `loop()` | 1 board check (six of 8,000 bytes kept) |
| the README saying anyone who knows the id could claim the board | the shipped-text check at the end of the suite |

`demo-c` fails only once under the older rule. It compares the demo's HMAC over the library's own input bytes, so it cannot judge the shape.
## Where the library is stricter than `chirp_prov.py`, on purpose

- A string longer than its field is refused with `bad-json`. The limits are: Wi-Fi name 32 bytes, password 64, token 128, id 128, host 128.
- A non-empty `key` must be 44 base64 characters that decode to 32 bytes (the bare-metal demo's rule).
- A control character (below 0x20, or 0x7f) in any string is refused.
- `NaN` and `Infinity` are refused, because they are not JSON. CPython's `json` accepts them.
- The announce leaves `hw=` out when there is no id, as the contract says.
- After `CHIRP= ok` the board restarts only when the push changed its id or its token, as the MicroPython demos do after every push; Wi-Fi, a key or a host are used at once. The contract allows either.
- The line limit is 2,048 characters; `chirp_prov.py`'s is 4,096.
