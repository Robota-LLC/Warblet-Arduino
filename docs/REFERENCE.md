# Warblet for Arduino: reference

Every call the library offers, what each one does, what goes on the wire, and the limits. For a first board, start with the [README](../README.md); for a whole device built on it, see the DeskDisplay example.

## The calls

| Call | What it does |
|---|---|
| `Warblet board;` | One connection to Warblet, over HTTPS. |
| `Warblet board(Warblet::MQTT);` | The same over MQTT: one session stays open, so commands arrive as soon as they are sent. ESP32 boards on the Arduino core for ESP32 3.x. Only a sketch that uses this form carries the MQTT code. |
| `board.begin()` | Starts `Serial` and loads the settings the setup page stored on the board. Returns at once; joining Wi-Fi happens in `loop()`. |
| `board.begin(wifi, password, deviceId, token, key)` | Starts with settings typed into the sketch instead. `key` (the 44-character signing key from the device page) is optional. The setup page is then not answered. |
| `board.loop()` | Does the board's work once without waiting: answers the setup page, keeps Wi-Fi up, and over HTTPS asks for commands every 10 s once a command function is set; over MQTT it hands over the commands that arrived. Call it every time `loop()` runs if you keep your own timing with `millis()`. |
| `board.wait(ms)` | Does that work for `ms` milliseconds: use it wherever you would use `delay()`. A request running when the time is up finishes first, so it can take longer. Inside a command function it only answers the setup page. |
| `board.sendNumber(value, tag)` | Sends a number as text: `"21.53"`, `"101325"`, `"-5.0"`. Any integer type, `float` (7 significant digits) or `double` (15). NaN and infinity are refused. Its decoder is one line: `{"value": float(str(payload))}`. |
| `board.sendTenths(value, tag)` | Sends the public temperature demos' two-byte format: the value times ten, rounded, as a signed 16-bit number, high byte first (`00 c9` is 20.1). From -3276.8 to 3276.7; a value outside is refused and said. |
| `board.sendText(text, tag)` | Sends the text's bytes. |
| `board.send(bytes, length, tag)` | Sends raw bytes. |
| `board.onCommand(function)` | Hands each command to `function(String text)`. Over HTTPS, the board asks for commands only once a function is set, every 10 s; after a command it asks again at once, up to ten in a row, so commands sent together arrive together. |
| `board.checkCommandsEvery(ms)` | Over HTTPS, how often to ask for commands instead of every 10 s; never more often than every 5 s. |
| `board.connected()` | True while Wi-Fi is up and the settings are complete; over MQTT, also while the session is open. |
| `board.state()` | What the board is doing, for a screen or a light: `NOT_SET_UP`, `JOINING_WIFI`, `NO_WIFI`, `REACHING_WARBLET`, `ANSWERED` (Warblet took the latest request), `REFUSED` (Warblet turned the board away: 401, 403, 404, 429, or the MQTT broker refusing the token), `NO_ANSWER` (nothing answered, the board has no clock, or Warblet answered with a 5xx fault of its own) or `NOT_ON_ACCOUNT`. |
| `board.answer()` | The HTTP status of Warblet's latest answer: 200, 202 or 204 when all is well (`ANSWERED`), 401 refused or 429 too many (`REFUSED`); a 5xx fault on Warblet's side (`NO_ANSWER`, the code still here); 0 before any. Over MQTT, 200 while the session is open and 401 when the broker refuses the token. |
| `board.onState(function)` | Runs `function(Warblet::State state)` each time `state()` or `answer()` changes, and once at `begin()`. It runs inside the library's work, even while Wi-Fi joins, so it should only show the state. |
| `board.deviceId()` | The device id the setup page gave the board, or `""` before it has one. |
| `board.demo(slug)` | For a demo's sketch: names the demo (`"demo-cyd-desk-display-arduino"`), so the board tells the setup page which demo it runs. Call it before `begin()`. **Only a published demo's own sketch calls this.** Delete the line in a sketch you change, or the setup page sets your board up as that demo, with its spec, decoder and dashboard. |
| `board.restartWhenStuck(ms)` | A last resort for a board on its own: when a request gets no answer at all while Wi-Fi says it is connected, and nothing has answered for `ms`, the board restarts. Any answer counts, a refusal too, and so does the start; never with no settings, or once stopped. Under 5 minutes means 5 minutes, and is said; `0` turns it off, which is how a board starts. A board on Wi-Fi that cannot set its clock (NTP blocked) sends nothing, so it is `NO_ANSWER` too and counts the same. A restart loses what the sketch had in memory. |

`tag` is optional on every send. It is 1 to 32 letters, digits, `_`, `.` or `-`, such as `button` or `heartbeat`; routines can match on it.

**What a send returns.** Over HTTPS, `true` only when Warblet answered that it stored the message on this device. Over MQTT, `true` when the broker acknowledged it. `false` when it was not sent or not stored, with a `warblet:` line saying why the first time.

**After a refusal or no answer** the board holds its messages for a while and a send returns `false` at once:

| Answer | First wait | Then |
|---|---|---|
| 401, 403 or 404 (a refusal) | 60 s | doubles, up to 10 min |
| 429 (too many, or the day's storage budget) | 20 s, or `Retry-After` | doubles, up to 10 min; `Retry-After` honoured up to an hour |
| 5xx, or no complete answer | 20 s | doubles, up to 1 min |

A stored message ends the wait. So does a new setup over USB: the next message goes at once, and if it is refused too the wait starts again at its first length. Command checks wait by the same rules; after a new setup the board checks at once.

## Settings

The setup page sends settings over USB, and the board stores them in its flash (NVS):

| Setting | Limit |
|---|---|
| Wi-Fi networks | up to 8, each a name of up to 32 bytes and a password of up to 64 characters |
| device id | 1 to 128 letters, digits, `:`, `-`, `_` or `.` |
| credential | a setup code (`CHIRP-` and 8 characters) or a device token, up to 128 characters |
| signing key | 44 base64 characters that decode to 32 bytes, or none |
| host | up to 128 characters, or none for Warblet's own |

The board tries its networks in order, 12 s each, and tries the list again every 15 s until one joins. After it answers the setup page's probe (`CHIRP?`) it listens only on USB for 10 s, so the settings that follow are read as they arrive. A setup line can be up to 2,048 characters. Settings that change the device id or the token are a new start: the board answers `CHIRP= ok`, then restarts, so the sketch starts afresh under the new setup (a hello again, nothing the old setup showed). Any other settings - Wi-Fi networks, a key, a host - take effect as soon as they are stored, without a restart.

The setup code is the board's credential until its first message is stored: that message adds the board to your account, and from then on the same code is its token. A new token, or a signing key, can be pushed over USB at any time.

## On the wire

Nothing is sent until the clock is set from network time; certificates are checked against the date, and signing uses the time. The board sets its clock again every 6 hours.

### HTTPS

To `https.warbletiot.com:443`. Every connection checks the server's certificate against four pinned Let's Encrypt roots and the host name; a connection gets 10 s, its TLS handshake 15 s, and an answer 20 s.

A message is `POST /ingest/{deviceId}` with the bytes as its body, and these headers:

| Header | When |
|---|---|
| `X-Chirp-Token` | always: the credential |
| `X-Chirp-Claim` | when the credential is a setup code (it starts `CHIRP-`), also after it became the token |
| `X-Chirp-Tag` | with a tag |
| `X-Chirp-Nonce`, `X-Chirp-Signature` | with a signing key: the time in milliseconds, strictly increasing, and the HMAC-SHA256 in hex over `<nonce> "\n" "tag=" <tag> "\n" <body>` (empty fields left empty) |

A command check is `POST /ingest/{deviceId}/down` with an empty body. With a signing key it carries a nonce and a signature over `"chirp-down\n" <deviceId> "\n" <nonce>`.

| Answer | What the board does |
|---|---|
| `202` | stored: `true` |
| `202 {"status":"unclaimed"}` | Warblet does not know the board: it stops (below) |
| `204` | no command waiting |
| `200` with a body | a command, handed to the command function; or, when the body starts `CHIRPKEY1 `, a new signing key, which is stored and used from then on. Only a sketch with a command function asks, so only it can get a key over the air; every other board, and every board over MQTT, gets its key only over USB, from the setup page |
| `401`, `403`, `404`, `429`, `5xx` | not stored; it waits (above). A `429` that carries `X-Chirp-Budget` is the day's storage budget; one without it is the rate. |

A reply cut short, or with neither a length nor chunking, is treated as no answer.

### MQTT

To `mqtt.warbletiot.com:8883` over TLS, with the same pinned roots, on Espressif's MQTT client. The client id and the username are the device id; the password is the credential. Keepalive is 60 s and every network wait 10 s. After a drop the client tries again by itself, about every 10 s plus the time a failed attempt takes.

| What | Topic | QoS |
|---|---|---|
| a message | `chirp/{deviceId}/up`, or `chirp/{deviceId}/up/{tag}` | 1 |
| commands | `chirp/{deviceId}/down` | 1 |

With a signing key, a message is the 32 raw HMAC-SHA256 bytes over `"\ntag=" <tag> "\n" <payload>` followed by the payload. The envelope has no nonce. The board waits up to 10 s for the broker's acknowledgement. The client does not send a message again on its own: its retransmit timeout (60 s) is longer than the 30 s a message stays in its outbox. Commands that arrive while the sketch is busy wait for it, up to 32 KB together; one that finds no room, or is cut off by a dropped session, is said on the console. A signing key reaches an MQTT board only over USB.

When the broker refuses the board's token, the board says so and tries again a minute later, doubling up to 10 min. When it refuses the connection because no device has the board's id (MQTT code 4), the board stops (below).

## Not on an account

When Warblet answers a message `202 {"status":"unclaimed"}`, or refuses the MQTT connection with code 4, the board's device was deleted or its setup code was never used. The board then:
- sends no messages and asks for no commands;
- leaves its Wi-Fi network (`WiFi.disconnect()`; the radio stays on) and says so once;
- stays stopped after a restart. It keeps a 16-character fingerprint of the refused setup, never the setup itself.

A new setup over USB (a new setup code, token, key or device id) starts it again; a Wi-Fi-only push does not. A board with typed settings starts again with new settings typed into `begin()` and uploaded. `state()` is `NOT_ON_ACCOUNT` while it is stopped.

## Limits

| What | Limit |
|---|---|
| a command | 8,192 bytes; a longer one is dropped and said |
| a tag | 1 to 32 characters: letters, digits, `_`, `.`, `-` |
| `sendTenths` | -3276.8 to 3276.7, one decimal |
| `sendNumber` | any value; `float` to 7 significant digits, `double` to 15 |
| commands waiting (MQTT) | 32 KB together |
| how often | command checks every 10 s (HTTPS); messages as often as your sketch sends them, within Warblet's own rate and storage limits |

## `Serial`

`begin()` starts `Serial` at 115200 baud. On ESP32 boards it first raises the receive buffer to 2 KB, so a setup push that arrives while the sketch is busy waits in the buffer. Call `board.begin()` before any `Serial.begin()` of your own. The library reads every byte that arrives on `Serial`, answers the lines that start with `CHIRP?` and `CHIRP+`, and discards the rest. It prints its own lines starting `warblet:`. A command is said with its size, never its words; a build with `-DWARBLET_VERBOSE` adds the first 60 characters, and a line for each request.

## Boards

| Board | Status |
|---|---|
| ESP32 boards on the Arduino core for ESP32 3.x | Supported. Run on a Seeed XIAO ESP32-S3 with core 3.3.12; built for a plain ESP32, the Arduino Nano ESP32 and the Cheap Yellow Display (ESP32-2432S028R) on the same core. |
| ESP32 boards on an older core (2.x) | Not tried. The library leaves MQTT out on a core older than ESP-IDF 5, and `Warblet board(Warblet::MQTT);` then says MQTT is not available. |
| Arduino UNO R4 WiFi, Raspberry Pi Pico W | Not supported yet: the build stops with a message. |
