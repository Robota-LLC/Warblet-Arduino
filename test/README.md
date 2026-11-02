# Host tests

Run from the repository root with a C++11 compiler, a C compiler, Python 3 and a POSIX shell:

```sh
sh test/run_tests.sh
```

On Windows, Git Bash and MinGW-w64 work. Set `CXX`, `CC`, `PYTHON` or `OUT` to choose tools or the output directory. No board, network or Arduino installation is needed.

Exit code 0 means all checks that ran passed. The output lists each group and its count. These tests do not prove a board build or a hardware workflow.

## Coverage

| Group | What it checks |
|---|---|
| Portable core | Signing, wire framing, JSON, setup merges, refusals and persisted records |
| CHIRP-PROV vectors | The shared v1.8 push and capability cases in `chirp-prov-v1.8.json` |
| Public API | Documented calls and overloads compile and link |
| Simulation | Setup, Wi-Fi recovery, reporting, commands, throttling and credential changes |
| ESP32 adapter | Serial buffering, clock, TLS, storage and MQTT events using stand-ins |
| Examples | Unmodified sketches, their LED outputs, payload decoders and refusal recovery |
| Desk display | Screen commands, touch acknowledgement, messages, disk readings and reconnect behaviour |

The setup checks cover 4,096-byte lines, 3 KiB settings records, the 1.5-second idle deadline and 70-second overall deadline. They also cover credential and network limits, NUL rejection, a stored key, device identity changes, and the firmware capability announcement. See [the reference](../docs/REFERENCE.md#settings) for the user-facing rules.

## Optional reference comparisons

Some HMAC and transport comparisons use a fixed reference checkout:

```sh
git clone --branch v1.0.2 https://github.com/Robota-LLC/Warblet-IoT.git ../wiot-ref
REF=../wiot-ref sh test/run_tests.sh
```

Without it, `demo-c` and `demo-cam` are skipped and committed vectors are used. Do not substitute a current demo checkout: those comparisons expect the reference's file layout. The current signing envelope is checked separately against contract examples and an independent Python oracle.

Real USB drivers, Wi-Fi, TLS connections, storage interruptions and screen hardware still need board tests.
