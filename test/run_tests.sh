#!/bin/sh
# Build and run the host tests. No board, no network, nothing installed.
#
#   REF   the Warblet-IoT v1.0.2 clone (read-only), whose own code is used,
#         unmodified, as oracles: the bare-metal demo's sha256.c and
#         chirp_sign.c; the HTTPS camera demo's chirp_send.c and chirp_sign.c
#         (with ESP-IDF stand-ins in test/demo_cam); and the MicroPython
#         demos' chirp_sign.py and chirp_prov.py (run on CPython by
#         test/oracle.py and test/prov_oracle.py).
#   CXX/CC  the host compilers.
#
# Run from the repository root: sh test/run_tests.sh
set -e
REF=${REF:-../wiot-ref}
CXX=${CXX:-g++}
CC=${CC:-gcc}
PYTHON=${PYTHON:-python}
OUT=${OUT:-test/build}
mkdir -p "$OUT"

"$CXX" --version | head -1
"$CC" --version | head -1

if [ -f "$REF/nucleo-wba65-baremetal/src/chirp_sign.c" ]; then
  echo "reference: $REF at $(git -C "$REF" describe --tags --always 2>/dev/null || echo unknown)"
  "$CC" -std=c11 -Wall -Wextra -O2 -I"$REF/nucleo-wba65-baremetal/include" -c \
    "$REF/nucleo-wba65-baremetal/src/sha256.c" -o "$OUT/demo_sha256.o"
  "$CC" -std=c11 -Wall -Wextra -O2 -I"$REF/nucleo-wba65-baremetal/include" -c \
    "$REF/nucleo-wba65-baremetal/src/chirp_sign.c" -o "$OUT/demo_chirp_sign.o"
  CAM="$REF/freenove-wrover-cam/main"
  "$CC" -std=c11 -Wall -Wextra -O2 -Itest/demo_cam/stub -I"$REF/nucleo-wba65-baremetal/include" -c \
    test/demo_cam/stub_mbedtls.c -o "$OUT/cam_mbedtls.o"
  "$CC" -std=c11 -Wall -O2 -Itest/demo_cam/stub -I"$CAM" -c test/demo_cam/stub_idf.c -o "$OUT/cam_idf.o"
  "$CC" -std=c11 -Wall -O2 -Itest/demo_cam/stub -I"$CAM" -c test/demo_cam/cam_oracle.c -o "$OUT/cam_oracle.o"
  "$CC" -std=c11 -Wall -O2 -Itest/demo_cam/stub -I"$CAM" -c "$CAM/chirp_sign.c" -o "$OUT/cam_sign.o"
  DEMO="-DWITH_DEMO_C -DWITH_DEMO_CAM $OUT/demo_sha256.o $OUT/demo_chirp_sign.o \
    $OUT/cam_mbedtls.o $OUT/cam_idf.o $OUT/cam_oracle.o $OUT/cam_sign.o"
  if "$PYTHON" --version >/dev/null 2>&1; then
    "$PYTHON" test/oracle.py "$REF" test/vectors.txt
    "$PYTHON" test/prov_oracle.py "$REF" test/prov_vectors.txt
  else
    echo "python not found: using the committed test/vectors.txt and test/prov_vectors.txt"
  fi
else
  echo "reference clone not found at $REF: demo-c and demo-cam skipped, committed vectors used"
  DEMO=""
fi

# -Werror on the library's own files: they must stay warning-free.
"$CXX" -std=c++11 -Wall -Wextra -Wshadow -Werror -O2 -c src/warblet/core/sha256.cpp -o "$OUT/sha256.o"
"$CXX" -std=c++11 -Wall -Wextra -Wshadow -Werror -O2 -c src/warblet/core/text.cpp -o "$OUT/text.o"
"$CXX" -std=c++11 -Wall -Wextra -Wshadow -Werror -O2 -c src/warblet/core/sign.cpp -o "$OUT/sign.o"
"$CXX" -std=c++11 -Wall -Wextra -Wshadow -Werror -O2 -c src/warblet/core/wire.cpp -o "$OUT/wire.o"
"$CXX" -std=c++11 -Wall -Wextra -Wshadow -Werror -O2 -c src/warblet/core/json.cpp -o "$OUT/json.o"
"$CXX" -std=c++11 -Wall -Wextra -Wshadow -Werror -O2 -c src/warblet/core/prov.cpp -o "$OUT/prov.o"
# shellcheck disable=SC2086
"$CXX" -std=c++11 -Wall -Wextra -O2 -o "$OUT/host_test" test/host_test.cpp \
  "$OUT/sha256.o" "$OUT/text.o" "$OUT/sign.o" "$OUT/wire.o" "$OUT/json.o" "$OUT/prov.o" $DEMO
"$OUT/host_test" test/vectors.txt test/prov_vectors.txt

# The sketch-facing layer, run on the PC against stand-in Arduino headers and a
# fake server (test/sim). This is a logic check, not a board compile.
"$CXX" -std=c++11 -Wall -Wextra -O1 -Itest/sim -Isrc -o "$OUT/sim_test" \
  test/sim/sim_test.cpp test/sim/fake_board.cpp test/sim/sketch_light.cpp src/Warblet.cpp src/WarbletMqtt.cpp \
  src/warblet/core/sha256.cpp src/warblet/core/text.cpp src/warblet/core/sign.cpp \
  src/warblet/core/wire.cpp src/warblet/core/json.cpp src/warblet/core/prov.cpp
"$OUT/sim_test"

# The ESP32 section of src/WarbletBoard.cpp, unmodified, against stand-ins of
# the ESP32 core and Espressif's MQTT client (test/esp32), driven with the
# events the real client produces. Not a board compile either.
"$CXX" -std=gnu++11 -Wall -Wextra -O1 -DARDUINO_ARCH_ESP32 -Itest/esp32 -Itest/sim -Isrc -o "$OUT/board_test"   test/esp32/board_test.cpp test/esp32/esp32_stubs.cpp src/WarbletBoard.cpp
"$OUT/board_test"

# Every example, run as it is on the simulated board (test/sim/example_run.cpp):
# what it sends is recorded, and its own decoder.star must read each message
# back as the value the example meant (test/run_decoders.py, on CPython).
for pair in SensorReading:EX_SENSOR Button:EX_BUTTON LightByCommand:EX_LIGHT \
            LightByCommandMqtt:EX_LIGHT_MQTT TwoBoards:EX_TWO; do
  ex=${pair%%:*}
  "$CXX" -std=c++11 -Wall -Wextra -O1 -Itest/sim -Isrc -I. -D"${pair#*:}" -DEXAMPLE_INO="\"examples/$ex/$ex.ino\"" \
    -o "$OUT/example_$ex" test/sim/example_run.cpp test/sim/fake_board.cpp src/Warblet.cpp src/WarbletMqtt.cpp \
    src/warblet/core/sha256.cpp src/warblet/core/text.cpp src/warblet/core/sign.cpp \
    src/warblet/core/wire.cpp src/warblet/core/json.cpp src/warblet/core/prov.cpp
  "$OUT/example_$ex" record "$OUT/sent_$ex.txt"
  # Every request refused (401, 403) or failing (500) for ten minutes: the
  # examples that say hello until it is stored must not flood Warblet.
  for status in 401 403 500; do "$OUT/example_$ex" flood $status; done
  # The examples that say hello until it is stored: after 10 and 20 minutes
  # of 401s, a corrected token pushed over USB gets the hello stored at once.
  case $ex in
    LightByCommand|LightByCommandMqtt|TwoBoards)
      for minutes in 10 20; do "$OUT/example_$ex" corrected $minutes; done ;;
  esac
  if "$PYTHON" --version >/dev/null 2>&1; then
    "$PYTHON" test/run_decoders.py "examples/$ex" "$OUT/sent_$ex.txt"
  else
    echo "python not found: $ex's decoder not run"
  fi
done
# The DeskDisplay example: its screen driver against a fake SPI bus with each
# kind of screen chip behind it, and the sketch with a fake screen and touch
# on the simulated board (test/cyd).
"$CXX" -std=c++11 -Wall -Wextra -O1 -Itest/cyd -Itest/sim -Iexamples/DeskDisplay -o "$OUT/screen_test"   test/cyd/screen_test.cpp examples/DeskDisplay/screen.cpp
"$OUT/screen_test"
"$CXX" -std=c++11 -Wall -Wextra -O1 -Itest/cyd -Itest/sim -Isrc -Iexamples/DeskDisplay -o "$OUT/desk_display_test"   test/cyd/desk_display_test.cpp test/cyd/fake_screen.cpp test/sim/fake_board.cpp src/Warblet.cpp src/WarbletMqtt.cpp   src/warblet/core/sha256.cpp src/warblet/core/text.cpp src/warblet/core/sign.cpp   src/warblet/core/wire.cpp src/warblet/core/json.cpp src/warblet/core/prov.cpp
"$OUT/desk_display_test"

# DeskDisplay's CHIP setting: a name it does not know stops the build, rather
# than quietly using the ST7789's settings.
mkdir -p "$OUT/chip"
for chip in "" ILI9341 ST7789 ST7798 ili9341; do
  sed "s/^constexpr const char\* CHIP = \"[^\"]*\";/constexpr const char* CHIP = \"$chip\";/" \
    examples/DeskDisplay/screen.h > "$OUT/chip/screen.h"
  grep -q "CHIP = \"$chip\";" "$OUT/chip/screen.h" || { echo "FAIL  CHIP could not be set to \"$chip\""; exit 1; }
  printf '#include "screen.h"\nint main() { return 0; }\n' > "$OUT/chip/main.cpp"
  if "$CXX" -std=c++11 -fsyntax-only -Itest/sim -I"$OUT/chip" "$OUT/chip/main.cpp" 2>/dev/null; then built=yes; else built=no; fi
  case "$chip" in ""|ILI9341|ST7789) want=yes ;; *) want=no ;; esac
  if [ "$built" != "$want" ]; then
    echo "FAIL  DeskDisplay with CHIP \"$chip\": built=$built, expected $want"
    exit 1
  fi
done
echo "ok    DeskDisplay builds with CHIP \"\", ILI9341 or ST7789, and refuses any other name"

# The LED examples once more, built as for each Seeed XIAO whose LED lights on
# LOW (the macro its core build defines), and once with no board macro, where
# the LED lights on HIGH. The level is held to what the board needs, not to
# the sketch's own constant, so a misspelt macro fails.
for pair in LightByCommand:EX_LIGHT LightByCommandMqtt:EX_LIGHT_MQTT TwoBoards:EX_TWO; do
  ex=${pair%%:*}
  for board in ARDUINO_XIAO_ESP32S3:LOW ARDUINO_XIAO_ESP32S3_PLUS:LOW ARDUINO_XIAO_ESP32C6:LOW \
               ARDUINO_XIAO_ESP32C5:LOW NO_BOARD_MACRO:HIGH; do
    "$CXX" -std=c++11 -Wall -Wextra -O1 -Itest/sim -Isrc -I. -D"${pair#*:}" -D"${board%%:*}" \
      -DEXPECT_LED_ON="${board#*:}" -DEXAMPLE_INO="\"examples/$ex/$ex.ino\"" \
      -o "$OUT/example_${ex}_board" test/sim/example_run.cpp test/sim/fake_board.cpp src/Warblet.cpp \
      src/WarbletMqtt.cpp src/warblet/core/sha256.cpp src/warblet/core/text.cpp src/warblet/core/sign.cpp \
      src/warblet/core/wire.cpp src/warblet/core/json.cpp src/warblet/core/prov.cpp
    echo "      $ex built with ${board%%:*}"
    "$OUT/example_${ex}_board" record "$OUT/sent_${ex}_board.txt"
    if "$PYTHON" --version >/dev/null 2>&1; then
      "$PYTHON" test/run_decoders.py "examples/$ex" "$OUT/sent_${ex}_board.txt"
    fi
  done
done

# A board Warblet does not know stops; the shipped text says only why. Under
# contract v0.47 nobody can claim a board by knowing its id, so no shipped
# file may say they could.
if grep -rniE "could claim|anyone who knows" README.md docs src examples; then
  echo "FAIL  a shipped file says anyone could claim a board by its id"
  exit 1
fi
echo "ok    no shipped file says anyone could claim a board by its id"

# A stopped board leaves its Wi-Fi network; the radio stays on. No shipped
# file may say it turns Wi-Fi off.
if grep -rniE "wi-fi off" README.md docs src examples; then
  echo "FAIL  a shipped file says a board turns Wi-Fi off"
  exit 1
fi
echo "ok    no shipped file says a board turns Wi-Fi off"

# A push with a new id or token restarts the board: the README says so where
# a person sets a board up, and the reference where the settings are.
if ! tr '\n' ' ' < README.md | grep -q "The board restarts once a new device id or token is stored" ||
   ! tr '\n' ' ' < docs/REFERENCE.md | grep -q "Settings that change the device id or the token are a new start"; then
  echo "FAIL  the README or the reference no longer says a new id or token restarts the board"
  exit 1
fi
echo "ok    the README and the reference say a new id or token restarts the board"

# board.demo() makes the setup page treat the board as a published demo, so
# the reference row and the one sketch that calls it say who may call it.
for f in docs/REFERENCE.md examples/DeskDisplay/DeskDisplay.ino; do
  if ! tr '\n' ' ' < "$f" | sed 's#// # #g' | tr -s ' ' | grep -q "Only a published demo's own sketch"; then
    echo "FAIL  $f does not say that only a published demo's own sketch calls board.demo()"
    exit 1
  fi
done
echo "ok    the reference and DeskDisplay say only a published demo's own sketch calls board.demo()"

# The owner's rule for keys over the air: only a sketch with a command
# function gets one; every other board gets its key over USB. Both the README
# and the reference say so.
for f in README.md docs/REFERENCE.md; do
  if ! tr '\n' ' ' < "$f" | grep -q "key only over USB"; then
    echo "FAIL  $f does not say that other boards get their key only over USB"
    exit 1
  fi
done
echo "ok    the README and the reference say who gets a key over the air"
