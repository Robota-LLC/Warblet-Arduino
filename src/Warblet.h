// Warblet for Arduino: send readings to your Warblet dashboard and react to
// its buttons, from a short sketch.
//
//   #include <Warblet.h>
//   Warblet board;
//   void setup() { board.begin(); board.onCommand(command); }
//   void loop()  { board.sendNumber(analogRead(A0)); board.wait(60000); }
//
// Wi-Fi and the device's settings come from the Warblet setup page over the
// USB cable, and are kept on the board. See docs/REFERENCE.md.
//
// The library uses Serial: it starts it at 115200 baud and reads every byte
// that arrives, answering the lines that start with CHIRP and discarding the
// rest. A sketch that reads Serial itself will not see those bytes.
#pragma once

#include <Arduino.h>

#include "warblet/core/prov.h"
#include "warblet/core/wire.h"

class Warblet {
 public:
  // The function the dashboard's commands are handed to, as text.
  typedef void (*CommandHandler)(String command);

  // How the board talks to Warblet. HTTPS asks for commands every 10 s;
  // MQTT keeps one session open over TLS, so commands arrive at once
  // (ESP32 boards: it uses Espressif's own MQTT client, in the core).
  enum Transport { HTTPS, MQTT };

  // Warblet board; is HTTPS. Warblet board(Warblet::MQTT); is MQTT; only a
  // sketch that names a transport carries the MQTT code.
  Warblet();
  explicit Warblet(Transport transport);

  // For a demo's sketch: the demo it is, as the setup page names it
  // ("demo-cyd-desk-display-arduino"). The board then tells the setup page
  // which demo it runs. Call it before begin().
  void demo(const char* slug);

  // Start with the settings the setup page stored on the board over USB.
  // Returns at once; loop() joins Wi-Fi when the settings are there.
  void begin();

  // Or start with settings typed into the sketch. The USB setup page is
  // then not answered: the sketch is where the settings live.
  // signingKey is optional (the base64 key from the device page).
  void begin(const char* wifiName, const char* wifiPassword, const char* deviceId,
             const char* deviceToken, const char* signingKey = nullptr);

  // Call this every time loop() runs. It answers the setup page, keeps
  // Wi-Fi up, checks for commands, and calls your command function.
  void loop();

  // Use this wherever you would use delay(): it waits ms milliseconds while
  // still answering the setup page, keeping Wi-Fi up and collecting commands.
  // A request that is running when the time is up finishes first, so it can
  // take longer than ms. Called inside your command function, it only keeps
  // answering the setup page.
  void wait(unsigned long ms);

  // Send a reading. Over HTTPS each returns true only when Warblet answered
  // that it stored the message on this device; over MQTT, when the broker
  // acknowledged it. After a refusal, or no answer, reports wait a while
  // (said once on the console) and return false until then.
  // A tag (optional) says what the message is, like "button" or "heartbeat".
  //
  // sendNumber sends the number as text ("21.53", "101325", "-5.0"), any
  // size; the decoder is one line: {"value": float(str(payload))}.
  bool sendNumber(int value, const char* tag = nullptr);
  bool sendNumber(long value, const char* tag = nullptr);
  bool sendNumber(long long value, const char* tag = nullptr);
  bool sendNumber(unsigned int value, const char* tag = nullptr);
  bool sendNumber(unsigned long value, const char* tag = nullptr);
  bool sendNumber(unsigned long long value, const char* tag = nullptr);
  bool sendNumber(float value, const char* tag = nullptr);   // 7 significant digits
  bool sendNumber(double value, const char* tag = nullptr);  // 15 significant digits
  // sendTenths sends the public temperature demos' compact format: two
  // bytes, value x 10, from -3276.8 to 3276.7, one decimal.
  bool sendTenths(double value, const char* tag = nullptr);
  bool sendText(const char* text, const char* tag = nullptr);
  bool send(const uint8_t* bytes, size_t length, const char* tag = nullptr);

  // Run this function when a command arrives from the dashboard. Over HTTPS
  // the board asks for commands only once a function is set.
  void onCommand(CommandHandler handler);

  // Over HTTPS, how often to ask for commands: every 10 s unless set here,
  // and never more often than every 5 s.
  void checkCommandsEvery(unsigned long ms);

  // A last resort for a board on its own: after a request gets no answer at
  // all while Wi-Fi says it is connected, restart the board if nothing has
  // answered for ms (10 minutes is a good value). Any answer counts, a
  // refusal too, and so does the start; it is never used with no settings
  // or once stopped. A restart loses what the sketch had in memory.
  void restartWhenStuck(unsigned long ms);

  // True while Wi-Fi is up and the settings are complete (over MQTT, also
  // while the session is open).
  bool connected();

  // What the board is doing, for a screen or a light to show.
  enum State {
    NOT_SET_UP,        // no settings: waiting for the setup page over USB
    JOINING_WIFI,      // joining a stored Wi-Fi network
    NO_WIFI,           // no stored network joined; it tries again every 15 s
    REACHING_WARBLET,  // Wi-Fi joined; Warblet has not answered yet
    ANSWERED,          // Warblet took the latest request (2xx): answer() says which
    REFUSED,           // Warblet turned the board away (401, 403, 404, 429...): answer() says why
    NO_ANSWER,         // no answer (no name, no TLS, nothing back, no clock), or a 5xx fault: answer() has the 5xx
    NOT_ON_ACCOUNT,    // Warblet does not know this board: stopped until a new setup
  };
  State state() const { return state_; }
  // The device id the setup page gave this board, "" before it has one.
  const char* deviceId() const { return settings_.hwId; }
  // The HTTP status of Warblet's latest answer: 200, 202 or 204 when all is
  // well (ANSWERED); 401 refused, 429 too many (REFUSED); 5xx a fault on
  // Warblet's side (NO_ANSWER); 0 before any. Over MQTT, 200 while the session is open and 401 when the
  // broker refuses the token.
  int answer() const { return answer_; }
  // Run this function each time state() or answer() changes, and once at
  // begin(). It runs inside the library's work, even while Wi-Fi joins, so
  // keep it short: show the state, and return.
  typedef void (*StateHandler)(State state);
  void onState(StateHandler handler);

 private:
  bool sendNumberText(const char* text, size_t length, const char* tag);
  bool ensureOnline();
  bool serviceSetup();
  void joinWifi();
  bool post(const uint8_t* bytes, size_t length, const char* tag);
  bool publishMqtt(const uint8_t* bytes, size_t length, const char* tag);
  void serviceMqtt();
  void restartMqtt();
  void deliverCommand(size_t len);
  const char* mqttHost() const;
  uint8_t* commandBuffer(size_t* cap);
  // The MQTT code (WarbletMqtt.cpp) is reached only through this table, and
  // only the Warblet(Transport) constructor names it: the linker leaves it,
  // and Espressif's MQTT client, out of a sketch that uses HTTPS.
  struct MqttHooks {
    void (Warblet::*service)();
    bool (Warblet::*publish)(const uint8_t* bytes, size_t length, const char* tag);
    void (Warblet::*restart)();
    bool (*connected)();
  };
  static const MqttHooks kMqttHooks;
  int poll();  // one command check; returns its HTTP status (-1: no answer)
  int exchange(size_t headLen, const uint8_t* body, size_t bodyLen, uint8_t* answer, size_t answerCap);
  const char* host() const;
  bool setupQuiet() const;
  void stopUnclaimed();
  void checkRefused();
  void sayStopped();
  void setState(State state, int answer);
  void answered(int status);  // after a request: ANSWERED with its status, or NO_ANSWER
  void say(const char* text);

  warblet::Settings settings_;
  warblet::Settings scratch_;
  warblet::ProvResponder setup_;
  warblet::NonceClock nonces_;
  warblet::HttpResponse response_;
  warblet::Backoff commandChecks_;
  warblet::Backoff reports_;  // after a refused or unanswered report, the next waits
  warblet::Backoff mqttRefusals_;
  CommandHandler handler_ = nullptr;
  StateHandler stateHandler_ = nullptr;
  State state_ = NOT_SET_UP;
  int answer_ = 0;
  unsigned long stuckMs_ = 0;      // restartWhenStuck(); 0: never
  unsigned long lastAnswerMs_ = 0;  // when Warblet last answered at all
  char head_[warblet::kHttpHeadMax];
  uint8_t io_[128];
  uint8_t answerBody_[64];
  uint8_t key_[32];
  bool hasKey_ = false;
  bool typed_ = false;
  bool started_ = false;
  bool inLoop_ = false;
  bool stopped_ = false;      // Warblet answered "unclaimed": stopped until a new setup
  bool stoppedSaid_ = false;
  const MqttHooks* mqtt_ = nullptr;  // null: HTTPS
  bool mqttStarted_ = false;
  bool mqttMissingSaid_ = false;
  bool mqttNoSessionSaid_ = false;
  int mqttLastNews_ = 0;
  unsigned long mqttRetryAt_ = 0;
  bool waitingSaid_ = false;
  bool badSlug_ = false;  // demo() was given a slug the setup page cannot use
  bool joinFailSaid_ = false;
  bool clockSet_ = false;
  bool noClockSaid_ = false;
  bool headSaid_ = false;
  unsigned long nextPoll_ = 0;
  int taken_ = 0;  // commands taken one after another this round
  unsigned long lastJoin_ = 0;
  unsigned long lastClock_ = 0;
  unsigned long lastRefused_ = 0;
  unsigned long quietSince_ = 0;
  bool quiet_ = false;
  unsigned long holdSince_ = 0;
  unsigned long holdMs_ = 0;
};
