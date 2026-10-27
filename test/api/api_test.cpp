// The library's public calls, as Warblet.h and docs/REFERENCE.md list them.
// Built and linked with the library (run_tests.sh), never run past main():
// every call, every overload and every name a sketch may use is taken here
// with its exact type, and called as a sketch calls it, so removing or
// changing one, or leaving one declared with no definition, stops the
// suite. Adding a call does not; add it here when the reference lists it.
#include "Warblet.h"

namespace {

void command(String) {}
void state(Warblet::State) {}

// Each overload by its exact type, as a sketch's call would pick it.
bool (Warblet::*const kSendNumberInt)(int, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendNumberLong)(long, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendNumberLongLong)(long long, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendNumberUnsigned)(unsigned int, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendNumberUnsignedLong)(unsigned long, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendNumberUnsignedLongLong)(unsigned long long, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendNumberFloat)(float, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendNumberDouble)(double, const char*) = &Warblet::sendNumber;
bool (Warblet::*const kSendTenths)(double, const char*) = &Warblet::sendTenths;
bool (Warblet::*const kSendText)(const char*, const char*) = &Warblet::sendText;
bool (Warblet::*const kSend)(const uint8_t*, size_t, const char*) = &Warblet::send;
void (Warblet::*const kBegin)() = &Warblet::begin;
void (Warblet::*const kBeginTyped)(const char*, const char*, const char*, const char*, const char*) = &Warblet::begin;
void (Warblet::*const kLoop)() = &Warblet::loop;
void (Warblet::*const kWait)(unsigned long) = &Warblet::wait;
void (Warblet::*const kDemo)(const char*) = &Warblet::demo;
void (Warblet::*const kOnCommand)(Warblet::CommandHandler) = &Warblet::onCommand;
void (Warblet::*const kCheckCommandsEvery)(unsigned long) = &Warblet::checkCommandsEvery;
void (Warblet::*const kRestartWhenStuck)(unsigned long) = &Warblet::restartWhenStuck;
bool (Warblet::*const kConnected)() = &Warblet::connected;
Warblet::State (Warblet::*const kState)() const = &Warblet::state;
int (Warblet::*const kAnswer)() const = &Warblet::answer;
const char* (Warblet::*const kDeviceId)() const = &Warblet::deviceId;
void (Warblet::*const kOnState)(Warblet::StateHandler) = &Warblet::onState;

// The handler types are the ones the reference shows: function(String text)
// and function(Warblet::State state).
Warblet::CommandHandler const kCommandHandler = command;
Warblet::StateHandler const kStateHandler = state;

// Every state and transport by name.
const Warblet::State kStates[] = {Warblet::NOT_SET_UP,       Warblet::JOINING_WIFI, Warblet::NO_WIFI,
                                  Warblet::REACHING_WARBLET, Warblet::ANSWERED,     Warblet::REFUSED,
                                  Warblet::NO_ANSWER,        Warblet::NOT_ON_ACCOUNT};
const Warblet::Transport kTransports[] = {Warblet::HTTPS, Warblet::MQTT};

}  // namespace

// The calls as a sketch writes them, with the optional arguments left out.
void sketch() {
  Warblet board;
  Warblet mqtt(Warblet::MQTT);
  board.demo("demo-example");
  board.onCommand(command);
  board.onState(state);
  board.checkCommandsEvery(5000);
  board.restartWhenStuck(600000);
  board.begin();
  mqtt.begin("wifi", "password", "device-id", "token");
  mqtt.begin("wifi", "password", "device-id", "token", "key");
  board.loop();
  board.wait(1000);
  board.sendNumber(1);
  board.sendNumber(1L);
  board.sendNumber(1LL);
  board.sendNumber(1u);
  board.sendNumber(1UL);
  board.sendNumber(1ULL);
  board.sendNumber(1.5f);
  board.sendNumber(1.5);
  board.sendNumber(21.5, "tag");
  board.sendTenths(20.1);
  board.sendText("text");
  const uint8_t bytes[1] = {1};
  board.send(bytes, sizeof bytes);
  bool up = board.connected();
  const Warblet& view = board;
  Warblet::State now = view.state();
  int status = view.answer();
  const char* id = view.deviceId();
  (void)up;
  (void)now;
  (void)status;
  (void)id;
}

// Linked, so each call above must have its definition; sketch() itself is
// never run (it would need a board).
int main(int argc, char**) {
  if (argc > 1000) sketch();
  return 0;
}
