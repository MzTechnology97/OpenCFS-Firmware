# Verification without hardware (OpenCFS)

What is checked before any hardware qualification, and how.

| Check | Where | What it proves |
| --- | --- | --- |
| Host simulation tests | `cpp/tests/test_main.cpp` (MSVC /W4 /WX, GCC with ASan/UBSan in CI) | Protocol, feeder stages, preload, RFID decoders, runtime config interlock, application loop, against a deterministic filament-path simulator |
| Deterministic fuzzing | `test_fuzz` in the same binary | 200 000 noise bytes into the frame receiver, 4 000 random frames through the dispatcher (commands that cannot start motion, plus unknown codes), 20 000 random buffers through the NDEF/JSON parsers, the parameter-page codec and the `0x60` encoder: no crash, no UB under the sanitizers |
| **Host-code conformance** | `cpp/tests/kalico_conformance.py` + `cfs_host_bridge` | The real Kalico host modules (`box_protocol.py`, `serial_485.py`, `box_cfs_runtime.py`, loaded from a Kalico checkout, not copied here) talk to OpenCFS running on the simulator. Auto-addressing from power-up, every `BoxDriver` query, a forced RFID read, and Runtime Config API v2 (INFO, DESCRIBE and GET for all 28 IDs, descriptor validation against the host catalogue, SET with the interlock, RESET) must pass Kalico's own envelope, CRC, length and payload checks |
| ARM object build | CI, `arm-none-eabi-g++ -Werror -Wpedantic`, with and without local vendor keys | Every core and board file compiles for the Cortex-M4F |
| Static analysis | MSVC `/analyze` (local), `cppcheck` (CI, non-blocking for now) | No analyzer warnings on core and board sources |

Result on 2026-10-16: 269 host checks, 101 host-code conformance checks
(73 frames), 0 analyzer warnings.

## What this does not cover

- **Recorded stock traffic.** The Kalico logs on the reference printer do not
  contain raw RS-485 frames, so OpenCFS replies have not been compared byte for
  byte with stock replies. Conformance with the host code is checked instead.
  Capturing real traffic needs the bus or Kalico logging; both are left for
  later by choice.
- **Anything electrical:** feeder direction, hub PWM polarity and FG, current
  thresholds, odometer scale, RFID timings, flash page writes, the interlock with
  real motors. These need the hardware qualification step.

## Running it

```
cmake -S cpp -B build && cmake --build build && ctest --test-dir build
git clone --depth 1 --branch k2-pro-openhost https://github.com/MzTechnology97/kalico-k2pro /tmp/kalico
python3 cpp/tests/kalico_conformance.py --bridge build/cfs_host_bridge --extras /tmp/kalico/klippy/extras
```
