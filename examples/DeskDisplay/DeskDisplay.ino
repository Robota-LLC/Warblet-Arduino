// Desk display for the Cheap Yellow Display (ESP32-2432S028R).
// Type in the Message box on its Warblet device page and the words fill the
// screen. Tap the screen and Warblet hears that the message was seen.
// A routine can send lines for the lower part of the screen ("info:..."); the
// one that comes with this example (desk-display-disk.star) sends how full
// your computers' disks are, and each computer keeps its own line.
//
// Choose the board "ESP32-2432S028R CYD" and upload. Then set it up over USB
// at warbletiot.com/flash; the screen says what to do. Paste decoder.star
// into the device's spec, and add a Message box and a "Clear" button (text
// "clear") to its dashboard. screen.h has the settings for a wrong picture.
#include <WiFi.h>
#include <Warblet.h>
#include <ctype.h>

#include <vector>

#include "screen.h"

// Tells the setup page which demo this is. Only a published demo's own
// sketch says this: delete this line and board.demo(DEMO) in a sketch you
// change, or the setup page sets your board up as this demo, with its spec,
// decoder and dashboard.
const char* DEMO = "demo-cyd-desk-display-arduino";
const unsigned long REPORT_EVERY_MS = 30000;  // a report with the Wi-Fi signal; it also shows Warblet the board is there
const unsigned long CHECK_EVERY_MS = 5000;    // how often to ask Warblet for new messages
const unsigned long STUCK_MS = 600000;        // Wi-Fi up, yet no answer at all this long: restart

// Tags say what a message is. The first report after a start says hello, so
// the routine can give a display that just started the last info line again.
const char* HELLO_TAG = "hello";
const char* SEEN_TAG = "seen";
const int INFO_ROWS = 3;  // lines for the routine: one per computer, the one heard from longest ago goes first

// Colours: red, green, blue, each 0-255.
const uint16_t BLACK = screen::rgb(0, 0, 0);
const uint16_t BAR = screen::rgb(30, 30, 40);
const uint16_t WHITE = screen::rgb(255, 255, 255);
const uint16_t GREY = screen::rgb(130, 130, 130);
const uint16_t YELLOW = screen::rgb(255, 210, 0);
const uint16_t GREEN = screen::rgb(0, 210, 90);
const uint16_t AMBER = screen::rgb(255, 150, 0);
const uint16_t RED = screen::rgb(240, 50, 50);

// Where things go on the 320 x 240 screen: a strip at the top, the message,
// three lines for the routine's info, two lines at the bottom.
const int MESSAGE_TOP = 24, MESSAGE_HEIGHT = 108;
const int INFO_TOP = 136;
const int FOOTER_TOP = 200;

Warblet board;

// What the screen shows.
String message = "";          // what was typed on the dashboard
unsigned long messageAt = 0;  // when it came (millis)
struct Info {
  String name;                // the computer, or the line itself
  String line;
  unsigned long at;
};
Info infos[INFO_ROWS];        // the routine's lines, one per computer
int infoCount = 0;
enum Seen { NOT_YET, SENDING, SENT, WAITING };  // WAITING: tapped while offline
Seen seen = NOT_YET;
bool saidHello = false;
unsigned long lastReport = 0;
bool reported = false;
unsigned long lastSeenTry = 0;
bool pressed = false;

// What each line of the screen shows now, so a line is drawn only when it changes.
const int LINES = 6;
int drawnY[LINES];
String drawnWords[LINES];
uint16_t drawnColour[LINES];
int drawnCount = 0;

// The letters the screen can show: one per character, '?' for any it has
// no shape for (accents, emoji). A newline or a tab separates words, as in
// the MicroPython desk display.
String letters(const String& words) {
  String out = "";
  for (unsigned i = 0; i < words.length(); i++) {
    unsigned char c = (unsigned char)words[i];
    if (c >= 0x80 && c < 0xC0) continue;  // the rest of a character already counted
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    out += (c >= ' ' && c <= '~') ? (char)c : '?';
  }
  return out;
}

// How long ago, in a few words.
String age(unsigned long then) {
  unsigned long s = (millis() - then) / 1000;
  if (s < 60) return "just now";
  if (s < 3600) return String(s / 60) + " min ago";
  if (s < 48UL * 3600) return String(s / 3600) + " h ago";
  return String(s / 86400) + " days ago";
}

// Split words into lines of at most width letters. A word longer than a
// line is cut.
std::vector<String> wrap(const String& words, unsigned width) {
  std::vector<String> lines;
  String line = "";
  unsigned i = 0;
  while (i < words.length()) {
    while (i < words.length() && isspace((unsigned char)words[i])) i++;
    unsigned start = i;
    while (i < words.length() && !isspace((unsigned char)words[i])) i++;
    String word = words.substring(start, i);
    if (word.length() == 0) break;
    while (word.length() > width) {
      if (line.length() > 0) {
        lines.push_back(line);
        line = "";
      }
      lines.push_back(word.substring(0, width));
      word = word.substring(width);
    }
    if (line.length() == 0) {
      line = word;
    } else if (line.length() + 1 + word.length() <= width) {
      line += " " + word;
    } else {
      lines.push_back(line);
      line = word;
    }
  }
  if (line.length() > 0) lines.push_back(line);
  return lines;
}

// One line of big letters (16 pixels each) at height y, drawn only when it
// changes, so the screen does not flicker. The strip at the top starts 4
// pixels in, so it holds 19 letters; other lines hold 20.
void put(int y, const String& words, uint16_t colour, uint16_t background = BLACK) {
  int slot = 0;
  while (slot < drawnCount && drawnY[slot] != y) slot++;
  if (slot < drawnCount && drawnWords[slot] == words && drawnColour[slot] == colour) return;
  if (slot == LINES) return;  // cannot happen: the screen has six lines
  if (slot == drawnCount) drawnCount++;
  drawnY[slot] = y;
  drawnWords[slot] = words;
  drawnColour[slot] = colour;
  int x = background == BAR ? 4 : 0;
  screen::fill(0, y, screen::WIDTH, 18, background);
  screen::text(x, y + 1, letters(words).substring(0, (screen::WIDTH - x) / 16).c_str(), 2, colour, background);
}

// The strip at the top: how the board is doing.
void showStatus(const char* words, uint16_t colour) { put(0, words, colour, BAR); }

// The middle: the message, as big as it fits, with no word cut in two while
// smaller letters would keep it whole.
void showMessage(const String& words, uint16_t colour = WHITE) {
  screen::fill(0, MESSAGE_TOP, screen::WIDTH, MESSAGE_HEIGHT, BLACK);
  String shown = letters(words);
  unsigned longest = 0, run = 0;  // the longest word
  for (unsigned i = 0; i <= shown.length(); i++) {
    if (i == shown.length() || isspace((unsigned char)shown[i])) {
      longest = run > longest ? run : longest;
      run = 0;
    } else {
      run++;
    }
  }
  int size = 4, pitch = 0;
  std::vector<String> lines;
  for (size = 4; size >= 1; size--) {
    pitch = 9 * size + 1;  // the letters, and a gap between lines
    unsigned width = screen::WIDTH / (8 * size);
    lines = wrap(shown, width);
    if ((int)lines.size() * pitch <= MESSAGE_HEIGHT && longest <= width) break;
  }
  if (size < 1) size = 1;
  for (int i = 0; i < (int)lines.size() && i < MESSAGE_HEIGHT / pitch; i++) {
    screen::text(0, MESSAGE_TOP + i * pitch, lines[i].c_str(), size, colour, BLACK);
  }
}

// Keep the routine's line, one per computer. A disk line reads
// "87% full: raspberrypi /": the computer's name comes after "full:", and
// the screen shows it as "87% raspberrypi /" so it fits one line.
void addInfo(String line) {
  String name = line;  // any other line is its own
  int full = line.indexOf(" full: ");
  if (full >= 0) {
    String rest = line.substring(full + 7);
    int space = rest.indexOf(' ');
    name = space >= 0 ? rest.substring(0, space) : rest;
    line = line.substring(0, full) + " " + rest;
  }
  for (int i = 0; i < infoCount; i++) {
    if (infos[i].name == name) {
      infos[i].line = line;
      infos[i].at = millis();
      return;
    }
  }
  if (infoCount == INFO_ROWS) {  // full: the one heard from longest ago goes
    int oldest = 0;
    for (int i = 1; i < infoCount; i++) {
      if (infos[i].at < infos[oldest].at) oldest = i;
    }
    for (int i = oldest; i < infoCount - 1; i++) infos[i] = infos[i + 1];
    infoCount--;
  }
  infos[infoCount++] = {name, line, millis()};
}

// Below it: the routine's lines, one per computer.
void showInfo() {
  for (int i = 0; i < INFO_ROWS; i++) {
    put(INFO_TOP + 18 * i, i < infoCount ? letters(infos[i].line).substring(0, 20) : "", YELLOW);
  }
}

// The bottom: when the message came, and whether it was seen.
void showFooter() {
  if (message.length() == 0) {
    put(FOOTER_TOP, "", GREY);
    put(FOOTER_TOP + 20, "", WHITE);
    return;
  }
  put(FOOTER_TOP, "Message " + age(messageAt), GREY);
  const char* words[] = {"Tap: I've seen it", "Seen: sending", "Seen: sent", "Seen: not sent yet"};
  put(FOOTER_TOP + 20, words[seen], seen == SENT ? GREEN : WHITE);
}

// If the screen did not say which chip it is, say so in small letters
// between the disk lines and the footer: with the wrong guess the picture
// is mirrored or its colours are wrong, and screen.h has the settings.
void showGuess() {
  if (screen::GUESSED) {
    screen::text(0, INFO_TOP + 18 * INFO_ROWS + 1, "Screen chip guessed: see screen.h", 1, GREY, BLACK);
  }
}

void showWaiting() {
  showMessage(String("Type a message for ") + board.deviceId() + " on your Warblet dashboard.", GREY);
}

// Warblet does not know this board: its device was deleted, or it was
// never added. Nothing it sends would reach an account, so the library has
// stopped it: no reports, no checks for messages, off its Wi-Fi network. Only USB setup
// (warbletiot.com/flash) starts it again.
void showNoAccount() {
  message = "";
  infoCount = 0;
  seen = NOT_YET;
  showStatus("Not on an account", AMBER);
  screen::fill(0, MESSAGE_TOP, screen::WIDTH, screen::HEIGHT - MESSAGE_TOP, BLACK);
  for (int slot = 0; slot < drawnCount; slot++) {  // only the strip is still on the screen
    if (drawnY[slot] != 0) drawnWords[slot] = "(cleared)";
  }
  showMessage("Set me up at warbletiot.com/flash");  // no id, and nothing an account sent
  showGuess();
}

// The library calls this whenever the board's state changes.
void showState(Warblet::State state) {
  static Warblet::State before = Warblet::NOT_SET_UP;
  if (state == Warblet::NOT_ON_ACCOUNT) {
    showNoAccount();
  } else if (state == Warblet::NOT_SET_UP) {
    showStatus("Not set up", RED);
    showMessage("Set me up at warbletiot.com/flash");
  } else {
    if (before == Warblet::NOT_SET_UP || before == Warblet::NOT_ON_ACCOUNT) {
      showWaiting();  // a new setup has just been stored
      showInfo();
      showFooter();
    }
    if (state == Warblet::JOINING_WIFI) showStatus("Joining Wi-Fi", AMBER);
    if (state == Warblet::NO_WIFI) showStatus("No Wi-Fi, retrying", AMBER);
    if (state == Warblet::REACHING_WARBLET) showStatus("Reaching Warblet", AMBER);
    if (state == Warblet::NO_ANSWER) showStatus("Warblet: no answer", AMBER);
    if (state == Warblet::ANSWERED) showStatus("Connected", GREEN);
    if (state == Warblet::REFUSED) {
      // Turned away: too many is a pause; anything else names its code. (A
      // fault on Warblet's side is NO_ANSWER, shown above.)
      if (board.answer() == 429) {
        showStatus("Warblet: slow down", AMBER);
      } else {
        showStatus((String("Warblet refused ") + board.answer()).c_str(), RED);
      }
    }
  }
  before = state;
}

// Text from Warblet as it came; bytes that are not text keep the letters the
// screen can show. Spaces at either end go.
String textOf(const String& raw) {
  String text = raw;
  bool valid = true;
  for (unsigned i = 0; i < raw.length() && valid;) {
    unsigned char c = (unsigned char)raw[i];
    int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    valid = more >= 0 && i + more < raw.length();
    for (int k = 1; valid && k <= more; k++) valid = ((unsigned char)raw[i + k] >> 6) == 2;
    i += more + 1;
  }
  if (!valid) {
    text = "";
    for (unsigned i = 0; i < raw.length(); i++) {
      unsigned char c = (unsigned char)raw[i];
      text += (c >= 32 && c < 127) ? (char)c : '?';
    }
  }
  text.trim();
  return text;
}

// A message from Warblet: "clear" clears the screen, "info:..." sets the
// lower lines, anything else is a message to show big.
void onCommand(String command) {
  if (command == "clear") {
    message = "";
    seen = NOT_YET;
    showWaiting();
  } else if (command.startsWith("info:")) {
    addInfo(textOf(command.substring(5)));
    showInfo();
  } else {
    message = textOf(command);
    messageAt = millis();
    seen = NOT_YET;
    showMessage(message);
  }
  showFooter();
}

// The first n characters of words, as JSON text.
String jsonText(const String& words, int n) {
  String out = "\"";
  for (unsigned i = 0; i < words.length(); i++) {
    unsigned char c = (unsigned char)words[i];
    if (c < 0x80 || c >= 0xC0) {
      if (n-- == 0) break;  // a new character
    }
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20) {
      char escaped[8];
      snprintf(escaped, sizeof escaped, "\\u%04x", c);
      out += escaped;
    } else {
      out += (char)c;
    }
  }
  return out + "\"";
}

// What a message got, with the memory left, on the console: the line the
// MicroPython desk display prints, so a long run on this image shows the
// memory over time.
void saySent(const char* what, bool ok) {
  Serial.print("desk: ");
  Serial.print(what);
  Serial.print(ok ? " stored, " : " not stored, ");
  Serial.print((unsigned)ESP.getFreeHeap());
  Serial.print(" bytes free, largest block ");
  Serial.println((unsigned)ESP.getMaxAllocHeap());
}

// Tell Warblet the message on screen was seen.
void sendSeen() {
  seen = SENDING;
  showFooter();
  bool ok = board.sendText(("{\"seen\": " + jsonText(message, 60) + "}").c_str(), SEEN_TAG);
  saySent("seen", ok);
  seen = ok ? SENT : WAITING;  // WAITING is tried again while the board is online
  lastSeenTry = millis();
  showFooter();
}

// A report with the Wi-Fi signal every REPORT_EVERY_MS; the first says hello.
void reportWhenDue() {
  if (!board.connected() || (reported && millis() - lastReport < REPORT_EVERY_MS)) return;
  String body = "{\"wifi_dbm\": " + String(WiFi.RSSI()) + "}";
  bool ok = board.sendText(body.c_str(), saidHello ? nullptr : HELLO_TAG);
  saySent(saidHello ? "report" : "hello", ok);
  saidHello = saidHello || ok;
  lastReport = millis();
  reported = true;
}

// A tap says the message was seen: once per message.
void watchTouch() {
  bool touch = screen::touched();
  if (touch && !pressed && message.length() > 0 && (seen == NOT_YET || seen == WAITING)) {
    if (board.connected()) {
      sendSeen();
    } else {
      seen = WAITING;  // sent once the board is back online
      showFooter();
    }
  }
  pressed = touch;
}

void setup() {
  const char* found = screen::start();
  showGuess();
  board.demo(DEMO);
  board.onState(showState);
  board.onCommand(onCommand);
  board.checkCommandsEvery(CHECK_EVERY_MS);
  board.restartWhenStuck(STUCK_MS);
  board.begin();  // shows the first state
  Serial.println(found);
  Warblet::State state = board.state();
  if (state != Warblet::NOT_SET_UP && state != Warblet::NOT_ON_ACCOUNT) showWaiting();
}

void loop() {
  Warblet::State state = board.state();
  if (state != Warblet::NOT_SET_UP && state != Warblet::NOT_ON_ACCOUNT) {
    reportWhenDue();
    if (seen == WAITING && board.connected() && millis() - lastSeenTry >= CHECK_EVERY_MS) sendSeen();
    watchTouch();
    showFooter();  // the ages move on
  }
  board.wait(50);  // answers USB setup, keeps Wi-Fi up, asks for messages
}
